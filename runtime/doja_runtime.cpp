#if !defined(__ANDROID__) && !defined(__IPHONEOS__)
#define SDL_MAIN_HANDLED
#endif
#include "doja_runtime.hpp"
#include "net_config.hpp"
#include "touch_controls.hpp"
#include "../common/cp932_table.hpp"
#include "../common/localization.hpp"
#include "../common/pvp_protocol.hpp"
#include "../common/ymu762_fm.hpp"

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/prctl.h>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include "../common/stb_truetype.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace rmexe {

DojaRuntime* g_runtime = nullptr;

// ============================================================================
// Portable CJK + English 12px Font Engine (EBLC/EBDT 12x12 Bitmap + stb_truetype)
// ============================================================================
namespace {

inline int default_doja_char_width(uint32_t cp) {
    return (cp < 0x80 || (cp >= 0xFF61 && cp <= 0xFF9F)) ? 6 : 12;
}

inline bool in_bounds(const std::vector<uint8_t>& buf, size_t off, size_t len) {
    return off <= buf.size() && len <= buf.size() - off;
}

inline bool read_u16_be(const std::vector<uint8_t>& buf, size_t off, uint16_t& out) {
    if (!in_bounds(buf, off, 2)) return false;
    out = static_cast<uint16_t>((static_cast<uint16_t>(buf[off]) << 8) |
                                static_cast<uint16_t>(buf[off + 1]));
    return true;
}

inline bool read_u32_be(const std::vector<uint8_t>& buf, size_t off, uint32_t& out) {
    if (!in_bounds(buf, off, 4)) return false;
    out = (static_cast<uint32_t>(buf[off]) << 24) |
          (static_cast<uint32_t>(buf[off + 1]) << 16) |
          (static_cast<uint32_t>(buf[off + 2]) << 8) |
          static_cast<uint32_t>(buf[off + 3]);
    return true;
}

inline int8_t read_i8(const std::vector<uint8_t>& buf, size_t off) {
    return static_cast<int8_t>(buf[off]);
}

inline bool has_font_extension(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    for (char& ch : ext) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return ext == ".ttf" || ext == ".ttc" || ext == ".otf";
}

inline std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

inline bool looks_like_pixel_font_path(const std::string& path) {
    std::string fn = to_lower_ascii(std::filesystem::path(path).filename().string());
    return fn.find("pixel") != std::string::npos ||
           fn.find("zpix") != std::string::npos ||
           fn.find("cubic") != std::string::npos ||
           fn.find("bitmap") != std::string::npos ||
           fn.find("unifont") != std::string::npos ||
           fn.find("misaki") != std::string::npos ||
           fn.find("galmuri") != std::string::npos ||
           fn.find("dotgothic") != std::string::npos ||
           fn.find("boutique") != std::string::npos ||
           fn == "msgothic.ttc" || fn == "simsun.ttc" || fn == "mingliu.ttc";
}

// Premultiplied ARGB8888 Porter-Duff OVER compositing helpers for decoupled text overlays
inline void blend_pm_over(uint32_t& dst_pm, uint8_t sr, uint8_t sg, uint8_t sb, uint32_t sa) {
    if (sa == 0) return;
    uint32_t spr = (static_cast<uint32_t>(sr) * sa + 127u) / 255u;
    uint32_t spg = (static_cast<uint32_t>(sg) * sa + 127u) / 255u;
    uint32_t spb = (static_cast<uint32_t>(sb) * sa + 127u) / 255u;
    if (sa >= 255u || dst_pm == 0u) {
        dst_pm = (sa << 24) | (spr << 16) | (spg << 8) | spb;
        return;
    }
    uint32_t inv_a = 255u - sa;
    uint32_t da = (dst_pm >> 24) & 0xFFu;
    uint32_t dpr = (dst_pm >> 16) & 0xFFu;
    uint32_t dpg = (dst_pm >> 8) & 0xFFu;
    uint32_t dpb = dst_pm & 0xFFu;
    uint32_t oa = std::min<uint32_t>(255u, sa + (da * inv_a + 127u) / 255u);
    uint32_t opr = std::min<uint32_t>(255u, spr + (dpr * inv_a + 127u) / 255u);
    uint32_t opg = std::min<uint32_t>(255u, spg + (dpg * inv_a + 127u) / 255u);
    uint32_t opb = std::min<uint32_t>(255u, spb + (dpb * inv_a + 127u) / 255u);
    dst_pm = (oa << 24) | (opr << 16) | (opg << 8) | opb;
}

inline void blend_pm_pixel_over(uint32_t& dst_pm, uint32_t src_pm) {
    uint32_t sa = (src_pm >> 24) & 0xFFu;
    if (sa == 0) return;
    if (sa >= 255u || dst_pm == 0u) {
        dst_pm = src_pm;
        return;
    }
    uint32_t inv_a = 255u - sa;
    uint32_t spr = (src_pm >> 16) & 0xFFu;
    uint32_t spg = (src_pm >> 8) & 0xFFu;
    uint32_t spb = src_pm & 0xFFu;
    uint32_t da = (dst_pm >> 24) & 0xFFu;
    uint32_t dpr = (dst_pm >> 16) & 0xFFu;
    uint32_t dpg = (dst_pm >> 8) & 0xFFu;
    uint32_t dpb = dst_pm & 0xFFu;
    uint32_t oa = std::min<uint32_t>(255u, sa + (da * inv_a + 127u) / 255u);
    uint32_t opr = std::min<uint32_t>(255u, spr + (dpr * inv_a + 127u) / 255u);
    uint32_t opg = std::min<uint32_t>(255u, spg + (dpg * inv_a + 127u) / 255u);
    uint32_t opb = std::min<uint32_t>(255u, spb + (dpb * inv_a + 127u) / 255u);
    dst_pm = (oa << 24) | (opr << 16) | (opg << 8) | opb;
}

inline uint32_t composite_pm_over_opaque(uint32_t bg_rgb, uint32_t pm) {
    uint32_t sa = (pm >> 24) & 0xFFu;
    if (sa == 0) return bg_rgb | 0xFF000000u;
    if (sa >= 255u) return pm | 0xFF000000u;
    uint32_t inv_a = 255u - sa;
    uint32_t spr = (pm >> 16) & 0xFFu;
    uint32_t spg = (pm >> 8) & 0xFFu;
    uint32_t spb = pm & 0xFFu;
    uint32_t dr = (bg_rgb >> 16) & 0xFFu;
    uint32_t dg = (bg_rgb >> 8) & 0xFFu;
    uint32_t db = bg_rgb & 0xFFu;
    uint32_t nr = std::min<uint32_t>(255u, spr + (dr * inv_a + 127u) / 255u);
    uint32_t ng = std::min<uint32_t>(255u, spg + (dg * inv_a + 127u) / 255u);
    uint32_t nb = std::min<uint32_t>(255u, spb + (db * inv_a + 127u) / 255u);
    return 0xFF000000u | (nr << 16) | (ng << 8) | nb;
}

inline void attenuate_pm(uint32_t& dst_pm, uint32_t cover_alpha) {
    if (dst_pm == 0 || cover_alpha == 0) return;
    if (cover_alpha >= 255u) {
        dst_pm = 0;
        return;
    }
    uint32_t keep = 255u - cover_alpha;
    uint32_t da = ((dst_pm >> 24) & 0xFFu) * keep / 255u;
    uint32_t dpr = ((dst_pm >> 16) & 0xFFu) * keep / 255u;
    uint32_t dpg = ((dst_pm >> 8) & 0xFFu) * keep / 255u;
    uint32_t dpb = (dst_pm & 0xFFu) * keep / 255u;
    dst_pm = (da << 24) | (dpr << 16) | (dpg << 8) | dpb;
}

inline void clear_hd_block(uint32_t* hd_buf, int logic_w, int logic_h, int hd_scale, int x, int y) {
    if (!hd_buf || x < 0 || y < 0 || x >= logic_w || y >= logic_h) return;
    int hw = logic_w * hd_scale;
    int hy0 = y * hd_scale;
    int hx0 = x * hd_scale;
    for (int sy = 0; sy < hd_scale; ++sy) {
        uint32_t* row = hd_buf + (hy0 + sy) * hw + hx0;
        for (int sx = 0; sx < hd_scale; ++sx) {
            row[sx] = 0;
        }
    }
}

inline void attenuate_hd_block(uint32_t* hd_buf, int logic_w, int logic_h, int hd_scale, int x, int y, uint32_t cover_alpha) {
    if (!hd_buf || x < 0 || y < 0 || x >= logic_w || y >= logic_h || cover_alpha == 0) return;
    if (cover_alpha >= 255u) {
        clear_hd_block(hd_buf, logic_w, logic_h, hd_scale, x, y);
        return;
    }
    int hw = logic_w * hd_scale;
    int hy0 = y * hd_scale;
    int hx0 = x * hd_scale;
    for (int sy = 0; sy < hd_scale; ++sy) {
        uint32_t* row = hd_buf + (hy0 + sy) * hw + hx0;
        for (int sx = 0; sx < hd_scale; ++sx) {
            attenuate_pm(row[sx], cover_alpha);
        }
    }
}

std::atomic<bool> g_quit_signal{false};

void on_termination_signal(int sig) {
    g_quit_signal.store(true, std::memory_order_relaxed);
    std::signal(sig, SIG_DFL);
}

std::string get_exe_base_dir() {
    char* base = SDL_GetBasePath();
    if (!base) return "";
    std::string s(base);
    SDL_free(base);
    return s;
}

std::string resolve_runtime_path(const std::string& rel_or_abs, const std::string& base_dir) {
    if (rel_or_abs.empty()) return rel_or_abs;
    std::error_code ec;
    if (std::filesystem::exists(rel_or_abs, ec) || std::filesystem::path(rel_or_abs).is_absolute() || base_dir.empty()) {
        return rel_or_abs;
    }
    std::filesystem::path cand = std::filesystem::path(base_dir) / rel_or_abs;
    if (std::filesystem::exists(cand, ec)) {
        return cand.string();
    }
    return rel_or_abs;
}

struct StbFontEngine {
    struct CachedGlyph {
        bool valid = false;
        bool is_bitmap = false;
        int adv_x = 6;
        int xoff = 0;
        int yoff = 0;
        int width = 0;
        int rows = 0;
        std::vector<uint8_t> bmp;
    };

    struct FontFace {
        std::shared_ptr<const std::vector<uint8_t>> font_bytes;
        stbtt_fontinfo info{};
        float scale = 1.0f;
        float pixel_scale = 1.0f;
        bool has_cjk = false;
        bool has_eblc_12 = false;
        bool is_pixel_outline = false;
        uint32_t eblc_off = 0;
        uint32_t eblc_len = 0;
        uint32_t ebdt_off = 0;
        uint32_t ebdt_len = 0;
        uint32_t subtable_array_off = 0;
        uint32_t num_subtables = 0;
        std::unordered_map<int, int> hd_y_adjust_by_scale;
    };

    std::string cli_pixel_font_path;
    std::string cli_hd_font_path;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<uint8_t>>> file_bytes_cache;
    std::vector<FontFace> pixel_faces;
    std::vector<FontFace> hd_faces;
    std::vector<std::string> pixel_loaded_keys;
    std::vector<std::string> hd_loaded_keys;
    bool loaded = false;
    bool initialized = false;
    std::unordered_map<uint32_t, CachedGlyph> pixel_cache;
    std::unordered_map<uint64_t, CachedGlyph> hd_cache;

    static std::string canonical_path_key(const std::string& path) {
        std::error_code ec;
        std::filesystem::path p(path);
        std::filesystem::path canon = std::filesystem::weakly_canonical(p, ec);
        std::string key = ec ? p.lexically_normal().string() : canon.string();
#ifdef _WIN32
        for (char& ch : key) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
#endif
        return key;
    }

    static bool parse_eblc_12(FontFace& face, int sfnt_off) {
        if (sfnt_off < 0 || !face.font_bytes) return false;
        const auto& buf = *face.font_bytes;
        uint16_t num_tables = 0;
        if (!read_u16_be(buf, static_cast<size_t>(sfnt_off) + 4, num_tables)) return false;
        if (num_tables == 0 || num_tables > 256) return false;
        size_t dir_off = static_cast<size_t>(sfnt_off) + 12;
        if (!in_bounds(buf, dir_off, static_cast<size_t>(num_tables) * 16)) return false;

        uint32_t eblc_off = 0, eblc_len = 0;
        uint32_t ebdt_off = 0, ebdt_len = 0;
        uint32_t bloc_off = 0, bloc_len = 0;
        uint32_t bdat_off = 0, bdat_len = 0;

        for (uint16_t i = 0; i < num_tables; ++i) {
            size_t rec = dir_off + static_cast<size_t>(i) * 16;
            uint32_t toff = 0, tlen = 0;
            if (!read_u32_be(buf, rec + 8, toff) || !read_u32_be(buf, rec + 12, tlen)) continue;
            if (!in_bounds(buf, toff, tlen)) continue;
            if (std::memcmp(&buf[rec], "EBLC", 4) == 0) {
                eblc_off = toff; eblc_len = tlen;
            } else if (std::memcmp(&buf[rec], "EBDT", 4) == 0) {
                ebdt_off = toff; ebdt_len = tlen;
            } else if (std::memcmp(&buf[rec], "bloc", 4) == 0) {
                bloc_off = toff; bloc_len = tlen;
            } else if (std::memcmp(&buf[rec], "bdat", 4) == 0) {
                bdat_off = toff; bdat_len = tlen;
            }
        }

        if (eblc_len == 0 || ebdt_len == 0) {
            eblc_off = bloc_off; eblc_len = bloc_len;
            ebdt_off = bdat_off; ebdt_len = bdat_len;
        }
        if (eblc_len < 8 || ebdt_len < 4) return false;

        uint32_t num_sizes = 0;
        if (!read_u32_be(buf, static_cast<size_t>(eblc_off) + 4, num_sizes)) return false;
        if (num_sizes == 0 || num_sizes > 1024) return false;
        if (!in_bounds(buf, static_cast<size_t>(eblc_off) + 8, static_cast<size_t>(num_sizes) * 48)) {
            return false;
        }

        for (uint32_t s = 0; s < num_sizes; ++s) {
            size_t bo = static_cast<size_t>(eblc_off) + 8 + static_cast<size_t>(s) * 48;
            uint8_t ppem_x = buf[bo + 44];
            uint8_t ppem_y = buf[bo + 45];
            uint8_t bit_depth = buf[bo + 46];
            if (ppem_x != 12 || ppem_y != 12 || bit_depth != 1) continue;

            uint32_t sub_arr_rel = 0, num_sub = 0;
            if (!read_u32_be(buf, bo, sub_arr_rel) || !read_u32_be(buf, bo + 8, num_sub)) continue;
            if (sub_arr_rel > eblc_len || num_sub == 0 || num_sub > 65536) continue;
            uint64_t sub_arr_abs = static_cast<uint64_t>(eblc_off) + sub_arr_rel;
            if (sub_arr_abs > static_cast<uint64_t>(eblc_off) + eblc_len) continue;
            if (!in_bounds(buf, static_cast<size_t>(sub_arr_abs), static_cast<size_t>(num_sub) * 8)) continue;

            face.has_eblc_12 = true;
            face.eblc_off = eblc_off;
            face.eblc_len = eblc_len;
            face.ebdt_off = ebdt_off;
            face.ebdt_len = ebdt_len;
            face.subtable_array_off = static_cast<uint32_t>(sub_arr_abs);
            face.num_subtables = num_sub;
            return true;
        }
        return false;
    }

    static bool try_get_eblc_glyph(const FontFace& face, uint32_t cp, CachedGlyph& cg) {
        if (!face.has_eblc_12 || !face.font_bytes || cp > 0xFFFFu) return false;
        int gid_i = stbtt_FindGlyphIndex(&face.info, static_cast<int>(cp));
        if (gid_i <= 0 || gid_i > 0xFFFF) return false;
        uint16_t gid = static_cast<uint16_t>(gid_i);

        const auto& buf = *face.font_bytes;
        const size_t eblc_end = static_cast<size_t>(face.eblc_off) + face.eblc_len;
        const size_t ebdt_end = static_cast<size_t>(face.ebdt_off) + face.ebdt_len;

        for (uint32_t st = 0; st < face.num_subtables; ++st) {
            size_t entry = static_cast<size_t>(face.subtable_array_off) + static_cast<size_t>(st) * 8;
            uint16_t fg = 0, lg = 0;
            uint32_t add_off = 0;
            if (!read_u16_be(buf, entry, fg) || !read_u16_be(buf, entry + 2, lg) ||
                !read_u32_be(buf, entry + 4, add_off)) {
                continue;
            }
            if (gid < fg || gid > lg) continue;

            uint64_t ist64 = static_cast<uint64_t>(face.subtable_array_off) + add_off;
            if (ist64 + 8 > eblc_end) continue;
            size_t ist = static_cast<size_t>(ist64);

            uint16_t idx_fmt = 0, img_fmt = 0;
            uint32_t img_data_off = 0;
            if (!read_u16_be(buf, ist, idx_fmt) || !read_u16_be(buf, ist + 2, img_fmt) ||
                !read_u32_be(buf, ist + 4, img_data_off)) {
                continue;
            }
            if (img_data_off > face.ebdt_len) continue;
            uint64_t ebdt_base = static_cast<uint64_t>(face.ebdt_off) + img_data_off;

            bool found_glyph = false;
            uint64_t g_off64 = 0;
            uint32_t g_len = 0;
            bool has_const_metrics = false;
            uint8_t ch = 0, cw = 0, cadv = 0;
            int8_t cbx = 0, cby = 0;

            if (idx_fmt == 1) {
                size_t rel = static_cast<size_t>(gid - fg);
                size_t off_pos = ist + 8 + rel * 4;
                if (off_pos + 8 > eblc_end) continue;
                uint32_t o1 = 0, o2 = 0;
                if (read_u32_be(buf, off_pos, o1) && read_u32_be(buf, off_pos + 4, o2) && o2 > o1) {
                    g_off64 = ebdt_base + o1;
                    g_len = o2 - o1;
                    found_glyph = true;
                }
            } else if (idx_fmt == 2) {
                if (ist + 20 > eblc_end) continue;
                uint32_t img_sz = 0;
                if (!read_u32_be(buf, ist + 8, img_sz) || img_sz == 0) continue;
                ch = buf[ist + 12];
                cw = buf[ist + 13];
                cbx = read_i8(buf, ist + 14);
                cby = read_i8(buf, ist + 15);
                cadv = buf[ist + 16];
                has_const_metrics = true;
                g_off64 = ebdt_base + static_cast<uint64_t>(gid - fg) * img_sz;
                g_len = img_sz;
                found_glyph = true;
            } else if (idx_fmt == 3) {
                size_t rel = static_cast<size_t>(gid - fg);
                size_t off_pos = ist + 8 + rel * 2;
                if (off_pos + 4 > eblc_end) continue;
                uint16_t o1 = 0, o2 = 0;
                if (read_u16_be(buf, off_pos, o1) && read_u16_be(buf, off_pos + 2, o2) && o2 > o1) {
                    g_off64 = ebdt_base + o1;
                    g_len = static_cast<uint32_t>(o2 - o1);
                    found_glyph = true;
                }
            } else if (idx_fmt == 4) {
                if (ist + 12 > eblc_end) continue;
                uint32_t num_g = 0;
                if (!read_u32_be(buf, ist + 8, num_g) || num_g == 0) continue;
                if (static_cast<uint64_t>(ist) + 12 + (static_cast<uint64_t>(num_g) + 1) * 4 > eblc_end) continue;
                uint32_t lo = 0, hi = num_g;
                while (lo < hi) {
                    uint32_t mid = lo + (hi - lo) / 2;
                    uint16_t cgid = 0;
                    read_u16_be(buf, ist + 12 + static_cast<size_t>(mid) * 4, cgid);
                    if (cgid < gid) lo = mid + 1;
                    else hi = mid;
                }
                if (lo < num_g) {
                    size_t pair_off = ist + 12 + static_cast<size_t>(lo) * 4;
                    uint16_t cgid = 0, o1 = 0, o2 = 0;
                    if (read_u16_be(buf, pair_off, cgid) && cgid == gid &&
                        read_u16_be(buf, pair_off + 2, o1) && read_u16_be(buf, pair_off + 6, o2) && o2 > o1) {
                        g_off64 = ebdt_base + o1;
                        g_len = static_cast<uint32_t>(o2 - o1);
                        found_glyph = true;
                    }
                }
            } else if (idx_fmt == 5) {
                if (ist + 24 > eblc_end) continue;
                uint32_t img_sz = 0, num_g = 0;
                if (!read_u32_be(buf, ist + 8, img_sz) || img_sz == 0) continue;
                if (!read_u32_be(buf, ist + 20, num_g) || num_g == 0) continue;
                if (static_cast<uint64_t>(ist) + 24 + static_cast<uint64_t>(num_g) * 2 > eblc_end) continue;
                ch = buf[ist + 12];
                cw = buf[ist + 13];
                cbx = read_i8(buf, ist + 14);
                cby = read_i8(buf, ist + 15);
                cadv = buf[ist + 16];
                has_const_metrics = true;
                uint32_t lo = 0, hi = num_g;
                while (lo < hi) {
                    uint32_t mid = lo + (hi - lo) / 2;
                    uint16_t cgid = 0;
                    read_u16_be(buf, ist + 24 + static_cast<size_t>(mid) * 2, cgid);
                    if (cgid < gid) lo = mid + 1;
                    else hi = mid;
                }
                if (lo < num_g) {
                    uint16_t cgid = 0;
                    if (read_u16_be(buf, ist + 24 + static_cast<size_t>(lo) * 2, cgid) && cgid == gid) {
                        g_off64 = ebdt_base + static_cast<uint64_t>(lo) * img_sz;
                        g_len = img_sz;
                        found_glyph = true;
                    }
                }
            }

            if (!found_glyph) continue;
            if (g_off64 > ebdt_end || g_len > ebdt_end - g_off64) continue;
            size_t g_off = static_cast<size_t>(g_off64);

            uint8_t h = 0, w = 0, adv = 0;
            int8_t bx = 0, by = 0;
            size_t bmp_off = g_off;
            uint32_t hdr_sz = 0;
            bool byte_aligned = false;

            if (img_fmt == 1 || img_fmt == 2) {
                if (g_len < 5) return false;
                h = buf[g_off];
                w = buf[g_off + 1];
                bx = read_i8(buf, g_off + 2);
                by = read_i8(buf, g_off + 3);
                adv = buf[g_off + 4];
                hdr_sz = 5;
                bmp_off = g_off + 5;
                byte_aligned = (img_fmt == 1);
            } else if (img_fmt == 5) {
                if (!has_const_metrics) return false;
                h = ch; w = cw; bx = cbx; by = cby; adv = cadv;
                hdr_sz = 0;
                bmp_off = g_off;
                byte_aligned = false;
            } else if (img_fmt == 6 || img_fmt == 7) {
                if (g_len < 8) return false;
                h = buf[g_off];
                w = buf[g_off + 1];
                bx = read_i8(buf, g_off + 2);
                by = read_i8(buf, g_off + 3);
                adv = buf[g_off + 4];
                hdr_sz = 8;
                bmp_off = g_off + 8;
                byte_aligned = (img_fmt == 6);
            } else {
                return false;
            }

            if (w == 0 || h == 0) {
                cg.valid = true;
                cg.is_bitmap = true;
                cg.width = 0;
                cg.rows = 0;
                cg.xoff = 0;
                cg.yoff = 0;
                cg.adv_x = (adv > 0) ? static_cast<int>(adv) : default_doja_char_width(cp);
                cg.bmp.clear();
                return true;
            }
            if (w > 32 || h > 32) return false;

            uint32_t bpr = byte_aligned ? ((static_cast<uint32_t>(w) + 7u) / 8u) : 0u;
            uint32_t need_bytes = byte_aligned
                ? (bpr * static_cast<uint32_t>(h))
                : ((static_cast<uint32_t>(w) * static_cast<uint32_t>(h) + 7u) / 8u);
            if (hdr_sz + need_bytes > g_len || !in_bounds(buf, bmp_off, need_bytes)) return false;

            cg.width = static_cast<int>(w);
            cg.rows = static_cast<int>(h);
            cg.xoff = static_cast<int>(bx);
            cg.yoff = -static_cast<int>(by);
            cg.adv_x = (adv > 0) ? static_cast<int>(adv) : default_doja_char_width(cp);
            cg.bmp.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);

            for (int ry = 0; ry < cg.rows; ++ry) {
                for (int rx = 0; rx < cg.width; ++rx) {
                    uint8_t byte_val = 0;
                    uint8_t mask = 0;
                    if (byte_aligned) {
                        byte_val = buf[bmp_off + static_cast<size_t>(ry) * bpr + static_cast<size_t>(rx >> 3)];
                        mask = static_cast<uint8_t>(0x80u >> (rx & 7));
                    } else {
                        size_t bit_idx = static_cast<size_t>(ry) * static_cast<size_t>(w) + static_cast<size_t>(rx);
                        byte_val = buf[bmp_off + (bit_idx >> 3)];
                        mask = static_cast<uint8_t>(0x80u >> (bit_idx & 7));
                    }
                    if ((byte_val & mask) != 0) {
                        cg.bmp[static_cast<size_t>(ry) * static_cast<size_t>(w) + static_cast<size_t>(rx)] = 255;
                    }
                }
            }
            cg.is_bitmap = true;
            cg.valid = true;
            return true;
        }
        return false;
    }

    bool try_load_face_into(std::vector<FontFace>& target_faces,
                            std::vector<std::string>& target_keys,
                            const std::string& path,
                            bool require_cjk,
                            bool require_eblc_only = false,
                            bool force_pixel_outline = false,
                            bool prefer_sc_ttc = false) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) return false;

        std::string key = canonical_path_key(path);
        if (std::find(target_keys.begin(), target_keys.end(), key) != target_keys.end()) {
            return false;
        }

        std::shared_ptr<const std::vector<uint8_t>> bytes_ptr;
        auto cache_it = file_bytes_cache.find(key);
        if (cache_it != file_bytes_cache.end()) {
            bytes_ptr = cache_it->second;
        } else {
            std::ifstream f(path, std::ios::binary);
            if (!f) return false;
            auto vec = std::make_shared<std::vector<uint8_t>>(
                (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()
            );
            if (vec->size() < 64) return false;
            bytes_ptr = vec;
            file_bytes_cache.emplace(key, bytes_ptr);
        }

        int ttc_idx = 0;
        if (prefer_sc_ttc && to_lower_ascii(path).find("notosanscjk") != std::string::npos) {
            if (stbtt_GetFontOffsetForIndex(bytes_ptr->data(), 2) >= 0) {
                ttc_idx = 2; // NotoSansCJK TTC index 2 is SC
            }
        }
        int offset = stbtt_GetFontOffsetForIndex(bytes_ptr->data(), ttc_idx);
        if (offset < 0) offset = stbtt_GetFontOffsetForIndex(bytes_ptr->data(), 0);
        if (offset < 0) return false;

        FontFace face{};
        face.font_bytes = bytes_ptr;
        if (!stbtt_InitFont(&face.info, face.font_bytes->data(), offset)) return false;

        face.has_cjk = (stbtt_FindGlyphIndex(&face.info, 0x3042) != 0 ||
                        stbtt_FindGlyphIndex(&face.info, 0x4E2D) != 0);
        if (require_cjk && !face.has_cjk) return false;

        parse_eblc_12(face, offset);
        if (require_eblc_only && !face.has_eblc_12) return false;

        face.scale = stbtt_ScaleForMappingEmToPixels(&face.info, 12.0f);
        if (face.scale <= 0.0f) {
            face.scale = stbtt_ScaleForPixelHeight(&face.info, 13.0f);
        }
        face.pixel_scale = stbtt_ScaleForMappingEmToPixels(&face.info, 12.0f);
        if (face.pixel_scale <= 0.0f) {
            face.pixel_scale = face.scale;
        }
        if (!face.has_eblc_12 && (force_pixel_outline || looks_like_pixel_font_path(path))) {
            face.is_pixel_outline = true;
        }

        target_keys.push_back(std::move(key));
        target_faces.push_back(std::move(face));
        return true;
    }

    void init(const std::string& base_dir, const std::string& extra_asset_dir = "") {
        if (initialized) return;
        initialized = true;
        pixel_faces.clear();
        hd_faces.clear();
        pixel_loaded_keys.clear();
        hd_loaded_keys.clear();
        pixel_cache.clear();
        hd_cache.clear();

        std::vector<std::string> asset_dirs;
        if (!extra_asset_dir.empty()) {
            asset_dirs.push_back((std::filesystem::path(extra_asset_dir) / "fonts").string());
            asset_dirs.push_back((std::filesystem::path(extra_asset_dir) / "assets" / "fonts").string());
        }
        if (!base_dir.empty()) {
            asset_dirs.push_back((std::filesystem::path(base_dir) / "assets" / "fonts").string());
        }
        asset_dirs.push_back("assets/fonts");

        std::vector<std::string> scanned_assets;
        for (const std::string& dir : asset_dirs) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) continue;
            std::vector<std::string> extra;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                if (ec) break;
                if (entry.is_regular_file(ec) && has_font_extension(entry.path())) {
                    extra.push_back(entry.path().string());
                }
            }
            std::sort(extra.begin(), extra.end());
            scanned_assets.insert(scanned_assets.end(), extra.begin(), extra.end());
        }

#ifdef _WIN32
        const char* win_dir = std::getenv("WINDIR");
        std::string wfonts = (win_dir && *win_dir) ? (std::string(win_dir) + "\\Fonts\\") : "C:\\Windows\\Fonts\\";
#endif

        // ---- Chain 1: 12px Pixel Font Faces ----
        if (!cli_pixel_font_path.empty()) {
            std::string p = resolve_runtime_path(cli_pixel_font_path, base_dir);
            try_load_face_into(pixel_faces, pixel_loaded_keys, p, false, false, true);
        }

        const char* pixel_priority_names[] = {
            "pixel12.ttf", "pixel12.ttc", "fusion-pixel.ttf", "zpix.ttf",
            "msgothic.ttc", "simsun.ttc", "unifont.ttf"
        };
        for (const std::string& dir : asset_dirs) {
            for (const char* name : pixel_priority_names) {
                try_load_face_into(pixel_faces, pixel_loaded_keys, (std::filesystem::path(dir) / name).string(), false);
            }
        }
        for (const std::string& p : scanned_assets) {
            if (looks_like_pixel_font_path(p)) {
                try_load_face_into(pixel_faces, pixel_loaded_keys, p, false);
            }
        }

        // System 12x12 EBLC/EBDT bitmap fonts (Windows & WSL2)
#ifdef _WIN32
        try_load_face_into(pixel_faces, pixel_loaded_keys, wfonts + "msgothic.ttc", false, true);
        try_load_face_into(pixel_faces, pixel_loaded_keys, wfonts + "simsun.ttc", false, true);
        try_load_face_into(pixel_faces, pixel_loaded_keys, wfonts + "mingliu.ttc", false, true);
#else
        try_load_face_into(pixel_faces, pixel_loaded_keys, "/mnt/c/Windows/Fonts/msgothic.ttc", false, true);
        try_load_face_into(pixel_faces, pixel_loaded_keys, "/mnt/c/Windows/Fonts/simsun.ttc", false, true);
        try_load_face_into(pixel_faces, pixel_loaded_keys, "/mnt/c/Windows/Fonts/mingliu.ttc", false, true);
#endif

        // Pixel vector fallback (only 1 CJK fallback so missing glyphs stay readable)
        std::vector<std::string> cjk_fallbacks;
        for (const std::string& dir : asset_dirs) {
            cjk_fallbacks.push_back((std::filesystem::path(dir) / "wqy-microhei.ttc").string());
        }
        if (!base_dir.empty()) {
            cjk_fallbacks.push_back((std::filesystem::path(base_dir) / "wqy-microhei.ttc").string());
        }
        cjk_fallbacks.push_back("wqy-microhei.ttc");
#ifdef _WIN32
        cjk_fallbacks.push_back(wfonts + "msyh.ttc");
        cjk_fallbacks.push_back(wfonts + "YuGothR.ttc");
        cjk_fallbacks.push_back(wfonts + "meiryo.ttc");
#elif defined(__ANDROID__)
        cjk_fallbacks.push_back("/system/fonts/NotoSansCJK-Regular.ttc");
        cjk_fallbacks.push_back("/system/fonts/NotoSerifCJK-Regular.ttc");
        cjk_fallbacks.push_back("/system/fonts/DroidSansFallback.ttf");
#else
        cjk_fallbacks.push_back("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc");
        cjk_fallbacks.push_back("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc");
        cjk_fallbacks.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
        cjk_fallbacks.push_back("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc");
        cjk_fallbacks.push_back("/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc");
#endif
        for (const auto& p : cjk_fallbacks) {
            if (try_load_face_into(pixel_faces, pixel_loaded_keys, p, true)) break;
        }
        if (pixel_faces.empty()) {
            const char* fallback_paths[] = {
#ifdef _WIN32
                "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__ANDROID__)
                "/system/fonts/Roboto-Regular.ttf",
                "/system/fonts/DroidSans.ttf",
#else
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
#endif
            };
            for (const char* p : fallback_paths) {
                if (try_load_face_into(pixel_faces, pixel_loaded_keys, p, false)) break;
            }
        }

        // ---- Chain 2: HD Smooth Vector Font Faces ----
        if (!cli_hd_font_path.empty()) {
            std::string p = resolve_runtime_path(cli_hd_font_path, base_dir);
            try_load_face_into(hd_faces, hd_loaded_keys, p, false, false, false, true);
        }

        const char* hd_priority_names[] = {
            "hd_zh.ttf", "hd_zh.otf", "hd_zh.ttc",
            "NotoSansSC-Medium.ttf", "NotoSansSC_500Medium.ttf", "NotoSansSC-Regular.ttf",
            "sarasa-ui-sc-regular.ttf", "ChillDuanSans_Medium.otf", "wqy-microhei.ttc"
        };
        for (const std::string& dir : asset_dirs) {
            for (const char* name : hd_priority_names) {
                try_load_face_into(hd_faces, hd_loaded_keys, (std::filesystem::path(dir) / name).string(), false, false, false, true);
            }
        }
        for (const std::string& p : scanned_assets) {
            if (!looks_like_pixel_font_path(p)) {
                try_load_face_into(hd_faces, hd_loaded_keys, p, false, false, false, true);
            }
        }

        std::vector<std::string> hd_sys_fallbacks;
#ifdef _WIN32
        hd_sys_fallbacks.push_back(wfonts + "msyh.ttc");
        hd_sys_fallbacks.push_back(wfonts + "NotoSansSC-VF.ttf");
        hd_sys_fallbacks.push_back(wfonts + "Deng.ttf");
        hd_sys_fallbacks.push_back(wfonts + "YuGothM.ttc");
        hd_sys_fallbacks.push_back(wfonts + "meiryo.ttc");
#elif defined(__ANDROID__)
        hd_sys_fallbacks.push_back("/system/fonts/NotoSansCJK-Regular.ttc");
        hd_sys_fallbacks.push_back("/system/fonts/DroidSansFallback.ttf");
#else
        hd_sys_fallbacks.push_back("/mnt/c/Windows/Fonts/msyh.ttc");
        hd_sys_fallbacks.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
        hd_sys_fallbacks.push_back("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc");
        hd_sys_fallbacks.push_back("/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc");
        hd_sys_fallbacks.push_back("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc");
        hd_sys_fallbacks.push_back("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc");
#endif
        for (const auto& p : hd_sys_fallbacks) {
            if (try_load_face_into(hd_faces, hd_loaded_keys, p, true, false, false, true)) break;
        }
        if (hd_faces.empty()) {
            const char* fallback_paths[] = {
#ifdef _WIN32
                "C:\\Windows\\Fonts\\arial.ttf",
#else
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
#endif
            };
            for (const char* p : fallback_paths) {
                if (try_load_face_into(hd_faces, hd_loaded_keys, p, false)) break;
            }
        }

        loaded = !pixel_faces.empty() || !hd_faces.empty();
    }

    const CachedGlyph* get_glyph(uint32_t cp) {
        if (pixel_faces.empty()) return nullptr;
        auto it = pixel_cache.find(cp);
        if (it != pixel_cache.end()) {
            return it->second.valid ? &it->second : nullptr;
        }

        CachedGlyph cg{};
        // Pass 1: Respect priority order across EBLC/EBDT strikes and dedicated pixel-outline fonts
        for (const auto& face : pixel_faces) {
            if (face.has_eblc_12) {
                if (try_get_eblc_glyph(face, cp, cg)) {
                    auto [ins_it, _] = pixel_cache.emplace(cp, std::move(cg));
                    return &ins_it->second;
                }
            } else if (face.is_pixel_outline) {
                int gidx = stbtt_FindGlyphIndex(&face.info, static_cast<int>(cp));
                if (gidx == 0) continue;

                int adv_raw = 0, lsb_raw = 0;
                stbtt_GetCodepointHMetrics(&face.info, static_cast<int>(cp), &adv_raw, &lsb_raw);
                int adv_px = static_cast<int>(std::lround(adv_raw * face.pixel_scale));

                int w = 0, h = 0, xoff = 0, yoff = 0;
                unsigned char* raw_bmp = stbtt_GetCodepointBitmap(&face.info, face.pixel_scale, face.pixel_scale, static_cast<int>(cp), &w, &h, &xoff, &yoff);
                if (raw_bmp && w > 0 && h > 0 && w < 64 && h < 64) {
                    cg.width = w;
                    cg.rows = h;
                    cg.xoff = xoff;
                    if (h <= 12) {
                        if (yoff < -10) yoff = -10;
                        else if (yoff + h > 2) yoff = 2 - h;
                    }
                    cg.yoff = yoff;
                    cg.bmp.resize(static_cast<size_t>(w * h));
                    for (int k = 0; k < w * h; ++k) {
                        cg.bmp[k] = (raw_bmp[k] >= 112) ? 255 : 0;
                    }
                } else {
                    cg.xoff = 0;
                    cg.yoff = 0;
                }
                if (raw_bmp) stbtt_FreeBitmap(raw_bmp, nullptr);

                int def_w = default_doja_char_width(cp);
                cg.adv_x = (cp < 0x80u && cg.width + std::max(0, cg.xoff) > def_w && adv_px > def_w) ? adv_px : def_w;
                cg.is_bitmap = true;
                cg.valid = true;
                auto [ins_it, _] = pixel_cache.emplace(cp, std::move(cg));
                return &ins_it->second;
            }
        }

        // Pass 2: Vector outline fallback via stb_truetype
        for (size_t i = 0; i < pixel_faces.size(); ++i) {
            const auto& face = pixel_faces[i];
            int gidx = stbtt_FindGlyphIndex(&face.info, static_cast<int>(cp));
            if (gidx == 0 && !(cp <= 0x7Fu && i + 1 == pixel_faces.size())) {
                continue;
            }

            int adv_raw = 0, lsb_raw = 0;
            stbtt_GetCodepointHMetrics(&face.info, static_cast<int>(cp), &adv_raw, &lsb_raw);
            int adv_px = static_cast<int>(std::lround(adv_raw * face.scale));
            cg.adv_x = (adv_px > 0) ? adv_px : default_doja_char_width(cp);

            int w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char* raw_bmp = stbtt_GetCodepointBitmap(&face.info, face.scale, face.scale, static_cast<int>(cp), &w, &h, &xoff, &yoff);
            if (raw_bmp && w > 0 && h > 0 && w < 64 && h < 64) {
                cg.width = w;
                cg.rows = h;
                cg.xoff = xoff;
                cg.yoff = yoff;
                cg.bmp.assign(raw_bmp, raw_bmp + (w * h));
            }
            if (raw_bmp) {
                stbtt_FreeBitmap(raw_bmp, nullptr);
            }
            cg.is_bitmap = false;
            cg.valid = true;
            auto [ins_it, _] = pixel_cache.emplace(cp, std::move(cg));
            return &ins_it->second;
        }

        pixel_cache.emplace(cp, std::move(cg));
        return nullptr;
    }

    const CachedGlyph* get_hd_glyph(uint32_t cp, int hd_scale) {
        if (hd_faces.empty()) return nullptr;
        hd_scale = std::clamp(hd_scale, 2, 4);
        uint64_t key = (static_cast<uint64_t>(hd_scale) << 32) | static_cast<uint64_t>(cp);
        auto it = hd_cache.find(key);
        if (it != hd_cache.end()) {
            return it->second.valid ? &it->second : nullptr;
        }

        CachedGlyph cg{};
        for (size_t i = 0; i < hd_faces.size(); ++i) {
            auto& face = hd_faces[i];
            int gidx = stbtt_FindGlyphIndex(&face.info, static_cast<int>(cp));
            if (gidx == 0 && !(cp <= 0x7Fu && i + 1 == hd_faces.size())) {
                continue;
            }

            float px_em = 11.0f * static_cast<float>(hd_scale);
            float scale_y = stbtt_ScaleForMappingEmToPixels(&face.info, px_em);
            if (scale_y <= 0.0f) {
                scale_y = stbtt_ScaleForPixelHeight(&face.info, 11.5f * static_cast<float>(hd_scale));
            }

            int hd_y_adjust = 0;
            auto yit = face.hd_y_adjust_by_scale.find(hd_scale);
            if (yit != face.hd_y_adjust_by_scale.end()) {
                hd_y_adjust = yit->second;
            } else {
                int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
                int ref_cp = 0x56FD; // '国'
                if (stbtt_FindGlyphIndex(&face.info, ref_cp) == 0) {
                    ref_cp = (stbtt_FindGlyphIndex(&face.info, 0x4E2D) != 0) ? 0x4E2D : 'H';
                }
                stbtt_GetCodepointBitmapBox(&face.info, ref_cp, scale_y, scale_y, &bx0, &by0, &bx1, &by1);
                hd_y_adjust = static_cast<int>(std::lround(-4.5f * static_cast<float>(hd_scale) - 0.5f * static_cast<float>(by0 + by1)));
                face.hd_y_adjust_by_scale[hd_scale] = hd_y_adjust;
            }

            int cell_w = default_doja_char_width(cp) * hd_scale;
            int adv_raw = 0, lsb_raw = 0;
            stbtt_GetCodepointHMetrics(&face.info, static_cast<int>(cp), &adv_raw, &lsb_raw);
            int raw_adv = static_cast<int>(std::lround(adv_raw * scale_y));
            float scale_x = scale_y;
            if (raw_adv > cell_w && raw_adv > 0) {
                scale_x = scale_y * (static_cast<float>(cell_w) / static_cast<float>(raw_adv));
            }

            int w = 0, h = 0, xoff = 0, yoff = 0;
            unsigned char* raw_bmp = stbtt_GetCodepointBitmap(&face.info, scale_x, scale_y, static_cast<int>(cp), &w, &h, &xoff, &yoff);
            int eff_adv = std::min(cell_w, static_cast<int>(std::lround(adv_raw * scale_x)));
            cg.xoff = xoff + (cell_w - eff_adv) / 2;
            cg.yoff = yoff + hd_y_adjust;
            cg.adv_x = cell_w;
            cg.is_bitmap = false;
            cg.valid = true;
            if (raw_bmp && w > 0 && h > 0 && w < 256 && h < 256) {
                cg.width = w;
                cg.rows = h;
                cg.bmp.assign(raw_bmp, raw_bmp + (w * h));
            }
            if (raw_bmp) {
                stbtt_FreeBitmap(raw_bmp, nullptr);
            }
            auto [ins_it, _] = hd_cache.emplace(key, std::move(cg));
            return &ins_it->second;
        }

        hd_cache.emplace(key, std::move(cg));
        return nullptr;
    }
};

static StbFontEngine g_ft;
static loc::LocalizationManager g_loc;

// Convert Shift-JIS (CP932) bytes to UTF-8 string
std::string cp932_to_utf8(const uint8_t* data, size_t len) {
    return cp932::decode_to_utf8(data, len);
}

// Convert UTF-8 string back to CP932 bytes
std::vector<uint8_t> utf8_to_cp932(const std::string& utf8) {
    return cp932::encode_from_utf8(utf8);
}

// Decode UTF-8 into Unicode codepoints (UTF-32) for accurate Java char indexing
std::vector<uint32_t> utf8_to_codepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) {
            cps.push_back(c);
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F);
            cps.push_back(cp);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            uint32_t cp = ((c & 0x0F) << 12) |
                          ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<uint8_t>(s[i + 2]) & 0x3F);
            cps.push_back(cp);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            uint32_t cp = ((c & 0x07) << 18) |
                          ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 12) |
                          ((static_cast<uint8_t>(s[i + 2]) & 0x3F) << 6) |
                          (static_cast<uint8_t>(s[i + 3]) & 0x3F);
            cps.push_back(cp);
            i += 4;
        } else {
            cps.push_back(c);
            i += 1;
        }
    }
    return cps;
}

std::string codepoints_to_utf8(const std::vector<uint32_t>& cps, size_t start, size_t end) {
    std::string out;
    for (size_t i = start; i < end && i < cps.size(); ++i) {
        uint32_t cp = cps[i];
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// ============================================================================
// Self-contained ZIP / JarInflater extractor using zlib
// ============================================================================
bool extract_zip_entry(const uint8_t* zip_data, size_t zip_size, const std::string& target_name, std::vector<uint8_t>& out) {
    std::string clean_target = target_name;
    while (!clean_target.empty() && clean_target[0] == '/') {
        clean_target.erase(0, 1);
    }

    size_t pos = 0;
    while (pos + 30 <= zip_size) {
        if ( zip_data[pos] != 'P' || zip_data[pos + 1] != 'K') {
            break;
        }
        uint8_t sig2 = zip_data[pos + 2];
        uint8_t sig3 = zip_data[pos + 3];
        if (sig2 != 3 || sig3 != 4) {
            break; // Reached central directory
        }
        uint16_t flags = *reinterpret_cast<const uint16_t*>(zip_data + pos + 6);
        uint16_t method = *reinterpret_cast<const uint16_t*>(zip_data + pos + 8);
        uint32_t comp_size = *reinterpret_cast<const uint32_t*>(zip_data + pos + 18);
        uint32_t uncomp_size = *reinterpret_cast<const uint32_t*>(zip_data + pos + 22);
        uint16_t name_len = *reinterpret_cast<const uint16_t*>(zip_data + pos + 26);
        uint16_t extra_len = *reinterpret_cast<const uint16_t*>(zip_data + pos + 28);

        std::string entry_name(reinterpret_cast<const char*>(zip_data + pos + 30), name_len);
        size_t data_start = pos + 30 + name_len + extra_len;

        // If bit 3 of general purpose flag is set and sizes are 0 in local header, scan central directory
        if ((flags & 0x0008) && comp_size == 0) {
            // Find central directory entry for this file
            for (size_t cpos = data_start; cpos + 46 <= zip_size; ++cpos) {
                if (zip_data[cpos] == 'P' && zip_data[cpos + 1] == 'K' && zip_data[cpos + 2] == 1 && zip_data[cpos + 3] == 2) {
                    uint16_t cname_len = *reinterpret_cast<const uint16_t*>(zip_data + cpos + 28);
                    uint16_t cextra_len = *reinterpret_cast<const uint16_t*>(zip_data + cpos + 30);
                    uint16_t ccomment_len = *reinterpret_cast<const uint16_t*>(zip_data + cpos + 32);
                    if (cpos + 46 + cname_len <= zip_size) {
                        std::string cname(reinterpret_cast<const char*>(zip_data + cpos + 46), cname_len);
                        if (cname == entry_name) {
                            comp_size = *reinterpret_cast<const uint32_t*>(zip_data + cpos + 20);
                            uncomp_size = *reinterpret_cast<const uint32_t*>(zip_data + cpos + 24);
                            break;
                        }
                    }
                    cpos += 45 + cname_len + cextra_len + ccomment_len;
                }
            }
        }

        if (entry_name == clean_target && data_start + comp_size <= zip_size) {
            if (method == 0) {
                out.assign(zip_data + data_start, zip_data + data_start + comp_size);
                return true;
            } else if (method == 8) {
                out.resize(uncomp_size > 0 ? uncomp_size : comp_size * 4 + 4096);
                z_stream strm{};
                strm.next_in = const_cast<Bytef*>(zip_data + data_start);
                strm.avail_in = comp_size;
                strm.next_out = out.data();
                strm.avail_out = out.size();
                if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return false;
                int ret = inflate(&strm, Z_FINISH);
                while (ret == Z_OK || ret == Z_BUF_ERROR) {
                    if (strm.avail_in == 0) break;
                    size_t old_sz = out.size();
                    out.resize(old_sz * 2);
                    strm.next_out = out.data() + strm.total_out;
                    strm.avail_out = out.size() - strm.total_out;
                    ret = inflate(&strm, Z_FINISH);
                }
                out.resize(strm.total_out);
                inflateEnd(&strm);
                return true;
            }
        }

        pos = data_start + comp_size;
        if ((flags & 0x0008) && pos + 16 <= zip_size) {
            if (zip_data[pos] == 'P' && zip_data[pos + 1] == 'K' && zip_data[pos + 2] == 7 && zip_data[pos + 3] == 8) {
                pos += 16;
            }
        }
    }
    return false;
}

// ============================================================================
// Self-contained GIF87a / GIF89a Indexed Decoder (for PalettedImage & Image)
// ============================================================================
bool decode_gif(const uint8_t* data, size_t size, ImageData& img_out, PaletteData& pal_out) {
    if (size < 13 || std::memcmp(data, "GIF8", 4) != 0) {
        return false;
    }
    int width = data[6] | (data[7] << 8);
    int height = data[8] | (data[9] << 8);
    uint8_t packed = data[10];
    bool has_gct = (packed & 0x80) != 0;
    int gct_size = 1 << ((packed & 0x07) + 1);

    size_t pos = 13;
    if (has_gct && pos + gct_size * 3 <= size) {
        pal_out.count = gct_size;
        for (int i = 0; i < gct_size; ++i) {
            uint8_t r = data[pos + i * 3 + 0];
            uint8_t g = data[pos + i * 3 + 1];
            uint8_t b = data[pos + i * 3 + 2];
            pal_out.entries[i] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                                 (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
        }
        pos += gct_size * 3;
    }

    int transparent_idx = -1;
    img_out.width = width;
    img_out.height = height;
    img_out.is_paletted = true;
    img_out.indices.assign(width * height, 0);

    while (pos < size) {
        uint8_t intro = data[pos++];
        if (intro == 0x3B) {
            break; // Trailer
        } else if (intro == 0x21) {
            // Extension
            if (pos >= size) break;
            uint8_t label = data[pos++];
            if (label == 0xF9 && pos + 6 <= size) {
                // Graphic Control Extension
                uint8_t block_sz = data[pos++];
                if (block_sz >= 4) {
                    uint8_t gce_flags = data[pos];
                    if (gce_flags & 0x01) {
                        transparent_idx = data[pos + 3];
                    }
                }
                pos += block_sz;
                while (pos < size && data[pos] != 0) {
                    pos += 1 + data[pos];
                }
                if (pos < size) pos++; // Skip block terminator
            } else {
                while (pos < size && data[pos] != 0) {
                    pos += 1 + data[pos];
                }
                if (pos < size) pos++;
            }
        } else if (intro == 0x2C) {
            // Image Descriptor
            if (pos + 9 > size) break;
            int left = data[pos] | (data[pos + 1] << 8);
            int top = data[pos + 2] | (data[pos + 3] << 8);
            int iw = data[pos + 4] | (data[pos + 5] << 8);
            int ih = data[pos + 6] | (data[pos + 7] << 8);
            uint8_t ipacked = data[pos + 8];
            bool has_lct = (ipacked & 0x80) != 0;
            bool interlaced = (ipacked & 0x40) != 0;
            int lct_size = 1 << ((ipacked & 0x07) + 1);
            pos += 9;

            if (has_lct && pos + lct_size * 3 <= size) {
                pal_out.count = lct_size;
                for (int i = 0; i < lct_size; ++i) {
                    uint8_t r = data[pos + i * 3 + 0];
                    uint8_t g = data[pos + i * 3 + 1];
                    uint8_t b = data[pos + i * 3 + 2];
                    pal_out.entries[i] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                                         (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
                }
                pos += lct_size * 3;
            }

            if (pos >= size) break;
            int lzw_min_code_size = data[pos++];
            if (lzw_min_code_size < 2 || lzw_min_code_size > 8) break;

            // Gather sub-blocks
            std::vector<uint8_t> lzw_bytes;
            while (pos < size) {
                uint8_t bsize = data[pos++];
                if (bsize == 0) break;
                if (pos + bsize > size) break;
                lzw_bytes.insert(lzw_bytes.end(), data + pos, data + pos + bsize);
                pos += bsize;
            }

            // LZW decode
            const int clear_code = 1 << lzw_min_code_size;
            const int end_code = clear_code + 1;
            int code_size = lzw_min_code_size + 1;
            int next_code = end_code + 1;

            std::vector<int> prefix(4096, -1);
            std::vector<uint8_t> suffix(4096, 0);
            std::vector<uint8_t> first_byte(4096, 0);
            for (int c = 0; c < clear_code; ++c) {
                suffix[c] = static_cast<uint8_t>(c);
                first_byte[c] = static_cast<uint8_t>(c);
            }

            size_t bit_pos = 0;
            size_t total_bits = lzw_bytes.size() * 8;
            auto read_code = [&]() -> int {
                if (bit_pos + code_size > total_bits) return -1;
                int code = 0;
                for (int b = 0; b < code_size; ++b) {
                    size_t bp = bit_pos + b;
                    if (lzw_bytes[bp >> 3] & (1 << (bp & 7))) {
                        code |= (1 << b);
                    }
                }
                bit_pos += code_size;
                return code;
            };

            std::vector<uint8_t> decoded;
            decoded.reserve(iw * ih);
            std::vector<uint8_t> stack_buf;
            int old_code = -1;

            while (decoded.size() < static_cast<size_t>(iw * ih)) {
                int code = read_code();
                if (code < 0 || code == end_code) break;
                if (code == clear_code) {
                    code_size = lzw_min_code_size + 1;
                    next_code = end_code + 1;
                    old_code = -1;
                    continue;
                }

                int in_code = code;
                stack_buf.clear();
                if (code >= next_code) {
                    if (old_code < 0) break;
                    stack_buf.push_back(first_byte[old_code]);
                    code = old_code;
                }
                while (code >= clear_code) {
                    stack_buf.push_back(suffix[code]);
                    code = prefix[code];
                }
                stack_buf.push_back(suffix[code]);
                uint8_t first_ch = stack_buf.back();

                for (auto it = stack_buf.rbegin(); it != stack_buf.rend(); ++it) {
                    if (decoded.size() < static_cast<size_t>(iw * ih)) {
                        decoded.push_back(*it);
                    }
                }

                if (old_code >= 0 && next_code < 4096) {
                    prefix[next_code] = old_code;
                    suffix[next_code] = first_ch;
                    first_byte[next_code] = first_byte[old_code];
                    next_code++;
                    if (next_code == (1 << code_size) && code_size < 12) {
                        code_size++;
                    }
                }
                old_code = in_code;
            }

            // Copy decoded pixels into img_out.indices
            auto map_row = [&](int r) -> int {
                if (!interlaced) return r;
                const int pass_start[4] = {0, 4, 2, 1};
                const int pass_step[4] = {8, 8, 4, 2};
                int row_idx = r;
                for (int p = 0; p < 4; ++p) {
                    int count_in_pass = (ih - pass_start[p] + pass_step[p] - 1) / pass_step[p];
                    if (ih <= pass_start[p]) count_in_pass = 0;
                    if (row_idx < count_in_pass) {
                        return pass_start[p] + row_idx * pass_step[p];
                    }
                    row_idx -= count_in_pass;
                }
                return r;
            };

            for (int y = 0; y < ih; ++y) {
                int dy = top + map_row(y);
                if (dy < 0 || dy >= height) continue;
                for (int x = 0; x < iw; ++x) {
                    int dx = left + x;
                    if (dx < 0 || dx >= width) continue;
                    size_t src_i = static_cast<size_t>(y * iw + x);
                    if (src_i < decoded.size()) {
                        img_out.indices[dy * width + dx] = decoded[src_i];
                    }
                }
            }
            break;
        } else {
            break;
        }
    }

    img_out.transparent_index = transparent_idx;
    return true;
}

// ============================================================================
// Yamaha MA-3 (YMU762) / MA-5 (YMU765) Stereo FM + Drum + SE + ADPCM Engine
// ============================================================================
struct MldSound {
    std::vector<uint8_t> raw_mld;
    std::vector<int16_t> pcm; // Interleaved Stereo L/R (2 * total_frames)
    size_t total_frames = 0;
    size_t loop_start_frame = 0;
    size_t loop_end_frame = 0;
    bool has_loop = false;
    bool synthesized = false;
    int32_t duration_ms = 250;
};

struct MldAudioEngine {
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int NUM_PORTS = 4;

    struct PortState {
        int sound_id = -1;
        size_t frame_pos = 0;
        int volume = 100;
        bool playing = false;
        bool loop = false;
        Ref active_presenter = 0;
        int64_t start_time_ms = 0;
        int32_t duration_ms = 0;
    };

    SDL_AudioDeviceID dev_id = 0;
    std::mutex mtx;
    std::vector<std::shared_ptr<MldSound>> sounds;
    std::unordered_map<uint64_t, int> mld_cache;
    PortState ports[NUM_PORTS];
    int master_volume = 100;
    bool muted = false;

    void init_sdl_audio() {
        if (dev_id != 0) return;
        SDL_AudioSpec want{}, have{};
        want.freq = SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 2; // Hardware Stereo for MFi5 Panpot & MA-5 Chorus/Room DSP
        want.samples = 1024;
        want.callback = &MldAudioEngine::sdl_audio_callback;
        want.userdata = this;
        dev_id = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (dev_id != 0) {
            SDL_PauseAudioDevice(dev_id, 0);
        } else {
            std::cerr << "SDL_OpenAudioDevice warning: " << SDL_GetError() << "\n";
        }
    }

    ~MldAudioEngine() {
        shutdown_sdl_audio();
    }

    void shutdown_sdl_audio() {
        if (dev_id != 0) {
            SDL_PauseAudioDevice(dev_id, 1);
            SDL_CloseAudioDevice(dev_id);
            dev_id = 0;
        }
        std::lock_guard<std::mutex> lk(mtx);
        for (int i = 0; i < NUM_PORTS; ++i) {
            ports[i] = PortState{};
        }
    }

    static void SDLCALL sdl_audio_callback(void* userdata, Uint8* stream, int len) {
        auto* self = static_cast<MldAudioEngine*>(userdata);
        int16_t* out = reinterpret_cast<int16_t*>(stream);
        int num_frames = len / static_cast<int>(2 * sizeof(int16_t));
        std::lock_guard<std::mutex> lk(self->mtx);

        int eff_vol = self->muted ? 0 : self->master_volume;
        for (int f = 0; f < num_frames; ++f) {
            int32_t mix_l = 0;
            int32_t mix_r = 0;
            for (int p = 0; p < NUM_PORTS; ++p) {
                PortState& ps = self->ports[p];
                if (!ps.playing || ps.sound_id < 0 || static_cast<size_t>(ps.sound_id) >= self->sounds.size()) {
                    continue;
                }
                const MldSound& snd = *self->sounds[ps.sound_id];
                if (!snd.synthesized || snd.total_frames == 0 || snd.pcm.size() < snd.total_frames * 2) {
                    ps.playing = false;
                    continue;
                }
                if (ps.loop && snd.has_loop && ps.frame_pos >= snd.loop_end_frame) {
                    ps.frame_pos = snd.loop_start_frame;
                } else if (ps.frame_pos >= snd.total_frames) {
                    if (ps.loop) {
                        ps.frame_pos = snd.has_loop ? snd.loop_start_frame : 0;
                    } else {
                        ps.playing = false;
                        continue;
                    }
                }
                size_t idx = ps.frame_pos * 2;
                ps.frame_pos++;
                mix_l += (static_cast<int32_t>(snd.pcm[idx]) * ps.volume) / 100;
                mix_r += (static_cast<int32_t>(snd.pcm[idx + 1]) * ps.volume) / 100;
            }
            mix_l = (mix_l * eff_vol) / 100;
            mix_r = (mix_r * eff_vol) / 100;
            out[f * 2]     = static_cast<int16_t>(std::clamp(mix_l, -32768, 32767));
            out[f * 2 + 1] = static_cast<int16_t>(std::clamp(mix_r, -32768, 32767));
        }
    }

    static uint16_t read_be16(const uint8_t* p) {
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    }

    static uint32_t read_be32(const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }

    static std::vector<int16_t> decode_adpcm_8k(const uint8_t* data, size_t len) {
        static const int step_table[49] = {
            16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
            50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
            157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
            494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552
        };
        static const int index_table[16] = {
            -1, -1, -1, -1, 2, 4, 6, 8,
            -1, -1, -1, -1, 2, 4, 6, 8
        };
        std::vector<int16_t> out;
        out.reserve(len * 2);
        int predictor = 0;
        int step_idx = 0;
        for (size_t i = 0; i < len; ++i) {
            uint8_t byte = data[i];
            for (int nib_idx = 0; nib_idx < 2; ++nib_idx) {
                uint8_t nib = (nib_idx == 0) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
                int step = step_table[step_idx];
                int diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                if (nib & 8) predictor -= diff;
                else predictor += diff;
                predictor = std::clamp(predictor, -2048, 2047);
                step_idx = std::clamp(step_idx + index_table[nib], 0, 48);
                out.push_back(static_cast<int16_t>(predictor * 12));
            }
        }
        return out;
    }

    static uint64_t hash_mld_bytes(const uint8_t* data, size_t size) {
        uint64_t h = 14695981039346656037ULL ^ static_cast<uint64_t>(size);
        for (size_t i = 0; i < size; ++i) {
            h ^= static_cast<uint64_t>(data[i]);
            h *= 1099511628211ULL;
        }
        return h;
    }

    int load_mld(const uint8_t* data, size_t size) {
        if (size >= 10 && std::memcmp(data, "melo", 4) == 0) {
            uint64_t h = hash_mld_bytes(data, size);
            std::lock_guard<std::mutex> lk(mtx);
            auto it = mld_cache.find(h);
            if (it != mld_cache.end()) {
                return it->second;
            }
            auto snd = std::make_shared<MldSound>();
            snd->raw_mld.assign(data, data + size);
            // Fast 0.01ms metadata scan for duration_ms & has_loop (PCM synthesized lazily on first play)
            synthesize_mld_impl(*snd, false);
            int id = static_cast<int>(sounds.size());
            sounds.push_back(std::move(snd));
            mld_cache[h] = id;
            return id;
        }
        auto snd = std::make_shared<MldSound>();
        snd->synthesized = true;
        std::lock_guard<std::mutex> lk(mtx);
        int id = static_cast<int>(sounds.size());
        sounds.push_back(std::move(snd));
        return id;
    }

    static void synthesize_mld_impl(MldSound& snd, bool render_pcm) {
        const uint8_t* data = snd.raw_mld.data();
        const size_t size = snd.raw_mld.size();
        if (size < 10 || std::memcmp(data, "melo", 4) != 0) {
            snd.synthesized = true;
            return;
        }

        const auto& rom = audio::ym_rom();

        uint32_t file_len = read_be32(data + 4);
        uint16_t hdr_len = read_be16(data + 8);
        size_t p = 10 + hdr_len;
        size_t end = std::min(size, static_cast<size_t>(8 + file_len));

        std::vector<int16_t> adpcm_8k;

        struct TempoChange {
            int32_t tick;
            double sec_per_tick;
        };
        struct TickFloatEv {
            int32_t tick;
            float val;
        };
        struct FrameFloatEv {
            size_t frame;
            float val;
        };
        struct FramePanEv {
            size_t frame;
            float pan_l;
            float pan_r;
        };
        struct NoteEv {
            int32_t start_tick;
            int32_t end_tick;
            int ch;
            int midi_note;
            int velocity;
            int prog;
            int ch_vol;
            int init_pan;
            bool is_drum;
        };
        struct AdpcmEv {
            int32_t tick;
            bool play;
            int vol;
        };

        std::vector<TempoChange> tempos = {{0, 60.0 / (120.0 * 48.0)}};
        std::vector<TickFloatEv> ch_pitch_ticks[16];
        std::vector<TickFloatEv> ch_mod_ticks[16];
        std::vector<TickFloatEv> ch_vol_ticks[16];
        std::vector<TickFloatEv> ch_pan_ticks[16];
        std::vector<NoteEv> notes;
        std::vector<AdpcmEv> adpcm_evs;

        int32_t loop_start_tick = 0;
        int32_t loop_end_tick = 0;
        bool has_loop = false;
        bool has_dsp_fx = false;
        int master_mld_vol = 110; // MFi5 0xB0 master volume (0..127)
        int32_t max_tick = 0;

        int bank[16] = {0};
        int prog[16] = {0};
        int ch_vol[16];
        int ch_pan[16];
        int bend_range[16];
        int bend_msb[16];
        int bend_lsb[16];
        bool has_e9[16] = {false};
        bool is_drum[16] = {false};
        for (int i = 0; i < 16; ++i) {
            ch_vol[i] = 52;
            ch_pan[i] = 32;
            bend_range[i] = 2;
            bend_msb[i] = 32;
            bend_lsb[i] = 0;
        }
        is_drum[9] = true; // MFi2/MFi5 trac 2 voice 1 (ch 9) is standard percussion

        int trac_idx = 0;
        while (p + 8 <= end) {
            const uint8_t* tag = data + p;
            uint32_t clen = read_be32(data + p + 4);
            p += 8;
            if (p + clen > end) break;

            if (std::memcmp(tag, "adat", 4) == 0 && clen > 13) {
                uint16_t ahdr = read_be16(data + p);
                size_t aoff = std::min<size_t>(clen, 2 + ahdr);
                adpcm_8k = decode_adpcm_8k(data + p + aoff, clen - aoff);
            } else if (std::memcmp(tag, "trac", 4) == 0 && trac_idx < 4) {
                size_t tp = p;
                size_t tend = p + clen;
                int32_t ticks = 0;
                while (tp + 4 <= tend) {
                    uint8_t dt = data[tp];
                    uint8_t st = data[tp + 1];
                    ticks += dt;
                    if (st == 0xFF) {
                        uint8_t cmd = data[tp + 2];
                        if (cmd >= 0xF0) {
                            if (tp + 5 > tend) break;
                            uint16_t slen = read_be16(data + tp + 3);
                            if (tp + 5 + slen <= tend && slen >= 3 &&
                                data[tp + 5] == 0x01 && data[tp + 6] == 0x40) {
                                has_dsp_fx = true; // Yamaha MA-5 DSP chorus/reverb SysEx
                            }
                            tp += 5 + slen;
                        } else {
                            uint8_t arg = data[tp + 3];
                            if (cmd == 0xDC) {
                                ticks += static_cast<int32_t>(arg) * 256;
                            } else if (cmd == 0xB0) {
                                master_mld_vol = std::clamp<int>(arg & 0x7F, 24, 127);
                            } else if (cmd >= 0xC0 && cmd <= 0xCF) {
                                int tb = cmd & 0x07;
                                int tpqn = 24 << std::max(0, tb - 2);
                                int bpm = std::max<int>(1, arg);
                                tempos.push_back({ticks, 60.0 / (static_cast<double>(bpm) * tpqn)});
                            } else if (cmd == 0xDD) {
                                if (arg == 0) {
                                    loop_start_tick = ticks;
                                } else if (arg == 1) {
                                    loop_end_tick = ticks;
                                    has_loop = true;
                                }
                            } else if (cmd == 0xBA) {
                                int d_ch = (arg >> 3) & 0x0F;
                                is_drum[d_ch] = (arg & 1) != 0;
                            } else if (cmd == 0xE1) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                bank[ch] = (arg & 1) << 6;
                            } else if (cmd == 0xE0) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                prog[ch] = bank[ch] | (arg & 0x3F);
                            } else if (cmd == 0xE2) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                ch_vol[ch] = arg & 0x3F;
                                ch_vol_ticks[ch].push_back({ticks, static_cast<float>(ch_vol[ch]) / 63.0f});
                            } else if (cmd == 0xE3) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                ch_pan[ch] = arg & 0x3F;
                                ch_pan_ticks[ch].push_back({ticks, static_cast<float>(ch_pan[ch])});
                            } else if (cmd == 0xE7) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                bend_range[ch] = std::max(1, static_cast<int>(arg & 0x3F));
                            } else if (cmd == 0xE4 || cmd == 0xE9) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                if (cmd == 0xE4) {
                                    bend_msb[ch] = arg & 0x3F;
                                } else {
                                    bend_lsb[ch] = arg & 0x3F;
                                    has_e9[ch] = true;
                                }
                                double norm = 0.0;
                                if (has_e9[ch]) {
                                    int raw12 = (bend_msb[ch] << 6) | bend_lsb[ch];
                                    norm = static_cast<double>(raw12 - 2048) / 2048.0;
                                } else {
                                    norm = static_cast<double>(bend_msb[ch] - 32) / 32.0;
                                }
                                float semi = static_cast<float>(norm * bend_range[ch]);
                                ch_pitch_ticks[ch].push_back({ticks, semi});
                            } else if (cmd == 0xEA || cmd == 0xE8) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                float mod_norm = static_cast<float>(arg & 0x3F) / 63.0f;
                                ch_mod_ticks[ch].push_back({ticks, mod_norm});
                            }
                            tp += 4;
                        }
                    } else if ((st & 0x3F) == 0x3F) {
                        uint8_t cmd = data[tp + 2];
                        if (cmd == 0x00 && tp + 5 <= tend) {
                            uint8_t arg1 = data[tp + 3];
                            uint8_t arg2 = data[tp + 4];
                            adpcm_evs.push_back({ticks, (arg1 & 0x40) == 0, arg2 & 0x3F});
                            tp += 5;
                        } else {
                            uint8_t arg = data[tp + 3];
                            adpcm_evs.push_back({ticks, (arg & 0x40) == 0, 60});
                            tp += 4;
                        }
                    } else {
                        uint8_t gate = data[tp + 2];
                        uint8_t vel_oct = data[tp + 3];
                        int v = (st >> 6) & 3;
                        int ch = trac_idx * 4 + v;
                        int key_off = st & 0x3F;
                        int vel = (vel_oct >> 2) & 0x3F;
                        int ob = vel_oct & 3;
                        int osh = (ob == 0) ? 0 : (ob == 1 ? 1 : (ob == 2 ? -2 : -1));
                        int midi_note = 35 + key_off + osh * 12;
                        if (gate > 0) {
                            notes.push_back({
                                ticks, ticks + gate, ch, midi_note, vel,
                                prog[ch], ch_vol[ch], ch_pan[ch], is_drum[ch]
                            });
                            max_tick = std::max(max_tick, ticks + gate);
                        }
                        tp += 4;
                    }
                }
                max_tick = std::max(max_tick, ticks);
                trac_idx++;
            }
            p += clen;
        }

        std::sort(tempos.begin(), tempos.end(), [](const TempoChange& a, const TempoChange& b) {
            return a.tick < b.tick;
        });

        auto tick_to_sec = [&](int32_t target_tick) -> double {
            double sec = 0.0;
            int32_t cur_tick = 0;
            double cur_spt = tempos[0].sec_per_tick;
            for (const auto& tc : tempos) {
                if (tc.tick >= target_tick) break;
                if (tc.tick > cur_tick) {
                    sec += (tc.tick - cur_tick) * cur_spt;
                    cur_tick = tc.tick;
                }
                cur_spt = tc.sec_per_tick;
            }
            if (target_tick > cur_tick) {
                sec += (target_tick - cur_tick) * cur_spt;
            }
            return sec;
        };

        double total_sec = tick_to_sec(max_tick);
        if (!has_loop && !notes.empty()) {
            // Allow natural SE / Jingle release tail so final notes don't cut off abruptly
            total_sec += 0.16;
        }
        if (!adpcm_8k.empty()) {
            double adpcm_sec = static_cast<double>(adpcm_8k.size()) / 8000.0;
            if (!adpcm_evs.empty()) {
                for (const auto& ae : adpcm_evs) {
                    if (ae.play) {
                        total_sec = std::max(total_sec, tick_to_sec(ae.tick) + adpcm_sec);
                    }
                }
            } else {
                total_sec = std::max(total_sec, adpcm_sec);
            }
        }
        total_sec = std::clamp(total_sec, 0.08, 300.0);

        size_t total_frames = static_cast<size_t>(total_sec * SAMPLE_RATE) + 1;
        snd.total_frames = total_frames;
        if (has_loop && loop_end_tick > loop_start_tick) {
            snd.has_loop = true;
            snd.loop_start_frame = std::min(total_frames - 1, static_cast<size_t>(tick_to_sec(loop_start_tick) * SAMPLE_RATE));
            snd.loop_end_frame = std::min(total_frames, static_cast<size_t>(tick_to_sec(loop_end_tick) * SAMPLE_RATE));
        }
        snd.duration_ms = std::max<int32_t>(80, static_cast<int32_t>(total_sec * 1000.0));

        if (!render_pcm) {
            return;
        }

        std::vector<float> mix_l(total_frames, 0.0f);
        std::vector<float> mix_r(total_frames, 0.0f);

        // Convert per-channel parameter events to sorted sample-frame timelines for O(1) cursor lookup
        auto calc_pan_gains = [](float pan_val, float& out_l, float& out_r) {
            // Map 0..63 across natural 78% stereo stage (theta in [0.11..0.89] * pi/2)
            float norm = std::clamp(pan_val / 63.0f, 0.0f, 1.0f);
            float theta = (0.11f + 0.78f * norm) * 1.5707963268f;
            out_l = std::cos(theta);
            out_r = std::sin(theta);
        };

        std::vector<FrameFloatEv> ch_pitches[16];
        std::vector<FrameFloatEv> ch_mods[16];
        std::vector<FrameFloatEv> ch_vols[16];
        std::vector<FramePanEv>   ch_pans[16];
        for (int ch = 0; ch < 16; ++ch) {
            for (const auto& ev : ch_pitch_ticks[ch]) {
                ch_pitches[ch].push_back({static_cast<size_t>(tick_to_sec(ev.tick) * SAMPLE_RATE), ev.val});
            }
            for (const auto& ev : ch_mod_ticks[ch]) {
                ch_mods[ch].push_back({static_cast<size_t>(tick_to_sec(ev.tick) * SAMPLE_RATE), ev.val});
            }
            for (const auto& ev : ch_vol_ticks[ch]) {
                ch_vols[ch].push_back({static_cast<size_t>(tick_to_sec(ev.tick) * SAMPLE_RATE), ev.val});
            }
            for (const auto& ev : ch_pan_ticks[ch]) {
                float pl = 0.7071f, pr = 0.7071f;
                calc_pan_gains(ev.val, pl, pr);
                ch_pans[ch].push_back({static_cast<size_t>(tick_to_sec(ev.tick) * SAMPLE_RATE), pl, pr});
            }
        }

        const float inv_sr = 1.0f / static_cast<float>(SAMPLE_RATE);
        const float master_track_gain = (static_cast<float>(master_mld_vol) / 110.0f) * 0.255f;

        // Per-channel cursors (notes within each channel are monotonically ordered in start_tick)
        size_t cur_pitch_idx[16] = {0};
        size_t cur_mod_idx[16]   = {0};
        size_t cur_vol_idx[16]   = {0};
        size_t cur_pan_idx[16]   = {0};

        uint32_t lfsr = 0xACE1u;

        for (const auto& n : notes) {
            size_t s0 = static_cast<size_t>(tick_to_sec(n.start_tick) * SAMPLE_RATE);
            size_t s1 = static_cast<size_t>(tick_to_sec(n.end_tick) * SAMPLE_RATE);
            if (s0 >= total_frames) continue;

            const int ch = n.ch & 15;
            const audio::YmFmPatch patch = (!n.is_drum && n.prog < 118)
                ? audio::get_ymu762_patch(n.prog)
                : audio::YmFmPatch{};

            // Determine release tail duration accurately per instrument / drum / SE
            double rel_sec = 0.08;
            if (n.is_drum) {
                if (n.midi_note == 49 || n.midi_note == 52 || n.midi_note == 57 ||
                    n.midi_note == 51 || n.midi_note == 59) {
                    rel_sec = 0.55; // Crash / Ride shimmer tail
                } else if (n.midi_note == 46 || n.midi_note == 53 || n.midi_note == 54 || n.midi_note == 55) {
                    rel_sec = 0.30; // Open hat / Splash / Tambourine / Bell
                } else {
                    rel_sec = 0.16; // Kick / Snare / Tom / Closed Hat
                }
            } else if (n.prog == 127) {
                rel_sec = 0.42; // Explosion rumble tail
            } else if (n.prog >= 118) {
                rel_sec = 0.14;
            } else {
                rel_sec = std::clamp(3.8 / static_cast<double>(std::max(8.0f, patch.rel_rate)), 0.035, 0.28);
            }

            size_t s_end = std::min(total_frames, s1 + static_cast<size_t>(rel_sec * SAMPLE_RATE));
            if (s_end <= s0) continue;

            const float vel_norm = static_cast<float>(n.velocity) / 63.0f;
            // Slightly gentle velocity curve so soft backing harmonies stay audible & musical
            const float vel_gain = (0.22f + 0.78f * vel_norm) * master_track_gain;
            if (vel_gain <= 0.0005f) continue;

            // Advance channel baseline cursors up to s0
            auto advance_to = [](const auto& vec, size_t& idx, size_t frame) {
                while (idx > 0 && vec[idx].frame > frame) --idx;
                while (idx + 1 < vec.size() && vec[idx + 1].frame <= frame) ++idx;
            };
            advance_to(ch_pitches[ch], cur_pitch_idx[ch], s0);
            advance_to(ch_mods[ch],    cur_mod_idx[ch],   s0);
            advance_to(ch_vols[ch],    cur_vol_idx[ch],   s0);
            advance_to(ch_pans[ch],    cur_pan_idx[ch],   s0);

            size_t p_idx   = cur_pitch_idx[ch];
            size_t m_idx   = cur_mod_idx[ch];
            size_t v_idx   = cur_vol_idx[ch];
            size_t pan_idx = cur_pan_idx[ch];

            float init_pl = 0.7071f, init_pr = 0.7071f;
            if (n.is_drum) {
                // Subtle natural kit stereo placement (hats slightly right, toms across stereo field)
                float drum_pan = 32.0f;
                if (n.midi_note == 42 || n.midi_note == 44 || n.midi_note == 46) drum_pan = 41.0f;
                else if (n.midi_note == 51 || n.midi_note == 53 || n.midi_note == 59) drum_pan = 44.0f;
                else if (n.midi_note == 49 || n.midi_note == 55) drum_pan = 22.0f;
                else if (n.midi_note == 50 || n.midi_note == 48) drum_pan = 24.0f;
                else if (n.midi_note == 45 || n.midi_note == 47) drum_pan = 33.0f;
                else if (n.midi_note == 41 || n.midi_note == 43) drum_pan = 42.0f;
                calc_pan_gains(drum_pan, init_pl, init_pr);
            } else {
                calc_pan_gains(static_cast<float>(n.init_pan), init_pl, init_pr);
            }

            const double base_freq = 440.0 * std::pow(2.0, (n.midi_note - 69) / 12.0);
            const float gate_dur = static_cast<float>(std::max<size_t>(1, s1 - s0)) * inv_sr;

            if (n.is_drum) {
                // ============================================================
                // Yamaha MA-3 / DoCoMo Flip-Phone Percussion Kit (Multiplicative Envelopes)
                // ============================================================
                double ph1 = 0.0, ph2 = 0.0, ph3 = 0.0;
                float prev_noise = 0.0f;
                float bp_state = 0.0f;
                const float drum_amp = vel_gain * (static_cast<float>(n.ch_vol) / 63.0f);
                const int dn = n.midi_note;

                float r1 = 18.0f, r2 = 45.0f, r3 = 90.0f;
                if (dn == 35 || dn == 36) { r1 = (dn == 36) ? 26.0f : 30.0f; r2 = 52.0f; r3 = 95.0f; }
                else if (dn == 37) { r1 = 62.0f; }
                else if (dn == 38 || dn == 40) { r1 = (dn == 40) ? 42.0f : 34.0f; r2 = (dn == 40) ? 23.0f : 19.0f; r3 = 55.0f; }
                else if (dn == 42 || dn == 44 || dn == 46) { r1 = (dn == 46) ? 10.5f : ((dn == 44) ? 42.0f : 56.0f); }
                else if (dn == 49 || dn == 52 || dn == 55 || dn == 57) { r1 = (dn == 55) ? 9.5f : ((dn == 52) ? 7.5f : 5.6f); }
                else if (dn == 51 || dn == 53 || dn == 59) { r1 = (dn == 53) ? 9.0f : 7.2f; r2 = 16.0f; }
                else if (dn == 62 || dn == 67) { r1 = (dn == 67) ? 22.0f : 28.0f; r2 = 45.0f; r3 = 60.0f; }
                else if (dn == 69 || dn == 70) { r1 = 42.0f; }
                else if (dn == 79) { r1 = 14.0f; }
                else { r1 = 17.5f; r2 = 28.0f; r3 = 50.0f; }

                const float m_e1 = std::exp(-r1 * inv_sr);
                const float m_e2 = std::exp(-r2 * inv_sr);
                const float m_e3 = std::exp(-r3 * inv_sr);
                float e1 = 1.0f, e2 = 1.0f, e3 = 1.0f;

                double tom_base = 130.0;
                if (dn <= 41) tom_base = 86.0;
                else if (dn <= 43) tom_base = 105.0;
                else if (dn <= 45) tom_base = 128.0;
                else if (dn <= 47) tom_base = 152.0;
                else if (dn <= 48) tom_base = 178.0;
                else tom_base = 208.0;

                for (size_t s = s0; s < s_end; ++s) {
                    float t = static_cast<float>(s - s0) * inv_sr;
                    lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
                    float noise = (static_cast<float>(lfsr & 0xFFFF) * (1.0f / 32768.0f)) - 1.0f;
                    float hp_noise = noise - prev_noise;
                    prev_noise = noise;
                    bp_state += 0.24f * (noise - bp_state);
                    float mid_noise = bp_state - 0.35f * prev_noise;

                    float out_s = 0.0f;

                    if (dn == 35 || dn == 36) {
                        double kf = 47.0 + 138.0 * e2;
                        ph1 += kf * inv_sr;
                        ph2 += (kf * 2.4) * inv_sr;
                        float click_mod = rom.sample_mod(audio::WS_SINE, ph2) * (0.32f * e3);
                        out_s = rom.sample_car(audio::WS_SINE, ph1 + click_mod) * e1 * 1.75f;
                    } else if (dn == 37) {
                        ph1 += 435.0 * inv_sr;
                        ph2 += 1210.0 * inv_sr;
                        float mod = rom.sample_mod(audio::WS_PULSE_SINE, ph2) * 0.35f * e1;
                        out_s = (rom.sample_car(audio::WS_SINE, ph1 + mod) * 0.85f + hp_noise * 0.25f) * e1 * 1.35f;
                    } else if (dn == 38 || dn == 40) {
                        bool tight = (dn == 40);
                        double sf = (tight ? 205.0 : 184.0) + 75.0 * e3;
                        ph1 += sf * inv_sr;
                        ph2 += (sf * 1.58) * inv_sr;
                        float body = rom.sample_car(audio::WS_SINE, ph1 + rom.sample_mod(audio::WS_SINE, ph2) * 0.25f * e1);
                        out_s = (body * e1 * 0.72f + (0.65f * mid_noise + 0.35f * hp_noise) * e2 * 0.92f) * 1.35f;
                    } else if (dn == 39) {
                        float env = 0.0f;
                        if (t < 0.011f) env = 1.0f - t * 65.0f;
                        else if (t < 0.022f) env = 1.0f - (t - 0.011f) * 65.0f;
                        else env = 0.88f * e1;
                        out_s = mid_noise * std::max(0.0f, env) * 1.45f;
                    } else if (dn == 42 || dn == 44 || dn == 46) {
                        ph1 += 508.0 * inv_sr;
                        ph2 += 819.0 * inv_sr;
                        ph3 += 1487.0 * inv_sr;
                        float m1 = rom.sample_mod(audio::WS_PULSE_SINE, ph2 * 1.351);
                        float m2 = rom.sample_mod(audio::WS_ABS_SINE, ph3);
                        float metal = rom.sample_car(audio::WS_SINE, ph1 + 0.38f * m1 + 0.28f * m2);
                        out_s = (0.42f * metal + 0.58f * hp_noise * 0.7f) * e1 * 0.92f;
                    } else if (dn == 49 || dn == 52 || dn == 55 || dn == 57) {
                        ph1 += 462.0 * inv_sr;
                        ph2 += 978.0 * inv_sr;
                        ph3 += 1690.0 * inv_sr;
                        float m = rom.sample_mod(audio::WS_ABS_SINE, ph2) * 0.52f +
                                  rom.sample_mod(audio::WS_PULSE_SINE, ph3) * 0.36f;
                        float metal = rom.sample_car(audio::WS_SINE, ph1 + m);
                        out_s = (0.48f * metal + 0.52f * (0.6f * hp_noise + 0.4f * mid_noise)) * e1 * 0.95f;
                    } else if (dn == 51 || dn == 53 || dn == 59) {
                        bool bell = (dn == 53);
                        ph1 += (bell ? 880.0 : 587.0) * inv_sr;
                        ph2 += (bell ? 2402.0 : 1602.0) * inv_sr;
                        float ping = rom.sample_car(audio::WS_SINE, ph1 + rom.sample_mod(audio::WS_SINE, ph2) * 0.32f * e1);
                        out_s = (ping * e1 * (bell ? 0.82f : 0.58f) + hp_noise * e2 * 0.32f) * 1.0f;
                    } else if (dn == 54) {
                        float env = (t < 0.015f) ? (1.0f - t * 45.0f) : (0.85f * e1);
                        ph1 += 2650.0 * inv_sr;
                        ph2 += 5480.0 * inv_sr;
                        float jingle = rom.sample_car(audio::WS_SINE, ph1 + rom.sample_mod(audio::WS_SINE, ph2) * 0.4f);
                        out_s = (0.55f * jingle + 0.45f * hp_noise * 0.7f) * env * 0.92f;
                    } else if (dn == 62 || dn == 67) {
                        bool agogo = (dn == 67);
                        double f0 = agogo ? 698.0 : (295.0 + 45.0 * e2);
                        ph1 += f0 * inv_sr;
                        ph2 += (f0 * (agogo ? 1.414 : 2.15)) * inv_sr;
                        float mod = rom.sample_mod(audio::WS_SINE, ph2) * (agogo ? 0.32f : 0.24f * e3);
                        out_s = rom.sample_car(audio::WS_SINE, ph1 + mod) * e1 * 1.25f;
                    } else if (dn == 69 || dn == 70) {
                        float atk = std::min(1.0f, t * 140.0f);
                        out_s = hp_noise * (atk * e1) * 0.85f;
                    } else if (dn == 79) {
                        float atk = std::min(1.0f, t * 80.0f);
                        double cf = 740.0 + 180.0 * rom.sample_car(audio::WS_SINE, t * 6.0);
                        ph1 += cf * inv_sr;
                        ph2 += (cf * 2.0) * inv_sr;
                        out_s = rom.sample_car(audio::WS_SINE, ph1 + rom.sample_mod(audio::WS_SINE, ph2) * 0.22f) * (atk * e1) * 1.05f;
                    } else {
                        double tf = tom_base * (1.0 + 0.48 * e2);
                        ph1 += tf * inv_sr;
                        ph2 += (tf * 1.62) * inv_sr;
                        float mod = rom.sample_mod(audio::WS_SINE, ph2) * (0.24f * e3);
                        out_s = rom.sample_car(audio::WS_SINE, ph1 + mod) * e1 * 1.45f;
                    }

                    e1 *= m_e1;
                    e2 *= m_e2;
                    e3 *= m_e3;

                    float v = out_s * drum_amp;
                    mix_l[s] += v * init_pl;
                    mix_r[s] += v * init_pr;
                }
                continue;
            }

            if (n.prog >= 118) {
                // ============================================================
                // Special Rockman EXE Battle SE Programs (118..127)
                // ============================================================
                double ph1 = 0.0, ph2 = 0.0;
                float prev_noise = 0.0f;
                float lp_noise = 0.0f;
                float cur_pl = init_pl, cur_pr = init_pr;
                float cur_vol = static_cast<float>(n.ch_vol) / 63.0f;
                float cur_semi = 0.0f;

                for (size_t s = s0; s < s_end; ++s) {
                    if (((s - s0) & 15) == 0) {
                        while (p_idx + 1 < ch_pitches[ch].size() && ch_pitches[ch][p_idx + 1].frame <= s) ++p_idx;
                        if (p_idx < ch_pitches[ch].size() && ch_pitches[ch][p_idx].frame <= s) cur_semi = ch_pitches[ch][p_idx].val;
                        while (v_idx + 1 < ch_vols[ch].size() && ch_vols[ch][v_idx + 1].frame <= s) ++v_idx;
                        if (v_idx < ch_vols[ch].size() && ch_vols[ch][v_idx].frame <= s) cur_vol = ch_vols[ch][v_idx].val;
                        while (pan_idx + 1 < ch_pans[ch].size() && ch_pans[ch][pan_idx + 1].frame <= s) ++pan_idx;
                        if (pan_idx < ch_pans[ch].size() && ch_pans[ch][pan_idx].frame <= s) {
                            cur_pl = ch_pans[ch][pan_idx].pan_l;
                            cur_pr = ch_pans[ch][pan_idx].pan_r;
                        }
                    }

                    float t = static_cast<float>(s - s0) * inv_sr;
                    lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
                    float noise = (static_cast<float>(lfsr & 0xFFFF) * (1.0f / 32768.0f)) - 1.0f;
                    float hp_noise = noise - prev_noise;
                    prev_noise = noise;

                    double f_bent = base_freq * std::pow(2.0, cur_semi / 12.0);
                    float out_s = 0.0f;

                    if (n.prog == 118) {
                        // 118: Synth Drum (Buster / Laser / Synth Tom downward FM sweep!)
                        float env = (s < s1) ? std::exp(-t * 14.0f) : std::exp(-gate_dur * 14.0f) * std::exp(-(t - gate_dur) * 38.0f);
                        double sf = f_bent * (0.42 + 1.65 * std::exp(-t * 22.0f));
                        ph1 += sf * inv_sr;
                        ph2 += (sf * 1.5) * inv_sr;
                        float mod = rom.sample_mod(audio::WS_SINE, ph2) * (0.36f * std::exp(-t * 25.0f));
                        out_s = rom.sample_car(audio::WS_SINE, ph1 + mod) * env * 1.35f;
                    } else if (n.prog == 119) {
                        // 119: Reverse Cymbal (Battle Start / Chip-In rising metallic shimmer!)
                        float env = 0.0f;
                        if (s < s1) {
                            float u = std::clamp(t / std::max(0.015f, gate_dur), 0.0f, 1.0f);
                            env = u * u;
                        } else {
                            env = std::exp(-(t - gate_dur) * 28.0f);
                        }
                        ph1 += (f_bent * 1.45) * inv_sr;
                        ph2 += (f_bent * 3.17) * inv_sr;
                        float metal = rom.sample_car(audio::WS_SINE, ph1 + rom.sample_mod(audio::WS_ABS_SINE, ph2) * 0.48f);
                        out_s = (0.46f * metal + 0.54f * hp_noise * 0.72f) * env * 1.15f;
                    } else if (n.prog == 127) {
                        // 127: Gunshot / Explosion (Heavy sub impact + roaring rumble tail!)
                        float env = std::exp(-t * 8.5f);
                        lp_noise += 0.14f * (noise - lp_noise);
                        double ef = 34.0 + 115.0 * std::exp(-t * 32.0f);
                        ph1 += ef * inv_sr;
                        ph2 += (ef * 2.1) * inv_sr;
                        float sub = rom.sample_car(audio::WS_SINE, ph1 + rom.sample_mod(audio::WS_ABS_SINE, ph2) * 0.45f);
                        out_s = (sub * std::exp(-t * 18.0f) * 0.85f + lp_noise * env * 1.25f) * 1.45f;
                    } else if (n.prog == 122 || n.prog == 126) {
                        // 122/126: Seashore / Applause / Wind SE
                        float atk = std::min(1.0f, t / std::max(0.01f, 0.25f * gate_dur));
                        float env = (s < s1) ? atk : std::exp(-(t - gate_dur) * 16.0f);
                        float cutoff = 0.10f + 0.18f * (0.5f + 0.5f * std::sin(t * 9.0f));
                        lp_noise += cutoff * (noise - lp_noise);
                        out_s = (lp_noise - 0.3f * hp_noise) * env * 1.05f;
                    } else {
                        // 120 (Fret Noise) / 123 (Bird Tweet / PET Chirp) & misc SE
                        float env = (s < s1) ? std::exp(-t * 10.0f) : std::exp(-(t - gate_dur) * 35.0f);
                        double cf = f_bent * (1.8 + 0.35 * std::sin(t * 110.0));
                        ph1 += cf * inv_sr;
                        ph2 += (cf * 2.5) * inv_sr;
                        out_s = rom.sample_car(audio::WS_SINE, ph1 + rom.sample_mod(audio::WS_SINE, ph2) * 0.32f) * env * 1.05f;
                    }

                    float v = out_s * vel_gain * cur_vol;
                    mix_l[s] += v * cur_pl;
                    mix_r[s] += v * cur_pr;
                }
                continue;
            }

            // ================================================================
            // Authentic 4-Operator (Dual 2-Op) Yamaha MA-3 / MA-5 Melodic FM
            // ================================================================
            // Key-Scale Level (KSL) softens upper-register FM brightness so high leads sing cleanly
            float high_oct = std::max(0.0f, static_cast<float>(n.midi_note - 66) / 12.0f);
            float ksl_scale = 1.0f / (1.0f + patch.ksl * high_oct);
            // Dynamic velocity sensitivity on FM modulation index (louder notes = brighter harmonics!)
            float vel_idx_scale = (0.58f + 0.42f * vel_norm) * ksl_scale;

            const float idx1_atk = patch.idx1_atk * vel_idx_scale;
            const float idx1_sus = patch.idx1_sus * vel_idx_scale;
            const float idx2_atk = patch.idx2_atk * vel_idx_scale;
            const float idx2_sus = patch.idx2_sus * vel_idx_scale;
            const bool use_pair2 = (patch.mix2 > 0.001f);
            const float detune2_mul = use_pair2 ? std::pow(2.0f, patch.detune2_cents / 1200.0f) : 1.0f;
            const float pair_norm = 1.0f / (1.0f + patch.mix2 * 0.65f);

            // Multiplicative per-sample envelope coefficients (zero std::exp in inner loop!)
            const float mod1_mul = std::exp(-patch.mod1_dec * inv_sr);
            const float mod2_mul = std::exp(-patch.mod2_dec * inv_sr);
            const float dec1_mul = std::exp(-patch.dec1_rate * inv_sr);
            const float dec2_mul = (patch.dec2_rate > 0.0f) ? std::exp(-patch.dec2_rate * inv_sr) : 1.0f;
            const float rel_mul  = std::exp(-patch.rel_rate * inv_sr);
            const float atk_inc  = 1.0f / std::max(1.0f, patch.atk_sec * SAMPLE_RATE);

            float mod1_env = patch.brass_swell ? (idx1_atk * 0.25f) : idx1_atk;
            float mod2_env = idx2_atk;
            const float swell_inc = patch.brass_swell ? ((idx1_atk * 0.75f) / (0.032f * SAMPLE_RATE)) : 0.0f;
            const size_t swell_end_s = s0 + static_cast<size_t>(0.032f * SAMPLE_RATE);

            float car_env = 0.0f;
            float sus_cur = patch.sus_level;
            bool in_attack = true;

            double ph_c1 = 0.0, ph_m1 = 0.0;
            double ph_c2 = 0.0, ph_m2 = 0.0;
            float fb1_p0 = 0.0f, fb1_p1 = 0.0f;
            float fb2_p0 = 0.0f, fb2_p1 = 0.0f;
            double lfo_ph = 0.0;

            float cur_semi = 0.0f;
            float cur_mod  = 0.0f;
            float cur_vol  = static_cast<float>(n.ch_vol) / 63.0f;
            float cur_pl   = init_pl;
            float cur_pr   = init_pr;
            double step_base = base_freq * inv_sr;

            for (size_t s = s0; s < s_end; ++s) {
                // Control-rate update every 16 samples (2756 Hz) for MFi5 pitch bend, vibrato, vol & pan
                if (((s - s0) & 15) == 0) {
                    while (p_idx + 1 < ch_pitches[ch].size() && ch_pitches[ch][p_idx + 1].frame <= s) ++p_idx;
                    if (p_idx < ch_pitches[ch].size() && ch_pitches[ch][p_idx].frame <= s) cur_semi = ch_pitches[ch][p_idx].val;

                    while (m_idx + 1 < ch_mods[ch].size() && ch_mods[ch][m_idx + 1].frame <= s) ++m_idx;
                    if (m_idx < ch_mods[ch].size() && ch_mods[ch][m_idx].frame <= s) cur_mod = ch_mods[ch][m_idx].val;

                    while (v_idx + 1 < ch_vols[ch].size() && ch_vols[ch][v_idx + 1].frame <= s) ++v_idx;
                    if (v_idx < ch_vols[ch].size() && ch_vols[ch][v_idx].frame <= s) cur_vol = ch_vols[ch][v_idx].val;

                    while (pan_idx + 1 < ch_pans[ch].size() && ch_pans[ch][pan_idx + 1].frame <= s) ++pan_idx;
                    if (pan_idx < ch_pans[ch].size() && ch_pans[ch][pan_idx].frame <= s) {
                        cur_pl = ch_pans[ch][pan_idx].pan_l;
                        cur_pr = ch_pans[ch][pan_idx].pan_r;
                    }

                    // Advance 5.8 Hz Yamaha vibrato LFO over 16 samples
                    lfo_ph += 5.8 * 16.0 * inv_sr;
                    float vib_depth_semi = cur_mod * 0.34f; // Up to ~34 cents at MFi5 mod=63
                    if (vib_depth_semi < 0.02f && (s - s0) > static_cast<size_t>(0.24 * SAMPLE_RATE)) {
                        vib_depth_semi = 0.035f; // Subtle 3.5-cent warmth on long sustained notes
                    }
                    float total_semi = cur_semi + vib_depth_semi * rom.sample_car(audio::WS_SINE, lfo_ph);
                    step_base = (base_freq * std::pow(2.0, total_semi * (1.0 / 12.0))) * inv_sr;
                }

                // Carrier ADSR + D2R envelope
                if (s < s1) {
                    if (in_attack) {
                        car_env += atk_inc;
                        if (car_env >= 1.0f) {
                            car_env = 1.0f;
                            in_attack = false;
                        }
                    } else {
                        sus_cur *= dec2_mul;
                        car_env = sus_cur + (car_env - sus_cur) * dec1_mul;
                    }
                } else {
                    car_env *= rel_mul;
                }

                // Modulator envelopes (with optional acoustic brass lip-bloom ramp)
                if (patch.brass_swell && s < swell_end_s) {
                    mod1_env = std::min(idx1_atk, mod1_env + swell_inc);
                } else {
                    mod1_env = idx1_sus + (mod1_env - idx1_sus) * mod1_mul;
                }

                // Operator Pair 1 (2-sample averaged feedback eliminates Nyquist harshness!)
                ph_m1 += step_base * patch.ratio_m1;
                ph_c1 += step_base * patch.ratio_c1;
                float fb1_sig = 0.5f * (fb1_p0 + fb1_p1) * patch.fb1;
                float m1_raw = rom.sample_mod(patch.ws_m1, ph_m1 + fb1_sig);
                fb1_p1 = fb1_p0;
                fb1_p0 = m1_raw;
                float out_osc = rom.sample_car(patch.ws_c1, ph_c1 + m1_raw * mod1_env);

                // Operator Pair 2 (4-Op layer when active)
                if (use_pair2) {
                    mod2_env = idx2_sus + (mod2_env - idx2_sus) * mod2_mul;
                    double step2 = step_base * detune2_mul;
                    ph_m2 += step2 * patch.ratio_m2;
                    ph_c2 += step2 * patch.ratio_c2;
                    float fb2_sig = 0.5f * (fb2_p0 + fb2_p1) * patch.fb2;
                    float m2_raw = rom.sample_mod(patch.ws_m2, ph_m2 + fb2_sig);
                    fb2_p1 = fb2_p0;
                    fb2_p0 = m2_raw;
                    float c2_out = rom.sample_car(patch.ws_c2, ph_c2 + m2_raw * mod2_env);
                    out_osc = (out_osc + c2_out * patch.mix2) * pair_norm;
                }

                // Optional woodwind/flute attack breath chiff (first 22ms)
                if (patch.breath_chiff > 0.0f && (s - s0) < static_cast<size_t>(0.022 * SAMPLE_RATE)) {
                    lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
                    float nval = (static_cast<float>(lfsr & 0xFFFF) * (1.0f / 32768.0f)) - 1.0f;
                    float c_env = 1.0f - static_cast<float>(s - s0) / (0.022f * SAMPLE_RATE);
                    out_osc += nval * patch.breath_chiff * c_env;
                }

                float v = out_osc * car_env * patch.gain * vel_gain * cur_vol;
                mix_l[s] += v * cur_pl;
                mix_r[s] += v * cur_pr;
            }
        }

        // Mix 8kHz ADPCM stream events (upsampled to 44.1kHz stereo center)
        if (!adpcm_8k.empty()) {
            auto mix_adpcm_at = [&](size_t start_frame, size_t stop_frame, float vol_scale) {
                size_t max_f = std::min(total_frames, stop_frame);
                const float center_gain = vol_scale * 0.7071f;
                for (size_t f = start_frame; f < max_f; ++f) {
                    double pos_8k = static_cast<double>(f - start_frame) * 8000.0 / SAMPLE_RATE;
                    size_t idx0 = static_cast<size_t>(pos_8k);
                    if (idx0 >= adpcm_8k.size()) break;
                    size_t idx1 = std::min(adpcm_8k.size() - 1, idx0 + 1);
                    double frac = pos_8k - idx0;
                    double val = (1.0 - frac) * adpcm_8k[idx0] + frac * adpcm_8k[idx1];
                    float sv = static_cast<float>(val / 32768.0) * center_gain;
                    mix_l[f] += sv;
                    mix_r[f] += sv;
                }
            };

            if (adpcm_evs.empty()) {
                mix_adpcm_at(0, total_frames, 0.85f);
            } else {
                for (size_t i = 0; i < adpcm_evs.size(); ++i) {
                    if (!adpcm_evs[i].play) continue;
                    size_t s0 = static_cast<size_t>(tick_to_sec(adpcm_evs[i].tick) * SAMPLE_RATE);
                    size_t s1 = total_frames;
                    for (size_t j = i + 1; j < adpcm_evs.size(); ++j) {
                        s1 = static_cast<size_t>(tick_to_sec(adpcm_evs[j].tick) * SAMPLE_RATE);
                        break;
                    }
                    float vscale = (std::max(20, adpcm_evs[i].vol) / 63.0f) * 0.9f;
                    mix_adpcm_at(s0, s1, vscale);
                }
            }
        }

        // Yamaha MA-5 Stereo Early-Reflection / Micro-Chorus DSP for BGM tracks
        if ((snd.has_loop || has_dsp_fx) && total_frames > 1200) {
            constexpr size_t kDelayL = 582; // ~13.2 ms
            constexpr size_t kDelayR = 869; // ~19.7 ms
            constexpr float kWet = 0.115f;
            float lp_l = 0.0f, lp_r = 0.0f;

            // Pre-warm filter state from end of loop so loop-back reflection is seamless
            if (snd.has_loop && snd.loop_end_frame > snd.loop_start_frame + kDelayR) {
                for (size_t f = snd.loop_end_frame - kDelayR; f < snd.loop_end_frame; ++f) {
                    lp_l += 0.42f * (mix_l[f] - lp_l);
                    lp_r += 0.42f * (mix_r[f] - lp_r);
                }
            }

            std::vector<float> wet_l(total_frames, 0.0f);
            std::vector<float> wet_r(total_frames, 0.0f);
            for (size_t f = 0; f < total_frames; ++f) {
                size_t src_l = (f >= kDelayL) ? (f - kDelayL)
                    : (snd.has_loop && snd.loop_end_frame > kDelayL ? (snd.loop_end_frame - (kDelayL - f)) : 0);
                size_t src_r = (f >= kDelayR) ? (f - kDelayR)
                    : (snd.has_loop && snd.loop_end_frame > kDelayR ? (snd.loop_end_frame - (kDelayR - f)) : 0);
                lp_l += 0.42f * (mix_l[src_l] - lp_l);
                lp_r += 0.42f * (mix_r[src_r] - lp_r);
                // Cross-coupled stereo reflection adds flip-phone MA-5 spaciousness
                wet_l[f] = lp_r * kWet;
                wet_r[f] = lp_l * kWet;
            }
            for (size_t f = 0; f < total_frames; ++f) {
                mix_l[f] += wet_l[f];
                mix_r[f] += wet_r[f];
            }
        }

        // Click-free 64-frame micro-crossfade right before loop_end_frame
        if (snd.has_loop && snd.loop_end_frame > snd.loop_start_frame + 128 && snd.loop_start_frame >= 64) {
            constexpr size_t kXf = 64;
            for (size_t k = 0; k < kXf; ++k) {
                float a = static_cast<float>(k) / static_cast<float>(kXf);
                size_t fe = snd.loop_end_frame - kXf + k;
                size_t fs = snd.loop_start_frame - kXf + k;
                mix_l[fe] = (1.0f - a) * mix_l[fe] + a * mix_l[fs];
                mix_r[fe] = (1.0f - a) * mix_r[fe] + a * mix_r[fs];
            }
        }

        // Convert stereo float mix to interleaved int16 PCM with warm soft-knee saturation
        snd.pcm.resize(total_frames * 2);
        auto soft_sat = [](float x) -> int16_t {
            // Smooth Padé tanh-like curve: linear at normal levels, zero hard-clipping on peaks
            float y = x / (1.0f + 0.28f * std::abs(x));
            y = std::clamp(y * 1.24f, -1.0f, 1.0f);
            return static_cast<int16_t>(y * 30500.0f);
        };
        for (size_t f = 0; f < total_frames; ++f) {
            snd.pcm[f * 2]     = soft_sat(mix_l[f]);
            snd.pcm[f * 2 + 1] = soft_sat(mix_r[f]);
        }
        snd.synthesized = true;
    }

    void play_port(int port, int sound_id, int volume, Ref presenter_ref, int64_t now_ms) {
        if (port < 0 || port >= NUM_PORTS) return;
        std::shared_ptr<MldSound> snd_ptr;
        bool need_synth = false;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (sound_id < 0 || static_cast<size_t>(sound_id) >= sounds.size()) return;
            snd_ptr = sounds[sound_id];
            need_synth = (dev_id != 0 && !snd_ptr->synthesized);
        }
        if (need_synth && snd_ptr) {
            // Synthesize outside audio callback lock so active ports never stutter
            MldSound tmp;
            tmp.raw_mld = snd_ptr->raw_mld;
            synthesize_mld_impl(tmp, true);
            std::lock_guard<std::mutex> lk(mtx);
            if (!snd_ptr->synthesized) {
                snd_ptr->pcm = std::move(tmp.pcm);
                snd_ptr->total_frames = tmp.total_frames;
                snd_ptr->loop_start_frame = tmp.loop_start_frame;
                snd_ptr->loop_end_frame = tmp.loop_end_frame;
                snd_ptr->has_loop = tmp.has_loop;
                snd_ptr->duration_ms = tmp.duration_ms;
                snd_ptr->synthesized = true;
            }
        }
        std::lock_guard<std::mutex> lk(mtx);
        const MldSound& snd = *snd_ptr;
        PortState& ps = ports[port];
        ps.sound_id = sound_id;
        ps.frame_pos = 0;
        ps.volume = std::clamp(volume, 0, 100);
        ps.playing = true;
        ps.loop = (port == 0 && snd.has_loop);
        ps.active_presenter = presenter_ref;
        ps.start_time_ms = now_ms;
        ps.duration_ms = snd.duration_ms;
    }

    void stop_port(int port, Ref presenter_ref) {
        if (port < 0 || port >= NUM_PORTS) return;
        std::lock_guard<std::mutex> lk(mtx);
        PortState& ps = ports[port];
        if (ps.active_presenter == presenter_ref) {
            ps.playing = false;
            ps.frame_pos = 0;
        }
    }

    void set_port_volume(int port, Ref presenter_ref, int volume) {
        if (port < 0 || port >= NUM_PORTS) return;
        std::lock_guard<std::mutex> lk(mtx);
        PortState& ps = ports[port];
        if (ps.active_presenter == presenter_ref) {
            ps.volume = std::clamp(volume, 0, 100);
        }
    }

    int32_t get_current_time(int port, Ref presenter_ref, int64_t now_ms) {
        if (port < 0 || port >= NUM_PORTS) return 0;
        std::lock_guard<std::mutex> lk(mtx);
        PortState& ps = ports[port];
        if (!ps.playing || ps.active_presenter != presenter_ref) {
            return 0;
        }
        int64_t elapsed = std::max<int64_t>(1, now_ms - ps.start_time_ms);
        if (!ps.loop && elapsed >= ps.duration_ms) {
            ps.playing = false;
            return 0;
        }
        return static_cast<int32_t>(elapsed);
    }
};

static MldAudioEngine g_audio;

// ============================================================================
// Flip-Phone Softkey Bar, Customizable Keybindings & Pause/Settings Overlay
// ============================================================================
enum BindAction : int {
    ACT_UP = 0,
    ACT_DOWN,
    ACT_LEFT,
    ACT_RIGHT,
    ACT_SELECT,
    ACT_CANCEL,
    ACT_SOFT1,
    ACT_SOFT2,
    ACT_POUND,
    ACT_KEY1,
    ACT_KEY4,
    ACT_TURBO,
    ACT_COUNT
};

static constexpr int PAD_BTN_LT = 100;
static constexpr int PAD_BTN_RT = 101;

struct ActionBinding {
    const char* cfg_id;
    const char* label_zh;
    int doja_key; // -1 for emulator-only action (e.g. ACT_TURBO)
    SDL_Keycode keys[2];
    int pad_btn;  // SDL_GameControllerButton (-1 == unbound)

    // Live physical state tracking
    bool key_down[2] = {false, false};
    bool pad_down = false;
    bool axis_down = false;
    bool touch_down = false;
    bool effective_down = false;
};

static net::CapcomServiceState g_capcom_local;
static pvp::PvpClientSession g_pvp;

struct HttpConnState {
    std::string url;
    std::string method = "GET";
    std::unordered_map<std::string, std::string> req_headers;
    std::shared_ptr<StreamData> req_stream;
    net::HttpResponse resp;
    bool connected = false;
};

static std::unordered_map<Ref, HttpConnState> g_http_conns;

static uint32_t entropy_mix32() {
    static std::atomic<uint32_t> s_seq{0};
    uint64_t x = 0x9E3779B97F4A7C15ULL * static_cast<uint64_t>(s_seq.fetch_add(1, std::memory_order_relaxed) + 1u);
    try {
        std::random_device rd;
        x ^= static_cast<uint64_t>(rd()) << 32;
        x ^= static_cast<uint64_t>(rd());
    } catch (...) {
    }
    x ^= static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    x ^= static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count()) << 13;
#ifdef _WIN32
    x ^= static_cast<uint64_t>(GetCurrentProcessId()) << 21;
#else
    x ^= static_cast<uint64_t>(getpid()) << 21;
#endif
    uintptr_t stack_addr = reinterpret_cast<uintptr_t>(&x);
    x ^= static_cast<uint64_t>(stack_addr);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x = x ^ (x >> 31);
    return static_cast<uint32_t>(x & 0xFFFFFFFFu);
}

static std::string generate_random_uid() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "NV%06X", entropy_mix32() & 0xFFFFFFu);
    return std::string(buf);
}

static std::string generate_pvp_session_nonce() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X", entropy_mix32() & 0xFFFFFFu);
    return std::string(buf);
}

struct UiInputManager {
    std::string config_path = "controls.cfg";
    std::string asset_dir;
    std::string data_dir;
    bool show_softkey_bar = true;
    bool high_fps_mode = true; // true = 60 FPS smooth interpolation, false = 15 FPS original flip-phone rate
    bool hd_font_mode = true;  // true = native window-resolution HD smooth font (default), false = 12px pixel font
    bool is_fullscreen = false;
    std::string soft_label_left;
    std::string soft_label_right;

    // Network & PvP settings (persisted in controls.cfg, overridable via CLI)
    bool online_mode = false;
    std::string server_url = "127.0.0.1:8765";
    std::string player_uid = generate_random_uid();
    std::string pvp_session_nonce = generate_pvp_session_nonce();
    std::string player_name = "ROCKMAN";

    std::string effective_pvp_uid() const {
        return cli_player_uid.empty() ? (player_uid + "-" + pvp_session_nonce) : cli_player_uid;
    }
    bool pvp_bo3_mode = false;
    bool is_lon_game = false;
    int pvp_menu_choice = 0; // 0 = Create Room, 1 = Join Room

    bool has_cli_net_override = false;
    bool cli_online_mode = false;
    bool has_cli_fps_override = false;
    bool cli_high_fps_mode = true;
    bool has_cli_font_mode = false;
    bool cli_hd_font_mode = true;
    bool has_cli_pvp_bo3 = false;
    bool cli_pvp_bo3 = false;
    loc::LangMode lang_mode = loc::LangMode::Auto;
    bool has_cli_lang_override = false;
    loc::LangMode cli_lang_mode = loc::LangMode::Auto;
    std::string cli_translation_path;
    std::string exe_base_dir;
    std::string cli_server_url;
    std::string cli_player_uid;
    std::string cli_player_name;
    std::string custom_sav_path;
    int cli_pvp_mode = 0;         // 0 = none, 1 = create, 2 = join
    uint32_t cli_pvp_room_id = 0;
    int cli_pvp_join_retries = 0;

    AssistConfig assist_cfg;
    AssistConfig cli_assist_cfg;
    bool has_cli_assist_buster = false;
    bool has_cli_assist_damage = false;
    bool has_cli_assist_encounter = false;
    bool has_cli_assist_wild_boss = false;
    bool has_cli_assist_show_ex_boss = false;
    bool has_cli_assist_ex_boss_tier = false;
    bool has_cli_assist_zenny = false;
    bool has_cli_assist_run_speed = false;
    bool has_cli_assist_auto_full_hp = false;
    bool has_cli_assist_always_escape = false;
    bool has_cli_assist_custom_gauge = false;
    bool has_cli_assist_drop_rank = false;
    bool assist_hold_down = false;
    AssistHudStatus assist_hud;

    std::string toast_text;
    int toast_frames = 0;

    int64_t paused_duration_ms = 0;
    int64_t turbo_bonus_ms = 0;

    ActionBinding binds[ACT_COUNT];
    TouchOverlayController touch;
    std::vector<SDL_GameController*> controllers;
    uint32_t display_buf[240 * 260] = {0};
    int hd_scale = 3;
    std::vector<uint32_t> display_buf_hd;

    UiInputManager() {
        reset_defaults();
    }

    void reset_defaults() {
        binds[ACT_UP]     = {"up",     "上移",     0x11, {SDLK_UP,    SDLK_w},        SDL_CONTROLLER_BUTTON_DPAD_UP};
        binds[ACT_DOWN]   = {"down",   "下移",     0x13, {SDLK_DOWN,  SDLK_s},        SDL_CONTROLLER_BUTTON_DPAD_DOWN};
        binds[ACT_LEFT]   = {"left",   "左移",     0x10, {SDLK_LEFT,  SDLK_a},        SDL_CONTROLLER_BUTTON_DPAD_LEFT};
        binds[ACT_RIGHT]  = {"right",  "右移",     0x12, {SDLK_RIGHT, SDLK_d},        SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
        binds[ACT_SELECT] = {"select", "决定/A",   0x14, {SDLK_z,     SDLK_j},        SDL_CONTROLLER_BUTTON_A};
        binds[ACT_CANCEL] = {"cancel", "取消/B",   0x0A, {SDLK_x,     SDLK_k},        SDL_CONTROLLER_BUTTON_B};
        binds[ACT_SOFT1]  = {"soft1",  "左软键/L", 0x15, {SDLK_q,     SDLK_u},        SDL_CONTROLLER_BUTTON_LEFTSHOULDER};
        binds[ACT_SOFT2]  = {"soft2",  "右软键/R", 0x16, {SDLK_e,     SDLK_i},        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
        binds[ACT_POUND]  = {"pound",  "井号/#",   0x0B, {SDLK_c,     SDLK_l},        SDL_CONTROLLER_BUTTON_X};
        binds[ACT_KEY1]   = {"key1",   "上翻页/1", 0x01, {SDLK_1,     SDLK_PAGEUP},   PAD_BTN_LT};
        binds[ACT_KEY4]   = {"key4",   "下翻页/4", 0x04, {SDLK_4,     SDLK_PAGEDOWN}, PAD_BTN_RT};
        binds[ACT_TURBO]  = {"turbo",  "按住倍速", -1,   {SDLK_TAB,   SDLK_BACKQUOTE}, SDL_CONTROLLER_BUTTON_Y};
    }

    void clear_held_states() {
        assist_hold_down = false;
        for (int i = 0; i < ACT_COUNT; ++i) {
            binds[i].key_down[0] = false;
            binds[i].key_down[1] = false;
            binds[i].pad_down = false;
            binds[i].axis_down = false;
            binds[i].touch_down = false;
            binds[i].effective_down = false;
        }
        touch.clear_all_fingers();
    }

    bool is_turbo_held() const {
        return binds[ACT_TURBO].effective_down;
    }

    bool is_keycode_bound(SDL_Keycode k) const {
        if (k == SDLK_UNKNOWN) return false;
        for (int i = 0; i < ACT_COUNT; ++i) {
            if (binds[i].keys[0] == k || binds[i].keys[1] == k) return true;
        }
        return false;
    }

    static std::string trim(const std::string& s) {
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
        size_t b = s.size();
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
        return s.substr(a, b - a);
    }

    static SDL_Keycode parse_key_str(const std::string& v) {
        if (v.empty() || v == "None" || v == "none") return SDLK_UNKNOWN;
        return SDL_GetKeyFromName(v.c_str());
    }

    static std::string format_key_cfg(SDL_Keycode k) {
        if (k == SDLK_UNKNOWN) return "None";
        const char* nm = SDL_GetKeyName(k);
        return (nm && nm[0]) ? std::string(nm) : "None";
    }

    static int parse_pad_str(const std::string& v) {
        if (v.empty() || v == "None" || v == "none") return SDL_CONTROLLER_BUTTON_INVALID;
        if (v == "lefttrigger"  || v == "LT" || v == "lt" || v == "L2" || v == "l2") return PAD_BTN_LT;
        if (v == "righttrigger" || v == "RT" || v == "rt" || v == "R2" || v == "r2") return PAD_BTN_RT;
        return static_cast<int>(SDL_GameControllerGetButtonFromString(v.c_str()));
    }

    static std::string format_pad_cfg(int btn) {
        if (btn < 0) return "None";
        if (btn == PAD_BTN_LT) return "lefttrigger";
        if (btn == PAD_BTN_RT) return "righttrigger";
        const char* nm = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(btn));
        return (nm && nm[0]) ? std::string(nm) : "None";
    }

    static std::string short_key_name(SDL_Keycode k) {
        if (k == SDLK_UNKNOWN) return "---";
        switch (k) {
            case SDLK_RETURN: case SDLK_KP_ENTER: return "Enter";
            case SDLK_SPACE:     return "Space";
            case SDLK_BACKSPACE: return "Bksp";
            case SDLK_TAB:       return "Tab";
            case SDLK_LSHIFT:    return "LShf";
            case SDLK_RSHIFT:    return "RShf";
            case SDLK_LCTRL:     return "LCtl";
            case SDLK_RCTRL:     return "RCtl";
            case SDLK_LALT:      return "LAlt";
            case SDLK_RALT:      return "RAlt";
            case SDLK_BACKQUOTE: return "`";
            case SDLK_PAGEUP:    return "PgUp";
            case SDLK_PAGEDOWN:  return "PgDn";
            case SDLK_HOME:      return "Home";
            case SDLK_END:       return "End";
            case SDLK_UP:        return "Up";
            case SDLK_DOWN:      return "Down";
            case SDLK_LEFT:      return "Left";
            case SDLK_RIGHT:     return "Right";
            default: break;
        }
        const char* nm = SDL_GetKeyName(k);
        if (!nm || !nm[0]) return "?";
        std::string s(nm);
        if (s.rfind("Keypad ", 0) == 0) {
            s = "KP" + s.substr(7);
        }
        if (s.size() > 6) s.resize(6);
        return s;
    }

    static std::string short_pad_name(int btn) {
        switch (btn) {
            case SDL_CONTROLLER_BUTTON_A:             return "PadA";
            case SDL_CONTROLLER_BUTTON_B:             return "PadB";
            case SDL_CONTROLLER_BUTTON_X:             return "PadX";
            case SDL_CONTROLLER_BUTTON_Y:             return "PadY";
            case SDL_CONTROLLER_BUTTON_BACK:          return "Back";
            case SDL_CONTROLLER_BUTTON_START:         return "Start";
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "LB";
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
            case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "L3";
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return "R3";
            case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "DpUp";
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "DpDn";
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "DpLt";
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "DpRt";
            case PAD_BTN_LT:                          return "LT";
            case PAD_BTN_RT:                          return "RT";
            default: return "---";
        }
    }

    std::string primary_soft_key_label(int act_idx) const {
        if (binds[act_idx].keys[0] != SDLK_UNKNOWN) return short_key_name(binds[act_idx].keys[0]);
        if (binds[act_idx].keys[1] != SDLK_UNKNOWN) return short_key_name(binds[act_idx].keys[1]);
        if (binds[act_idx].pad_btn >= 0) return short_pad_name(binds[act_idx].pad_btn);
        return "-";
    }

    static std::string format_lang_cfg(loc::LangMode m) {
        if (m == loc::LangMode::Zh) return "zh";
        if (m == loc::LangMode::Orig) return "orig";
        return "auto";
    }

    static loc::LangMode parse_lang_cfg(const std::string& val) {
        std::string low;
        low.reserve(val.size());
        for (char c : val) {
            if (c >= 'A' && c <= 'Z') low.push_back(static_cast<char>(c - 'A' + 'a'));
            else low.push_back(c);
        }
        if (low == "zh" || low == "cn" || low == "chinese") return loc::LangMode::Zh;
        if (low == "orig" || low == "original" || low == "jp" || low == "en") return loc::LangMode::Orig;
        return loc::LangMode::Auto;
    }

    void discover_and_load_translation() {
        std::error_code ec;
        std::vector<std::string> candidates;
        if (!cli_translation_path.empty()) {
            candidates.push_back(resolve_runtime_path(cli_translation_path, exe_base_dir));
        } else {
            const char* fname = is_lon_game ? "lon_zh.json" : "pon_zh.json";
            if (!asset_dir.empty()) {
                candidates.push_back((std::filesystem::path(asset_dir) / "translations" / fname).string());
                candidates.push_back((std::filesystem::path(asset_dir) / "assets" / "translations" / fname).string());
            }
            if (!exe_base_dir.empty()) {
                candidates.push_back((std::filesystem::path(exe_base_dir) / "assets" / "translations" / fname).string());
            }
            candidates.push_back((std::filesystem::path("assets") / "translations" / fname).string());
        }
        for (const auto& p : candidates) {
            if (std::filesystem::is_regular_file(p, ec)) {
                g_loc.load_json_file(p);
                g_loc.set_lang_mode(lang_mode);
                return;
            }
        }
        if (!candidates.empty()) {
            g_loc.set_configured_path(candidates.back());
        }
        g_loc.set_lang_mode(lang_mode);
    }

    void apply_cli_overrides() {
        if (has_cli_net_override) {
            online_mode = cli_online_mode;
            if (!cli_server_url.empty()) server_url = cli_server_url;
        }
        if (has_cli_fps_override) {
            high_fps_mode = cli_high_fps_mode;
        }
        if (has_cli_font_mode) {
            hd_font_mode = cli_hd_font_mode;
        }
        if (has_cli_pvp_bo3) {
            pvp_bo3_mode = cli_pvp_bo3;
        }
        if (has_cli_lang_override) {
            lang_mode = cli_lang_mode;
        }
        g_loc.set_lang_mode(lang_mode);
        if (!cli_player_uid.empty()) player_uid = cli_player_uid;
        if (!cli_player_name.empty()) player_name = cli_player_name;
        g_pvp.set_preferred_format(pvp_bo3_mode ? pvp::PVP_FORMAT_TRIPLE : pvp::PVP_FORMAT_SINGLE);

        if (has_cli_assist_buster) assist_cfg.buster_mult = cli_assist_cfg.buster_mult;
        if (has_cli_assist_damage) assist_cfg.damage_taken_pct = cli_assist_cfg.damage_taken_pct;
        if (has_cli_assist_encounter) assist_cfg.encounter_rate_pct = cli_assist_cfg.encounter_rate_pct;
        if (has_cli_assist_wild_boss) assist_cfg.wild_boss_only = cli_assist_cfg.wild_boss_only;
        if (has_cli_assist_show_ex_boss) assist_cfg.show_ex_boss = cli_assist_cfg.show_ex_boss;
        if (has_cli_assist_ex_boss_tier) assist_cfg.ex_boss_tier = cli_assist_cfg.ex_boss_tier;
        if (has_cli_assist_zenny) assist_cfg.zenny_rate_pct = cli_assist_cfg.zenny_rate_pct;
        if (has_cli_assist_run_speed) assist_cfg.run_speed_pct = cli_assist_cfg.run_speed_pct;
        if (has_cli_assist_auto_full_hp) assist_cfg.auto_full_hp = cli_assist_cfg.auto_full_hp;
        if (has_cli_assist_always_escape) assist_cfg.always_escape = cli_assist_cfg.always_escape;
        if (has_cli_assist_custom_gauge) assist_cfg.custom_gauge_pct = cli_assist_cfg.custom_gauge_pct;
        if (has_cli_assist_drop_rank) assist_cfg.drop_rank_mode = cli_assist_cfg.drop_rank_mode;
        assist_cfg.sanitize();
    }

    void load_config() {
        std::ifstream f(config_path);
        if (!f) {
            apply_cli_overrides();
            return;
        }
        bool seen_key[ACT_COUNT][2] = {};
        bool seen_pad[ACT_COUNT] = {};
        std::string line;
        while (std::getline(f, line)) {
            std::string s = trim(line);
            if (s.empty() || s[0] == '#') continue;
            auto eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(s.substr(0, eq));
            std::string val = trim(s.substr(eq + 1));

            if (key == "master_volume") {
                std::lock_guard<std::mutex> lk(g_audio.mtx);
                g_audio.master_volume = std::clamp(std::atoi(val.c_str()), 0, 100);
            } else if (key == "muted") {
                std::lock_guard<std::mutex> lk(g_audio.mtx);
                g_audio.muted = (std::atoi(val.c_str()) != 0);
            } else if (key == "show_softkey_bar") {
                show_softkey_bar = (std::atoi(val.c_str()) != 0);
            } else if (key == "high_fps_mode") {
                high_fps_mode = (std::atoi(val.c_str()) != 0);
            } else if (key == "hd_font_mode") {
                hd_font_mode = (std::atoi(val.c_str()) != 0);
            } else if (key == "online_mode") {
                online_mode = (std::atoi(val.c_str()) != 0);
            } else if (key == "server_url" && !val.empty()) {
                server_url = val;
            } else if (key == "player_uid" && !val.empty()) {
                player_uid = val;
            } else if (key == "player_name" && !val.empty()) {
                player_name = val;
            } else if (key == "pvp_bo3_mode") {
                pvp_bo3_mode = (std::atoi(val.c_str()) != 0);
            } else if (key == "language_mode") {
                lang_mode = parse_lang_cfg(val);
            } else if (key == "assist_master") {
                assist_cfg.master_enabled = (std::atoi(val.c_str()) != 0);
            } else if (key == "assist_hotkey_mode") {
                assist_cfg.hotkey_mode = std::atoi(val.c_str());
            } else if (key == "assist_buster_mult") {
                assist_cfg.buster_mult = std::atoi(val.c_str());
            } else if (key == "assist_last_buster_mult") {
                assist_cfg.last_nonzero_buster_mult = std::atoi(val.c_str());
            } else if (key == "assist_damage_pct") {
                assist_cfg.damage_taken_pct = std::atoi(val.c_str());
            } else if (key == "assist_encounter_pct") {
                assist_cfg.encounter_rate_pct = std::atoi(val.c_str());
            } else if (key == "assist_wild_boss_only") {
                assist_cfg.wild_boss_only = (std::atoi(val.c_str()) != 0);
            } else if (key == "assist_show_ex_boss") {
                assist_cfg.show_ex_boss = std::atoi(val.c_str());
            } else if (key == "assist_ex_boss_tier") {
                assist_cfg.ex_boss_tier = std::atoi(val.c_str());
            } else if (key == "assist_zenny_pct") {
                assist_cfg.zenny_rate_pct = std::atoi(val.c_str());
            } else if (key == "assist_run_speed_pct") {
                assist_cfg.run_speed_pct = std::atoi(val.c_str());
            } else if (key == "assist_auto_full_hp") {
                assist_cfg.auto_full_hp = (std::atoi(val.c_str()) != 0);
            } else if (key == "assist_always_escape") {
                assist_cfg.always_escape = (std::atoi(val.c_str()) != 0);
            } else if (key == "assist_custom_gauge_pct") {
                assist_cfg.custom_gauge_pct = std::atoi(val.c_str());
            } else if (key == "assist_drop_rank_mode") {
                assist_cfg.drop_rank_mode = std::atoi(val.c_str());
            } else if (key == "assist_show_hud") {
                assist_cfg.show_assist_hud = (std::atoi(val.c_str()) != 0);
            } else {
                for (int i = 0; i < ACT_COUNT; ++i) {
                    std::string pfx = std::string(binds[i].cfg_id) + "_";
                    if (key == pfx + "key1") { binds[i].keys[0] = parse_key_str(val); seen_key[i][0] = true; }
                    else if (key == pfx + "key2") { binds[i].keys[1] = parse_key_str(val); seen_key[i][1] = true; }
                    else if (key == pfx + "pad")  { binds[i].pad_btn = parse_pad_str(val); seen_pad[i] = true; }
                }
            }
        }
        // Ensure older configs without newly-added actions don't collide with explicit user bindings
        for (int i = 0; i < ACT_COUNT; ++i) {
            for (int s = 0; s < 2; ++s) {
                if (seen_key[i][s] && binds[i].keys[s] != SDLK_UNKNOWN) {
                    for (int j = 0; j < ACT_COUNT; ++j) {
                        for (int t = 0; t < 2; ++t) {
                            if ((j != i || t != s) && !seen_key[j][t] && binds[j].keys[t] == binds[i].keys[s]) {
                                binds[j].keys[t] = SDLK_UNKNOWN;
                            }
                        }
                    }
                }
            }
            if (seen_pad[i] && binds[i].pad_btn >= 0) {
                for (int j = 0; j < ACT_COUNT; ++j) {
                    if (j != i && !seen_pad[j] && binds[j].pad_btn == binds[i].pad_btn) {
                        binds[j].pad_btn = SDL_CONTROLLER_BUTTON_INVALID;
                    }
                }
            }
        }
        apply_cli_overrides();
    }

    void save_config() const {
        int master_vol = 100;
        bool is_muted = false;
        {
            std::lock_guard<std::mutex> lk(g_audio.mtx);
            master_vol = g_audio.master_volume;
            is_muted = g_audio.muted;
        }

        std::ostringstream out;
        out << "# rmexe-mobile-recomp keybindings, frame rate, network & emulator settings\n";
        out << "master_volume=" << master_vol << "\n";
        out << "muted=" << (is_muted ? 1 : 0) << "\n";
        out << "high_fps_mode=" << (high_fps_mode ? 1 : 0) << "\n";
        out << "hd_font_mode=" << (hd_font_mode ? 1 : 0) << "\n";
        out << "show_softkey_bar=" << (show_softkey_bar ? 1 : 0) << "\n";
        out << "online_mode=" << (online_mode ? 1 : 0) << "\n";
        out << "server_url=" << server_url << "\n";
        out << "player_uid=" << player_uid << "\n";
        out << "player_name=" << player_name << "\n";
        out << "pvp_bo3_mode=" << (pvp_bo3_mode ? 1 : 0) << "\n";
        out << "language_mode=" << format_lang_cfg(lang_mode) << "\n";
        out << "assist_master=" << (assist_cfg.master_enabled ? 1 : 0) << "\n";
        out << "assist_hotkey_mode=" << assist_cfg.hotkey_mode << "\n";
        out << "assist_buster_mult=" << assist_cfg.buster_mult << "\n";
        out << "assist_last_buster_mult=" << assist_cfg.last_nonzero_buster_mult << "\n";
        out << "assist_damage_pct=" << assist_cfg.damage_taken_pct << "\n";
        out << "assist_encounter_pct=" << assist_cfg.encounter_rate_pct << "\n";
        out << "assist_wild_boss_only=" << (assist_cfg.wild_boss_only ? 1 : 0) << "\n";
        out << "assist_show_ex_boss=" << assist_cfg.show_ex_boss << "\n";
        out << "assist_ex_boss_tier=" << assist_cfg.ex_boss_tier << "\n";
        out << "assist_zenny_pct=" << assist_cfg.zenny_rate_pct << "\n";
        out << "assist_run_speed_pct=" << assist_cfg.run_speed_pct << "\n";
        out << "assist_auto_full_hp=" << (assist_cfg.auto_full_hp ? 1 : 0) << "\n";
        out << "assist_always_escape=" << (assist_cfg.always_escape ? 1 : 0) << "\n";
        out << "assist_custom_gauge_pct=" << assist_cfg.custom_gauge_pct << "\n";
        out << "assist_drop_rank_mode=" << assist_cfg.drop_rank_mode << "\n";
        out << "assist_show_hud=" << (assist_cfg.show_assist_hud ? 1 : 0) << "\n\n";
        for (int i = 0; i < ACT_COUNT; ++i) {
            out << binds[i].cfg_id << "_key1=" << format_key_cfg(binds[i].keys[0]) << "\n";
            out << binds[i].cfg_id << "_key2=" << format_key_cfg(binds[i].keys[1]) << "\n";
            out << binds[i].cfg_id << "_pad="  << format_pad_cfg(binds[i].pad_btn) << "\n";
        }

        const std::string data = out.str();
        const std::string tmp_path = config_path + ".tmp";
        {
            std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!f.good()) {
                std::remove(tmp_path.c_str());
                return;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp_path, config_path, ec);
        if (ec) {
            std::ofstream f(config_path, std::ios::binary | std::ios::trunc);
            if (f) f.write(data.data(), static_cast<std::streamsize>(data.size()));
            std::remove(tmp_path.c_str());
        }
    }

    void open_controllers() {
        close_controllers();
        int n = SDL_NumJoysticks();
        for (int i = 0; i < n; ++i) {
            if (SDL_IsGameController(i)) {
                SDL_GameController* gc = SDL_GameControllerOpen(i);
                if (gc) controllers.push_back(gc);
            }
        }
    }

    void close_controllers() {
        for (SDL_GameController* gc : controllers) {
            if (gc) SDL_GameControllerClose(gc);
        }
        controllers.clear();
    }

    void show_toast(const std::string& msg, int frames = 90) {
        toast_text = msg;
        toast_frames = frames;
    }

    static void fill_rect_alpha(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh, uint32_t argb) {
        uint32_t a = (argb >> 24) & 0xFF;
        if (a == 0) return;
        uint32_t sr = (argb >> 16) & 0xFF;
        uint32_t sg = (argb >> 8) & 0xFF;
        uint32_t sb = argb & 0xFF;
        int x0 = std::max(0, rx), x1 = std::min(bw, rx + rw);
        int y0 = std::max(0, ry), y1 = std::min(bh, ry + rh);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (a >= 255) {
                    buf[y * bw + x] = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
                } else {
                    uint32_t dst = buf[y * bw + x];
                    uint32_t dr = (dst >> 16) & 0xFF;
                    uint32_t dg = (dst >> 8) & 0xFF;
                    uint32_t db = dst & 0xFF;
                    uint32_t nr = (sr * a + dr * (255 - a)) / 255;
                    uint32_t ng = (sg * a + dg * (255 - a)) / 255;
                    uint32_t nb = (sb * a + db * (255 - a)) / 255;
                    buf[y * bw + x] = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
                }
            }
        }
    }

    static void draw_rect_border(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh, uint32_t argb) {
        fill_rect_alpha(buf, bw, bh, rx, ry, rw, 1, argb);
        fill_rect_alpha(buf, bw, bh, rx, ry + rh - 1, rw, 1, argb);
        fill_rect_alpha(buf, bw, bh, rx, ry, 1, rh, argb);
        fill_rect_alpha(buf, bw, bh, rx + rw - 1, ry, 1, rh, argb);
    }

    int measure_ui_text(const std::string& utf8) const {
        auto cps = utf8_to_codepoints(utf8);
        int w = 0;
        for (uint32_t cp : cps) {
            if (hd_font_mode) {
                w += default_doja_char_width(cp);
            } else if (const auto* gl = g_ft.get_glyph(cp)) {
                w += gl->adv_x;
            } else {
                w += default_doja_char_width(cp);
            }
        }
        return w;
    }

    static void render_glyphs_to_buf(uint32_t* buf, int bw, int bh,
                                     const std::string& utf8, int pen_x, int baseline_y,
                                     uint32_t color, bool is_pm, int hd_scale) {
        if (!buf || bw <= 0 || bh <= 0) return;
        auto cps = utf8_to_codepoints(utf8);
        uint32_t a_col = (color >> 24) & 0xFFu;
        if (a_col == 0) a_col = 255u;
        uint8_t sr = static_cast<uint8_t>((color >> 16) & 0xFFu);
        uint8_t sg = static_cast<uint8_t>((color >> 8) & 0xFFu);
        uint8_t sb = static_cast<uint8_t>(color & 0xFFu);
        int box_scale = std::max(1, hd_scale);

        for (uint32_t cp : cps) {
            const StbFontEngine::CachedGlyph* gl = (hd_scale >= 2)
                ? g_ft.get_hd_glyph(cp, hd_scale)
                : g_ft.get_glyph(cp);
            if (gl) {
                if (!gl->bmp.empty() && gl->rows > 0 && gl->width > 0) {
                    for (int ry = 0; ry < gl->rows; ++ry) {
                        int dy = baseline_y + gl->yoff + ry;
                        if (dy < 0 || dy >= bh) continue;
                        for (int rx = 0; rx < gl->width; ++rx) {
                            int dx = pen_x + gl->xoff + rx;
                            if (dx < 0 || dx >= bw) continue;
                            uint8_t raw_a = gl->bmp[static_cast<size_t>(ry) * static_cast<size_t>(gl->width) + static_cast<size_t>(rx)];
                            if (gl->is_bitmap) {
                                if (raw_a < 128) continue;
                                raw_a = 255;
                            }
                            uint32_t ga = (static_cast<uint32_t>(raw_a) * a_col) / 255u;
                            if (is_pm) {
                                if (ga == 0) continue;
                                blend_pm_over(buf[dy * bw + dx], sr, sg, sb, ga);
                            } else {
                                bool draw = gl->is_bitmap || (hd_scale >= 2 ? (ga > 0u) : (ga > 24u));
                                if (!draw) continue;
                                if (ga >= 255u) {
                                    buf[dy * bw + dx] = 0xFF000000u | (static_cast<uint32_t>(sr) << 16) |
                                                        (static_cast<uint32_t>(sg) << 8) | static_cast<uint32_t>(sb);
                                } else {
                                    uint32_t dst = buf[dy * bw + dx];
                                    uint32_t dr = (dst >> 16) & 0xFFu;
                                    uint32_t dg = (dst >> 8) & 0xFFu;
                                    uint32_t db = dst & 0xFFu;
                                    uint32_t nr = (static_cast<uint32_t>(sr) * ga + dr * (255u - ga)) / 255u;
                                    uint32_t ng = (static_cast<uint32_t>(sg) * ga + dg * (255u - ga)) / 255u;
                                    uint32_t nb = (static_cast<uint32_t>(sb) * ga + db * (255u - ga)) / 255u;
                                    buf[dy * bw + dx] = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
                                }
                            }
                        }
                    }
                }
                pen_x += gl->adv_x;
            } else {
                int adv = default_doja_char_width(cp);
                if (cp > 0x20u) {
                    for (int gy = 0; gy < 10; ++gy) {
                        int dy0 = baseline_y + (-9 + gy) * box_scale;
                        for (int gx = 0; gx < adv - 1; ++gx) {
                            if (((cp >> ((gx + gy) % 7)) & 1u) == 0u) continue;
                            int dx0 = pen_x + gx * box_scale;
                            for (int sy = 0; sy < box_scale; ++sy) {
                                int dy = dy0 + sy;
                                if (dy < 0 || dy >= bh) continue;
                                for (int sx = 0; sx < box_scale; ++sx) {
                                    int dx = dx0 + sx;
                                    if (dx < 0 || dx >= bw) continue;
                                    if (is_pm) {
                                        blend_pm_over(buf[dy * bw + dx], sr, sg, sb, a_col);
                                    } else {
                                        buf[dy * bw + dx] = 0xFF000000u | (static_cast<uint32_t>(sr) << 16) |
                                                            (static_cast<uint32_t>(sg) << 8) | static_cast<uint32_t>(sb);
                                    }
                                }
                            }
                        }
                    }
                }
                pen_x += adv * box_scale;
            }
        }
    }

    static void draw_ui_text(uint32_t* buf, int bw, int bh, const std::string& utf8, int x, int baseline_y, uint32_t color, bool shadow = true) {
        if (shadow) {
            uint32_t sa = ((color >> 24) & 0xFFu);
            if (sa == 0) sa = 255u;
            render_glyphs_to_buf(buf, bw, bh, utf8, x + 1, baseline_y + 1, (sa << 24) | 0x060810u, false, 0);
        }
        render_glyphs_to_buf(buf, bw, bh, utf8, x, baseline_y, color, false, 0);
    }

    static void draw_ui_text_hd(uint32_t* buf, int bw, int bh, int s, const std::string& utf8, int x_hd, int baseline_y_hd, uint32_t color, bool shadow = true) {
        if (shadow) {
            uint32_t sa = ((color >> 24) & 0xFFu);
            if (sa == 0) sa = 255u;
            uint32_t sh_a = (sa * 190u) / 255u;
            int sh_off = std::max(1, (s * 2 + 1) / 3);
            render_glyphs_to_buf(buf, bw, bh, utf8, x_hd + sh_off, baseline_y_hd + sh_off, (sh_a << 24) | 0x060810u, false, s);
        }
        render_glyphs_to_buf(buf, bw, bh, utf8, x_hd, baseline_y_hd, color, false, s);
    }

    static void draw_ui_text_pm(uint32_t* pix_buf, uint32_t* hd_buf, int logic_w, int logic_h, int s,
                                const std::string& utf8, int x, int baseline_y, uint32_t color, bool shadow = true) {
        uint32_t sa = ((color >> 24) & 0xFFu);
        if (sa == 0) sa = 255u;
        if (pix_buf) {
            if (shadow) {
                render_glyphs_to_buf(pix_buf, logic_w, logic_h, utf8, x + 1, baseline_y + 1, (sa << 24) | 0x060810u, true, 0);
            }
            render_glyphs_to_buf(pix_buf, logic_w, logic_h, utf8, x, baseline_y, color, true, 0);
        }
        if (hd_buf) {
            int hw = logic_w * s;
            int hh = logic_h * s;
            if (shadow) {
                uint32_t sh_a = (sa * 190u) / 255u;
                int sh_off = std::max(1, (s * 2 + 1) / 3);
                render_glyphs_to_buf(hd_buf, hw, hh, utf8, x * s + sh_off, baseline_y * s + sh_off, (sh_a << 24) | 0x060810u, true, s);
            }
            render_glyphs_to_buf(hd_buf, hw, hh, utf8, x * s, baseline_y * s, color, true, s);
        }
    }

    int active_ui_scale() const { return hd_font_mode ? std::clamp(hd_scale, 2, 4) : 1; }
    uint32_t* active_ui_buf() { return hd_font_mode ? display_buf_hd.data() : display_buf; }
    int active_ui_w() const { return 240 * active_ui_scale(); }
    int active_ui_h() const { return 260 * active_ui_scale(); }

    void ui_fill_rect(int rx, int ry, int rw, int rh, uint32_t argb) {
        int s = active_ui_scale();
        fill_rect_alpha(active_ui_buf(), active_ui_w(), active_ui_h(), rx * s, ry * s, rw * s, rh * s, argb);
    }

    void ui_draw_border(int rx, int ry, int rw, int rh, uint32_t argb) {
        int s = active_ui_scale();
        uint32_t* buf = active_ui_buf();
        int bw = active_ui_w(), bh = active_ui_h();
        int x = rx * s, y = ry * s, w = rw * s, h = rh * s;
        int t = (s >= 3) ? 2 : 1;
        if (w <= 0 || h <= 0) return;
        fill_rect_alpha(buf, bw, bh, x, y, w, t, argb);
        fill_rect_alpha(buf, bw, bh, x, y + h - t, w, t, argb);
        if (h > 2 * t) {
            fill_rect_alpha(buf, bw, bh, x, y + t, t, h - 2 * t, argb);
            fill_rect_alpha(buf, bw, bh, x + w - t, y + t, t, h - 2 * t, argb);
        }
    }

    void ui_draw_text(const std::string& utf8, int x, int baseline_y, uint32_t color, bool shadow = true) {
        if (hd_font_mode) {
            int s = active_ui_scale();
            draw_ui_text_hd(active_ui_buf(), active_ui_w(), active_ui_h(), s, utf8, x * s, baseline_y * s, color, shadow);
        } else {
            draw_ui_text(active_ui_buf(), active_ui_w(), active_ui_h(), utf8, x, baseline_y, color, shadow);
        }
    }

    void render_softkey_bar(int32_t keypad_state) {
        // Metallic flip-phone bottom bar y = 240..259
        ui_fill_rect(0, 240, 240, 1, 0xFF445684u);
        for (int y = 241; y < 260; ++y) {
            uint32_t r = 26 - (y - 241) * 10 / 19;
            uint32_t g = 32 - (y - 241) * 12 / 19;
            uint32_t b = 50 - (y - 241) * 18 / 19;
            ui_fill_rect(0, y, 240, 1, 0xFF000000u | (r << 16) | (g << 8) | b);
        }

        // Left Softkey (SOFT_KEY_1 -> DoJa 0x15)
        bool s1_held = (keypad_state & (1 << 0x15)) != 0;
        std::string k1 = primary_soft_key_label(ACT_SOFT1);
        std::string left_txt = soft_label_left.empty() ? ("[" + k1 + "] ---") : ("[" + k1 + "] " + soft_label_left);
        ui_fill_rect(2, 242, 80, 16, s1_held ? 0xFF2B5EA8u : 0xFF202942u);
        ui_draw_border(2, 242, 80, 16, s1_held ? 0xFF74B2FFu : 0xFF394970u);
        ui_draw_text(left_txt, 5, 254, soft_label_left.empty() ? 0xFF7080A0u : 0xFFF0F6FFu);

        // Right Softkey (SOFT_KEY_2 -> DoJa 0x16)
        bool s2_held = (keypad_state & (1 << 0x16)) != 0;
        std::string k2 = primary_soft_key_label(ACT_SOFT2);
        std::string right_txt = soft_label_right.empty() ? ("--- [" + k2 + "]") : (soft_label_right + " [" + k2 + "]");
        ui_fill_rect(158, 242, 80, 16, s2_held ? 0xFF2B5EA8u : 0xFF202942u);
        ui_draw_border(158, 242, 80, 16, s2_held ? 0xFF74B2FFu : 0xFF394970u);
        int rw = measure_ui_text(right_txt);
        ui_draw_text(right_txt, std::max(161, 235 - rw), 254, soft_label_right.empty() ? 0xFF7080A0u : 0xFFF0F6FFu);

        // Center Status / F1 Hint
        if (is_turbo_held() && !g_pvp.active()) {
            std::string c_txt = ">> 倍速";
            int cw = measure_ui_text(c_txt);
            ui_draw_text(c_txt, 120 - cw / 2, 254, 0xFFFFD448u);
        } else if (g_pvp.active()) {
            std::string c_txt = "#" + std::to_string(g_pvp.room_id());
            if (g_pvp.match_started() && g_pvp.rtt_ms() > 0) {
                c_txt += " " + std::to_string(g_pvp.rtt_ms()) + "ms";
            }
            int cw = measure_ui_text(c_txt);
            ui_draw_text(c_txt, 120 - cw / 2, 254, g_pvp.match_started() ? 0xFF68FF9Au : 0xFF74C4FFu);
        } else if (online_mode) {
            std::string c_txt = "在线:F1";
            int cw = measure_ui_text(c_txt);
            ui_draw_text(c_txt, 120 - cw / 2, 254, 0xFF74D8FFu);
        } else {
            std::string c_txt = "F1:设置";
            int cw = measure_ui_text(c_txt);
            ui_draw_text(c_txt, 120 - cw / 2, 254, 0xFF8A9CC2u);
        }
    }

    void compose_display_buffer(const uint32_t* game_fb,
                                const uint32_t* game_pix_text,
                                const uint32_t* game_hd_text,
                                int cur_hd_scale,
                                int32_t keypad_state,
                                bool tick_toast = true,
                                bool hide_assist_hud = false) {
        hd_scale = std::clamp(cur_hd_scale, 2, 4);
        if (hd_font_mode) {
            int s = hd_scale;
            int hw = 240 * s;
            int hh = 260 * s;
            size_t need = static_cast<size_t>(hw) * static_cast<size_t>(hh);
            if (display_buf_hd.size() != need) {
                display_buf_hd.assign(need, 0xFF000000u);
            }
            for (int y = 0; y < 240; ++y) {
                const uint32_t* fb_row = game_fb + static_cast<size_t>(y) * 240;
                for (int sy = 0; sy < s; ++sy) {
                    int hy = y * s + sy;
                    uint32_t* dst_row = display_buf_hd.data() + static_cast<size_t>(hy) * hw;
                    const uint32_t* hdt_row = game_hd_text ? (game_hd_text + static_cast<size_t>(hy) * hw) : nullptr;
                    for (int x = 0; x < 240; ++x) {
                        uint32_t bg = fb_row[x];
                        int hx0 = x * s;
                        for (int sx = 0; sx < s; ++sx) {
                            uint32_t pm = hdt_row ? hdt_row[hx0 + sx] : 0u;
                            dst_row[hx0 + sx] = (pm != 0u) ? composite_pm_over_opaque(bg, pm) : (bg | 0xFF000000u);
                        }
                    }
                }
            }
        } else {
            for (int i = 0; i < 240 * 240; ++i) {
                uint32_t pm = game_pix_text ? game_pix_text[i] : 0u;
                display_buf[i] = (pm != 0u) ? composite_pm_over_opaque(game_fb[i], pm) : (game_fb[i] | 0xFF000000u);
            }
        }
        if (show_softkey_bar) {
            render_softkey_bar(keypad_state);
        }
        if (!hide_assist_hud && !overlay_open) {
            auto truncate_hud_text = [&](std::string txt, int max_px) {
                while (measure_ui_text(txt) > max_px && txt.size() > 4) {
                    pop_utf8_char(txt);
                }
                if (measure_ui_text(txt) > max_px) return std::string("...");
                return txt;
            };
            if (!assist_hud.ranking_warning_banner.empty()) {
                std::string rtxt = truncate_hud_text(assist_hud.ranking_warning_banner, 224);
                int rtw = measure_ui_text(rtxt);
                int rw = std::min(234, rtw + 10);
                int rx = (240 - rw) / 2;
                ui_fill_rect(rx, 2, rw, 15, 0xDC480A14u);
                ui_draw_border(rx, 2, rw, 15, 0xFFFF5A5Au);
                ui_draw_text(rtxt, rx + (rw - rtw) / 2, 13, 0xFFFFEA88u);
            }
            if (!assist_hud.ex_boss_banner.empty()) {
                int ey = assist_hud.ranking_warning_banner.empty() ? 2 : 19;
                std::string etxt = truncate_hud_text(assist_hud.ex_boss_banner, 224);
                int ew = std::min(234, measure_ui_text(etxt) + 8);
                ui_fill_rect(3, ey, ew, 14, 0xB80A1224u);
                ui_draw_border(3, ey, ew, 14, assist_hud.ex_boss_color);
                ui_draw_text(etxt, 7, ey + 11, assist_hud.ex_boss_color);
            }
            if (assist_cfg.show_assist_hud) {
                std::string summary = assist_cfg.build_hud_summary(assist_hold_down, assist_hud.en_native_bas_max);
                if (!summary.empty()) {
                    if (measure_ui_text(summary) > 224) {
                        while (measure_ui_text(summary + "..") > 224 && summary.size() > 3) {
                            pop_utf8_char(summary);
                        }
                        summary += "..";
                    }
                    int sw = std::min(234, measure_ui_text(summary) + 8);
                    ui_fill_rect(3, 224, sw, 14, 0xB00C162Au);
                    ui_draw_border(3, 224, sw, 14, 0xFF4F88D8u);
                    ui_draw_text(summary, 7, 235, 0xFF88FFAAu);
                }
            }
        }
        if (overlay_open) {
            render_overlay_onto_display_buf();
        }
        if (toast_frames > 0 && !toast_text.empty()) {
            uint32_t alpha = (toast_frames < 20) ? static_cast<uint32_t>(toast_frames * 255 / 20) : 235u;
            int tw = measure_ui_text(toast_text);
            int bw = std::min(232, tw + 14);
            int bx = (240 - bw) / 2;
            ui_fill_rect(bx, 4, bw, 17, ((alpha * 220 / 255) << 24) | 0x10182Cu);
            ui_draw_border(bx, 4, bw, 17, (alpha << 24) | 0x5898E8u);
            ui_draw_text(toast_text, bx + (bw - tw) / 2, 16, (alpha << 24) | 0xFFFFFFu);
            if (tick_toast) --toast_frames;
        }
    }

    void present_sdl_frame(SDL_Renderer* ren, SDL_Texture* tex_pix, SDL_Texture* tex_hd) {
        if (!ren) return;
        int logical_h = show_softkey_bar ? 260 : 240;
        const bool use_hd = (hd_font_mode && tex_hd && !display_buf_hd.empty());
        SDL_Texture* active_tex = use_hd ? tex_hd : tex_pix;
        if (!active_tex) return;

        int src_w = 240;
        int src_h = logical_h;
        if (use_hd) {
            int s = std::clamp(hd_scale, 2, 4);
            src_w = 240 * s;
            src_h = logical_h * s;
            SDL_UpdateTexture(tex_hd, nullptr, display_buf_hd.data(), src_w * static_cast<int>(sizeof(uint32_t)));
        } else {
            SDL_UpdateTexture(tex_pix, nullptr, display_buf, 240 * static_cast<int>(sizeof(uint32_t)));
        }
        SDL_Rect src_rect{0, 0, src_w, src_h};

        if (touch.enabled()) {
            int cur_lw = 0, cur_lh = 0;
            SDL_RenderGetLogicalSize(ren, &cur_lw, &cur_lh);
            if (cur_lw != 0 || cur_lh != 0) {
                SDL_RenderSetLogicalSize(ren, 0, 0);
            }
            int out_w = 0, out_h = 0;
            SDL_GetRendererOutputSize(ren, &out_w, &out_h);
            touch.update_layout(out_w, out_h, logical_h);
            SDL_Rect dst_rect = touch.game_rect();
            SDL_SetRenderDrawColor(ren, 8, 12, 24, 255);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, active_tex, &src_rect, &dst_rect);
            touch.render(ren);
            SDL_RenderPresent(ren);
            return;
        }

        int cur_lw = 0, cur_lh = 0;
        SDL_RenderGetLogicalSize(ren, &cur_lw, &cur_lh);
        if (cur_lw != src_w || cur_lh != src_h) {
            SDL_RenderSetLogicalSize(ren, src_w, src_h);
        }
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, active_tex, &src_rect, nullptr);
        SDL_RenderPresent(ren);
    }

    void apply_softbar_geometry(SDL_Window* win, SDL_Renderer* ren, int scale) {
        if (!ren) return;
        int logical_h = show_softkey_bar ? 260 : 240;
        if (touch.enabled()) {
            int out_w = 0, out_h = 0;
            SDL_GetRendererOutputSize(ren, &out_w, &out_h);
            touch.update_layout(out_w, out_h, logical_h);
            return;
        }
        int s = hd_font_mode ? std::clamp(hd_scale, 2, 4) : 1;
        SDL_RenderSetLogicalSize(ren, 240 * s, logical_h * s);
        if (win && !is_fullscreen && scale > 0) {
            SDL_SetWindowSize(win, 240 * scale, logical_h * scale);
        }
    }

    void toggle_fullscreen(SDL_Window* win) {
        if (!win) return;
        is_fullscreen = !is_fullscreen;
        SDL_SetWindowFullscreen(win, is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        show_toast(is_fullscreen ? "全屏模式: 开启" : "全屏模式: 关闭");
    }

    void save_screenshot_bmp(const uint32_t* game_fb, const uint32_t* game_pix_text, const uint32_t* game_hd_text, int cur_hd_scale) {
        int s = std::clamp(cur_hd_scale, 2, 4);
        int sw = hd_font_mode ? (240 * s) : 240;
        int sh = sw;
        std::vector<uint32_t> comp(static_cast<size_t>(sw) * static_cast<size_t>(sh));
        if (hd_font_mode) {
            for (int y = 0; y < 240; ++y) {
                for (int sy = 0; sy < s; ++sy) {
                    int hy = y * s + sy;
                    for (int x = 0; x < 240; ++x) {
                        uint32_t bg = game_fb[y * 240 + x];
                        for (int sx = 0; sx < s; ++sx) {
                            int hx = x * s + sx;
                            uint32_t pm = game_hd_text ? game_hd_text[hy * sw + hx] : 0u;
                            comp[static_cast<size_t>(hy) * sw + hx] = (pm != 0u) ? composite_pm_over_opaque(bg, pm) : (bg | 0xFF000000u);
                        }
                    }
                }
            }
        } else {
            for (int i = 0; i < 240 * 240; ++i) {
                uint32_t pm = game_pix_text ? game_pix_text[i] : 0u;
                comp[i] = (pm != 0u) ? composite_pm_over_opaque(game_fb[i], pm) : (game_fb[i] | 0xFF000000u);
            }
        }
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
            comp.data(),
            sw, sh, 32, sw * static_cast<int>(sizeof(uint32_t)),
            SDL_PIXELFORMAT_ARGB8888
        );
        if (!surf) return;
        std::time_t t = std::time(nullptr);
        char fname[64];
        std::snprintf(fname, sizeof(fname), "screenshot_%lld.bmp", static_cast<long long>(t));
        std::string out_path = data_dir.empty()
            ? std::string(fname)
            : (std::filesystem::path(data_dir) / fname).string();
        if (SDL_SaveBMP(surf, out_path.c_str()) == 0) {
            show_toast(std::string("已保存截图: ") + fname);
        }
        SDL_FreeSurface(surf);
    }

    void assign_key_unique(int act_idx, int slot_idx, SDL_Keycode k) {
        if (k != SDLK_UNKNOWN) {
            for (int i = 0; i < ACT_COUNT; ++i) {
                for (int s = 0; s < 2; ++s) {
                    if (binds[i].keys[s] == k) binds[i].keys[s] = SDLK_UNKNOWN;
                }
            }
        }
        binds[act_idx].keys[slot_idx] = k;
    }

    void assign_pad_unique(int act_idx, int btn) {
        if (btn >= 0) {
            for (int i = 0; i < ACT_COUNT; ++i) {
                if (binds[i].pad_btn == btn) binds[i].pad_btn = SDL_CONTROLLER_BUTTON_INVALID;
            }
        }
        binds[act_idx].pad_btn = btn;
    }

    // Layout order (Online & PvP right at top for instant access!):
    // 0: 联机对战 (Create / Quick Join / Room List / Exit Match)
    // 1: 对战赛制 (一回战 Single / 三回战 BO3·三局两胜)
    // 2: 服务器地址 (Enter to type/paste IP:Port, Left/Right preset toggle)
    // 3: 玩家昵称 (Enter to type/paste nickname)
    // 4: 通信模式 (Online Server / Local Offline)
    // 5: 主音量
    // 6: 帧率模式 (60 FPS / 15 FPS)
    // 7: 字体模式 (高清平滑 HD / 原版像素 12px)
    // 8: 底部软键栏
    // 9: 语言/汉化 (简体中文 / 原版 JP/EN)
    // 10: 减负与作弊设置 (Submenu)
    // 11..22: Keybindings (ACT_COUNT = 12)
    // 23: 恢复默认按键配置
    // 24: 返回游戏 (Resume)
    // 25: 保存进度并退出
    static constexpr int ASSIST_ROW_IDX = 10;
    static constexpr int BIND_START_ROW = 11;
    static constexpr int BIND_END_ROW = BIND_START_ROW + ACT_COUNT; // 23
    static constexpr int TOTAL_ROWS = BIND_END_ROW + 3;             // 26
    static constexpr int ASSIST_TOTAL_ROWS = 17;
    static constexpr int VISIBLE_ROWS = 9;

    bool overlay_open = false;
    int overlay_page = 0; // 0 = Main/Keybindings, 1 = Assist/QoL Cheats
    int overlay_sel_row = 0;
    int overlay_sel_col = 0; // 0 = Key1, 1 = Key2, 2 = Pad
    int overlay_scroll_top = 0;
    int overlay_main_sel_row = 0;
    int overlay_main_scroll_top = 0;
    bool overlay_waiting_bind = false;
    int overlay_text_edit_mode = 0; // 0=none, 1=server_url, 2=player_name, 10..16=assist numeric
    std::string overlay_text_edit_buf;
    bool overlay_ignore_next_textinput = false;
    std::vector<pvp::PvpRoomInfo> overlay_open_rooms;

    int current_total_rows() const {
        return (overlay_page == 1) ? ASSIST_TOTAL_ROWS : TOTAL_ROWS;
    }

    struct ProbeRequest {
        uint64_t seq = 0;
        bool probe_status = false;
        bool fetch_rooms = false;
        std::string server_url;
        std::string game;
    };
    struct ProbeResult {
        uint64_t seq = 0;
        bool has_status = false;
        bool status_ok = false;
        bool rooms_ok = false;
        std::string server_url;
        std::string stat_msg;
        std::vector<pvp::PvpRoomInfo> rooms;
        bool ready = false;
    };
    std::mutex probe_mtx;
    std::condition_variable probe_cv;
    std::thread probe_thread;
    bool probe_stop = false;
    bool probe_has_req = false;
    bool probe_inflight = false;
    uint64_t probe_seq = 0;
    uint64_t last_probe_ms = 0;
    ProbeRequest probe_req{};
    ProbeResult probe_res{};

    ~UiInputManager() {
        stop_probe_worker();
    }

    void ensure_probe_worker_locked() {
        if (probe_thread.joinable()) return;
        probe_stop = false;
        probe_thread = std::thread([this]() {
            for (;;) {
                ProbeRequest req{};
                {
                    std::unique_lock<std::mutex> lk(probe_mtx);
                    probe_cv.wait(lk, [this]() { return probe_stop || probe_has_req; });
                    if (probe_stop) return;
                    req = probe_req;
                    probe_has_req = false;
                    probe_inflight = true;
                }

                ProbeResult res{};
                res.seq = req.seq;
                res.server_url = req.server_url;
                bool ok_so_far = true;
                if (req.probe_status) {
                    res.has_status = true;
                    res.status_ok = pvp::PvpClientSession::check_server_status(req.server_url, res.stat_msg, 1500);
                    ok_so_far = res.status_ok;
                }
                if (ok_so_far && req.fetch_rooms) {
                    res.rooms_ok = pvp::PvpClientSession::list_open_rooms(req.server_url, req.game, res.rooms, 1500);
                }

                {
                    std::lock_guard<std::mutex> lk(probe_mtx);
                    probe_inflight = false;
                    if (!probe_stop && req.seq == probe_seq) {
                        res.ready = true;
                        probe_res = std::move(res);
                    }
                }
            }
        });
    }

    void stop_probe_worker() {
        std::thread to_join;
        {
            std::lock_guard<std::mutex> lk(probe_mtx);
            probe_stop = true;
            ++probe_seq;
            probe_has_req = false;
            probe_res = ProbeResult{};
            probe_cv.notify_all();
            if (probe_thread.joinable()) {
                to_join = std::move(probe_thread);
            }
        }
        if (to_join.joinable()) {
            to_join.join();
        }
    }

    bool is_probe_inflight() {
        std::lock_guard<std::mutex> lk(probe_mtx);
        return probe_inflight || probe_has_req;
    }

    void start_async_probe(bool probe_status, bool fetch_rooms) {
        if (server_url.empty()) return;
        std::lock_guard<std::mutex> lk(probe_mtx);
        if (probe_stop) return;
        ensure_probe_worker_locked();
        last_probe_ms = pvp::steady_millis();
        ProbeRequest req{};
        req.seq = ++probe_seq;
        req.probe_status = probe_status;
        req.fetch_rooms = fetch_rooms;
        req.server_url = server_url;
        req.game = is_lon_game ? "lon" : "pon";
        probe_req = std::move(req);
        probe_has_req = true;
        probe_res.ready = false;
        probe_cv.notify_one();
    }

    void cancel_pending_probe() {
        std::lock_guard<std::mutex> lk(probe_mtx);
        ++probe_seq;
        probe_res = ProbeResult{};
    }

    void poll_async_probe() {
        ProbeResult res{};
        {
            std::lock_guard<std::mutex> lk(probe_mtx);
            if (!probe_res.ready) return;
            if (probe_res.seq != probe_seq) {
                probe_res.ready = false;
                return;
            }
            res = std::move(probe_res);
            probe_res = ProbeResult{};
        }
        uint32_t prev_sel_room_id = 0;
        if (pvp_menu_choice >= 2 && static_cast<size_t>(pvp_menu_choice - 2) < overlay_open_rooms.size()) {
            prev_sel_room_id = overlay_open_rooms[static_cast<size_t>(pvp_menu_choice - 2)].room_id;
        }
        if (res.has_status) {
            if (res.status_ok) {
                show_toast("已连接 " + res.server_url + " (" + res.stat_msg + ")", 120);
            } else {
                overlay_open_rooms.clear();
                if (pvp_menu_choice >= 2) pvp_menu_choice = 1;
                show_toast("未连上 " + res.server_url + " (请检查地址或端口)", 120);
                return;
            }
        }
        if (!online_mode || g_pvp.active()) {
            overlay_open_rooms.clear();
            if (pvp_menu_choice >= 2) pvp_menu_choice = 0;
            return;
        }
        if (res.rooms_ok) {
            overlay_open_rooms = std::move(res.rooms);
            if (prev_sel_room_id != 0) {
                int new_choice = 1; // Keep cursor near join options if the previously highlighted room vanished
                for (size_t i = 0; i < overlay_open_rooms.size(); ++i) {
                    if (overlay_open_rooms[i].room_id == prev_sel_room_id) {
                        new_choice = 2 + static_cast<int>(i);
                        break;
                    }
                }
                pvp_menu_choice = new_choice;
            } else {
                int max_choices = 2 + static_cast<int>(overlay_open_rooms.size());
                if (pvp_menu_choice >= max_choices) pvp_menu_choice = 1;
            }
        }
    }

    static void pop_utf8_char(std::string& s) {
        while (!s.empty() && (static_cast<uint8_t>(s.back()) & 0xC0u) == 0x80u) {
            s.pop_back();
        }
        if (!s.empty()) s.pop_back();
    }

    static void sanitize_append(std::string& dst, const char* src, bool strip_spaces, size_t max_bytes) {
        if (!src) return;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(src); *p; ++p) {
            if (*p < 0x20 || *p == 0x7F) continue;
            if (strip_spaces && *p == ' ') continue;
            if (dst.size() >= max_bytes) break;
            dst.push_back(static_cast<char>(*p));
        }
    }

    void refresh_open_rooms() {
        if (!online_mode || server_url.empty() || g_pvp.active()) {
            cancel_pending_probe();
            overlay_open_rooms.clear();
            if (pvp_menu_choice >= 2) pvp_menu_choice = 0;
            return;
        }
        start_async_probe(false, true);
    }

    void maybe_periodic_room_refresh() {
        if (!overlay_open || !online_mode || server_url.empty() || g_pvp.active() || is_probe_inflight()) return;
        uint64_t now = pvp::steady_millis();
        if (now >= last_probe_ms && (now - last_probe_ms) >= 2500ULL) {
            refresh_open_rooms();
        }
    }

    void connect_and_probe_server() {
        online_mode = true;
        overlay_open_rooms.clear();
        if (pvp_menu_choice >= 2) pvp_menu_choice = 0;
        save_config();
        show_toast("正在连接 " + server_url + "...", 90);
        start_async_probe(true, true);
    }

    void cycle_server_preset(int dir) {
        if (g_pvp.active()) {
            show_toast("请先退出当前对战房间再修改地址", 90);
            return;
        }
        const std::vector<std::string> presets = {
            "127.0.0.1:8765",
            "localhost:8765"
        };
        int cur = -1;
        for (size_t i = 0; i < presets.size(); ++i) {
            if (server_url == presets[i]) { cur = static_cast<int>(i); break; }
        }
        int nxt = (cur < 0) ? 0 : ((cur + dir + static_cast<int>(presets.size())) % static_cast<int>(presets.size()));
        server_url = presets[static_cast<size_t>(nxt)];
        connect_and_probe_server();
    }

    void toggle_fps_mode() {
        high_fps_mode = !high_fps_mode;
        save_config();
        show_toast(high_fps_mode ? "帧率模式: 60 FPS (丝滑插值)" : "帧率模式: 15 FPS (原版帧率)", 90);
    }

    void toggle_font_mode() {
        hd_font_mode = !hd_font_mode;
        save_config();
        show_toast(hd_font_mode ? "字体模式: 高清平滑 (HD)" : "字体模式: 原版像素 (12px)", 90);
    }

    void toggle_language_mode() {
        bool currently_zh = (lang_mode == loc::LangMode::Zh) ||
                            (lang_mode == loc::LangMode::Auto && g_loc.is_enabled());
        if (currently_zh) {
            lang_mode = loc::LangMode::Orig;
            g_loc.set_lang_mode(lang_mode);
            save_config();
            show_toast("语言模式: 原版 (JP/EN) (静态菜单重启生效)", 120);
        } else {
            lang_mode = loc::LangMode::Zh;
            discover_and_load_translation();
            g_loc.set_lang_mode(lang_mode);
            save_config();
            if (g_loc.translated_count() > 0) {
                show_toast("汉化补丁: 开启 (已热重载 " + std::to_string(g_loc.translated_count()) + " 条, 静态菜单重启生效)", 140);
            } else {
                std::string fname = is_lon_game ? "lon_zh.json" : "pon_zh.json";
                show_toast("汉化补丁: 未找到有效译文 (assets/translations/" + fname + ")", 140);
            }
        }
    }

    void toggle_pvp_format() {
        if (g_pvp.active()) {
            show_toast("对战房间进行中，无法更改赛制", 90);
            return;
        }
        pvp_bo3_mode = !pvp_bo3_mode;
        g_pvp.set_preferred_format(pvp_bo3_mode ? pvp::PVP_FORMAT_TRIPLE : pvp::PVP_FORMAT_SINGLE);
        save_config();
        show_toast(pvp_bo3_mode ? "对战赛制: 三回战 (BO3·三局两胜)" : "对战赛制: 一回战 (Single)", 90);
    }

    void toggle_net_mode() {
        if (g_pvp.active()) {
            show_toast("请先在第一行退出当前对战房间", 90);
            return;
        }
        online_mode = !online_mode;
        save_config();
        if (online_mode) {
            connect_and_probe_server();
        } else {
            cancel_pending_probe();
            g_pvp.close_udp();
            overlay_open_rooms.clear();
            pvp_menu_choice = 0;
            show_toast("通信模式: 本地离线模拟", 90);
        }
    }

    static void sanitize_numeric_append(std::string& dst, const char* src, bool allow_minus, size_t max_bytes) {
        if (!src) return;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(src); *p; ++p) {
            if (dst.size() >= max_bytes) break;
            if (*p >= '0' && *p <= '9') {
                dst.push_back(static_cast<char>(*p));
            } else if (allow_minus && *p == '-' && dst.empty()) {
                dst.push_back('-');
            }
        }
    }

    static int32_t cycle_int_preset(int32_t cur, const int32_t* presets, int count, int dir) {
        int idx = -1;
        for (int i = 0; i < count; ++i) {
            if (presets[i] == cur) { idx = i; break; }
        }
        if (idx < 0) {
            if (dir >= 0) {
                for (int i = 0; i < count; ++i) {
                    if (presets[i] > cur) return presets[i];
                }
                return presets[0];
            } else {
                for (int i = count - 1; i >= 0; --i) {
                    if (presets[i] < cur) return presets[i];
                }
                return presets[count - 1];
            }
        }
        return presets[(idx + dir + count) % count];
    }

    void toggle_assist_master() {
        if (assist_cfg.hotkey_mode == 1) {
            assist_cfg.hotkey_mode = 0;
            assist_cfg.master_enabled = true;
        } else {
            assist_cfg.master_enabled = !assist_cfg.master_enabled;
        }
        assist_cfg.sanitize();
        save_config();
        show_toast(assist_cfg.master_enabled ? "减负辅助总开关: 开启" : "减负辅助总开关: 关闭", 90);
    }

    void toggle_assist_buster_quick() {
        if (assist_cfg.buster_mult > 1) {
            assist_cfg.last_nonzero_buster_mult = assist_cfg.buster_mult;
            assist_cfg.buster_mult = 1;
            assist_cfg.sanitize();
            save_config();
            show_toast("洛克炮强化: ×1 (原版)", 90);
        } else {
            assist_cfg.buster_mult = std::clamp<int32_t>(assist_cfg.last_nonzero_buster_mult, 2, 9999);
            assist_cfg.master_enabled = true;
            assist_cfg.sanitize();
            save_config();
            show_toast("洛克炮强化: ×" + std::to_string(assist_cfg.buster_mult), 90);
        }
    }

    void cycle_assist_encounter_quick() {
        std::string msg;
        if (assist_cfg.wild_boss_only) {
            assist_cfg.wild_boss_only = false;
            assist_cfg.encounter_rate_pct = 500;
            assist_cfg.master_enabled = true;
            msg = "遇敌模式: 500% 高遇敌";
        } else if (assist_cfg.encounter_rate_pct >= 200) {
            assist_cfg.encounter_rate_pct = 100;
            msg = "遇敌模式: 100% 原版";
        } else if (assist_cfg.encounter_rate_pct == 0) {
            assist_cfg.wild_boss_only = true;
            assist_cfg.encounter_rate_pct = 100;
            assist_cfg.master_enabled = true;
            msg = "遇敌模式: 仅遇野生Boss (潜行)";
        } else {
            assist_cfg.wild_boss_only = false;
            assist_cfg.encounter_rate_pct = 0;
            assist_cfg.master_enabled = true;
            msg = "遇敌模式: 0% 不遇敌";
        }
        assist_cfg.sanitize();
        save_config();
        show_toast(msg, 90);
    }

    void open_overlay(bool allow_room_probe) {
        // Ensure OS IME preedit composition is stopped so F1/Esc/Arrow/Enter are never swallowed
        SDL_StopTextInput();
        clear_held_states();
        overlay_page = 0;
        overlay_sel_row = 0;
        overlay_sel_col = 0;
        overlay_scroll_top = 0;
        overlay_main_sel_row = 0;
        overlay_main_scroll_top = 0;
        overlay_waiting_bind = false;
        overlay_text_edit_mode = 0;
        overlay_text_edit_buf.clear();
        overlay_ignore_next_textinput = false;
        overlay_open = true;
        if (allow_room_probe && !g_pvp.active()) {
            refresh_open_rooms();
        }
    }

    void close_overlay() {
        overlay_open = false;
        overlay_waiting_bind = false;
        overlay_text_edit_mode = 0;
        SDL_StopTextInput();
        clear_held_states();
    }

    void exec_pvp_action() {
        if (g_pvp.active()) {
            uint32_t old_rm = g_pvp.room_id();
            if (g_pvp.match_started()) {
                g_pvp.send_match_end(0);
            }
            g_pvp.close_udp();
            show_toast("已退出对战房间 #" + std::to_string(old_rm), 90);
            close_overlay();
            return;
        }
        online_mode = true;
        save_config();
        std::string gm = is_lon_game ? "lon" : "pon";
        std::string pvp_uid = effective_pvp_uid();
        if (pvp_menu_choice == 0) {
            g_pvp.set_preferred_format(pvp_bo3_mode ? pvp::PVP_FORMAT_TRIPLE : pvp::PVP_FORMAT_SINGLE);
            if (g_pvp.create_room(server_url, pvp_uid, player_name, gm)) {
                std::string fmt_tag = g_pvp.is_triple_battle() ? " [三回战]" : "";
                show_toast("已创建房间 #" + std::to_string(g_pvp.room_id()) + fmt_tag + " 等待挑战者...", 180);
                close_overlay();
            } else {
                show_toast("创建失败: 无法连接 " + server_url, 120);
            }
        } else {
            uint32_t target_room = 0;
            if (pvp_menu_choice >= 2 && static_cast<size_t>(pvp_menu_choice - 2) < overlay_open_rooms.size()) {
                target_room = overlay_open_rooms[static_cast<size_t>(pvp_menu_choice - 2)].room_id;
            }
            if (g_pvp.join_room(server_url, target_room, pvp_uid, player_name, gm)) {
                show_toast("已加入 #" + std::to_string(g_pvp.room_id()) + " VS " + g_pvp.opponent_name(), 150);
                close_overlay();
            } else {
                const std::string& jerr = g_pvp.last_join_error();
                refresh_open_rooms();
                if (jerr == "same_uid") {
                    show_toast("无法加入自己创建的房间", 120);
                } else if (jerr == "network_error") {
                    show_toast("加入失败: 无法连接 " + server_url, 120);
                } else {
                    show_toast("暂无可加入的对战房间 (" + server_url + ")", 120);
                }
            }
        }
    }

    void handle_overlay_event(const SDL_Event& ev, SDL_Window* win, SDL_Renderer* ren, int scale, bool& quit_game) {
        if (ev.type == SDL_QUIT ||
            ev.type == SDL_APP_TERMINATING ||
            (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
            quit_game = true;
            close_overlay();
            return;
        }
        if (ev.type == SDL_APP_WILLENTERBACKGROUND) {
            save_config();
            if (g_runtime) g_runtime->save_sav_overlay_public();
            if (g_audio.dev_id != 0) SDL_PauseAudioDevice(g_audio.dev_id, 1);
            return;
        }
        if (ev.type == SDL_APP_DIDENTERFOREGROUND) {
            if (g_audio.dev_id != 0 && g_pvp.active()) SDL_PauseAudioDevice(g_audio.dev_id, 0);
            return;
        }
        if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            open_controllers();
            return;
        }
        if (touch.enabled() && ren) {
            int out_w = 0, out_h = 0;
            SDL_GetRendererOutputSize(ren, &out_w, &out_h);
            touch.update_layout(out_w, out_h, show_softkey_bar ? 260 : 240);
            TouchActionDelta td;
            if (touch.handle_event(ev, out_w, out_h, true, td)) {
                for (SDL_Keycode k : td.overlay_keys) {
                    SDL_Event synth{};
                    synth.type = SDL_KEYDOWN;
                    synth.key.keysym.sym = k;
                    handle_overlay_event(synth, win, ren, scale, quit_game);
                    if (!overlay_open) break;
                }
                return;
            }
        }
        if (overlay_text_edit_mode != 0) {
            const bool is_numeric = (overlay_text_edit_mode >= 10);
            const size_t max_len = (overlay_text_edit_mode == 1) ? 44 : (is_numeric ? 6 : 18);
            const bool no_space = (overlay_text_edit_mode == 1 || is_numeric);
            auto append_edit_chars = [&](const char* txt) {
                if (is_numeric) {
                    sanitize_numeric_append(overlay_text_edit_buf, txt, overlay_text_edit_mode == 13, max_len);
                } else {
                    sanitize_append(overlay_text_edit_buf, txt, no_space, max_len);
                }
            };
            if (ev.type == SDL_TEXTINPUT) {
                if (overlay_ignore_next_textinput) {
                    overlay_ignore_next_textinput = false;
                } else {
                    append_edit_chars(ev.text.text);
                }
            } else if (ev.type == SDL_KEYDOWN) {
                overlay_ignore_next_textinput = false;
                SDL_Keycode k = ev.key.keysym.sym;
                uint16_t mod = ev.key.keysym.mod;
                if (k == SDLK_ESCAPE || k == SDLK_F1 || k == SDLK_AC_BACK) {
                    overlay_text_edit_mode = 0;
                    SDL_StopTextInput();
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    std::string trimmed = trim(overlay_text_edit_buf);
                    if (!trimmed.empty()) {
                        if (overlay_text_edit_mode == 1) {
                            if (trimmed.find(':') == std::string::npos) {
                                trimmed += ":8765";
                            }
                            overlay_text_edit_mode = 0;
                            SDL_StopTextInput();
                            if (!g_pvp.active()) {
                                server_url = trimmed;
                                connect_and_probe_server();
                            } else {
                                show_toast("请先退出当前对战房间再修改地址", 90);
                            }
                        } else if (overlay_text_edit_mode == 2) {
                            player_name = trimmed;
                            save_config();
                            overlay_text_edit_mode = 0;
                            SDL_StopTextInput();
                            show_toast("玩家昵称已更新: " + player_name, 90);
                        } else if (is_numeric && trimmed != "-") {
                            int32_t val = static_cast<int32_t>(std::atoi(trimmed.c_str()));
                            std::string msg;
                            switch (overlay_text_edit_mode) {
                                case 10:
                                    assist_cfg.buster_mult = val;
                                    assist_cfg.sanitize();
                                    msg = "洛克炮倍率: ×" + std::to_string(assist_cfg.buster_mult);
                                    break;
                                case 11:
                                    assist_cfg.damage_taken_pct = val;
                                    assist_cfg.sanitize();
                                    msg = "受伤倍率: " + std::to_string(assist_cfg.damage_taken_pct) + "%";
                                    break;
                                case 12:
                                    assist_cfg.encounter_rate_pct = val;
                                    assist_cfg.sanitize();
                                    msg = "遇敌率: " + std::to_string(assist_cfg.encounter_rate_pct) + "%";
                                    break;
                                case 13:
                                    assist_cfg.ex_boss_tier = val;
                                    assist_cfg.sanitize();
                                    msg = (assist_cfg.ex_boss_tier < 0)
                                        ? "EX Boss形态: 自动"
                                        : ("EX Boss形态: V" + std::to_string(assist_cfg.ex_boss_tier + 1));
                                    break;
                                case 14:
                                    assist_cfg.zenny_rate_pct = val;
                                    assist_cfg.sanitize();
                                    msg = "金钱倍率: " + std::to_string(assist_cfg.zenny_rate_pct) + "%";
                                    break;
                                case 15:
                                    assist_cfg.run_speed_pct = val;
                                    assist_cfg.sanitize();
                                    msg = "跑步速度: " + std::to_string(assist_cfg.run_speed_pct) + "%";
                                    break;
                                case 16:
                                    assist_cfg.custom_gauge_pct = val;
                                    assist_cfg.sanitize();
                                    msg = "自定义槽速度: " + std::to_string(assist_cfg.custom_gauge_pct) + "%";
                                    break;
                                default: break;
                            }
                            save_config();
                            overlay_text_edit_mode = 0;
                            SDL_StopTextInput();
                            if (!msg.empty()) show_toast(msg, 90);
                        } else {
                            overlay_text_edit_mode = 0;
                            SDL_StopTextInput();
                        }
                    } else {
                        overlay_text_edit_mode = 0;
                        SDL_StopTextInput();
                    }
                } else if (k == SDLK_BACKSPACE) {
                    if (mod & KMOD_CTRL) overlay_text_edit_buf.clear();
                    else pop_utf8_char(overlay_text_edit_buf);
                } else if (k == SDLK_DELETE) {
                    overlay_text_edit_buf.clear();
                } else if ((mod & KMOD_CTRL) && k == SDLK_v) {
                    if (SDL_HasClipboardText()) {
                        char* clip = SDL_GetClipboardText();
                        if (clip) {
                            append_edit_chars(clip);
                            SDL_free(clip);
                        }
                    }
                }
            }
        } else if (overlay_waiting_bind) {
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                SDL_Keycode k = ev.key.keysym.sym;
                int bind_idx = overlay_sel_row - BIND_START_ROW;
                if (k == SDLK_ESCAPE || k == SDLK_F1 || bind_idx < 0 || bind_idx >= ACT_COUNT) {
                    overlay_waiting_bind = false;
                } else if (k == SDLK_BACKSPACE || k == SDLK_DELETE) {
                    if (overlay_sel_col < 2) assign_key_unique(bind_idx, overlay_sel_col, SDLK_UNKNOWN);
                    else assign_pad_unique(bind_idx, SDL_CONTROLLER_BUTTON_INVALID);
                    save_config();
                    overlay_waiting_bind = false;
                } else {
                    int target_slot = (overlay_sel_col < 2) ? overlay_sel_col : 0;
                    assign_key_unique(bind_idx, target_slot, k);
                    save_config();
                    overlay_waiting_bind = false;
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                int bind_idx = overlay_sel_row - BIND_START_ROW;
                if (bind_idx >= 0 && bind_idx < ACT_COUNT) {
                    assign_pad_unique(bind_idx, ev.cbutton.button);
                    overlay_sel_col = 2;
                    save_config();
                }
                overlay_waiting_bind = false;
            } else if (ev.type == SDL_CONTROLLERAXISMOTION &&
                       (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                        ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) &&
                       ev.caxis.value > 20000) {
                int bind_idx = overlay_sel_row - BIND_START_ROW;
                if (bind_idx >= 0 && bind_idx < ACT_COUNT) {
                    int trig_btn = (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) ? PAD_BTN_LT : PAD_BTN_RT;
                    assign_pad_unique(bind_idx, trig_btn);
                    overlay_sel_col = 2;
                    save_config();
                }
                overlay_waiting_bind = false;
            }
        } else {
            auto enter_assist_page = [&]() {
                overlay_main_sel_row = overlay_sel_row;
                overlay_main_scroll_top = overlay_scroll_top;
                overlay_page = 1;
                overlay_sel_row = 0;
                overlay_scroll_top = 0;
            };
            auto exit_assist_page = [&]() {
                overlay_page = 0;
                overlay_sel_row = overlay_main_sel_row;
                overlay_scroll_top = overlay_main_scroll_top;
            };
            auto start_num_edit = [&](int mode, int32_t init_val) {
                overlay_text_edit_mode = mode;
                overlay_text_edit_buf = std::to_string(init_val);
                overlay_ignore_next_textinput = true;
                SDL_StartTextInput();
            };

            auto adjust_assist_lr = [&](int dir) {
                static constexpr int32_t kBusterPresets[] = {1, 2, 3, 5, 10, 50, 100};
                static constexpr int32_t kDmgPresets[] = {100, 80, 60, 40, 20, 0};
                static constexpr int32_t kEncPresets[] = {0, 50, 100, 200, 500, 10000};
                static constexpr int32_t kTierPresets[] = {-1, 0, 1, 2, 3};
                static constexpr int32_t kZennyPresets[] = {100, 200, 300, 400, 500, 1000};
                static constexpr int32_t kSpeedPresets[] = {100, 125, 150, 200, 250, 300};
                static constexpr int32_t kCusPresets[] = {100, 200, 300, 500, 1000};
                switch (overlay_sel_row) {
                    case 0:
                        assist_cfg.master_enabled = !assist_cfg.master_enabled;
                        break;
                    case 1:
                        assist_cfg.hotkey_mode = (assist_cfg.hotkey_mode + dir + 2) % 2;
                        break;
                    case 2:
                        assist_cfg.buster_mult = cycle_int_preset(assist_cfg.buster_mult, kBusterPresets, 7, dir);
                        break;
                    case 3:
                        assist_cfg.damage_taken_pct = cycle_int_preset(assist_cfg.damage_taken_pct, kDmgPresets, 6, dir);
                        break;
                    case 4:
                        assist_cfg.encounter_rate_pct = cycle_int_preset(assist_cfg.encounter_rate_pct, kEncPresets, 6, dir);
                        break;
                    case 5:
                        assist_cfg.wild_boss_only = !assist_cfg.wild_boss_only;
                        break;
                    case 6:
                        assist_cfg.show_ex_boss = (assist_cfg.show_ex_boss + dir + 3) % 3;
                        break;
                    case 7:
                        assist_cfg.ex_boss_tier = cycle_int_preset(assist_cfg.ex_boss_tier, kTierPresets, 5, dir);
                        break;
                    case 8:
                        assist_cfg.zenny_rate_pct = cycle_int_preset(assist_cfg.zenny_rate_pct, kZennyPresets, 6, dir);
                        break;
                    case 9:
                        assist_cfg.run_speed_pct = cycle_int_preset(assist_cfg.run_speed_pct, kSpeedPresets, 6, dir);
                        break;
                    case 10:
                        assist_cfg.auto_full_hp = !assist_cfg.auto_full_hp;
                        break;
                    case 11:
                        assist_cfg.always_escape = !assist_cfg.always_escape;
                        break;
                    case 12:
                        assist_cfg.custom_gauge_pct = cycle_int_preset(assist_cfg.custom_gauge_pct, kCusPresets, 5, dir);
                        break;
                    case 13:
                        assist_cfg.drop_rank_mode = (assist_cfg.drop_rank_mode + dir + 3) % 3;
                        break;
                    case 14:
                        assist_cfg.show_assist_hud = !assist_cfg.show_assist_hud;
                        break;
                    default:
                        return;
                }
                assist_cfg.sanitize();
                save_config();
            };

            auto activate_assist_row = [&]() {
                switch (overlay_sel_row) {
                    case 0: case 1: case 5: case 6: case 10: case 11: case 13: case 14:
                        adjust_assist_lr(1);
                        break;
                    case 2: start_num_edit(10, assist_cfg.buster_mult); break;
                    case 3: start_num_edit(11, assist_cfg.damage_taken_pct); break;
                    case 4: start_num_edit(12, assist_cfg.encounter_rate_pct); break;
                    case 7: start_num_edit(13, assist_cfg.ex_boss_tier); break;
                    case 8: start_num_edit(14, assist_cfg.zenny_rate_pct); break;
                    case 9: start_num_edit(15, assist_cfg.run_speed_pct); break;
                    case 12: start_num_edit(16, assist_cfg.custom_gauge_pct); break;
                    case 15:
                        assist_cfg.reset_to_vanilla();
                        save_config();
                        show_toast("已将全部减负辅助恢复原版默认", 90);
                        break;
                    case 16:
                        exit_assist_page();
                        break;
                    default: break;
                }
            };

            auto adjust_row_lr = [&](int dir) {
                if (overlay_page == 1) {
                    adjust_assist_lr(dir);
                    return;
                }
                if (overlay_sel_row == 0 && !g_pvp.active()) {
                    if (online_mode && !is_probe_inflight()) {
                        uint64_t now = pvp::steady_millis();
                        if (now < last_probe_ms || (now - last_probe_ms) >= 1500ULL) {
                            refresh_open_rooms();
                        }
                    }
                    int max_choices = 2 + static_cast<int>(overlay_open_rooms.size());
                    pvp_menu_choice = (pvp_menu_choice + dir + max_choices) % max_choices;
                } else if (overlay_sel_row == 1) {
                    toggle_pvp_format();
                } else if (overlay_sel_row == 2) {
                    cycle_server_preset(dir);
                } else if (overlay_sel_row == 4) {
                    toggle_net_mode();
                } else if (overlay_sel_row == 5) {
                    {
                        std::lock_guard<std::mutex> lk(g_audio.mtx);
                        g_audio.muted = false;
                        g_audio.master_volume = std::clamp(g_audio.master_volume + dir * 5, 0, 100);
                    }
                    save_config();
                } else if (overlay_sel_row == 6) {
                    toggle_fps_mode();
                } else if (overlay_sel_row == 7) {
                    toggle_font_mode();
                } else if (overlay_sel_row == 8) {
                    show_softkey_bar = !show_softkey_bar;
                    apply_softbar_geometry(win, ren, scale);
                    save_config();
                } else if (overlay_sel_row == 9) {
                    toggle_language_mode();
                } else if (overlay_sel_row == ASSIST_ROW_IDX) {
                    enter_assist_page();
                } else if (overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW) {
                    overlay_sel_col = (overlay_sel_col + dir + 3) % 3;
                }
            };
            auto activate_row = [&]() {
                if (overlay_page == 1) {
                    activate_assist_row();
                    return;
                }
                if (overlay_sel_row == 0) {
                    exec_pvp_action();
                } else if (overlay_sel_row == 1) {
                    toggle_pvp_format();
                } else if (overlay_sel_row == 2) {
                    if (g_pvp.active()) {
                        show_toast("请先退出当前对战房间再修改地址", 90);
                    } else {
                        overlay_text_edit_mode = 1;
                        overlay_text_edit_buf = server_url;
                        overlay_ignore_next_textinput = true;
                        SDL_StartTextInput();
                    }
                } else if (overlay_sel_row == 3) {
                    overlay_text_edit_mode = 2;
                    overlay_text_edit_buf = player_name;
                    overlay_ignore_next_textinput = true;
                    SDL_StartTextInput();
                } else if (overlay_sel_row == 4) {
                    toggle_net_mode();
                } else if (overlay_sel_row == 5) {
                    {
                        std::lock_guard<std::mutex> lk(g_audio.mtx);
                        g_audio.muted = !g_audio.muted;
                    }
                    save_config();
                } else if (overlay_sel_row == 6) {
                    toggle_fps_mode();
                } else if (overlay_sel_row == 7) {
                    toggle_font_mode();
                } else if (overlay_sel_row == 8) {
                    show_softkey_bar = !show_softkey_bar;
                    apply_softbar_geometry(win, ren, scale);
                    save_config();
                } else if (overlay_sel_row == 9) {
                    toggle_language_mode();
                } else if (overlay_sel_row == ASSIST_ROW_IDX) {
                    enter_assist_page();
                } else if (overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW) {
                    overlay_waiting_bind = true;
                } else if (overlay_sel_row == BIND_END_ROW) {
                    reset_defaults();
                    save_config();
                    show_toast("已恢复默认按键配置");
                } else if (overlay_sel_row == BIND_END_ROW + 1) {
                    close_overlay();
                } else if (overlay_sel_row == BIND_END_ROW + 2) {
                    quit_game = true;
                    close_overlay();
                }
            };

            auto matches_act = [&](int act_idx, SDL_Keycode k) {
                return (binds[act_idx].keys[0] != SDLK_UNKNOWN && binds[act_idx].keys[0] == k) ||
                       (binds[act_idx].keys[1] != SDLK_UNKNOWN && binds[act_idx].keys[1] == k);
            };

            const int cur_total = current_total_rows();
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                SDL_Keycode k = ev.key.keysym.sym;
                bool in_bind_rows = (overlay_page == 0 && overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW);
                if (k == SDLK_F1) {
                    close_overlay();
                } else if (k == SDLK_ESCAPE || (!in_bind_rows && matches_act(ACT_CANCEL, k))) {
                    if (overlay_page == 1) exit_assist_page();
                    else close_overlay();
                } else if (k == SDLK_BACKSPACE && overlay_page == 1) {
                    exit_assist_page();
                } else if (k == SDLK_F3) {
                    toggle_fps_mode();
                } else if (k == SDLK_F4) {
                    toggle_language_mode();
                } else if (k == SDLK_F5) {
                    toggle_font_mode();
                } else if (k == SDLK_F6) {
                    toggle_assist_master();
                } else if (k == SDLK_F7) {
                    toggle_assist_buster_quick();
                } else if (k == SDLK_F8) {
                    cycle_assist_encounter_quick();
                } else if (k == SDLK_UP || matches_act(ACT_UP, k)) {
                    overlay_sel_row = (overlay_sel_row - 1 + cur_total) % cur_total;
                } else if (k == SDLK_DOWN || matches_act(ACT_DOWN, k)) {
                    overlay_sel_row = (overlay_sel_row + 1) % cur_total;
                } else if (k == SDLK_LEFT || matches_act(ACT_LEFT, k)) {
                    adjust_row_lr(-1);
                } else if (k == SDLK_RIGHT || matches_act(ACT_RIGHT, k)) {
                    adjust_row_lr(1);
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE ||
                           (!in_bind_rows && matches_act(ACT_SELECT, k))) {
                    activate_row();
                } else if ((k == SDLK_DELETE || k == SDLK_BACKSPACE) && in_bind_rows) {
                    int bind_idx = overlay_sel_row - BIND_START_ROW;
                    if (overlay_sel_col < 2) assign_key_unique(bind_idx, overlay_sel_col, SDLK_UNKNOWN);
                    else assign_pad_unique(bind_idx, SDL_CONTROLLER_BUTTON_INVALID);
                    save_config();
                } else if (k == SDLK_F11) {
                    toggle_fullscreen(win);
                }
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:
                        overlay_sel_row = (overlay_sel_row - 1 + cur_total) % cur_total; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                        overlay_sel_row = (overlay_sel_row + 1) % cur_total; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                        adjust_row_lr(-1); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                        adjust_row_lr(1); break;
                    case SDL_CONTROLLER_BUTTON_A:
                        activate_row(); break;
                    case SDL_CONTROLLER_BUTTON_B:
                        if (overlay_page == 1) exit_assist_page();
                        else close_overlay();
                        break;
                    case SDL_CONTROLLER_BUTTON_START:
                        close_overlay(); break;
                    default: break;
                }
            }
        }

        if (overlay_sel_row < overlay_scroll_top) overlay_scroll_top = overlay_sel_row;
        if (overlay_sel_row >= overlay_scroll_top + VISIBLE_ROWS) overlay_scroll_top = overlay_sel_row - VISIBLE_ROWS + 1;
    }

    void render_overlay_onto_display_buf() {
        bool live_pvp = g_pvp.active();
        ui_fill_rect(0, 0, 240, 240, live_pvp ? 0xB8080C18u : 0xDC080C18u);

        // Title bar
        ui_fill_rect(4, 4, 232, 18, live_pvp ? 0xFF14362Au : 0xFF1A284Au);
        ui_draw_border(4, 4, 232, 18, live_pvp ? 0xFF4CC88Au : 0xFF4F78C8u);
        std::string title;
        if (overlay_page == 1) {
            title = live_pvp ? "减负设置 (联机PvP期间自动禁用)" : "减负与作弊设置 (Enter手输 / ←→切)";
        } else {
            title = live_pvp
                ? (g_pvp.match_started() ? "联机菜单 — 对战后台运行中 (F1关闭)" : "联机菜单 — 房间等待中 (F1关闭)")
                : "联机对战与模拟器设置 (F1 / Esc)";
        }
        ui_draw_text(title, 120 - measure_ui_text(title) / 2, 17, 0xFFFFFFFFu);

        // Dynamic section header
        if (overlay_page == 0 && overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW) {
            ui_draw_text("动作",  8,   34, 0xFF8EB0E8u);
            ui_draw_text("键位1", 80,  34, 0xFF8EB0E8u);
            ui_draw_text("键位2", 136, 34, 0xFF8EB0E8u);
            ui_draw_text("手柄",  192, 34, 0xFF8EB0E8u);
        } else if (overlay_page == 1) {
            ui_draw_text("减负项目", 8, 34, 0xFF8EB0E8u);
            ui_draw_text("当前倍率/状态 (Enter手输)", 84, 34, 0xFF8EB0E8u);
        } else {
            ui_draw_text("功能选项", 8, 34, 0xFF8EB0E8u);
            ui_draw_text("当前设置 (Enter / ←→)", 84, 34, 0xFF8EB0E8u);
        }
        ui_fill_rect(6, 37, 228, 1, 0xFF324672u);

        const int cur_total = current_total_rows();
        // Rows viewport (y = 40 .. 193, 9 rows * 17px)
        for (int vi = 0; vi < VISIBLE_ROWS; ++vi) {
            int r = overlay_scroll_top + vi;
            if (r >= cur_total) break;
            int ry = 40 + vi * 17;
            bool is_sel = (r == overlay_sel_row);
            if (is_sel) {
                ui_fill_rect(5, ry, 226, 16, 0xFF223A6Au);
                ui_draw_border(5, ry, 226, 16, 0xFF5A8AE0u);
            }

            if (overlay_page == 1) {
                std::string label;
                std::string val_str;
                uint32_t vcol = 0xFFD0DCF0u;
                bool centered = false;
                switch (r) {
                    case 0:
                        label = "辅助总开关";
                        if (assist_cfg.hotkey_mode == 1) {
                            val_str = assist_hold_down ? "< 按住F6生效中 >" : "< 待命(按住F6) >";
                            vcol = assist_hold_down ? 0xFF88FFAAu : 0xFFFFD874u;
                        } else {
                            val_str = assist_cfg.master_enabled ? "< 开启 >" : "< 关闭 >";
                            vcol = assist_cfg.master_enabled ? 0xFF88FFAAu : 0xFFFF8888u;
                        }
                        break;
                    case 1:
                        label = "F6热键模式";
                        val_str = (assist_cfg.hotkey_mode == 1) ? "< 按住生效(Hold) >" : "< 点按切换(Trigger) >";
                        vcol = (assist_cfg.hotkey_mode == 1) ? 0xFF74D8FFu : 0xFF88FFAAu;
                        break;
                    case 2:
                        label = "洛克炮倍率";
                        if (assist_cfg.buster_mult == 1) { val_str = "< ×1 (原版) >"; vcol = 0xFF98A8C0u; }
                        else if (assist_cfg.buster_mult == 100) { val_str = "< ×100 (百倍炮) >"; vcol = 0xFFFFD874u; }
                        else { val_str = "< ×" + std::to_string(assist_cfg.buster_mult) + " >"; vcol = 0xFFFFD874u; }
                        break;
                    case 3:
                        label = "受伤倍率";
                        if (assist_cfg.damage_taken_pct == 100) { val_str = "< 100% (原版) >"; vcol = 0xFF98A8C0u; }
                        else if (assist_cfg.damage_taken_pct == 0) { val_str = "< 0% (无伤) >"; vcol = 0xFF88FFAAu; }
                        else { val_str = "< " + std::to_string(assist_cfg.damage_taken_pct) + "% >"; vcol = (assist_cfg.damage_taken_pct < 100) ? 0xFF88FFAAu : 0xFFFF9874u; }
                        break;
                    case 4:
                        label = "遇敌率倍率";
                        if (assist_cfg.encounter_rate_pct == 100) { val_str = "< 100% (原版) >"; vcol = 0xFF98A8C0u; }
                        else if (assist_cfg.encounter_rate_pct == 0) { val_str = "< 0% (不遇敌) >"; vcol = 0xFF74D8FFu; }
                        else if (assist_cfg.encounter_rate_pct >= 10000) { val_str = "< 10000% (步步遇) >"; vcol = 0xFFFFD874u; }
                        else { val_str = "< " + std::to_string(assist_cfg.encounter_rate_pct) + "% >"; vcol = 0xFFFFD874u; }
                        break;
                    case 5:
                        label = "仅遇野生Boss";
                        val_str = assist_cfg.wild_boss_only ? "< 开启 (潜行锁定) >" : "< 关闭 >";
                        vcol = assist_cfg.wild_boss_only ? 0xFF88FFAAu : 0xFF98A8C0u;
                        break;
                    case 6:
                        label = "隐藏EX Boss";
                        if (assist_cfg.show_ex_boss == 0) { val_str = "< 关闭 >"; vcol = 0xFF98A8C0u; }
                        else if (assist_cfg.show_ex_boss == 1) { val_str = "< 雷达提示 >"; vcol = 0xFF74D8FFu; }
                        else { val_str = "< 雷达+无视剧情解锁 >"; vcol = 0xFFFFD874u; }
                        break;
                    case 7:
                        label = "EX Boss形态";
                        if (assist_cfg.ex_boss_tier < 0) { val_str = "< 自动 (按存档) >"; vcol = 0xFF98A8C0u; }
                        else {
                            static const char* kTierNames[4] = {"V1 (初遇)", "V2 (强化)", "V3 (EX)", "SP (最强)"};
                            val_str = std::string("< ") + kTierNames[std::clamp(assist_cfg.ex_boss_tier, 0, 3)] + " >";
                            vcol = 0xFFFFD874u;
                        }
                        break;
                    case 8:
                        label = "金钱倍率";
                        val_str = (assist_cfg.zenny_rate_pct == 100) ? "< 100% (原版) >" : ("< " + std::to_string(assist_cfg.zenny_rate_pct) + "% >");
                        vcol = (assist_cfg.zenny_rate_pct == 100) ? 0xFF98A8C0u : 0xFFFFD874u;
                        break;
                    case 9:
                        label = "跑步速度";
                        val_str = (assist_cfg.run_speed_pct == 100) ? "< 100% (原版) >" : ("< " + std::to_string(assist_cfg.run_speed_pct) + "% >");
                        vcol = (assist_cfg.run_speed_pct == 100) ? 0xFF98A8C0u : 0xFF88FFAAu;
                        break;
                    case 10:
                        label = "战后自动满血";
                        val_str = assist_cfg.auto_full_hp ? "< 开启 >" : "< 关闭 >";
                        vcol = assist_cfg.auto_full_hp ? 0xFF88FFAAu : 0xFF98A8C0u;
                        break;
                    case 11:
                        label = "必定逃跑成功";
                        val_str = assist_cfg.always_escape ? "< 开启 >" : "< 关闭 >";
                        vcol = assist_cfg.always_escape ? 0xFF88FFAAu : 0xFF98A8C0u;
                        break;
                    case 12:
                        label = "自定义槽加速";
                        val_str = (assist_cfg.custom_gauge_pct == 100) ? "< 100% (原版) >" : ("< " + std::to_string(assist_cfg.custom_gauge_pct) + "% >");
                        vcol = (assist_cfg.custom_gauge_pct == 100) ? 0xFF98A8C0u : 0xFF74D8FFu;
                        break;
                    case 13:
                        label = "芯片掉率/评价";
                        if (assist_cfg.drop_rank_mode == 0) { val_str = "< 原版 >"; vcol = 0xFF98A8C0u; }
                        else if (assist_cfg.drop_rank_mode == 1) { val_str = "< 必掉芯片 >"; vcol = 0xFF88FFAAu; }
                        else { val_str = "< 强制S级+必掉 >"; vcol = 0xFFFFD874u; }
                        break;
                    case 14:
                        label = "辅助角标提示";
                        val_str = assist_cfg.show_assist_hud ? "< 开启 >" : "< 关闭 >";
                        vcol = assist_cfg.show_assist_hud ? 0xFF88FFAAu : 0xFF98A8C0u;
                        break;
                    case 15:
                        val_str = "[ 全部恢复原版默认 ]";
                        vcol = is_sel ? 0xFFFFD866u : 0xFFC8D8F8u;
                        centered = true;
                        break;
                    case 16:
                        val_str = "[ 返回主设置菜单 ]";
                        vcol = is_sel ? 0xFF88FFAAu : 0xFFC8D8F8u;
                        centered = true;
                        break;
                }
                if (centered) {
                    ui_draw_text(val_str, 120 - measure_ui_text(val_str) / 2, ry + 12, vcol);
                } else {
                    ui_draw_text(label, 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                    int vx = std::max(84, std::min(92, 228 - measure_ui_text(val_str)));
                    ui_draw_text(val_str, vx, ry + 12, vcol);
                }
                continue;
            }

            if (r == 0) {
                ui_draw_text("联机对战", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFFFE484u);
                std::string pstr;
                uint32_t pcol = 0xFFFFD874u;
                if (g_pvp.active()) {
                    pstr = g_pvp.match_started()
                        ? ("[ 退出对战 #" + std::to_string(g_pvp.room_id()) + " ]")
                        : ("[ 取消等待 #" + std::to_string(g_pvp.room_id()) + " ]");
                    pcol = 0xFF88FFAAu;
                } else if (pvp_menu_choice == 0) {
                    pstr = "< 创建对战房间 >";
                } else if (pvp_menu_choice == 1) {
                    pstr = "< 快速加入配对 >";
                } else if (static_cast<size_t>(pvp_menu_choice - 2) < overlay_open_rooms.size()) {
                    const auto& rm = overlay_open_rooms[static_cast<size_t>(pvp_menu_choice - 2)];
                    std::string tag = (rm.match_format == pvp::PVP_FORMAT_TRIPLE) ? "[3战]" : "[1战]";
                    pstr = "< " + tag + "#" + std::to_string(rm.room_id) + " " + rm.host + " >";
                    pcol = 0xFF74D8FFu;
                } else {
                    pstr = "< 快速加入配对 >";
                }
                ui_draw_text(pstr, 84, ry + 12, pcol);
            } else if (r == 1) {
                ui_draw_text("对战赛制", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string fstr;
                uint32_t fcol = 0xFF88FFAAu;
                if (g_pvp.active()) {
                    bool is_bo3 = g_pvp.is_triple_battle();
                    fstr = is_bo3 ? "三回战 (BO3·三局两胜)" : "一回战 (Single)";
                    fcol = is_bo3 ? 0xFFFFD874u : 0xFF88FFAAu;
                } else {
                    fstr = pvp_bo3_mode ? "< 三回战 (BO3·三局两胜) >" : "< 一回战 (Single) >";
                    fcol = pvp_bo3_mode ? 0xFFFFD874u : 0xFF88FFAAu;
                }
                int fx = std::max(64, std::min(84, 228 - measure_ui_text(fstr)));
                ui_draw_text(fstr, fx, ry + 12, fcol);
            } else if (r == 2) {
                ui_draw_text("服务器地址", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string disp_srv = server_url;
                if (disp_srv.size() > 21) disp_srv = disp_srv.substr(0, 20) + "..";
                std::string ustr = "[" + disp_srv + "]";
                ui_draw_text(ustr, 76, ry + 12, is_sel ? 0xFF88EEFFu : 0xFF74D8FFu);
            } else if (r == 3) {
                ui_draw_text("玩家昵称", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string nstr = "[" + player_name + "]";
                ui_draw_text(nstr, 84, ry + 12, is_sel ? 0xFFFFEC88u : 0xFF88FFAAu);
            } else if (r == 4) {
                ui_draw_text("通信模式", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string nstr = online_mode ? "< 在线服务器 >" : "< 本地离线模拟 >";
                ui_draw_text(nstr, 86, ry + 12, online_mode ? 0xFF74D8FFu : 0xFF98A8C0u);
            } else if (r == 5) {
                int vol = 100;
                bool muted = false;
                {
                    std::lock_guard<std::mutex> lk(g_audio.mtx);
                    vol = g_audio.master_volume;
                    muted = g_audio.muted;
                }
                ui_draw_text("主音量", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string vstr = muted ? ("< 静音 (" + std::to_string(vol) + "%) >") : ("< " + std::to_string(vol) + "% >");
                ui_draw_text(vstr, 102, ry + 12, muted ? 0xFFFF8888u : 0xFF88FFAAu);
            } else if (r == 6) {
                ui_draw_text("帧率模式", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string fstr = high_fps_mode ? "< 60 FPS 丝滑插值 >" : "< 15 FPS 原版帧率 >";
                ui_draw_text(fstr, 84, ry + 12, high_fps_mode ? 0xFF88FFAAu : 0xFFFFD874u);
            } else if (r == 7) {
                ui_draw_text("字体模式", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string fmstr = hd_font_mode ? "< 高清平滑 (HD) >" : "< 原版像素 (12px) >";
                ui_draw_text(fmstr, 84, ry + 12, hd_font_mode ? 0xFF74D8FFu : 0xFFFFD874u);
            } else if (r == 8) {
                ui_draw_text("底部软键栏", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string sstr = show_softkey_bar ? "< 开启 >" : "< 关闭 >";
                ui_draw_text(sstr, 110, ry + 12, show_softkey_bar ? 0xFF88FFAAu : 0xFF98A8C0u);
            } else if (r == 9) {
                ui_draw_text("语言/汉化", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string lstr;
                uint32_t lcol = 0xFF98A8C0u;
                if (lang_mode == loc::LangMode::Orig) {
                    lstr = "< 原版语言 (JP/EN) >";
                    lcol = 0xFF98A8C0u;
                } else if (g_loc.has_loaded_pack() && g_loc.translated_count() > 0) {
                    lstr = "< 简体中文 (" + std::to_string(g_loc.translated_count()) + "条) >";
                    lcol = 0xFF88FFAAu;
                } else {
                    lstr = "< 简体中文 (未放字表) >";
                    lcol = 0xFFFFD874u;
                }
                ui_draw_text(lstr, 84, ry + 12, lcol);
            } else if (r == ASSIST_ROW_IDX) {
                ui_draw_text("减负辅助", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFFFE484u);
                int cnt = assist_cfg.active_cheat_count();
                std::string astr = (cnt > 0)
                    ? ("[ 进入设置 (已开" + std::to_string(cnt) + "项) > ]")
                    : "[ 进入减负与作弊设置 > ]";
                uint32_t acol = (cnt > 0) ? 0xFFFFD874u : 0xFF74D8FFu;
                ui_draw_text(astr, 84, ry + 12, acol);
            } else if (r >= BIND_START_ROW && r < BIND_END_ROW) {
                int bi = r - BIND_START_ROW;
                ui_draw_text(binds[bi].label_zh, 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string cols[3] = {
                    short_key_name(binds[bi].keys[0]),
                    short_key_name(binds[bi].keys[1]),
                    short_pad_name(binds[bi].pad_btn)
                };
                int cx[3] = {74, 130, 186};
                int cw[3] = {52, 52, 42};
                for (int c = 0; c < 3; ++c) {
                    bool cell_sel = is_sel && (overlay_sel_col == c);
                    if (cell_sel) {
                        ui_fill_rect(cx[c], ry + 1, cw[c], 14, 0xFF345898u);
                        ui_draw_border(cx[c], ry + 1, cw[c], 14, 0xFFFFD24Cu);
                    }
                    int tw = measure_ui_text(cols[c]);
                    uint32_t tcol = (cols[c] == "---") ? 0xFF687898u : (cell_sel ? 0xFFFFEC88u : 0xFFE8F0FFu);
                    ui_draw_text(cols[c], cx[c] + (cw[c] - tw) / 2, ry + 12, tcol);
                }
            } else if (r == BIND_END_ROW) {
                std::string lbl = "[ 恢复默认按键配置 ]";
                ui_draw_text(lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFFFFD866u : 0xFFC8D8F8u);
            } else if (r == BIND_END_ROW + 1) {
                std::string lbl = "[ 返回游戏 (Resume) ]";
                ui_draw_text(lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFF88FFAAu : 0xFFC8D8F8u);
            } else if (r == BIND_END_ROW + 2) {
                std::string lbl = "[ 保存进度并退出 ]";
                ui_draw_text(lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFFFF8888u : 0xFFE8B8B8u);
            }
        }

        // Scrollbar track on right edge
        ui_fill_rect(233, 40, 3, VISIBLE_ROWS * 17, 0xFF182238u);
        int sb_h = std::max(14, (VISIBLE_ROWS * 17 * VISIBLE_ROWS) / cur_total);
        int sb_y = 40 + ((VISIBLE_ROWS * 17 - sb_h) * overlay_scroll_top) / std::max(1, cur_total - VISIBLE_ROWS);
        ui_fill_rect(233, sb_y, 3, sb_h, 0xFF6894E0u);

        // Footer instructions
        ui_fill_rect(6, 196, 228, 1, 0xFF324672u);
        if (overlay_page == 1) {
            if (overlay_sel_row == 2 || overlay_sel_row == 3) {
                ui_draw_text("Enter:手输数值  ←→:预设  (影响排行记录)", 6, 211, 0xFFFFB060u);
            } else if (overlay_sel_row == 4 || overlay_sel_row == 7 || overlay_sel_row == 8 ||
                       overlay_sel_row == 9 || overlay_sel_row == 12) {
                ui_draw_text("Enter:手动输入数值  ←→:预设档位切换", 10, 211, 0xFF88EEFFu);
            } else {
                ui_draw_text("Enter/←→:切换开关  Esc/取消:返回主菜单", 8, 211, 0xFFA4BCE4u);
            }
            ui_draw_text("联机PvP自动禁用 | F6总开关 F7炮 F8遇敌", 4, 227, 0xFF849AC0u);
        } else {
            if (overlay_sel_row == 1) {
                if (live_pvp) {
                    ui_draw_text("当前房间赛制已锁定 (退出房间后可修改)", 6, 211, 0xFF88FFAAu);
                } else {
                    ui_draw_text("←→/Enter:切换 一回战 / 三回战(BO3)", 12, 211, 0xFFFFD874u);
                }
            } else if (overlay_sel_row == 2) {
                ui_draw_text("Enter:手动输入/粘贴地址  ←→:预设切换", 6, 211, 0xFF88EEFFu);
            } else if (overlay_sel_row == 3) {
                ui_draw_text("Enter:输入/粘贴昵称 (自动保存)", 14, 211, 0xFFFFEC88u);
            } else if (overlay_sel_row == ASSIST_ROW_IDX) {
                ui_draw_text("Enter:进入百倍炮/减伤/Boss潜行等设置", 8, 211, 0xFFFFD874u);
            } else if (overlay_sel_row == 0) {
                if (live_pvp) {
                    ui_draw_text("Enter:立即退出房间   F1/Esc:继续对战", 10, 211, 0xFF88FFAAu);
                } else {
                    ui_draw_text("←→:建房/快速配对/选房  Enter:出发!", 12, 211, 0xFFFFE484u);
                }
            } else {
                ui_draw_text("↑↓:选择  ←→:切换  Enter:确认/改键", 9, 211, 0xFFA4BCE4u);
            }
            ui_draw_text("F3帧 F4汉 F5字 F6辅 F7炮 F8遇 F11全屏", 3, 227, 0xFF849AC0u);
        }

        // Text / numeric input modal popup
        if (overlay_text_edit_mode != 0) {
            ui_fill_rect(8, 62, 224, 106, 0xF60C162Cu);
            ui_draw_border(8, 62, 224, 106, 0xFF68B8FFu);
            std::string hdr;
            switch (overlay_text_edit_mode) {
                case 1:  hdr = "输入服务器地址 (IP:端口)"; break;
                case 2:  hdr = "输入联机玩家昵称"; break;
                case 10: hdr = "输入洛克炮倍率 (1 ~ 9999)"; break;
                case 11: hdr = "输入受伤倍率% (0无伤 ~ 500)"; break;
                case 12: hdr = "输入遇敌率% (0~10000)"; break;
                case 13: hdr = "输入EX Boss形态 (-1自动 / 0~3)"; break;
                case 14: hdr = "输入金钱倍率% (0 ~ 10000)"; break;
                case 15: hdr = "输入跑步速度% (50 ~ 500)"; break;
                case 16: hdr = "输入自定义槽速度% (50 ~ 10000)"; break;
                default: hdr = "输入数值"; break;
            }
            ui_draw_text(hdr, 120 - measure_ui_text(hdr) / 2, 82, 0xFFFFE070u);

            ui_fill_rect(16, 94, 208, 24, 0xFF060B18u);
            ui_draw_border(16, 94, 208, 24, 0xFF88D8FFu);
            std::string cursor_txt = overlay_text_edit_buf + "_";
            while (measure_ui_text(cursor_txt) > 198 && cursor_txt.size() > 2) {
                cursor_txt.erase(0, 1);
            }
            ui_draw_text(cursor_txt, 22, 111, 0xFFFFFFFFu);

            std::string h1 = (overlay_text_edit_mode >= 10)
                ? "请直接输入数字 (支持 Ctrl+V 粘贴)"
                : "支持直接打字或 Ctrl+V 粘贴剪贴板";
            std::string h2 = "Enter:保存  Bksp:退格  Esc:取消";
            ui_draw_text(h1, 120 - measure_ui_text(h1) / 2, 136, 0xFF88CCFFu);
            ui_draw_text(h2, 120 - measure_ui_text(h2) / 2, 155, 0xFFA0B8E0u);
        }

        // Waiting for key modal popup
        if (overlay_waiting_bind && overlay_page == 0 &&
            overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW) {
            int bi = overlay_sel_row - BIND_START_ROW;
            ui_fill_rect(12, 72, 216, 86, 0xF4101830u);
            ui_draw_border(12, 72, 216, 86, 0xFFFFD24Cu);
            std::string p1 = std::string("正在设置: ") + binds[bi].label_zh;
            std::string p2 = "请按下键盘按键或手柄按钮...";
            std::string p3 = "Esc:取消   Backspace:清空槽位";
            ui_draw_text(p1, 120 - measure_ui_text(p1) / 2, 95,  0xFFFFE070u);
            ui_draw_text(p2, 120 - measure_ui_text(p2) / 2, 118, 0xFFFFFFFFu);
            ui_draw_text(p3, 120 - measure_ui_text(p3) / 2, 142, 0xFFA0B8E0u);
        }
    }

    // Blocking pause modal used ONLY in single-player (!g_pvp.active()).
    // Returns true if user chose "Save & Quit".
    bool run_pause_config_modal(SDL_Window* win, SDL_Renderer* ren, SDL_Texture* tex_pix, SDL_Texture* tex_hd,
                                const uint32_t* game_fb, const uint32_t* game_pix_text, const uint32_t* game_hd_text,
                                int cur_hd_scale, int scale) {
        if (!win || !ren || (!tex_pix && !tex_hd)) return false;
        auto pause_t0 = std::chrono::steady_clock::now();
        if (g_audio.dev_id != 0) SDL_PauseAudioDevice(g_audio.dev_id, 1);

        open_overlay(true);
        bool quit_game = false;

        while (overlay_open) {
            if (g_quit_signal.load(std::memory_order_relaxed)) {
                quit_game = true;
                close_overlay();
                break;
            }
            poll_async_probe();
            maybe_periodic_room_refresh();
            if (g_pvp.poll_udp()) {
                show_toast("对战配对成功! VS " + g_pvp.opponent_name(), 150);
                close_overlay();
                break;
            }
            if (g_pvp.active() && !g_pvp.match_started() && g_pvp.peer_disconnected()) {
                g_pvp.close_udp();
                show_toast("房间已超时或对手已离开", 150);
            }
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                handle_overlay_event(ev, win, ren, scale, quit_game);
                if (!overlay_open) break;
            }
            if (!overlay_open) break;

            compose_display_buffer(game_fb, game_pix_text, game_hd_text, cur_hd_scale, 0, true);
            present_sdl_frame(ren, tex_pix, tex_hd);
            SDL_Delay(16);
        }

        close_overlay();
        if (g_audio.dev_id != 0) SDL_PauseAudioDevice(g_audio.dev_id, 0);
        auto pause_t1 = std::chrono::steady_clock::now();
        paused_duration_ms += std::chrono::duration_cast<std::chrono::milliseconds>(pause_t1 - pause_t0).count();
        return quit_game;
    }
};

static UiInputManager g_ui;

// ============================================================================
// 60 FPS Display-List Sub-Frame Motion Interpolator (with 60/15 FPS Switch)
// ============================================================================
struct SurfaceTarget {
    uint32_t* gfx = nullptr;
    uint32_t* pix_text = nullptr;
    uint32_t* hd_text = nullptr;
    int w = 0;
    int h = 0;
    int hd_scale = 3;
};

static void attenuate_text_rect(uint32_t* pix_buf, uint32_t* hd_buf, int w, int h, int hd_scale,
                                int rx, int ry, int rw, int rh, uint32_t cover_alpha) {
    if (rw <= 0 || rh <= 0 || cover_alpha == 0u) return;
    int y0 = std::max(0, ry), y1 = std::min(h, ry + rh);
    int x0 = std::max(0, rx), x1 = std::min(w, rx + rw);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (pix_buf) attenuate_pm(pix_buf[y * w + x], cover_alpha);
            if (hd_buf) attenuate_hd_block(hd_buf, w, h, hd_scale, x, y, cover_alpha);
        }
    }
}

static void attenuate_text_border(uint32_t* pix_buf, uint32_t* hd_buf, int w, int h, int hd_scale,
                                  int rx, int ry, int rw, int rh, uint32_t cover_alpha) {
    if (rw <= 0 || rh <= 0 || cover_alpha == 0u) return;
    attenuate_text_rect(pix_buf, hd_buf, w, h, hd_scale, rx, ry, rw, 1, cover_alpha);
    if (rh > 1) attenuate_text_rect(pix_buf, hd_buf, w, h, hd_scale, rx, ry + rh - 1, rw, 1, cover_alpha);
    if (rh > 2) {
        attenuate_text_rect(pix_buf, hd_buf, w, h, hd_scale, rx, ry + 1, 1, rh - 2, cover_alpha);
        if (rw > 1) attenuate_text_rect(pix_buf, hd_buf, w, h, hd_scale, rx + rw - 1, ry + 1, 1, rh - 2, cover_alpha);
    }
}

static void blit_rect_surface(const SurfaceTarget& dst, bool fill,
                              int rx, int ry, int rw, int rh, uint32_t col) {
    if (!dst.gfx || rw <= 0 || rh <= 0) return;
    auto clear_px = [&](int x, int y) {
        if (x < 0 || x >= dst.w || y < 0 || y >= dst.h) return;
        dst.gfx[y * dst.w + x] = col;
        if (dst.pix_text) dst.pix_text[y * dst.w + x] = 0u;
        if (dst.hd_text) clear_hd_block(dst.hd_text, dst.w, dst.h, dst.hd_scale, x, y);
    };
    if (fill) {
        int y0 = std::max(0, ry), y1 = std::min(dst.h, ry + rh);
        int x0 = std::max(0, rx), x1 = std::min(dst.w, rx + rw);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                dst.gfx[y * dst.w + x] = col;
                if (dst.pix_text) dst.pix_text[y * dst.w + x] = 0u;
                if (dst.hd_text) clear_hd_block(dst.hd_text, dst.w, dst.h, dst.hd_scale, x, y);
            }
        }
    } else {
        int x0 = std::max(0, rx), x1 = std::min(dst.w - 1, rx + rw);
        for (int x = x0; x <= x1; ++x) {
            clear_px(x, ry);
            clear_px(x, ry + rh);
        }
        int y0 = std::max(0, ry), y1 = std::min(dst.h - 1, ry + rh);
        for (int y = y0; y <= y1; ++y) {
            clear_px(rx, y);
            clear_px(rx + rw, y);
        }
    }
}

static void blit_image_surface(DojaRuntime& rt, const SurfaceTarget& dst,
                               Ref img_ref, int base_dx, int base_dy,
                               int sx, int sy, int sw, int sh, int dw, int dh, int flip_mode) {
    if (!dst.gfx || img_ref <= 0 || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    HeapObject& img_obj = rt.get(img_ref);
    if (!img_obj.img) return;
    ImageData& img = *img_obj.img;

    const PaletteData* pal = nullptr;
    if (img.is_paletted && img.palette_ref > 0) {
        pal = rt.get(img.palette_ref).pal.get();
    }

    bool flip_h = (flip_mode == 1 || flip_mode == 3);
    bool flip_v = (flip_mode == 2 || flip_mode == 3);
    int S = dst.hd_scale;
    bool src_has_pix = !img.is_paletted &&
                       img.pix_text.size() == static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    bool src_has_hd = !img.is_paletted && S >= 2 &&
                      img.hd_text.size() == static_cast<size_t>(img.width * S) * static_cast<size_t>(img.height * S);

    for (int py = 0; py < dh; ++py) {
        int out_y = base_dy + py;
        if (out_y < 0 || out_y >= dst.h) continue;
        int rel_sy = (py * sh) / dh;
        if (flip_v) rel_sy = sh - 1 - rel_sy;
        int src_y = sy + rel_sy;
        if (src_y < 0 || src_y >= img.height) continue;

        for (int px = 0; px < dw; ++px) {
            int out_x = base_dx + px;
            if (out_x < 0 || out_x >= dst.w) continue;
            int rel_sx = (px * sw) / dw;
            if (flip_h) rel_sx = sw - 1 - rel_sx;
            int src_x = sx + rel_sx;
            if (src_x < 0 || src_x >= img.width) continue;

            bool is_trans = false;
            uint32_t opaque_col = 0xFF000000u;
            if (img.is_paletted) {
                uint8_t idx = img.indices[static_cast<size_t>(src_y) * img.width + src_x];
                if (img.transparent_enabled && idx == img.transparent_index) {
                    is_trans = true;
                } else {
                    uint32_t col = pal ? pal->entries[idx] : 0xFFFF00FFu;
                    opaque_col = col | 0xFF000000u;
                }
            } else {
                uint32_t col = img.argb[static_cast<size_t>(src_y) * img.width + src_x];
                if ((col >> 24) == 0 ||
                    (img.transparent_enabled && ((col & 0xFFFFFFu) == (img.transparent_color & 0xFFFFFFu)))) {
                    is_trans = true;
                } else {
                    opaque_col = col | 0xFF000000u;
                }
            }

            if (!is_trans) {
                dst.gfx[static_cast<size_t>(out_y) * dst.w + out_x] = opaque_col;
                if (dst.pix_text) {
                    dst.pix_text[static_cast<size_t>(out_y) * dst.w + out_x] =
                        src_has_pix ? img.pix_text[static_cast<size_t>(src_y) * img.width + src_x] : 0u;
                }
            } else if (src_has_pix && dst.pix_text) {
                uint32_t spm = img.pix_text[static_cast<size_t>(src_y) * img.width + src_x];
                if (spm != 0u) {
                    blend_pm_pixel_over(dst.pix_text[static_cast<size_t>(out_y) * dst.w + out_x], spm);
                }
            }

            if (dst.hd_text) {
                if (!src_has_hd) {
                    if (!is_trans) {
                        clear_hd_block(dst.hd_text, dst.w, dst.h, S, out_x, out_y);
                    }
                } else {
                    int dst_hw = dst.w * S;
                    int src_hw = img.width * S;
                    int src_hh = img.height * S;
                    for (int sy_sub = 0; sy_sub < S; ++sy_sub) {
                        int dst_hy = out_y * S + sy_sub;
                        int rel_hy = ((py * S + sy_sub) * sh) / dh;
                        if (flip_v) rel_hy = sh * S - 1 - rel_hy;
                        int src_hy = sy * S + rel_hy;
                        if (src_hy < 0 || src_hy >= src_hh) continue;
                        for (int sx_sub = 0; sx_sub < S; ++sx_sub) {
                            int dst_hx = out_x * S + sx_sub;
                            int rel_hx = ((px * S + sx_sub) * sw) / dw;
                            if (flip_h) rel_hx = sw * S - 1 - rel_hx;
                            int src_hx = sx * S + rel_hx;
                            if (src_hx < 0 || src_hx >= src_hw) continue;
                            uint32_t spm = img.hd_text[static_cast<size_t>(src_hy) * src_hw + src_hx];
                            if (!is_trans) {
                                dst.hd_text[static_cast<size_t>(dst_hy) * dst_hw + dst_hx] = spm;
                            } else if (spm != 0u) {
                                blend_pm_pixel_over(dst.hd_text[static_cast<size_t>(dst_hy) * dst_hw + dst_hx], spm);
                            }
                        }
                    }
                }
            }
        }
    }
}

static void blit_text_surface(const SurfaceTarget& dst,
                              const std::string& text, int pen_x, int baseline_y, uint32_t color,
                              bool also_draw_1x_to_gfx) {
    if (dst.pix_text) {
        UiInputManager::render_glyphs_to_buf(dst.pix_text, dst.w, dst.h, text, pen_x, baseline_y, color, true, 0);
    }
    if (dst.hd_text && dst.hd_scale >= 2) {
        int S = dst.hd_scale;
        UiInputManager::render_glyphs_to_buf(dst.hd_text, dst.w * S, dst.h * S, text,
                                             pen_x * S, baseline_y * S, color, true, S);
    }
    if (also_draw_1x_to_gfx && dst.gfx) {
        UiInputManager::render_glyphs_to_buf(dst.gfx, dst.w, dst.h, text, pen_x, baseline_y, color, false, 0);
    }
}

struct FrameInterpolator {
    enum class CmdKind : uint8_t {
        FillRect,
        DrawRect,
        Image,
        Text,
        OP_FB_FILL_RECT_ALPHA,
        OP_FB_DRAW_BORDER,
        OP_FB_TEXT
    };

    struct DrawCmd {
        CmdKind kind = CmdKind::Image;
        Ref img_ref = 0;
        uint32_t img_gen = 0;
        int dx = 0, dy = 0;
        int sx = 0, sy = 0;
        int sw = 0, sh = 0;
        int dw = 0, dh = 0;
        int flip_mode = 0;
        uint32_t color = 0;
        bool shadow = true;
        std::string text;
        int prev_dx = 0, prev_dy = 0;
        bool has_match = false;
    };

    std::unordered_map<Ref, uint32_t> img_gen;
    uint32_t lock_base_fb[240 * 240] = {0};
    uint32_t lock_base_pix_text[240 * 240] = {0};
    std::vector<uint32_t> lock_base_hd_text;
    uint32_t interp_fb[240 * 240] = {0};
    uint32_t interp_pix_text[240 * 240] = {0};
    std::vector<uint32_t> interp_hd_text;
    int hd_scale = 3;

    std::vector<DrawCmd> prev_cmds;
    std::vector<DrawCmd> curr_cmds;
    std::vector<DrawCmd> building_cmds;
    int building_coverage = 0;

    bool capturing = false;
    bool test_capture_override = false;
    bool can_interpolate = false;
    bool any_motion = false;
    int subframes_presented = 4; // 1..4 across ~62.5 ms 15Hz logic tick
    std::chrono::steady_clock::time_point tick_unlock_tp = std::chrono::steady_clock::now();

    void reset() {
        prev_cmds.clear();
        curr_cmds.clear();
        building_cmds.clear();
        building_coverage = 0;
        capturing = false;
        test_capture_override = false;
        can_interpolate = false;
        any_motion = false;
        subframes_presented = 4;
    }

    void mark_offscreen_dirty(Ref target_img) {
        if (target_img > 0) {
            img_gen[target_img]++;
        }
    }

    uint32_t get_img_gen(Ref r) const {
        auto it = img_gen.find(r);
        return (it != img_gen.end()) ? it->second : 0u;
    }

    void ensure_hd_buffers(int s) {
        hd_scale = std::clamp(s, 2, 4);
        size_t need = static_cast<size_t>(240 * hd_scale) * static_cast<size_t>(240 * hd_scale);
        if (lock_base_hd_text.size() != need) lock_base_hd_text.assign(need, 0u);
        if (interp_hd_text.size() != need) interp_hd_text.assign(need, 0u);
    }

    void copy_lock_base(const uint32_t* fb, const uint32_t* pix_text, const uint32_t* hd_text, int s) {
        ensure_hd_buffers(s);
        std::memcpy(lock_base_fb, fb, 240 * 240 * sizeof(uint32_t));
        if (pix_text) {
            std::memcpy(lock_base_pix_text, pix_text, 240 * 240 * sizeof(uint32_t));
        } else {
            std::fill(std::begin(lock_base_pix_text), std::end(lock_base_pix_text), 0u);
        }
        if (hd_text) {
            std::memcpy(lock_base_hd_text.data(), hd_text, lock_base_hd_text.size() * sizeof(uint32_t));
        } else {
            std::fill(lock_base_hd_text.begin(), lock_base_hd_text.end(), 0u);
        }
    }

    void on_screen_lock(const uint32_t* current_fb, const uint32_t* current_pix_text,
                        const uint32_t* current_hd_text, int s) {
        copy_lock_base(current_fb, current_pix_text, current_hd_text, s);
        building_cmds.clear();
        building_coverage = 0;
        capturing = true;
    }

    void add_coverage(int dx, int dy, int dw, int dh) {
        int x0 = std::clamp(dx, 0, 240), x1 = std::clamp(dx + dw, 0, 240);
        int y0 = std::clamp(dy, 0, 240), y1 = std::clamp(dy + dh, 0, 240);
        if (x1 > x0 && y1 > y0) {
            building_coverage += (x1 - x0) * (y1 - y0);
        }
    }

    void record_rect(bool fill, int rx, int ry, int rw, int rh, uint32_t col) {
        if (building_cmds.size() >= 2048) return;
        DrawCmd c{};
        c.kind = fill ? CmdKind::FillRect : CmdKind::DrawRect;
        c.dx = rx; c.dy = ry; c.dw = rw; c.dh = rh; c.color = col;
        building_cmds.push_back(std::move(c));
        if (fill) add_coverage(rx, ry, rw, rh);
    }

    void record_image(Ref img_ref, int dx, int dy, int sx, int sy, int sw, int sh, int dw, int dh, int flip) {
        if (building_cmds.size() >= 2048) return;
        DrawCmd c{};
        c.kind = CmdKind::Image;
        c.img_ref = img_ref;
        c.img_gen = get_img_gen(img_ref);
        c.dx = dx; c.dy = dy;
        c.sx = sx; c.sy = sy; c.sw = sw; c.sh = sh;
        c.dw = dw; c.dh = dh;
        c.flip_mode = flip;
        building_cmds.push_back(std::move(c));
        add_coverage(dx, dy, dw, dh);
    }

    void record_text(const std::string& txt, int px, int py, uint32_t col) {
        if (building_cmds.size() >= 2048) return;
        DrawCmd c{};
        c.kind = CmdKind::Text;
        c.dx = px; c.dy = py; c.color = col; c.text = txt;
        building_cmds.push_back(std::move(c));
    }

    void record_fb_rect_alpha(int rx, int ry, int rw, int rh, uint32_t argb) {
        if (!capturing || building_cmds.size() >= 2048) return;
        DrawCmd c{};
        c.kind = CmdKind::OP_FB_FILL_RECT_ALPHA;
        c.dx = rx; c.dy = ry; c.dw = rw; c.dh = rh; c.color = argb;
        building_cmds.push_back(std::move(c));
    }

    void record_fb_border(int rx, int ry, int rw, int rh, uint32_t argb) {
        if (!capturing || building_cmds.size() >= 2048) return;
        DrawCmd c{};
        c.kind = CmdKind::OP_FB_DRAW_BORDER;
        c.dx = rx; c.dy = ry; c.dw = rw; c.dh = rh; c.color = argb;
        building_cmds.push_back(std::move(c));
    }

    void record_fb_text(const std::string& txt, int px, int baseline_y, uint32_t col, bool shadow) {
        if (!capturing || building_cmds.size() >= 2048) return;
        DrawCmd c{};
        c.kind = CmdKind::OP_FB_TEXT;
        c.dx = px; c.dy = baseline_y; c.color = col; c.shadow = shadow; c.text = txt;
        building_cmds.push_back(std::move(c));
    }

    void on_screen_unlock(const uint32_t* final_fb, const uint32_t* final_pix_text,
                          const uint32_t* final_hd_text, int s, bool enable_interp) {
        capturing = false;
        prev_cmds = std::move(curr_cmds);
        curr_cmds = std::move(building_cmds);
        building_cmds.clear();
        int cov = building_coverage;
        building_coverage = 0;

        tick_unlock_tp = std::chrono::steady_clock::now();
        can_interpolate = false;
        any_motion = false;
        subframes_presented = 4;

        if (!enable_interp || prev_cmds.empty() || curr_cmds.empty()) {
            copy_lock_base(final_fb, final_pix_text, final_hd_text, s);
            return;
        }
        // Only interpolate full-viewport scenes (Overworld, Battle, Menus, Title) so tiny
        // incremental wipe patches never leave trails on uncleared backgrounds
        if (cov < 240 * 160) {
            copy_lock_base(final_fb, final_pix_text, final_hd_text, s);
            return;
        }
        int n_prev = static_cast<int>(prev_cmds.size());
        int n_curr = static_cast<int>(curr_cmds.size());
        if (std::abs(n_curr - n_prev) > std::max(16, (n_curr * 3) / 5)) {
            copy_lock_base(final_fb, final_pix_text, final_hd_text, s);
            return;
        }

        std::vector<uint8_t> used_prev(static_cast<size_t>(n_prev), 0);
        int matched_count = 0;

        auto same_primitive = [](const DrawCmd& c, const DrawCmd& p) -> bool {
            if (c.kind != p.kind) return false;
            if (c.kind == CmdKind::Image) {
                return c.img_ref == p.img_ref && c.img_gen == p.img_gen &&
                       c.sx == p.sx && c.sy == p.sy && c.sw == p.sw && c.sh == p.sh &&
                       c.dw == p.dw && c.dh == p.dh && c.flip_mode == p.flip_mode;
            }
            if (c.kind == CmdKind::FillRect || c.kind == CmdKind::DrawRect ||
                c.kind == CmdKind::OP_FB_FILL_RECT_ALPHA || c.kind == CmdKind::OP_FB_DRAW_BORDER) {
                return c.dw == p.dw && c.dh == p.dh && c.color == p.color;
            }
            if (c.kind == CmdKind::OP_FB_TEXT) {
                return c.color == p.color && c.shadow == p.shadow && c.text == p.text;
            }
            return c.color == p.color && c.text == p.text;
        };

        // Detect static full-screen backdrop scenes (e.g. BattleMain 6x3 grid at 240x240)
        // vs scrolling overworld camera scenes (MapDraw imgMap2 quadrants)
        const DrawCmd& c0 = curr_cmds[0];
        const DrawCmd& p0 = prev_cmds[0];
        bool is_static_backdrop = (c0.kind == CmdKind::Image && p0.kind == CmdKind::Image &&
                                   c0.img_ref == p0.img_ref && c0.img_gen == p0.img_gen &&
                                   c0.dx == 0 && c0.dy == 0 && p0.dx == 0 && p0.dy == 0 &&
                                   c0.dw >= 240 && c0.dh >= 240);

        // Pass 0: Lock stationary primitives first (dx == prev_dx && dy == prev_dy) so newly
        // spawned battle warp silhouettes (Attack 59) or shots never steal stationary slots
        for (int i = 0; i < n_curr; ++i) {
            DrawCmd& c = curr_cmds[i];
            int best_j = -1;
            int best_dist = 999999;
            for (int j = 0; j < n_prev; ++j) {
                if (used_prev[j]) continue;
                const DrawCmd& p = prev_cmds[j];
                if (c.dx != p.dx || c.dy != p.dy || !same_primitive(c, p)) continue;
                int idx_dist = std::abs(i - j);
                if (idx_dist < best_dist) {
                    best_dist = idx_dist;
                    best_j = j;
                    if (idx_dist == 0) break;
                }
            }
            if (best_j >= 0) {
                used_prev[best_j] = 1;
                c.prev_dx = c.dx;
                c.prev_dy = c.dy;
                c.has_match = true;
                matched_count++;
            }
        }

        // Pass 1: Continuous translation match (overworld scrolling 4..8 px, projectiles 3..15 px).
        // Kept strictly below 6x3 battle grid panel spacing (BT_SIZE_X = 40 px, BT_SIZE_Y = 24 px)
        // so player & enemy panel warps snap cleanly instead of drifting across panel borders.
        for (int i = 0; i < n_curr; ++i) {
            DrawCmd& c = curr_cmds[i];
            if (c.has_match ||
                c.kind == CmdKind::OP_FB_FILL_RECT_ALPHA ||
                c.kind == CmdKind::OP_FB_DRAW_BORDER ||
                c.kind == CmdKind::OP_FB_TEXT) {
                continue;
            }
            int best_j = -1;
            int best_score = 999999;
            for (int j = 0; j < n_prev; ++j) {
                if (used_prev[j]) continue;
                const DrawCmd& p = prev_cmds[j];
                if (!same_primitive(c, p)) continue;
                int adx = std::abs(c.dx - p.dx);
                int ady = std::abs(c.dy - p.dy);
                if (adx > 16 || ady > 15) continue;
                int score = (adx + ady) * 16 + std::min(15, std::abs(i - j));
                if (score < best_score) {
                    best_score = score;
                    best_j = j;
                }
            }
            if (best_j >= 0) {
                used_prev[best_j] = 1;
                c.prev_dx = prev_cmds[best_j].dx;
                c.prev_dy = prev_cmds[best_j].dy;
                c.has_match = true;
                matched_count++;
            }
        }

        // Pass 2: Overworld camera-scrolling walk cel match ONLY (!is_static_backdrop).
        // Disabled on static-backdrop battle scenes so multi-tile OAM characters never cross-match
        // head/leg pieces or smear discrete attack/warp cel transitions.
        if (!is_static_backdrop) {
            for (int i = 0; i < n_curr; ++i) {
                DrawCmd& c = curr_cmds[i];
                if (c.has_match || c.kind != CmdKind::Image || c.dw > 64 || c.dh > 64) continue;
                int best_j = -1;
                int best_score = 999999;
                for (int j = 0; j < n_prev; ++j) {
                    if (used_prev[j]) continue;
                    const DrawCmd& p = prev_cmds[j];
                    if (p.kind != CmdKind::Image || c.img_ref != p.img_ref || c.img_gen != p.img_gen ||
                        c.sw != p.sw || c.sh != p.sh || c.dw != p.dw || c.dh != p.dh) {
                        continue;
                    }
                    int adx = std::abs(c.dx - p.dx);
                    int ady = std::abs(c.dy - p.dy);
                    if (adx > 10 || ady > 6) continue;
                    int score = (adx + ady) * 16 + std::min(15, std::abs(i - j));
                    if (score < best_score) {
                        best_score = score;
                        best_j = j;
                    }
                }
                if (best_j >= 0) {
                    used_prev[best_j] = 1;
                    c.prev_dx = prev_cmds[best_j].dx;
                    c.prev_dy = prev_cmds[best_j].dy;
                    c.has_match = true;
                    matched_count++;
                }
            }
        }

        // Composite sprite rigidity guard on static-backdrop (battle) scenes:
        // If one sub-tile of a multi-part character swapped cel/warped (!has_match) or moved by a
        // different delta, snap reused sub-tiles (e.g. helmet/feet) together so poses stay crisp.
        if (is_static_backdrop) {
            for (int i = 0; i < n_curr; ++i) {
                DrawCmd& c = curr_cmds[i];
                if (!c.has_match || c.kind != CmdKind::Image || c.dw > 64 || c.dh > 64) continue;
                int vx = c.dx - c.prev_dx;
                int vy = c.dy - c.prev_dy;
                if (vx == 0 && vy == 0) continue;
                for (int k : {i - 1, i + 1}) {
                    if (k < 0 || k >= n_curr) continue;
                    const DrawCmd& nb = curr_cmds[k];
                    if (nb.kind != CmdKind::Image || nb.img_ref != c.img_ref || nb.dw > 64 || nb.dh > 64) continue;
                    if (std::abs(nb.dx - c.dx) <= 32 && std::abs(nb.dy - c.dy) <= 32) {
                        if (!nb.has_match || (nb.dx - nb.prev_dx) != vx || (nb.dy - nb.prev_dy) != vy) {
                            c.prev_dx = c.dx;
                            c.prev_dy = c.dy;
                            break;
                        }
                    }
                }
            }
        }

        for (const DrawCmd& c : curr_cmds) {
            if (c.has_match && (c.prev_dx != c.dx || c.prev_dy != c.dy)) {
                any_motion = true;
                break;
            }
        }

        if (matched_count * 5 >= n_curr * 2) {
            can_interpolate = true;
            subframes_presented = 1; // Sub-frame 1 (alpha = 0.25) presented on unlock
        }
    }

    void rasterize_subframe(DojaRuntime& rt, float alpha) {
        ensure_hd_buffers(hd_scale);
        std::memcpy(interp_fb, lock_base_fb, 240 * 240 * sizeof(uint32_t));
        std::memcpy(interp_pix_text, lock_base_pix_text, 240 * 240 * sizeof(uint32_t));
        std::memcpy(interp_hd_text.data(), lock_base_hd_text.data(), interp_hd_text.size() * sizeof(uint32_t));
        SurfaceTarget dst{interp_fb, interp_pix_text, interp_hd_text.data(), 240, 240, hd_scale};
        bool draw_1x_to_gfx = rt.should_bake_1x_text_into_gfx();

        for (const DrawCmd& c : curr_cmds) {
            int dx = c.dx;
            int dy = c.dy;
            if (c.has_match && (c.prev_dx != c.dx || c.prev_dy != c.dy)) {
                // Shift-invariant rounding prevents 1px seams across scrolling tile quadrants
                dx = c.prev_dx + static_cast<int>(std::floor(static_cast<float>(c.dx - c.prev_dx) * alpha + 0.5f));
                dy = c.prev_dy + static_cast<int>(std::floor(static_cast<float>(c.dy - c.prev_dy) * alpha + 0.5f));
            }
            switch (c.kind) {
                case CmdKind::FillRect:
                    blit_rect_surface(dst, true, dx, dy, c.dw, c.dh, c.color);
                    break;
                case CmdKind::DrawRect:
                    blit_rect_surface(dst, false, dx, dy, c.dw, c.dh, c.color);
                    break;
                case CmdKind::Image:
                    blit_image_surface(rt, dst, c.img_ref, dx, dy,
                                       c.sx, c.sy, c.sw, c.sh, c.dw, c.dh, c.flip_mode);
                    break;
                case CmdKind::Text:
                    blit_text_surface(dst, c.text, dx, dy, c.color, draw_1x_to_gfx);
                    break;
                case CmdKind::OP_FB_FILL_RECT_ALPHA: {
                    UiInputManager::fill_rect_alpha(interp_fb, 240, 240, dx, dy, c.dw, c.dh, c.color);
                    uint32_t ca = (c.color >> 24) & 0xFFu;
                    attenuate_text_rect(interp_pix_text, interp_hd_text.data(), 240, 240, hd_scale, dx, dy, c.dw, c.dh, ca);
                    break;
                }
                case CmdKind::OP_FB_DRAW_BORDER: {
                    UiInputManager::draw_rect_border(interp_fb, 240, 240, dx, dy, c.dw, c.dh, c.color);
                    uint32_t ca = (c.color >> 24) & 0xFFu;
                    attenuate_text_border(interp_pix_text, interp_hd_text.data(), 240, 240, hd_scale, dx, dy, c.dw, c.dh, ca);
                    break;
                }
                case CmdKind::OP_FB_TEXT:
                    UiInputManager::draw_ui_text_pm(interp_pix_text, interp_hd_text.data(), 240, 240, hd_scale,
                                                    c.text, dx, dy, c.color, c.shadow);
                    if (draw_1x_to_gfx) {
                        UiInputManager::draw_ui_text(interp_fb, 240, 240, c.text, dx, dy, c.color, c.shadow);
                    }
                    break;
            }
        }
    }
};

static FrameInterpolator g_interp;

} // namespace

// ============================================================================
// DojaRuntime implementation
// ============================================================================

DojaRuntime::DojaRuntime() {
    g_runtime = this;
    heap_.reserve(65536);
    audio_presenters_.reserve(kPermanentAudioPresenters + kDynamicAudioPresenterRing);
    // Index 0 is null
    heap_.push_back(HeapObject{});
    // Index 1 is shared Exception / System.out placeholder
    HeapObject exc{};
    exc.kind = ObjKind::ExceptionObj;
    heap_.push_back(exc);
}

Ref DojaRuntime::audio_presenter_sound_ref(Ref ap_ref) const {
    if ((ap_ref & 0x70000000) != kAudioPresenterTag) return 0;
    size_t idx = static_cast<size_t>(ap_ref & 0x0FFFFFFF);
    if (idx >= audio_presenters_.size()) return 0;
    return audio_presenters_[idx].sound_ref;
}

int DojaRuntime::last_played_port_sound_id(int port) const {
    if (port < 0 || port >= MldAudioEngine::NUM_PORTS) return -1;
    std::lock_guard<std::mutex> lk(g_audio.mtx);
    return g_audio.ports[port].sound_id;
}

DojaRuntime::~DojaRuntime() {
    shutdown();
}

void set_network_server(const std::string& host_port, bool enable_online) {
    g_ui.has_cli_net_override = true;
    g_ui.cli_online_mode = enable_online;
    if (!host_port.empty()) {
        g_ui.cli_server_url = host_port;
    }
}

void set_player_identity(const std::string& uid, const std::string& name) {
    if (!uid.empty()) g_ui.cli_player_uid = uid;
    if (!name.empty()) g_ui.cli_player_name = name;
}

void set_custom_sav_path(const std::string& sav_path) {
    g_ui.custom_sav_path = sav_path;
}

void set_cli_fps_override(int fps) {
    g_ui.has_cli_fps_override = true;
    g_ui.cli_high_fps_mode = (fps >= 30);
}

void set_cli_pvp_action(int mode, uint32_t room_id) {
    g_ui.cli_pvp_mode = mode;
    g_ui.cli_pvp_room_id = room_id;
    g_ui.cli_pvp_join_retries = 0;
}

void set_cli_lang_mode(const std::string& mode) {
    g_ui.has_cli_lang_override = true;
    g_ui.cli_lang_mode = UiInputManager::parse_lang_cfg(mode);
}

void set_cli_translation_path(const std::string& path) {
    g_ui.cli_translation_path = path;
}

void set_cli_font_mode(int mode) {
    if (mode == 0 || mode == 1) {
        g_ui.has_cli_font_mode = true;
        g_ui.cli_hd_font_mode = (mode != 0);
    }
}

void set_cli_font_paths(const std::string& pixel_font, const std::string& hd_font) {
    if (!pixel_font.empty()) g_ft.cli_pixel_font_path = pixel_font;
    if (!hd_font.empty()) g_ft.cli_hd_font_path = hd_font;
}

void set_cli_pvp_bo3(bool v) {
    g_ui.has_cli_pvp_bo3 = true;
    g_ui.cli_pvp_bo3 = v;
    if (v) {
        g_ui.pvp_bo3_mode = true;
        g_pvp.set_preferred_format(pvp::PVP_FORMAT_TRIPLE);
    } else {
        g_ui.pvp_bo3_mode = false;
        g_pvp.set_preferred_format(pvp::PVP_FORMAT_SINGLE);
    }
}

void set_cli_assist_buster_mult(int32_t mult) { g_ui.has_cli_assist_buster = true; g_ui.cli_assist_cfg.buster_mult = mult; }
void set_cli_assist_damage_pct(int32_t pct) { g_ui.has_cli_assist_damage = true; g_ui.cli_assist_cfg.damage_taken_pct = pct; }
void set_cli_assist_encounter_pct(int32_t pct) { g_ui.has_cli_assist_encounter = true; g_ui.cli_assist_cfg.encounter_rate_pct = pct; }
void set_cli_assist_wild_boss_only(bool en) { g_ui.has_cli_assist_wild_boss = true; g_ui.cli_assist_cfg.wild_boss_only = en; }
void set_cli_assist_show_ex_boss(int32_t mode) { g_ui.has_cli_assist_show_ex_boss = true; g_ui.cli_assist_cfg.show_ex_boss = mode; }
void set_cli_assist_ex_boss_tier(int32_t tier) { g_ui.has_cli_assist_ex_boss_tier = true; g_ui.cli_assist_cfg.ex_boss_tier = tier; }
void set_cli_assist_zenny_pct(int32_t pct) { g_ui.has_cli_assist_zenny = true; g_ui.cli_assist_cfg.zenny_rate_pct = pct; }
void set_cli_assist_run_speed_pct(int32_t pct) { g_ui.has_cli_assist_run_speed = true; g_ui.cli_assist_cfg.run_speed_pct = pct; }
void set_cli_assist_auto_full_hp(bool en) { g_ui.has_cli_assist_auto_full_hp = true; g_ui.cli_assist_cfg.auto_full_hp = en; }
void set_cli_assist_always_escape(bool en) { g_ui.has_cli_assist_always_escape = true; g_ui.cli_assist_cfg.always_escape = en; }
void set_cli_assist_custom_gauge_pct(int32_t pct) { g_ui.has_cli_assist_custom_gauge = true; g_ui.cli_assist_cfg.custom_gauge_pct = pct; }
void set_cli_assist_drop_rank_mode(int32_t mode) { g_ui.has_cli_assist_drop_rank = true; g_ui.cli_assist_cfg.drop_rank_mode = mode; }

const AssistConfig& DojaRuntime::assist_config() const { return g_ui.assist_cfg; }
AssistConfig& DojaRuntime::mutable_assist_config() { return g_ui.assist_cfg; }
bool DojaRuntime::assist_hold_key_down() const { return g_ui.assist_hold_down; }
void DojaRuntime::set_assist_hud_status(const AssistHudStatus& st) { g_ui.assist_hud = st; }
std::string DojaRuntime::decode_game_bytes_to_utf8(const uint8_t* data, size_t len) const {
    if (!data || len == 0) return "";
    if (g_loc.has_loaded_pack()) {
        return g_loc.decode_bytes_to_utf8(data, len, cp932_to_utf8);
    }
    return cp932_to_utf8(data, len);
}

pvp::PvpClientSession& DojaRuntime::pvp_session() {
    return g_pvp;
}

std::string DojaRuntime::player_nickname() const {
    return g_ui.player_name;
}

int DojaRuntime::take_cli_pvp_action(uint32_t& room_id_out) {
    if (g_ui.cli_pvp_mode == 0) return 0;
    std::string gm = g_ui.is_lon_game ? "lon" : "pon";
    std::string pvp_uid = g_ui.effective_pvp_uid();
    g_ui.online_mode = true;
    if (g_ui.cli_pvp_mode == 1) {
        g_ui.cli_pvp_mode = 0;
        bool use_bo3 = g_ui.has_cli_pvp_bo3 ? g_ui.cli_pvp_bo3 : g_ui.pvp_bo3_mode;
        g_pvp.set_preferred_format(use_bo3 ? pvp::PVP_FORMAT_TRIPLE : pvp::PVP_FORMAT_SINGLE);
        if (g_pvp.create_room(g_ui.server_url, pvp_uid, g_ui.player_name, gm)) {
            room_id_out = g_pvp.room_id();
            std::string fmt_tag = g_pvp.is_triple_battle() ? " [三回战]" : "";
            g_ui.show_toast("已创建房间 #" + std::to_string(room_id_out) + fmt_tag + " 等待加入...", 180);
            return 1;
        }
        return -1;
    }
    if (g_ui.cli_pvp_mode == 2) {
        if (g_pvp.join_room(g_ui.server_url, g_ui.cli_pvp_room_id, pvp_uid, g_ui.player_name, gm)) {
            g_ui.cli_pvp_mode = 0;
            room_id_out = g_pvp.room_id();
            g_ui.show_toast("已加入 #" + std::to_string(room_id_out) + " VS " + g_pvp.opponent_name(), 150);
            return 2;
        }
        if (++g_ui.cli_pvp_join_retries > 80) {
            g_ui.cli_pvp_mode = 0;
            return -1;
        }
        return 0;
    }
    g_ui.cli_pvp_mode = 0;
    return 0;
}

int32_t DojaRuntime::poll_local_pvp_keypad() {
    poll_events();
    if (g_ui.overlay_open) {
        return 0; // Keep local Navi neutral while player navigates the non-blocking F1 overlay!
    }
    int32_t mask = keypad_state_;
    int target_f = frame_count_ + 1;
    for (const auto& kv : injected_keys_) {
        if (kv.doja_key >= 0 && kv.doja_key < 31 && target_f >= kv.start_frame && target_f <= kv.end_frame) {
            mask |= (1 << kv.doja_key);
        }
    }
    return mask;
}

void DojaRuntime::save_full_snapshot(FullHeapSnapshot& out) const {
    out.heap = heap_;
    out.palettes.clear();
    out.offscreen_argb.clear();
    out.offscreen_pix_text.clear();
    out.offscreen_hd_text.clear();
    out.paletted_image_refs.clear();
    for (size_t i = 1; i < heap_.size(); ++i) {
        const HeapObject& o = heap_[i];
        if (o.kind == ObjKind::Palette && o.pal) {
            out.palettes.emplace_back(static_cast<Ref>(i), *o.pal);
        } else if (o.kind == ObjKind::Image && o.img && !o.img->argb.empty()) {
            out.offscreen_argb.emplace_back(static_cast<Ref>(i), o.img->argb);
            if (!o.img->pix_text.empty()) {
                out.offscreen_pix_text.emplace_back(static_cast<Ref>(i), o.img->pix_text);
            }
            if (!o.img->hd_text.empty()) {
                out.offscreen_hd_text.emplace_back(static_cast<Ref>(i), o.img->hd_text);
            }
        } else if (o.kind == ObjKind::PalettedImage && o.img) {
            out.paletted_image_refs.emplace_back(static_cast<Ref>(i), o.img->palette_ref);
        }
    }
}

void DojaRuntime::restore_full_snapshot(const FullHeapSnapshot& in) {
    heap_ = in.heap;
    for (const auto& p : in.palettes) {
        if (p.first > 0 && static_cast<size_t>(p.first) < heap_.size() && heap_[p.first].pal) {
            *heap_[p.first].pal = p.second;
        }
    }
    for (const auto& im : in.offscreen_argb) {
        if (im.first > 0 && static_cast<size_t>(im.first) < heap_.size() && heap_[im.first].img) {
            heap_[im.first].img->argb = im.second;
            heap_[im.first].img->pix_text.clear();
            heap_[im.first].img->hd_text.clear();
        }
    }
    for (const auto& pt : in.offscreen_pix_text) {
        if (pt.first > 0 && static_cast<size_t>(pt.first) < heap_.size() && heap_[pt.first].img) {
            heap_[pt.first].img->pix_text = pt.second;
        }
    }
    for (const auto& ht : in.offscreen_hd_text) {
        if (ht.first > 0 && static_cast<size_t>(ht.first) < heap_.size() && heap_[ht.first].img) {
            heap_[ht.first].img->hd_text = ht.second;
        }
    }
    for (const auto& pr : in.paletted_image_refs) {
        if (pr.first > 0 && static_cast<size_t>(pr.first) < heap_.size() && heap_[pr.first].img) {
            heap_[pr.first].img->palette_ref = pr.second;
        }
    }
    std::fill(std::begin(fb_pix_text_), std::end(fb_pix_text_), 0u);
    std::fill(fb_hd_text_.begin(), fb_hd_text_.end(), 0u);
    g_interp.reset();
}

void DojaRuntime::save_combat_snapshot(FastCombatSnapshot& out, size_t min_ref) const {
    if (out.base_heap_size == 0) {
        out.base_heap_size = heap_.size();
    }
    size_t base_lim = std::min(out.base_heap_size, heap_.size());
    out.states.clear();
    out.paletted_image_refs.clear();
    for (size_t i = std::max<size_t>(1, min_ref); i < base_lim; ++i) {
        const HeapObject& o = heap_[i];
        if (o.kind == ObjKind::ClassInstance ||
            o.kind == ObjKind::IntArray ||
            o.kind == ObjKind::RefArray ||
            o.kind == ObjKind::Random) {
            FastCombatObjState st;
            st.ref = static_cast<Ref>(i);
            st.fields_i32 = o.fields_i32;
            st.fields_i64 = o.fields_i64;
            st.rng_seed = o.rng_seed;
            out.states.push_back(std::move(st));
        } else if (o.kind == ObjKind::PalettedImage && o.img) {
            out.paletted_image_refs.emplace_back(static_cast<Ref>(i), o.img->palette_ref);
        }
    }
    out.extra_objs.clear();
    if (heap_.size() > base_lim) {
        out.extra_objs.assign(heap_.begin() + static_cast<ptrdiff_t>(base_lim), heap_.end());
    }
}

void DojaRuntime::restore_combat_snapshot(const FastCombatSnapshot& in) {
    if (in.base_heap_size > 0 && heap_.size() > in.base_heap_size) {
        heap_.resize(in.base_heap_size);
    }
    for (const auto& st : in.states) {
        if (st.ref > 0 && static_cast<size_t>(st.ref) < heap_.size()) {
            HeapObject& o = heap_[st.ref];
            o.fields_i32 = st.fields_i32;
            o.fields_i64 = st.fields_i64;
            o.rng_seed = st.rng_seed;
        }
    }
    if (!in.extra_objs.empty()) {
        heap_.insert(heap_.end(), in.extra_objs.begin(), in.extra_objs.end());
    }
    for (const auto& pr : in.paletted_image_refs) {
        if (pr.first > 0 && static_cast<size_t>(pr.first) < heap_.size() && heap_[pr.first].img) {
            heap_[pr.first].img->palette_ref = pr.second;
        }
    }
}

void DojaRuntime::reset_interpolator() {
    g_interp.reset();
}

uint32_t DojaRuntime::interp_test_img_gen(Ref img_ref) const {
    return g_interp.get_img_gen(img_ref);
}

void DojaRuntime::interp_test_begin_frame() {
    g_interp.test_capture_override = true;
    g_interp.on_screen_lock(framebuffer_, fb_pix_text_, fb_hd_text_.data(), hd_scale_);
}

bool DojaRuntime::interp_test_end_and_rasterize(float alpha, uint32_t* out_fb_240x240) {
    g_interp.on_screen_unlock(framebuffer_, fb_pix_text_, fb_hd_text_.data(), hd_scale_, true);
    g_interp.test_capture_override = false;
    if (!g_interp.can_interpolate) return false;
    g_interp.rasterize_subframe(*this, alpha);
    if (out_fb_240x240) {
        for (int i = 0; i < 240 * 240; ++i) {
            uint32_t pm = g_interp.interp_pix_text[i];
            out_fb_240x240[i] = (pm != 0u) ? composite_pm_over_opaque(g_interp.interp_fb[i], pm)
                                           : (g_interp.interp_fb[i] | 0xFF000000u);
        }
    }
    return true;
}

void DojaRuntime::fb_fill_rect_alpha(int rx, int ry, int rw, int rh, uint32_t argb) {
    UiInputManager::fill_rect_alpha(framebuffer_, 240, 240, rx, ry, rw, rh, argb);
    uint32_t ca = (argb >> 24) & 0xFFu;
    attenuate_text_rect(fb_pix_text_, fb_hd_text_.data(), 240, 240, hd_scale_, rx, ry, rw, rh, ca);
    if (g_interp.capturing && (g_interp.test_capture_override || (g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_))) {
        g_interp.record_fb_rect_alpha(rx, ry, rw, rh, argb);
    }
}

void DojaRuntime::fb_draw_border(int rx, int ry, int rw, int rh, uint32_t argb) {
    UiInputManager::draw_rect_border(framebuffer_, 240, 240, rx, ry, rw, rh, argb);
    uint32_t ca = (argb >> 24) & 0xFFu;
    attenuate_text_border(fb_pix_text_, fb_hd_text_.data(), 240, 240, hd_scale_, rx, ry, rw, rh, ca);
    if (g_interp.capturing && (g_interp.test_capture_override || (g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_))) {
        g_interp.record_fb_border(rx, ry, rw, rh, argb);
    }
}

int DojaRuntime::fb_measure_text(const std::string& utf8) const {
    return g_ui.measure_ui_text(utf8);
}

void DojaRuntime::fb_draw_text(const std::string& utf8, int x, int baseline_y, uint32_t color, bool shadow) {
    UiInputManager::draw_ui_text_pm(fb_pix_text_, fb_hd_text_.data(), 240, 240, hd_scale_,
                                    utf8, x, baseline_y, color, shadow);
    if (should_bake_1x_text_into_gfx()) {
        UiInputManager::draw_ui_text(framebuffer_, 240, 240, utf8, x, baseline_y, color, shadow);
    }
    if (g_interp.capturing && (g_interp.test_capture_override || (g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_))) {
        g_interp.record_fb_text(utf8, x, baseline_y, color, shadow);
    }
}

void DojaRuntime::show_ui_toast(const std::string& msg, int frames) {
    g_ui.show_toast(msg, frames);
}

void DojaRuntime::set_config_path(const std::string& path) {
    if (!path.empty()) {
        g_ui.config_path = path;
    }
}

void DojaRuntime::set_touch_controls(bool enabled) {
    g_ui.touch.set_enabled(enabled);
}

bool DojaRuntime::init(const std::string& jar_path, const std::string& sp_path, const std::string& /*jam_path*/, int scale, const std::string& window_title) {
#if defined(__ANDROID__) || defined(__IPHONEOS__)
    g_ui.touch.set_enabled(true);
#endif
    std::string base_dir = get_exe_base_dir();
#ifdef __ANDROID__
    if (data_dir_.empty()) {
        const char* ext = SDL_AndroidGetExternalStoragePath();
        if (ext && *ext) {
            data_dir_ = ext;
        } else if (const char* intr = SDL_AndroidGetInternalStoragePath(); intr && *intr) {
            data_dir_ = intr;
        }
    }
    if (asset_dir_.empty() && !data_dir_.empty()) {
        asset_dir_ = data_dir_;
    }
#elif defined(__IPHONEOS__)
    if (data_dir_.empty()) {
        if (char* pref = SDL_GetPrefPath("rmexe", "recomp")) {
            data_dir_ = pref;
            SDL_free(pref);
        }
    }
#endif
    std::string search_base = !asset_dir_.empty() ? asset_dir_ : base_dir;
    g_ui.exe_base_dir = base_dir;
    g_ui.asset_dir = asset_dir_;
    g_ui.data_dir = data_dir_;
    jar_path_ = resolve_runtime_path(jar_path, search_base);
    sp_path_ = resolve_runtime_path(sp_path, search_base);
    if (!g_ui.custom_sav_path.empty()) {
        sav_path_ = g_ui.custom_sav_path;
    } else if (!data_dir_.empty()) {
        std::filesystem::path sp_p(sp_path_);
        sav_path_ = (std::filesystem::path(data_dir_) / (sp_p.filename().string() + ".sav")).string();
    } else {
        sav_path_ = sp_path_ + ".sav";
    }
    if (g_ui.config_path == "controls.cfg") {
        if (!data_dir_.empty()) {
            g_ui.config_path = (std::filesystem::path(data_dir_) / "controls.cfg").string();
        } else if (!std::filesystem::exists(g_ui.config_path) && !base_dir.empty()) {
            g_ui.config_path = (std::filesystem::path(base_dir) / "controls.cfg").string();
        }
    }
    window_scale_ = std::max(1, scale);
    hd_scale_ = (scale >= 2) ? std::clamp(scale, 2, 4) : 3;
    g_ui.hd_scale = hd_scale_;
    fb_hd_text_.assign(static_cast<size_t>(240 * hd_scale_) * static_cast<size_t>(240 * hd_scale_), 0u);
    std::fill(std::begin(fb_pix_text_), std::end(fb_pix_text_), 0u);

    g_ft.init(base_dir, asset_dir_);

    // Load ScratchPad (.sp) in memory (Copy-on-Write)
    std::ifstream sp_file(sp_path_, std::ios::binary);
    if (!sp_file) {
        std::cerr << "Failed to open ScratchPad file: " << sp_path_ << "\n";
        return false;
    }
    sp_data_.assign(std::istreambuf_iterator<char>(sp_file), std::istreambuf_iterator<char>());
    // Detect 64-byte DoJa SDK header (e.g. 511,964 or 512,064 bytes)
    sp_header_offset_ = (sp_data_.size() % 100 == 64) ? 64 : 0;

    // Detect PoN ("10000") vs LoN ("10001") from ScratchPad AppParam and initialize local Capcom offline DB
    bool is_lon = (sp_path_.find("LoN") != std::string::npos);
    if (sp_header_offset_ + 410624 + 5 <= sp_data_.size()) {
        std::string ver(reinterpret_cast<const char*>(sp_data_.data() + sp_header_offset_ + 410624), 5);
        if (ver == "10001") is_lon = true;
    }
    g_ui.is_lon_game = is_lon;
    std::string db_path = "capcom_local.db";
    if (!data_dir_.empty()) {
        db_path = (std::filesystem::path(data_dir_) / "capcom_local.db").string();
    } else if (!std::filesystem::exists(db_path) && !base_dir.empty()) {
        db_path = (std::filesystem::path(base_dir) / "capcom_local.db").string();
    }
    g_capcom_local.set_db_path(db_path);
    if (sp_data_.size() > sp_header_offset_) {
        g_capcom_local.register_sp_image(
            is_lon,
            sp_data_.data() + sp_header_offset_,
            sp_data_.size() - sp_header_offset_
        );
    }

    load_sav_overlay();
    g_ui.load_config();
    g_ui.discover_and_load_translation();
#if defined(__linux__) && !defined(__ANDROID__)
    prctl(PR_SET_PDEATHSIG, SIGHUP);
#endif
    std::signal(SIGINT, on_termination_signal);
    std::signal(SIGTERM, on_termination_signal);
#ifdef SIGHUP
    std::signal(SIGHUP, on_termination_signal);
#endif

    // Allocate screen Graphics object and default Font
    HeapObject gfx{};
    gfx.kind = ObjKind::Graphics;
    gfx.aux_i32 = 0xFFFFFFFF;
    screen_graphics_ref_ = static_cast<Ref>(heap_.size());
    heap_.push_back(gfx);

    HeapObject fnt{};
    fnt.kind = ObjKind::Font;
    default_font_ref_ = static_cast<Ref>(heap_.size());
    heap_.push_back(fnt);

    // Initialize SDL2 window & renderer (unless headless)
    if (!headless_ && scale > 0) {
#ifdef SDL_HINT_TIMER_RESOLUTION
        // Request 1ms OS timer resolution on Windows so unfocused windows in multi-instance PvP
        // never degrade to 15.625ms sleep granularity during CpCanvas.exe() frame pacing
        SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");
#endif
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "1");
        if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
            std::cerr << "SDL_Init warning: " << SDL_GetError() << "\n";
        } else {
#if defined(__linux__) && !defined(__ANDROID__)
            prctl(PR_SET_PDEATHSIG, SIGHUP);
#endif
            std::signal(SIGINT, on_termination_signal);
            std::signal(SIGTERM, on_termination_signal);
#ifdef SIGHUP
            std::signal(SIGHUP, on_termination_signal);
#endif
            g_ui.load_config();
            if (!std::ifstream(g_ui.config_path).good()) {
                g_ui.save_config();
            }
            g_ui.open_controllers();
            int init_h = g_ui.show_softkey_bar ? 260 : 240;
            const char* wtitle = window_title.empty() ? "Rockman EXE Mobile Recomp (PoN / LoN)" : window_title.c_str();
#if defined(__ANDROID__) || defined(__IPHONEOS__)
            uint32_t win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#else
            uint32_t win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#endif
            SDL_Window* win = SDL_CreateWindow(
                wtitle,
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                240 * window_scale_,
                init_h * window_scale_,
                win_flags
            );
            if (win) {
                sdl_window_ = win;
                // Disable SDL2's default text-input mode during gameplay so Chinese IME never intercepts WASD/ZX/F1/Esc
                SDL_StopTextInput();
                SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                if (!ren) {
                    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
                }
                sdl_renderer_ = ren;
                if (ren) {
                    int init_s = g_ui.hd_font_mode ? hd_scale_ : 1;
                    SDL_RenderSetLogicalSize(ren, 240 * init_s, init_h * init_s);
                    sdl_texture_ = SDL_CreateTexture(
                        ren,
                        SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING,
                        240,
                        260
                    );
                    if (sdl_texture_) {
                        SDL_SetTextureScaleMode(static_cast<SDL_Texture*>(sdl_texture_), SDL_ScaleModeNearest);
                    }
                    sdl_texture_hd_ = SDL_CreateTexture(
                        ren,
                        SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING,
                        240 * hd_scale_,
                        260 * hd_scale_
                    );
                    if (sdl_texture_hd_) {
                        SDL_SetTextureScaleMode(static_cast<SDL_Texture*>(sdl_texture_hd_), SDL_ScaleModeLinear);
                    }
                }
            }
            g_audio.init_sdl_audio();
            std::string fps_tag = g_ui.high_fps_mode ? "60FPS" : "15FPS";
            g_ui.show_toast(
                g_ui.online_mode
                    ? ("在线 (" + g_ui.server_url + ") | " + fps_tag + " | F1:设置 F3:帧率")
                    : ("当前: " + fps_tag + " | F1:设置  F3:切换60/15帧"),
                120
            );
        }
    }

    std::fill(std::begin(framebuffer_), std::end(framebuffer_), 0xFF000000u);
    return true;
}

void DojaRuntime::shutdown() {
    static bool s_shut_down = false;
    if (!s_shut_down) {
        s_shut_down = true;
        save_sav_overlay();
    }
    g_pvp.close_udp();
    g_ui.stop_probe_worker();
    g_ui.close_controllers();
    g_audio.shutdown_sdl_audio();
    if (sdl_texture_hd_) {
        SDL_DestroyTexture(static_cast<SDL_Texture*>(sdl_texture_hd_));
        sdl_texture_hd_ = nullptr;
    }
    if (sdl_texture_) {
        SDL_DestroyTexture(static_cast<SDL_Texture*>(sdl_texture_));
        sdl_texture_ = nullptr;
    }
    if (sdl_renderer_) {
        SDL_DestroyRenderer(static_cast<SDL_Renderer*>(sdl_renderer_));
        sdl_renderer_ = nullptr;
    }
    if (sdl_window_) {
        SDL_DestroyWindow(static_cast<SDL_Window*>(sdl_window_));
        sdl_window_ = nullptr;
    }
    if (SDL_WasInit(0) != 0) {
        SDL_Quit();
    }
}

namespace {

[[noreturn]] void clean_exit_runtime(DojaRuntime& rt, bool save_ui_cfg, int code) {
    if (save_ui_cfg) {
        g_ui.save_config();
    }
    rt.shutdown();
    std::exit(code);
}

} // namespace

void DojaRuntime::load_sav_overlay() {
    std::ifstream sav(sav_path_, std::ios::binary);
    if (!sav) return;
    // Supports both legacy 4096-byte .sav files and full ScratchPad overlays
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(sav)), std::istreambuf_iterator<char>());
    if (sp_header_offset_ + buf.size() <= sp_data_.size()) {
        std::memcpy(sp_data_.data() + sp_header_offset_, buf.data(), buf.size());
    }
}

void DojaRuntime::save_sav_overlay() {
    if (sp_data_.size() <= sp_header_offset_) return;
    // Persist full mutable ScratchPad image (excluding 64-byte SDK header) so local saves,
    // downloaded quests (QDawn), ranking courses (GetRank), and sys_dat all persist cleanly
    size_t save_len = sp_data_.size() - sp_header_offset_;
    std::ofstream sav(sav_path_, std::ios::binary | std::ios::trunc);
    if (sav) {
        sav.write(reinterpret_cast<const char*>(sp_data_.data() + sp_header_offset_), save_len);
    }
}

Ref DojaRuntime::alloc_object(uint16_t class_id, size_t num_i32_fields, size_t num_i64_fields) {
    HeapObject obj{};
    obj.kind = ObjKind::ClassInstance;
    obj.class_id = class_id;
    obj.fields_i32.assign(num_i32_fields, 0);
    obj.fields_i64.assign(num_i64_fields, 0);
    Ref r = static_cast<Ref>(heap_.size());
    heap_.push_back(std::move(obj));
    return r;
}

Ref DojaRuntime::alloc_int_array(int32_t count) {
    if (count < 0) throw JavaException(1);
    HeapObject obj{};
    obj.kind = ObjKind::IntArray;
    obj.fields_i32.assign(static_cast<size_t>(count), 0);
    Ref r = static_cast<Ref>(heap_.size());
    heap_.push_back(std::move(obj));
    return r;
}

Ref DojaRuntime::alloc_byte_array(int32_t count) {
    if (count < 0) throw JavaException(1);
    HeapObject obj{};
    obj.kind = ObjKind::ByteArray;
    obj.bytes.assign(static_cast<size_t>(count), 0);
    Ref r = static_cast<Ref>(heap_.size());
    heap_.push_back(std::move(obj));
    return r;
}

Ref DojaRuntime::alloc_ref_array(int32_t count) {
    if (count < 0) throw JavaException(1);
    HeapObject obj{};
    obj.kind = ObjKind::RefArray;
    obj.fields_i32.assign(static_cast<size_t>(count), 0);
    Ref r = static_cast<Ref>(heap_.size());
    heap_.push_back(std::move(obj));
    return r;
}

Ref DojaRuntime::alloc_multi_int_array(int32_t dim1, int32_t dim2) {
    Ref outer = alloc_ref_array(dim1);
    for (int32_t i = 0; i < dim1; ++i) {
        Ref inner = alloc_int_array(dim2);
        heap_[outer].fields_i32[i] = inner;
    }
    return outer;
}

Ref DojaRuntime::alloc_multi_byte_array(int32_t dim1, int32_t dim2) {
    Ref outer = alloc_ref_array(dim1);
    for (int32_t i = 0; i < dim1; ++i) {
        Ref inner = alloc_byte_array(dim2);
        heap_[outer].fields_i32[i] = inner;
    }
    return outer;
}

Ref DojaRuntime::alloc_multi_int_array3(int32_t dim1, int32_t dim2, int32_t dim3) {
    Ref outer = alloc_ref_array(dim1);
    for (int32_t i = 0; i < dim1; ++i) {
        Ref mid = alloc_multi_int_array(dim2, dim3);
        heap_[outer].fields_i32[i] = mid;
    }
    return outer;
}

Ref DojaRuntime::alloc_raw_string(const std::string& utf8) {
    HeapObject obj{};
    obj.kind = ObjKind::String;
    obj.str = utf8;
    Ref r = static_cast<Ref>(heap_.size());
    heap_.push_back(std::move(obj));
    return r;
}

Ref DojaRuntime::alloc_string(const std::string& utf8) {
    if (g_loc.is_enabled()) {
        return alloc_raw_string(g_loc.translate_class_string(utf8));
    }
    return alloc_raw_string(utf8);
}

int32_t DojaRuntime::array_length(Ref r) {
    HeapObject& o = get(r);
    if (o.kind == ObjKind::ByteArray) {
        return static_cast<int32_t>(o.bytes.size());
    }
    return static_cast<int32_t>(o.fields_i32.size());
}

int32_t& DojaRuntime::iaload(Ref arr, int32_t idx) {
    HeapObject& o = get(arr);
    if (idx < 0 || static_cast<size_t>(idx) >= o.fields_i32.size()) {
        throw JavaException(1, "iaload", idx, static_cast<int32_t>(o.fields_i32.size()));
    }
    return o.fields_i32[idx];
}

int32_t DojaRuntime::baload(Ref arr, int32_t idx) {
    HeapObject& o = get(arr);
    if (idx < 0 || static_cast<size_t>(idx) >= o.bytes.size()) {
        throw JavaException(1, "baload", idx, static_cast<int32_t>(o.bytes.size()));
    }
    return static_cast<int8_t>(o.bytes[idx]);
}

void DojaRuntime::bastore(Ref arr, int32_t idx, int32_t val) {
    HeapObject& o = get(arr);
    if (idx < 0 || static_cast<size_t>(idx) >= o.bytes.size()) {
        throw JavaException(1, "bastore", idx, static_cast<int32_t>(o.bytes.size()));
    }
    o.bytes[idx] = static_cast<uint8_t>(val & 0xFF);
}

std::vector<uint8_t> DojaRuntime::read_jar_Bytes(const std::string& entry_path) {
    std::ifstream jf(jar_path_, std::ios::binary);
    if (!jf) return {};
    std::vector<uint8_t> zip_bytes((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
    std::vector<uint8_t> out;
    extract_zip_entry(zip_bytes.data(), zip_bytes.size(), entry_path, out);
    return out;
}

void DojaRuntime::draw_image_internal(HeapObject& gfx, Ref img_ref, int dx, int dy, int sx, int sy, int sw, int sh, int dw, int dh) {
    if (img_ref <= 0 || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;

    bool src_has_text = false;
    {
        HeapObject& s_obj = get(img_ref);
        if (s_obj.img && !s_obj.img->is_paletted) {
            src_has_text = !s_obj.img->pix_text.empty() || !s_obj.img->hd_text.empty();
        }
    }

    SurfaceTarget dst{framebuffer_, fb_pix_text_, fb_hd_text_.data(), 240, 240, hd_scale_};
    if (gfx.target_image > 0) {
        HeapObject& t_obj = get(gfx.target_image);
        if (t_obj.img && !t_obj.img->argb.empty()) {
            int tw = t_obj.img->width;
            int th = t_obj.img->height;
            if (src_has_text) {
                if (t_obj.img->pix_text.empty()) {
                    t_obj.img->pix_text.assign(static_cast<size_t>(tw) * static_cast<size_t>(th), 0u);
                }
                if (t_obj.img->hd_text.empty()) {
                    t_obj.img->hd_text.assign(static_cast<size_t>(tw * hd_scale_) * static_cast<size_t>(th * hd_scale_), 0u);
                }
            }
            dst.gfx = t_obj.img->argb.data();
            dst.pix_text = t_obj.img->pix_text.empty() ? nullptr : t_obj.img->pix_text.data();
            dst.hd_text = t_obj.img->hd_text.empty() ? nullptr : t_obj.img->hd_text.data();
            dst.w = tw;
            dst.h = th;
        }
        g_interp.mark_offscreen_dirty(gfx.target_image);
    }

    int base_dx = dx + gfx.origin_x;
    int base_dy = dy + gfx.origin_y;
    blit_image_surface(*this, dst, img_ref, base_dx, base_dy, sx, sy, sw, sh, dw, dh, gfx.flip_mode);

    if (gfx.target_image == 0 && (g_interp.test_capture_override || (g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_))) {
        g_interp.record_image(img_ref, base_dx, base_dy, sx, sy, sw, sh, dw, dh, gfx.flip_mode);
    }
}

void DojaRuntime::draw_text_internal(HeapObject& gfx, const std::string& text, int x, int y) {
    SurfaceTarget dst{framebuffer_, fb_pix_text_, fb_hd_text_.data(), 240, 240, hd_scale_};
    if (gfx.target_image > 0) {
        HeapObject& t_obj = get(gfx.target_image);
        if (t_obj.img && !t_obj.img->argb.empty()) {
            int tw = t_obj.img->width;
            int th = t_obj.img->height;
            if (t_obj.img->pix_text.empty()) {
                t_obj.img->pix_text.assign(static_cast<size_t>(tw) * static_cast<size_t>(th), 0u);
            }
            if (t_obj.img->hd_text.empty()) {
                t_obj.img->hd_text.assign(static_cast<size_t>(tw * hd_scale_) * static_cast<size_t>(th * hd_scale_), 0u);
            }
            dst.gfx = t_obj.img->argb.data();
            dst.pix_text = t_obj.img->pix_text.data();
            dst.hd_text = t_obj.img->hd_text.data();
            dst.w = tw;
            dst.h = th;
        }
        g_interp.mark_offscreen_dirty(gfx.target_image);
    }

    uint32_t color = static_cast<uint32_t>(gfx.aux_i32) | 0xFF000000u;
    int pen_x = x + gfx.origin_x;
    int baseline_y = y + gfx.origin_y;
    blit_text_surface(dst, text, pen_x, baseline_y, color, should_bake_1x_text_into_gfx());

    if (gfx.target_image == 0 && (g_interp.test_capture_override || (g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_))) {
        g_interp.record_text(text, pen_x, baseline_y, color);
    }
}

void DojaRuntime::poll_events() {
    if (g_quit_signal.load(std::memory_order_relaxed)) {
        clean_exit_runtime(*this, sdl_window_ != nullptr, 0);
    }
    g_ui.poll_async_probe();
    g_ui.maybe_periodic_room_refresh();
    if (!pvp_self_test_ && !suppress_av_ && !pvp_match_active_ &&
        g_pvp.active() && !g_pvp.match_started()) {
        if (g_pvp.poll_udp()) {
            g_ui.show_toast("挑战者已连接! VS " + g_ui.player_name + " <-> " + g_pvp.opponent_name(), 150);
        }
    }
    if (!sdl_window_) return;
    static bool in_modal = false;

    auto sync_action_edge = [&](int act_idx) {
        ActionBinding& ab = g_ui.binds[act_idx];
        bool now_down = ab.key_down[0] || ab.key_down[1] || ab.pad_down || ab.axis_down || ab.touch_down;
        if (now_down == ab.effective_down) return;
        ab.effective_down = now_down;
        if (ab.doja_key >= 0) {
            if (now_down) {
                keypad_state_ |= (1 << ab.doja_key);
                if (current_canvas_ref_ > 0 && !pvp_match_active_) {
                    dispatch_process_event(current_canvas_ref_, 0, ab.doja_key);
                }
            } else {
                keypad_state_ &= ~(1 << ab.doja_key);
                if (current_canvas_ref_ > 0 && !pvp_match_active_) {
                    dispatch_process_event(current_canvas_ref_, 1, ab.doja_key);
                }
            }
        }
    };

    auto trigger_direct_doja_key = [&](int doja_key, bool pressed) {
        if (doja_key < 0) return;
        int32_t mask = (1 << doja_key);
        bool was_down = (keypad_state_ & mask) != 0;
        if (pressed == was_down) return;
        if (pressed) {
            keypad_state_ |= mask;
            if (current_canvas_ref_ > 0 && !pvp_match_active_) {
                dispatch_process_event(current_canvas_ref_, 0, doja_key);
            }
        } else {
            keypad_state_ &= ~mask;
            if (current_canvas_ref_ > 0 && !pvp_match_active_) {
                dispatch_process_event(current_canvas_ref_, 1, doja_key);
            }
        }
    };

    auto open_pause_or_overlay = [&]() {
        keypad_state_ = 0;
        g_ui.clear_held_states();
        if (g_pvp.active()) {
            // Online PvP / Room Wait: open menu as a non-blocking background-running overlay!
            g_ui.open_overlay(false);
            return false; // Did NOT block
        }
        if (in_modal) return true;
        in_modal = true;
        bool quit = g_ui.run_pause_config_modal(
            static_cast<SDL_Window*>(sdl_window_),
            static_cast<SDL_Renderer*>(sdl_renderer_),
            static_cast<SDL_Texture*>(sdl_texture_),
            static_cast<SDL_Texture*>(sdl_texture_hd_),
            framebuffer_,
            fb_pix_text_,
            fb_hd_text_.data(),
            hd_scale_,
            window_scale_
        );
        in_modal = false;
        if (quit) {
            clean_exit_runtime(*this, true, 0);
        }
        return true; // Blocked inside modal
    };

    if (g_ui.overlay_open) {
        keypad_state_ = 0;
    }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (g_ui.overlay_open) {
            bool quit_req = false;
            g_ui.handle_overlay_event(
                ev,
                static_cast<SDL_Window*>(sdl_window_),
                static_cast<SDL_Renderer*>(sdl_renderer_),
                window_scale_,
                quit_req
            );
            keypad_state_ = 0;
            if (quit_req) {
                clean_exit_runtime(*this, true, 0);
            }
            continue;
        }
        if (g_ui.touch.enabled() && sdl_renderer_) {
            SDL_Renderer* ren = static_cast<SDL_Renderer*>(sdl_renderer_);
            int out_w = 0, out_h = 0;
            SDL_GetRendererOutputSize(ren, &out_w, &out_h);
            g_ui.touch.update_layout(out_w, out_h, g_ui.show_softkey_bar ? 260 : 240);
            TouchActionDelta td;
            if (g_ui.touch.handle_event(ev, out_w, out_h, false, td)) {
                for (int i = 0; i < ACT_COUNT; ++i) {
                    if (g_ui.binds[i].doja_key >= 0) {
                        g_ui.binds[i].touch_down = ((td.doja_mask & (1u << g_ui.binds[i].doja_key)) != 0u);
                    } else if (i == ACT_TURBO) {
                        g_ui.binds[i].touch_down = td.turbo_down;
                    }
                    sync_action_edge(i);
                }
                if (td.menu_triggered) {
                    if (open_pause_or_overlay()) return;
                }
                continue;
            }
        }
        if (ev.type == SDL_QUIT ||
            ev.type == SDL_APP_TERMINATING ||
            (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
            clean_exit_runtime(*this, true, 0);
        } else if (ev.type == SDL_APP_WILLENTERBACKGROUND) {
            save_sav_overlay();
            g_ui.save_config();
            if (g_audio.dev_id != 0) SDL_PauseAudioDevice(g_audio.dev_id, 1);
            g_ui.assist_hold_down = false;
        } else if (ev.type == SDL_APP_DIDENTERFOREGROUND) {
            if (g_audio.dev_id != 0) SDL_PauseAudioDevice(g_audio.dev_id, 0);
        } else if (ev.type == SDL_WINDOWEVENT &&
                   (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                    ev.window.event == SDL_WINDOWEVENT_MINIMIZED)) {
            g_ui.assist_hold_down = false;
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            g_ui.open_controllers();
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            if (ev.key.repeat) continue;
            bool pressed = (ev.type == SDL_KEYDOWN);
            SDL_Keycode sym = ev.key.keysym.sym;

            if (sym == SDLK_F6) {
                if (g_ui.assist_cfg.hotkey_mode == 1) {
                    g_ui.assist_hold_down = pressed;
                } else if (pressed) {
                    g_ui.toggle_assist_master();
                }
                continue;
            }

            // Emulator hotkeys on key-down
            if (pressed) {
                if (sym == SDLK_F1 || sym == SDLK_ESCAPE || sym == SDLK_AC_BACK) {
                    if (open_pause_or_overlay()) return;
                    continue;
                }
                if (sym == SDLK_F2) {
                    g_ui.show_softkey_bar = !g_ui.show_softkey_bar;
                    g_ui.apply_softbar_geometry(
                        static_cast<SDL_Window*>(sdl_window_),
                        static_cast<SDL_Renderer*>(sdl_renderer_),
                        window_scale_
                    );
                    g_ui.save_config();
                    g_ui.show_toast(g_ui.show_softkey_bar ? "底部软键栏: 开启" : "底部软键栏: 关闭");
                    continue;
                }
                if (sym == SDLK_F3) {
                    g_ui.high_fps_mode = !g_ui.high_fps_mode;
                    g_interp.reset();
                    g_ui.save_config();
                    g_ui.show_toast(g_ui.high_fps_mode ? "帧率模式: 60 FPS (丝滑插值)" : "帧率模式: 15 FPS (原版帧率)");
                    continue;
                }
                if (sym == SDLK_F4) {
                    g_ui.toggle_language_mode();
                    continue;
                }
                if (sym == SDLK_F5) {
                    g_ui.toggle_font_mode();
                    continue;
                }
                if (sym == SDLK_F7) {
                    g_ui.toggle_assist_buster_quick();
                    continue;
                }
                if (sym == SDLK_F8) {
                    g_ui.cycle_assist_encounter_quick();
                    continue;
                }
                if (sym == SDLK_F11 || (sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                    g_ui.toggle_fullscreen(static_cast<SDL_Window*>(sdl_window_));
                    continue;
                }
                if (sym == SDLK_F12) {
                    g_ui.save_screenshot_bmp(framebuffer_, fb_pix_text_, fb_hd_text_.data(), hd_scale_);
                    continue;
                }
                if (sym == SDLK_F9 || (sym == SDLK_m && !g_ui.is_keycode_bound(SDLK_m))) {
                    bool muted_now = false;
                    int vol_now = 100;
                    {
                        std::lock_guard<std::mutex> lk(g_audio.mtx);
                        g_audio.muted = !g_audio.muted;
                        muted_now = g_audio.muted;
                        vol_now = g_audio.master_volume;
                    }
                    g_ui.save_config();
                    g_ui.show_toast(muted_now ? "静音: 开启" : ("主音量: " + std::to_string(vol_now) + "%"));
                    continue;
                }
                if ((sym == SDLK_MINUS || sym == SDLK_EQUALS) && !g_ui.is_keycode_bound(sym)) {
                    int delta = (sym == SDLK_EQUALS) ? 5 : -5;
                    int vol_now = 100;
                    {
                        std::lock_guard<std::mutex> lk(g_audio.mtx);
                        g_audio.muted = false;
                        g_audio.master_volume = std::clamp(g_audio.master_volume + delta, 0, 100);
                        vol_now = g_audio.master_volume;
                    }
                    g_ui.save_config();
                    g_ui.show_toast("主音量: " + std::to_string(vol_now) + "%");
                    continue;
                }
            }

            // Check customizable action bindings
            bool matched_bind = false;
            for (int i = 0; i < ACT_COUNT; ++i) {
                for (int s = 0; s < 2; ++s) {
                    if (g_ui.binds[i].keys[s] != SDLK_UNKNOWN && g_ui.binds[i].keys[s] == sym) {
                        g_ui.binds[i].key_down[s] = pressed;
                        sync_action_edge(i);
                        matched_bind = true;
                    }
                }
            }
            if (matched_bind) continue;

            // Built-in convenience fallbacks when unbound: 0..9 digits + Enter/Space for Confirm
            int fallback_doja = -1;
            if (sym >= SDLK_0 && sym <= SDLK_9) {
                fallback_doja = sym - SDLK_0;
            } else if (sym >= SDLK_KP_1 && sym <= SDLK_KP_9) {
                fallback_doja = 1 + (sym - SDLK_KP_1);
            } else if (sym == SDLK_KP_0) {
                fallback_doja = 0;
            } else if (sym == SDLK_RETURN || sym == SDLK_SPACE) {
                fallback_doja = 0x14; // KEY_SELECT
            } else if (sym == SDLK_BACKSPACE) {
                fallback_doja = 0x16; // KEY_SOFT2
            }
            if (fallback_doja >= 0) {
                trigger_direct_doja_key(fallback_doja, pressed);
            }
        } else if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
            bool pressed = (ev.type == SDL_CONTROLLERBUTTONDOWN);
            int btn = ev.cbutton.button;
            if (pressed && btn == SDL_CONTROLLER_BUTTON_START && !in_modal) {
                if (open_pause_or_overlay()) return;
                continue;
            }
            for (int i = 0; i < ACT_COUNT; ++i) {
                if (g_ui.binds[i].pad_btn == btn) {
                    g_ui.binds[i].pad_down = pressed;
                    sync_action_edge(i);
                }
            }
        } else if (ev.type == SDL_CONTROLLERAXISMOTION) {
            constexpr int DEADZONE = 10000;
            if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                g_ui.binds[ACT_LEFT].axis_down  = (ev.caxis.value < -DEADZONE);
                g_ui.binds[ACT_RIGHT].axis_down = (ev.caxis.value >  DEADZONE);
                sync_action_edge(ACT_LEFT);
                sync_action_edge(ACT_RIGHT);
            } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                g_ui.binds[ACT_UP].axis_down   = (ev.caxis.value < -DEADZONE);
                g_ui.binds[ACT_DOWN].axis_down = (ev.caxis.value >  DEADZONE);
                sync_action_edge(ACT_UP);
                sync_action_edge(ACT_DOWN);
            } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                       ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                int trig_btn = (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) ? PAD_BTN_LT : PAD_BTN_RT;
                bool trig_down = (ev.caxis.value > DEADZONE);
                for (int i = 0; i < ACT_COUNT; ++i) {
                    if (g_ui.binds[i].pad_btn == trig_btn) {
                        g_ui.binds[i].pad_down = trig_down;
                        sync_action_edge(i);
                    }
                }
            }
        }
    }
}

void DojaRuntime::present_screen() {
    ++frame_count_;
    // Support deterministic synthetic UI hotkeys in --inject-key
    // (100=F1, 101=Enter, 102=Down, 103=Left, 104=Right, 105=Up)
    for (const auto& kv : injected_keys_) {
        if (kv.start_frame == frame_count_ && kv.doja_key >= 100) {
            SDL_Keycode sym = SDLK_DOWN;
            switch (kv.doja_key) {
                case 100: sym = SDLK_F1; break;
                case 101: sym = SDLK_RETURN; break;
                case 102: sym = SDLK_DOWN; break;
                case 103: sym = SDLK_LEFT; break;
                case 104: sym = SDLK_RIGHT; break;
                case 105: sym = SDLK_UP; break;
                case 106: sym = SDLK_F5; break;
                default: break;
            }
            if (sdl_window_) {
                SDL_Event kev{};
                kev.type = SDL_KEYDOWN;
                kev.key.state = SDL_PRESSED;
                kev.key.repeat = 0;
                kev.key.keysym.sym = sym;
                SDL_PushEvent(&kev);
            } else {
                // Headless mode fallback for automated UI overlay testing
                if (sym == SDLK_F1) {
                    if (g_ui.overlay_open) g_ui.close_overlay();
                    else { keypad_state_ = 0; g_ui.open_overlay(false); }
                } else if (g_ui.overlay_open) {
                    SDL_Event kev{};
                    kev.type = SDL_KEYDOWN;
                    kev.key.state = SDL_PRESSED;
                    kev.key.keysym.sym = sym;
                    bool quit_req = false;
                    g_ui.handle_overlay_event(kev, nullptr, nullptr, window_scale_, quit_req);
                    if (quit_req) clean_exit_runtime(*this, false, 0);
                }
            }
        }
    }
    if (!pvp_match_active_) {
        for (const auto& kv : injected_keys_) {
            if (kv.doja_key < 0 || kv.doja_key >= 31 || g_ui.overlay_open) continue;
            if (kv.start_frame == frame_count_) {
                keypad_state_ |= (1 << kv.doja_key);
                if (current_canvas_ref_ > 0) {
                    dispatch_process_event(current_canvas_ref_, 0, kv.doja_key);
                }
            } else if (kv.end_frame + 1 == frame_count_) {
                keypad_state_ &= ~(1 << kv.doja_key);
                if (current_canvas_ref_ > 0) {
                    dispatch_process_event(current_canvas_ref_, 1, kv.doja_key);
                }
            }
        }
        poll_events();
        if (g_pvp.active()) {
            if (g_pvp.poll_udp()) {
                g_ui.show_toast("挑战者已连接! VS " + g_ui.player_name + " <-> " + g_pvp.opponent_name(), 150);
            }
            if (!g_pvp.match_started() && (frame_count_ % 120) == 0) {
                g_pvp.send_ping();
            }
        }
    } else {
        poll_events();
    }
    if (sdl_renderer_ && (sdl_texture_ || sdl_texture_hd_)) {
        SDL_Renderer* ren = static_cast<SDL_Renderer*>(sdl_renderer_);
        SDL_Texture* tex_pix = static_cast<SDL_Texture*>(sdl_texture_);
        SDL_Texture* tex_hd = static_cast<SDL_Texture*>(sdl_texture_hd_);
        SDL_Window* win = static_cast<SDL_Window*>(sdl_window_);
        bool win_minimized = win && ((SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) != 0);
        bool interp_active = !win_minimized && g_ui.high_fps_mode && !fast_forward_ &&
                             (!g_ui.is_turbo_held() || g_pvp.active());
        g_interp.on_screen_unlock(framebuffer_, fb_pix_text_, fb_hd_text_.data(), hd_scale_, interp_active);

        if (!(win_minimized && g_pvp.active())) {
            const uint32_t* present_fb = framebuffer_;
            const uint32_t* present_pix_text = fb_pix_text_;
            const uint32_t* present_hd_text = fb_hd_text_.data();
            if (interp_active && g_interp.can_interpolate && g_interp.any_motion) {
                g_interp.rasterize_subframe(*this, 0.25f);
                present_fb = g_interp.interp_fb;
                present_pix_text = g_interp.interp_pix_text;
                present_hd_text = g_interp.interp_hd_text.data();
            }
            g_ui.compose_display_buffer(present_fb, present_pix_text, present_hd_text, hd_scale_, keypad_state_, true,
                                        pvp_match_active_ || pvp_self_test_);
            g_ui.present_sdl_frame(ren, tex_pix, tex_hd);
        } else if (g_ui.toast_frames > 0) {
            --g_ui.toast_frames;
        }
        // Stamp unlock timestamp AFTER VSync present completes so CpCanvas.sePlay() -> Thread.yield()
        // (called immediately after Graphics.unlock() during Buster Charging SE loops) sees ~0ms elapsed
        // and never triggers an extra blocking SDL_RenderPresent!
        g_interp.tick_unlock_tp = std::chrono::steady_clock::now();
    } else {
        g_interp.reset();
        if (g_ui.toast_frames > 0) --g_ui.toast_frames;
        if (!fast_forward_) {
            static thread_local auto s_last_headless_tp = std::chrono::steady_clock::now();
            auto now_tp = std::chrono::steady_clock::now();
            auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - s_last_headless_tp).count();
            if (dt_ms < 15) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15 - dt_ms));
            }
            s_last_headless_tp = std::chrono::steady_clock::now();
        }
    }
    if (max_frames_ > 0 && frame_count_ >= max_frames_) {
        if (!dump_ppm_path_.empty()) {
            g_ui.compose_display_buffer(framebuffer_, fb_pix_text_, fb_hd_text_.data(), hd_scale_, keypad_state_, false,
                                        pvp_match_active_ || pvp_self_test_);
            int dump_h = g_ui.show_softkey_bar ? 260 : 240;
            std::ofstream ppm(dump_ppm_path_, std::ios::binary | std::ios::trunc);
            if (ppm) {
                if (g_ui.hd_font_mode && !g_ui.display_buf_hd.empty()) {
                    int s = std::clamp(hd_scale_, 2, 4);
                    int out_w = 240 * s;
                    int out_h = dump_h * s;
                    ppm << "P6\n" << out_w << " " << out_h << "\n255\n";
                    for (int i = 0; i < out_w * out_h; ++i) {
                        uint32_t c = g_ui.display_buf_hd[static_cast<size_t>(i)];
                        uint8_t rgb[3] = {
                            static_cast<uint8_t>((c >> 16) & 0xFF),
                            static_cast<uint8_t>((c >> 8) & 0xFF),
                            static_cast<uint8_t>(c & 0xFF)
                        };
                        ppm.write(reinterpret_cast<const char*>(rgb), 3);
                    }
                } else {
                    int s = std::max(1, window_scale_);
                    int out_w = 240 * s;
                    int out_h = dump_h * s;
                    ppm << "P6\n" << out_w << " " << out_h << "\n255\n";
                    for (int y = 0; y < dump_h; ++y) {
                        for (int sy = 0; sy < s; ++sy) {
                            for (int x = 0; x < 240; ++x) {
                                uint32_t c = g_ui.display_buf[y * 240 + x];
                                uint8_t rgb[3] = {
                                    static_cast<uint8_t>((c >> 16) & 0xFF),
                                    static_cast<uint8_t>((c >> 8) & 0xFF),
                                    static_cast<uint8_t>(c & 0xFF)
                                };
                                for (int sx = 0; sx < s; ++sx) {
                                    ppm.write(reinterpret_cast<const char*>(rgb), 3);
                                }
                            }
                        }
                    }
                }
            }
        }
        clean_exit_runtime(*this, false, 0);
    }
}

int64_t DojaRuntime::current_time_millis() {
    poll_events();
    if (fast_forward_) {
        fake_time_ms_ += 20;
        return fake_time_ms_;
    }
    if (g_ui.is_turbo_held() && !g_pvp.active()) {
        g_ui.turbo_bonus_ms += 12;
    }
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return wall_ms - g_ui.paused_duration_ms + g_ui.turbo_bonus_ms;
}

void DojaRuntime::sleep_ms(int64_t ms) {
    poll_events();
    if (fast_forward_) return;
    if (g_ui.is_turbo_held() && !g_pvp.active()) {
        g_ui.turbo_bonus_ms += std::max<int64_t>(0, ms - 12);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(ms, 12)));
        return;
    }
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

Slot DojaRuntime::invoke_external(const char* cls, const char* name, const char* desc, Slot* args, int num_args) {
    std::string c(cls);
    std::string m(name);
    std::string d(desc);

    // ------------------------------------------------------------------------
    // 1. java.lang.System / Thread / Object / Boolean
    // ------------------------------------------------------------------------
    if (c == "java/lang/System") {
        if (m == "currentTimeMillis") return Slot(current_time_millis());
        if (m == "gc") return Slot(0);
    }
    if (c == "java/lang/Thread") {
        if (m == "sleep") {
            sleep_ms(args[0].j);
            return Slot(0);
        }
        if (m == "yield") {
            poll_events();
            if (fast_forward_) {
                return Slot(0);
            }
            if (g_ui.is_turbo_held() && !g_pvp.active()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return Slot(0);
            }
            // During CpCanvas's ~62.0 ms wait between 16 Hz logic ticks, present smooth
            // 60 FPS interpolated sub-frames when 60 FPS mode is active.
            // In online PvP, stop sub-frame VSync presents after sub-frame 2 (< 35ms after unlock VSync)
            // so a 60 Hz VSync boundary never overshoots CpCanvas.exe()'s 62ms tick clock (66.7ms vs 62.0ms).
            SDL_Window* ywin = static_cast<SDL_Window*>(sdl_window_);
            bool y_minimized = ywin && ((SDL_GetWindowFlags(ywin) & SDL_WINDOW_MINIMIZED) != 0);
            int max_sf = g_pvp.active() ? 3 : 4;
            if (!y_minimized && g_ui.high_fps_mode && sdl_renderer_ && (sdl_texture_ || sdl_texture_hd_) &&
                g_interp.subframes_presented < max_sf) {
                auto now_tp = std::chrono::steady_clock::now();
                double elapsed_ms = std::chrono::duration<double, std::milli>(now_tp - g_interp.tick_unlock_tp).count();
                static constexpr double kSubTargetsMs[4] = {0.0, 14.0, 28.5, 43.0};
                int next_sf = g_interp.subframes_presented; // 1 -> 2 (-> 3 in offline mode)
                double max_allowed_ms = g_pvp.active() ? 35.0 : 52.0;
                if (elapsed_ms >= kSubTargetsMs[next_sf] && elapsed_ms < max_allowed_ms) {
                    g_interp.subframes_presented = next_sf + 1;
                    const uint32_t* sub_fb = framebuffer_;
                    const uint32_t* sub_pix_text = fb_pix_text_;
                    const uint32_t* sub_hd_text = fb_hd_text_.data();
                    if (g_interp.can_interpolate && g_interp.any_motion && next_sf < (max_sf - 1)) {
                        float alpha = std::clamp(static_cast<float>((elapsed_ms + 15.5) / 62.0), 0.25f, 1.0f);
                        g_interp.rasterize_subframe(*this, alpha);
                        sub_fb = g_interp.interp_fb;
                        sub_pix_text = g_interp.interp_pix_text;
                        sub_hd_text = g_interp.interp_hd_text.data();
                    }
                    g_ui.compose_display_buffer(sub_fb, sub_pix_text, sub_hd_text, hd_scale_, keypad_state_, false,
                                                pvp_match_active_ || pvp_self_test_);
                    g_ui.present_sdl_frame(
                        static_cast<SDL_Renderer*>(sdl_renderer_),
                        static_cast<SDL_Texture*>(sdl_texture_),
                        static_cast<SDL_Texture*>(sdl_texture_hd_)
                    );
                    return Slot(0);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return Slot(0);
        }
    }
    if (c == "java/lang/Object" && m == "<init>") {
        return Slot(0);
    }
    if (c == "java/lang/Boolean") {
        if (m == "<init>") {
            get(args[0].r).aux_i32 = args[1].i;
            return Slot(0);
        }
        if (m == "toString") {
            return Slot(alloc_raw_string(get(args[0].r).aux_i32 ? "true" : "false"));
        }
    }
    if (c == "java/io/PrintStream" && m == "println") {
        if (args[1].r > 0) {
            std::cout << "[GameLog] " << get(args[1].r).str << "\n";
        }
        return Slot(0);
    }

    // ------------------------------------------------------------------------
    // 2. java.lang.String & java.lang.StringBuffer
    // ------------------------------------------------------------------------
    if (c == "java/lang/String") {
        auto decode_str_bytes = [&](const uint8_t* ptr, size_t len) -> std::string {
            if (g_loc.has_loaded_pack()) {
                return g_loc.decode_bytes_to_utf8(ptr, len, cp932_to_utf8);
            }
            return cp932_to_utf8(ptr, len);
        };
        if (m == "<init>") {
            HeapObject& s_obj = get(args[0].r);
            s_obj.kind = ObjKind::String;
            HeapObject& b_arr = get(args[1].r);
            if (d == "([B)V") {
                s_obj.str = decode_str_bytes(b_arr.bytes.data(), b_arr.bytes.size());
            } else if (d == "([BII)V") {
                int off = std::max(0, args[2].i);
                int len = std::max(0, args[3].i);
                if (static_cast<size_t>(off + len) <= b_arr.bytes.size()) {
                    s_obj.str = decode_str_bytes(b_arr.bytes.data() + off, static_cast<size_t>(len));
                }
            }
            return Slot(0);
        }
        const std::string& s = get(args[0].r).str;
        if (m == "length") {
            return Slot(static_cast<int32_t>(utf8_to_codepoints(s).size()));
        }
        if (m == "charAt") {
            auto cps = utf8_to_codepoints(s);
            int idx = args[1].i;
            if (idx < 0 || static_cast<size_t>(idx) >= cps.size()) throw JavaException(1);
            return Slot(static_cast<int32_t>(cps[idx]));
        }
        if (m == "substring") {
            auto cps = utf8_to_codepoints(s);
            int b = std::max(0, args[1].i);
            int e = std::min(static_cast<int>(cps.size()), args[2].i);
            return Slot(alloc_raw_string(codepoints_to_utf8(cps, b, e)));
        }
        if (m == "getBytes") {
            auto bytes = g_loc.is_enabled() ? g_loc.encode_text_to_bytes(s) : utf8_to_cp932(s);
            Ref barr = alloc_byte_array(static_cast<int32_t>(bytes.size()));
            get(barr).bytes = std::move(bytes);
            return Slot(barr);
        }
        if (m == "equals") {
            if (args[1].r <= 0) return Slot(0);
            return Slot(s == get(args[1].r).str ? 1 : 0);
        }
        if (m == "compareTo") {
            if (args[1].r <= 0) throw JavaException(1);
            int cmp = s.compare(get(args[1].r).str);
            return Slot(cmp < 0 ? -1 : (cmp > 0 ? 1 : 0));
        }
        if (m == "indexOf") {
            auto cps = utf8_to_codepoints(s);
            uint32_t ch = static_cast<uint32_t>(args[1].i);
            size_t start = (d == "(II)I") ? static_cast<size_t>(std::max(0, args[2].i)) : 0;
            for (size_t i = start; i < cps.size(); ++i) {
                if (cps[i] == ch) return Slot(static_cast<int32_t>(i));
            }
            return Slot(-1);
        }
    }

    if (c == "java/lang/StringBuffer") {
        HeapObject& sb = get(args[0].r);
        if (m == "<init>") {
            sb.kind = ObjKind::StringBuffer;
            sb.str.clear();
            return Slot(0);
        }
        if (m == "append") {
            if (d == "(Ljava/lang/String;)Ljava/lang/StringBuffer;") {
                sb.str += (args[1].r > 0) ? get(args[1].r).str : "null";
            } else if (d == "(I)Ljava/lang/StringBuffer;") {
                sb.str += std::to_string(args[1].i);
            } else if (d == "(C)Ljava/lang/StringBuffer;") {
                uint32_t cp = static_cast<uint32_t>(args[1].i);
                sb.str += codepoints_to_utf8({cp}, 0, 1);
            } else {
                sb.str += (args[1].r > 0) ? get(args[1].r).str : "null";
            }
            return Slot(args[0].r);
        }
        if (m == "toString") {
            return Slot(alloc_raw_string(sb.str));
        }
    }

    // ------------------------------------------------------------------------
    // 3. java.util.Random / Date / Calendar
    // ------------------------------------------------------------------------
    if (c == "java/util/Random") {
        HeapObject& rnd = get(args[0].r);
        if (m == "<init>") {
            rnd.kind = ObjKind::Random;
            rnd.rng_seed = (static_cast<uint64_t>(current_time_millis()) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
            return Slot(0);
        }
        if (m == "nextInt") {
            rnd.rng_seed = (rnd.rng_seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
            return Slot(static_cast<int32_t>(rnd.rng_seed >> 16));
        }
    }
    if (c == "java/util/Date" && m == "<init>") {
        return Slot(0);
    }
    if (c == "java/util/Calendar") {
        if (m == "getInstance") {
            Ref r = alloc_object(0, 4, 0);
            get(r).kind = ObjKind::Calendar;
            return Slot(r);
        }
        if (m == "setTime") return Slot(0);
        if (m == "get") {
            std::time_t t = std::time(nullptr);
            std::tm* lt = std::localtime(&t);
            int field = args[1].i;
            if (field == 1) return Slot(lt->tm_year + 1900);
            if (field == 2) return Slot(lt->tm_mon);
            if (field == 5) return Slot(lt->tm_mday);
            if (field == 11) return Slot(lt->tm_hour);
            if (field == 12) return Slot(lt->tm_min);
            return Slot(0);
        }
    }

    // ------------------------------------------------------------------------
    // 4. javax.microedition.io.Connector & java.io.* Streams & JarInflater
    // ------------------------------------------------------------------------
    if (c == "javax/microedition/io/Connector") {
        std::string url = (args[0].r > 0) ? get(args[0].r).str : "";
        if (m == "openInputStream" || m == "openDataInputStream") {
            HeapObject st_obj{};
            st_obj.kind = ObjKind::InputStream;
            st_obj.stream = std::make_shared<StreamData>();
            if (url.rfind("scratchpad:///0", 0) == 0) {
                size_t pos_val = 0;
                size_t len_val = sp_data_.size() - sp_header_offset_;
                auto p_idx = url.find("pos=");
                if (p_idx != std::string::npos) pos_val = std::stoul(url.substr(p_idx + 4));
                auto l_idx = url.find("length=");
                if (l_idx != std::string::npos) len_val = std::stoul(url.substr(l_idx + 7));
                size_t real_off = sp_header_offset_ + pos_val;
                if (real_off < sp_data_.size()) {
                    size_t avail = std::min(len_val, sp_data_.size() - real_off);
                    st_obj.stream->buffer.assign(sp_data_.begin() + real_off, sp_data_.begin() + real_off + avail);
                }
            } else if (url.rfind("resource:///", 0) == 0) {
                st_obj.stream->buffer = read_jar_Bytes(url.substr(12));
            }
            if (g_loc.is_enabled() && st_obj.stream->buffer.size() >= 4 &&
                std::memcmp(st_obj.stream->buffer.data(), "PK\x03\x04", 4) != 0) {
                uint64_t h = loc::fnv1a_64(st_obj.stream->buffer.data(), st_obj.stream->buffer.size());
                if (const auto* ov = g_loc.find_stream_overlay(h)) {
                    st_obj.stream->buffer = *ov;
                }
            }
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(st_obj));
            return Slot(r);
        }
        if (m == "openOutputStream" || m == "openDataOutputStream") {
            HeapObject st_obj{};
            st_obj.kind = ObjKind::OutputStream;
            st_obj.stream = std::make_shared<StreamData>();
            if (url.rfind("scratchpad:///0", 0) == 0) {
                auto p_idx = url.find("pos=");
                if (p_idx != std::string::npos) st_obj.stream->sp_write_offset = std::stoi(url.substr(p_idx + 4));
                auto l_idx = url.find("length=");
                if (l_idx != std::string::npos) st_obj.stream->sp_write_length = std::stoi(url.substr(l_idx + 7));
            }
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(st_obj));
            return Slot(r);
        }
        if (m == "open") {
            HeapObject hc{};
            hc.kind = ObjKind::HttpConnection;
            hc.str = url;
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(hc));

            HttpConnState st{};
            st.url = url;
            st.method = "GET";
            g_http_conns[r] = std::move(st);
            return Slot(r);
        }
    }

    if (c == "com/nttdocomo/util/JarInflater") {
        HeapObject& ji = get(args[0].r);
        if (m == "<init>") {
            ji.kind = ObjKind::JarInflater;
            ji.inflater = std::make_shared<JarInflaterData>();
            HeapObject& in_st = get(args[1].r);
            if (in_st.stream) {
                ji.inflater->zip_bytes.assign(
                    in_st.stream->buffer.begin() + in_st.stream->pos,
                    in_st.stream->buffer.end()
                );
                in_st.stream->pos = in_st.stream->buffer.size();
            }
            return Slot(0);
        }
        if (m == "getInputStream") {
            std::string entry = (args[1].r > 0) ? get(args[1].r).str : "data.dat";
            HeapObject st_obj{};
            st_obj.kind = ObjKind::InputStream;
            st_obj.stream = std::make_shared<StreamData>();
            if (ji.inflater) {
                extract_zip_entry(
                    ji.inflater->zip_bytes.data(),
                    ji.inflater->zip_bytes.size(),
                    entry,
                    st_obj.stream->buffer
                );
            }
            if (g_loc.is_enabled() && !st_obj.stream->buffer.empty()) {
                uint64_t h = loc::fnv1a_64(st_obj.stream->buffer.data(), st_obj.stream->buffer.size());
                if (const auto* ov = g_loc.find_stream_overlay(h)) {
                    st_obj.stream->buffer = *ov;
                }
            }
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(st_obj));
            return Slot(r);
        }
        if (m == "close") return Slot(0);
    }

    if (c == "java/io/InputStream" || c == "java/io/DataInputStream") {
        HeapObject& st = get(args[0].r);
        if (!st.stream) return Slot(-1);
        StreamData& sd = *st.stream;
        if (m == "read") {
            if (d == "()I") {
                if (sd.pos >= sd.buffer.size()) return Slot(-1);
                return Slot(static_cast<int32_t>(sd.buffer[sd.pos++]));
            }
            if (d == "([B)I") {
                HeapObject& barr = get(args[1].r);
                size_t want = barr.bytes.size();
                size_t avail = std::min(want, sd.buffer.size() - sd.pos);
                if (avail == 0) return Slot(-1);
                std::memcpy(barr.bytes.data(), sd.buffer.data() + sd.pos, avail);
                sd.pos += avail;
                return Slot(static_cast<int32_t>(avail));
            }
            if (d == "([BII)I") {
                HeapObject& barr = get(args[1].r);
                int off = std::max(0, args[2].i);
                int len = std::max(0, args[3].i);
                size_t avail = std::min<size_t>(len, sd.buffer.size() - sd.pos);
                if (avail == 0) return Slot(-1);
                if (static_cast<size_t>(off) + avail <= barr.bytes.size()) {
                    std::memcpy(barr.bytes.data() + off, sd.buffer.data() + sd.pos, avail);
                }
                sd.pos += avail;
                return Slot(static_cast<int32_t>(avail));
            }
        }
        if (m == "readInt") {
            if (sd.pos + 4 > sd.buffer.size()) throw JavaException(1);
            int32_t v = (static_cast<int32_t>(sd.buffer[sd.pos]) << 24) |
                        (static_cast<int32_t>(sd.buffer[sd.pos + 1]) << 16) |
                        (static_cast<int32_t>(sd.buffer[sd.pos + 2]) << 8) |
                        static_cast<int32_t>(sd.buffer[sd.pos + 3]);
            sd.pos += 4;
            return Slot(v);
        }
        if (m == "skip") {
            int64_t n = std::max<int64_t>(0, args[1].j);
            size_t avail = std::min<size_t>(static_cast<size_t>(n), sd.buffer.size() - sd.pos);
            sd.pos += avail;
            return Slot(static_cast<int64_t>(avail));
        }
        if (m == "close") return Slot(0);
    }

    if (c == "java/io/OutputStream" || c == "java/io/DataOutputStream") {
        HeapObject& st = get(args[0].r);
        if (!st.stream) return Slot(0);
        StreamData& sd = *st.stream;
        if (m == "write") {
            HeapObject& barr = get(args[1].r);
            if (d == "([B)V") {
                sd.buffer.insert(sd.buffer.end(), barr.bytes.begin(), barr.bytes.end());
            } else if (d == "([BII)V") {
                int off = std::max(0, args[2].i);
                int len = std::max(0, args[3].i);
                if (static_cast<size_t>(off + len) <= barr.bytes.size()) {
                    sd.buffer.insert(sd.buffer.end(), barr.bytes.begin() + off, barr.bytes.begin() + off + len);
                }
            }
            return Slot(0);
        }
        if (m == "writeInt") {
            int32_t v = args[1].i;
            sd.buffer.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
            sd.buffer.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            sd.buffer.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            sd.buffer.push_back(static_cast<uint8_t>(v & 0xFF));
            return Slot(0);
        }
        if (m == "flush" || m == "close") {
            if (sd.sp_write_offset >= 0 && !sd.buffer.empty()) {
                size_t dst_off = sp_header_offset_ + static_cast<size_t>(sd.sp_write_offset);
                if (dst_off + sd.buffer.size() <= sp_data_.size()) {
                    std::memcpy(sp_data_.data() + dst_off, sd.buffer.data(), sd.buffer.size());
                    save_sav_overlay();
                }
            }
            return Slot(0);
        }
    }

    // ------------------------------------------------------------------------
    // 5. com.nttdocomo.ui.* (IApplication, Display, Canvas, Graphics, Image, Audio)
    // ------------------------------------------------------------------------
    if (c == "com/nttdocomo/ui/IApplication") {
        if (m == "<init>") {
            current_app_ref_ = args[0].r;
            return Slot(0);
        }
        if (m == "getCurrentApp") return Slot(current_app_ref_);
        if (m == "getSourceURL") return Slot(alloc_raw_string("http://localhost/"));
        if (m == "getArgs") {
            std::string ver = "10000";
            if (sp_header_offset_ + 410624 + 5 <= sp_data_.size()) {
                ver.assign(reinterpret_cast<const char*>(sp_data_.data() + sp_header_offset_ + 410624), 5);
            }
            Ref arr = alloc_ref_array(1);
            get(arr).fields_i32[0] = alloc_raw_string(ver);
            return Slot(arr);
        }
        static int s_browser_launch_frame = -999;
        if (m == "launch") {
            s_browser_launch_frame = frame_count_;
            g_ui.show_toast("已模拟访问卡普空会员站点 (点数已自动补充)", 120);
            return Slot(0);
        }
        if (m == "terminate") {
            if (s_browser_launch_frame == frame_count_) {
                s_browser_launch_frame = -999;
                return Slot(0);
            }
            clean_exit_runtime(*this, false, 0);
        }
    }

    if (c == "com/nttdocomo/ui/PhoneSystem" && m == "setAttribute") {
        return Slot(0);
    }

    if (c == "com/nttdocomo/ui/Display" && m == "setCurrent") {
        current_canvas_ref_ = args[0].r;
        return Slot(0);
    }

    if (c == "com/nttdocomo/ui/Canvas" || c == "app/CpCanvas") {
        if (m == "<init>") return Slot(0);
        if (m == "getWidth") return Slot(240);
        if (m == "getHeight") return Slot(240);
        if (m == "getGraphics") return Slot(screen_graphics_ref_);
        if (m == "getKeypadState") {
            if (keypad_override_enabled_) {
                return Slot(keypad_override_mask_);
            }
            poll_events();
            return Slot(keypad_state_);
        }
        if (m == "setSoftLabel") {
            if (suppress_av_) return Slot(0);
            int idx = args[1].i;
            std::string lbl = (args[2].r > 0) ? get(args[2].r).str : "";
            if (idx == 0) g_ui.soft_label_left = lbl;
            else if (idx == 1) g_ui.soft_label_right = lbl;
            return Slot(0);
        }
    }

    if (c == "com/nttdocomo/ui/Font") {
        if (m == "getFont") return Slot(default_font_ref_);
        if (m == "getDescent") return Slot(2);
        if (m == "stringWidth") {
            if (args[1].r <= 0) return Slot(0);
            auto cps = utf8_to_codepoints(get(args[1].r).str);
            int w = 0;
            for (uint32_t cp : cps) w += default_doja_char_width(cp);
            return Slot(w);
        }
    }

    if (c == "com/nttdocomo/ui/Graphics") {
        if (m == "getColorOfRGB") {
            uint32_t r = static_cast<uint32_t>(args[0].i & 0xFF);
            uint32_t g = static_cast<uint32_t>(args[1].i & 0xFF);
            uint32_t b = static_cast<uint32_t>(args[2].i & 0xFF);
            return Slot(static_cast<int32_t>(0xFF000000u | (r << 16) | (g << 8) | b));
        }
        HeapObject& gfx = get(args[0].r);
        if (m == "setColor") {
            gfx.aux_i32 = args[1].i;
            return Slot(0);
        }
        if (m == "setOrigin") {
            gfx.origin_x = args[1].i;
            gfx.origin_y = args[2].i;
            return Slot(0);
        }
        if (m == "setFlipMode") {
            gfx.flip_mode = args[1].i;
            return Slot(0);
        }
        if (m == "setFont") return Slot(0);
        if (suppress_av_) return Slot(0);
        if (m == "lock") {
            graphics_locked_ = true;
            if (gfx.target_image == 0) {
                g_interp.on_screen_lock(framebuffer_, fb_pix_text_, fb_hd_text_.data(), hd_scale_);
            }
            return Slot(0);
        }
        if (m == "unlock") {
            graphics_locked_ = false;
            if (gfx.target_image == 0) {
                present_screen();
            }
            return Slot(0);
        }
        if (m == "fillRect" || m == "drawRect") {
            int rx = args[1].i + gfx.origin_x;
            int ry = args[2].i + gfx.origin_y;
            int rw = args[3].i;
            int rh = args[4].i;
            uint32_t col = static_cast<uint32_t>(gfx.aux_i32) | 0xFF000000u;

            SurfaceTarget dst{framebuffer_, fb_pix_text_, fb_hd_text_.data(), 240, 240, hd_scale_};
            if (gfx.target_image > 0) {
                HeapObject& t_obj = get(gfx.target_image);
                if (t_obj.img && !t_obj.img->argb.empty()) {
                    dst.gfx = t_obj.img->argb.data();
                    dst.pix_text = t_obj.img->pix_text.empty() ? nullptr : t_obj.img->pix_text.data();
                    dst.hd_text = t_obj.img->hd_text.empty() ? nullptr : t_obj.img->hd_text.data();
                    dst.w = t_obj.img->width;
                    dst.h = t_obj.img->height;
                }
                g_interp.mark_offscreen_dirty(gfx.target_image);
            }
            bool is_fill = (m == "fillRect");
            blit_rect_surface(dst, is_fill, rx, ry, rw, rh, col);
            if (gfx.target_image == 0 && (g_interp.test_capture_override || (g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_))) {
                g_interp.record_rect(is_fill, rx, ry, rw, rh, col);
            }
            return Slot(0);
        }
        if (m == "drawImage") {
            Ref img_ref = args[1].r;
            if (img_ref <= 0) return Slot(0);
            HeapObject& img_o = get(img_ref);
            if (!img_o.img) return Slot(0);
            if (d == "(Lcom/nttdocomo/ui/Image;II)V") {
                draw_image_internal(gfx, img_ref, args[2].i, args[3].i, 0, 0, img_o.img->width, img_o.img->height, img_o.img->width, img_o.img->height);
            } else {
                draw_image_internal(gfx, img_ref, args[2].i, args[3].i, args[4].i, args[5].i, args[6].i, args[7].i, args[6].i, args[7].i);
            }
            return Slot(0);
        }
        if (m == "drawScaledImage") {
            draw_image_internal(gfx, args[1].r, args[2].i, args[3].i, args[6].i, args[7].i, args[8].i, args[9].i, args[4].i, args[5].i);
            return Slot(0);
        }
        if (m == "drawString") {
            if (args[1].r > 0) {
                draw_text_internal(gfx, get(args[1].r).str, args[2].i, args[3].i);
            }
            return Slot(0);
        }
    }

    if (c == "com/nttdocomo/ui/Image") {
        if (m == "createImage") {
            int w = std::max(1, args[0].i);
            int h = std::max(1, args[1].i);
            HeapObject img_o{};
            img_o.kind = ObjKind::Image;
            img_o.img = std::make_shared<ImageData>();
            img_o.img->width = w;
            img_o.img->height = h;
            img_o.img->is_paletted = false;
            img_o.img->transparent_enabled = false;
            img_o.img->argb.assign(static_cast<size_t>(w * h), 0xFF000000u);
            Ref img_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(img_o));

            HeapObject gfx_o{};
            gfx_o.kind = ObjKind::Graphics;
            gfx_o.aux_i32 = 0xFFFFFFFF;
            gfx_o.target_image = img_ref;
            Ref gfx_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(gfx_o));
            heap_[img_ref].img->graphics_ref = gfx_ref;
            return Slot(img_ref);
        }
        HeapObject& img_o = get(args[0].r);
        if (m == "getGraphics" && img_o.img) return Slot(img_o.img->graphics_ref);
        if (m == "setTransparentEnabled" && img_o.img) {
            img_o.img->transparent_enabled = (args[1].i != 0);
            return Slot(0);
        }
        if (m == "setTransparentColor" && img_o.img) {
            img_o.img->transparent_color = static_cast<uint32_t>(args[1].i);
            return Slot(0);
        }
    }

    if (c == "com/nttdocomo/ui/PalettedImage") {
        if (m == "createPalettedImage") {
            HeapObject& barr = get(args[0].r);
            HeapObject pal_o{};
            pal_o.kind = ObjKind::Palette;
            pal_o.pal = std::make_shared<PaletteData>();
            Ref pal_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(pal_o));

            HeapObject img_o{};
            img_o.kind = ObjKind::PalettedImage;
            img_o.img = std::make_shared<ImageData>();
            decode_gif(barr.bytes.data(), barr.bytes.size(), *img_o.img, *heap_[pal_ref].pal);
            img_o.img->palette_ref = pal_ref;
            Ref img_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(img_o));
            return Slot(img_ref);
        }
        HeapObject& img_o = get(args[0].r);
        if (m == "getPalette" && img_o.img) {
            // Return a new Palette cloned from the image's current palette
            HeapObject pal_o{};
            pal_o.kind = ObjKind::Palette;
            pal_o.pal = std::make_shared<PaletteData>();
            if (img_o.img->palette_ref > 0 && get(img_o.img->palette_ref).pal) {
                *pal_o.pal = *get(img_o.img->palette_ref).pal;
            }
            Ref pal_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(pal_o));
            return Slot(pal_ref);
        }
        if (m == "setPalette" && img_o.img) {
            img_o.img->palette_ref = args[1].r;
            return Slot(0);
        }
    }

    if (c == "com/nttdocomo/ui/Palette" && m == "setEntry") {
        if (suppress_av_) return Slot(0);
        HeapObject& pal_o = get(args[0].r);
        int idx = args[1].i;
        if (pal_o.pal && idx >= 0 && idx < 256) {
            pal_o.pal->entries[idx] = static_cast<uint32_t>(args[2].i) | 0xFF000000u;
        }
        return Slot(0);
    }

    if (c == "com/nttdocomo/ui/MediaManager") {
        if (m == "getImage") {
            std::string url = (args[0].r > 0) ? get(args[0].r).str : "";
            std::vector<uint8_t> bytes;
            if (url.rfind("resource:///", 0) == 0) {
                bytes = read_jar_Bytes(url.substr(12));
            }
            HeapObject pal_o{};
            pal_o.kind = ObjKind::Palette;
            pal_o.pal = std::make_shared<PaletteData>();
            Ref pal_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(pal_o));

            HeapObject img_o{};
            img_o.kind = ObjKind::PalettedImage;
            img_o.img = std::make_shared<ImageData>();
            decode_gif(bytes.data(), bytes.size(), *img_o.img, *heap_[pal_ref].pal);
            img_o.img->palette_ref = pal_ref;
            Ref img_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(img_o));

            HeapObject mi_o{};
            mi_o.kind = ObjKind::MediaImage;
            mi_o.aux_i32 = img_ref;
            Ref mi_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(mi_o));
            return Slot(mi_ref);
        }
        if (m == "getSound") {
            std::vector<uint8_t> mld_bytes;
            if (d == "(Ljava/lang/String;)Lcom/nttdocomo/ui/MediaSound;") {
                std::string url = (args[0].r > 0) ? get(args[0].r).str : "";
                if (url.rfind("resource:///", 0) == 0) {
                    mld_bytes = read_jar_Bytes(url.substr(12));
                }
            } else if (args[0].r > 0) {
                HeapObject& barr = get(args[0].r);
                mld_bytes = barr.bytes;
            }
            int snd_id = g_audio.load_mld(mld_bytes.data(), mld_bytes.size());
            HeapObject ms_o{};
            ms_o.kind = ObjKind::MediaSound;
            ms_o.aux_i32 = snd_id;
            Ref ms_ref = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(ms_o));
            return Slot(ms_ref);
        }
    }

    if (c == "com/nttdocomo/ui/MediaImage") {
        if (m == "use") return Slot(0);
        if (m == "getImage") return Slot(get(args[0].r).aux_i32);
    }
    if (c == "com/nttdocomo/ui/MediaSound" && m == "use") return Slot(0);

    if (c == "com/nttdocomo/ui/AudioPresenter") {
        if (m == "getAudioPresenter") {
            int port = (num_args >= 1) ? args[0].i : 0;
            if (port < 0 || port >= 4) {
                throw JavaException(1);
            }
            // Keep AudioPresenter handles outside heap_ so rollback heap truncation never invalidates
            // CpCanvas.m_audio_se[1..3], while giving every startup BGM/jingle presenter (audio[0..15],
            // m_audio[0..29]) its own permanent independent slot so setSound() bindings never alias!
            size_t idx = 0;
            if (audio_presenters_.size() < kPermanentAudioPresenters + kDynamicAudioPresenterRing) {
                idx = audio_presenters_.size();
                audio_presenters_.push_back(AudioPresenterSlot{port, 0, 100});
                if (idx >= kPermanentAudioPresenters) {
                    dynamic_audio_presenter_cursor_ = (idx - kPermanentAudioPresenters + 1) % kDynamicAudioPresenterRing;
                }
            } else {
                idx = kPermanentAudioPresenters + dynamic_audio_presenter_cursor_;
                dynamic_audio_presenter_cursor_ = (dynamic_audio_presenter_cursor_ + 1) % kDynamicAudioPresenterRing;
                audio_presenters_[idx] = AudioPresenterSlot{port, 0, 100};
            }
            return Slot(kAudioPresenterTag | static_cast<Ref>(idx));
        }
        if ((args[0].r & 0x70000000) != kAudioPresenterTag) {
            return Slot(0);
        }
        size_t idx = static_cast<size_t>(args[0].r & 0x0FFFFFFF);
        if (idx >= audio_presenters_.size()) {
            return Slot(0);
        }
        AudioPresenterSlot& ap = audio_presenters_[idx];
        if (m == "setSound") {
            ap.sound_ref = args[1].r;
            return Slot(0);
        }
        if (m == "setAttribute") {
            if (suppress_av_) return Slot(0);
            if (args[1].i == 4) {
                ap.volume = std::clamp(args[2].i, 0, 100);
                g_audio.set_port_volume(ap.port, args[0].r, ap.volume);
            }
            return Slot(0);
        }
        if (m == "play") {
            if (suppress_av_) return Slot(0);
            if (ap.sound_ref > 0 && static_cast<size_t>(ap.sound_ref) < heap_.size()) {
                int snd_id = heap_[ap.sound_ref].aux_i32;
                g_audio.play_port(ap.port, snd_id, ap.volume, args[0].r, current_time_millis());
            }
            return Slot(0);
        }
        if (m == "stop") {
            if (suppress_av_) return Slot(0);
            g_audio.stop_port(ap.port, args[0].r);
            return Slot(0);
        }
        if (m == "getCurrentTime") {
            return Slot(g_audio.get_current_time(ap.port, args[0].r, current_time_millis()));
        }
    }

    if (c == "com/nttdocomo/ui/Dialog") {
        if (m == "<init>" || m == "setText") return Slot(0);
        if (m == "show") return Slot(0);
    }

    if (c == "com/nttdocomo/io/HttpConnection") {
        Ref hc_ref = args[0].r;
        if (hc_ref <= 0) return Slot(0);
        HttpConnState& st = g_http_conns[hc_ref];
        if (st.url.empty()) {
            st.url = get(hc_ref).str;
        }

        auto ensure_connected = [&]() {
            if (st.connected) return;
            st.connected = true;
            st.req_headers["X-RMEXE-UID"] = g_ui.player_uid;
            st.req_headers["X-RMEXE-NAME"] = net::url_encode(g_ui.player_name);
            const std::vector<uint8_t> empty_body;
            const std::vector<uint8_t>& req_body = st.req_stream ? st.req_stream->buffer : empty_body;

            bool handled_online = false;
            if (g_ui.online_mode && !g_ui.server_url.empty()) {
                if (net::http_request_tcp(g_ui.server_url, st.method, st.url, st.req_headers, req_body, st.resp, 2500)) {
                    handled_online = true;
                } else {
                    g_ui.show_toast("服务器未连接，已自动回退本地模拟", 90);
                }
            }
            if (!handled_online) {
                st.resp = g_capcom_local.handle_request(st.method, st.url, st.req_headers, req_body);
            }
        };

        if (m == "setRequestMethod") {
            if (args[1].r > 0) st.method = get(args[1].r).str;
            return Slot(0);
        }
        if (m == "setRequestProperty") {
            if (args[1].r > 0 && args[2].r > 0) {
                st.req_headers[get(args[1].r).str] = get(args[2].r).str;
            }
            return Slot(0);
        }
        if (m == "openOutputStream") {
            if (!st.req_stream) {
                st.req_stream = std::make_shared<StreamData>();
            }
            HeapObject st_obj{};
            st_obj.kind = ObjKind::OutputStream;
            st_obj.stream = st.req_stream;
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(st_obj));
            return Slot(r);
        }
        if (m == "connect") {
            ensure_connected();
            return Slot(0);
        }
        if (m == "getHeaderField") {
            ensure_connected();
            std::string key = (args[1].r > 0) ? get(args[1].r).str : "";
            std::string val = st.resp.get_header(key);
            if (val.empty()) return Slot(Ref(0));
            return Slot(alloc_raw_string(val));
        }
        if (m == "openInputStream") {
            ensure_connected();
            HeapObject st_obj{};
            st_obj.kind = ObjKind::InputStream;
            st_obj.stream = std::make_shared<StreamData>();
            st_obj.stream->buffer = st.resp.body;
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(st_obj));
            return Slot(r);
        }
        if (m == "close") {
            g_http_conns.erase(hc_ref);
            return Slot(0);
        }
        return Slot(0);
    }

    if (c == "com/nttdocomo/net/URLEncoder" && m == "encode") {
        if (args[0].r <= 0) return Slot(Ref(0));
        return Slot(alloc_raw_string(net::url_encode(get(args[0].r).str)));
    }

    return Slot(0);
}

} // namespace rmexe
