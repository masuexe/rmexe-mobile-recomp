#include "doja_runtime.hpp"

#include <SDL2/SDL.h>
#include <zlib.h>
#include <iconv.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

namespace rmexe {

DojaRuntime* g_runtime = nullptr;

// ============================================================================
// Dynamic FreeType2 loader for crisp CJK (Japanese) + English 12px font rendering
// ============================================================================
namespace {

struct FreeTypeFont {
    void* lib_handle = nullptr;
    void* ft_library = nullptr;
    void* ft_face = nullptr;
    bool initialized = false;

    using fn_Init = int (*)(void**);
    using fn_NewFace = int (*)(void*, const char*, long, void**);
    using fn_SetPixelSizes = int (*)(void*, unsigned int, unsigned int);
    using fn_LoadChar = int (*)(void*, unsigned long, int);

    fn_LoadChar load_char = nullptr;

    void init() {
        if (initialized) return;
        initialized = true;
        lib_handle = dlopen("libfreetype.so.6", RTLD_LAZY);
        if (!lib_handle) return;

        auto init_ft = reinterpret_cast<fn_Init>(dlsym(lib_handle, "FT_Init_FreeType"));
        auto new_face = reinterpret_cast<fn_NewFace>(dlsym(lib_handle, "FT_New_Face"));
        auto set_sizes = reinterpret_cast<fn_SetPixelSizes>(dlsym(lib_handle, "FT_Set_Pixel_Sizes"));
        load_char = reinterpret_cast<fn_LoadChar>(dlsym(lib_handle, "FT_Load_Char"));
        if (!init_ft || !new_face || !set_sizes || !load_char) return;

        if (init_ft(&ft_library) != 0) return;
        const char* font_paths[] = {
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        };
        for (const char* path : font_paths) {
            if (new_face(ft_library, path, 0, &ft_face) == 0 && ft_face) {
                set_sizes(ft_face, 0, 12);
                break;
            }
        }
    }
};

static FreeTypeFont g_ft;

// Convert Shift-JIS (CP932) bytes to UTF-8 string
std::string cp932_to_utf8(const uint8_t* data, size_t len) {
    if (len == 0) return "";
    // Strip trailing NULs if present
    size_t actual_len = 0;
    while (actual_len < len && data[actual_len] != 0) {
        actual_len++;
    }
    if (actual_len == 0) return "";

    iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        return std::string(reinterpret_cast<const char*>(data), actual_len);
    }
    std::string out(actual_len * 4 + 4, '\0');
    char* inbuf = const_cast<char*>(reinterpret_cast<const char*>(data));
    size_t inbytes = actual_len;
    char* outbuf = out.data();
    size_t outbytes = out.size();
    iconv(cd, &inbuf, &inbytes, &outbuf, &outbytes);
    iconv_close(cd);
    out.resize(out.size() - outbytes);
    return out;
}

// Convert UTF-8 string back to CP932 bytes
std::vector<uint8_t> utf8_to_cp932(const std::string& utf8) {
    if (utf8.empty()) return {};
    iconv_t cd = iconv_open("CP932", "UTF-8");
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        return std::vector<uint8_t>(utf8.begin(), utf8.end());
    }
    std::vector<uint8_t> out(utf8.size() * 2 + 4, 0);
    char* inbuf = const_cast<char*>(utf8.data());
    size_t inbytes = utf8.size();
    char* outbuf = reinterpret_cast<char*>(out.data());
    size_t outbytes = out.size();
    iconv(cd, &inbuf, &inbytes, &outbuf, &outbytes);
    iconv_close(cd);
    out.resize(out.size() - outbytes);
    return out;
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

} // namespace

// ============================================================================
// DojaRuntime implementation
// ============================================================================

DojaRuntime::DojaRuntime() {
    g_runtime = this;
    heap_.reserve(65536);
    // Index 0 is null
    heap_.push_back(HeapObject{});
    // Index 1 is shared Exception / System.out placeholder
    HeapObject exc{};
    exc.kind = ObjKind::ExceptionObj;
    heap_.push_back(exc);
}

DojaRuntime::~DojaRuntime() {
    shutdown();
}

bool DojaRuntime::init(const std::string& jar_path, const std::string& sp_path, const std::string& /*jam_path*/, int scale) {
    jar_path_ = jar_path;
    sp_path_ = sp_path;
    sav_path_ = sp_path + ".sav";
    window_scale_ = std::max(1, scale);

    g_ft.init();

    // Load ScratchPad (.sp) in memory (Copy-on-Write)
    std::ifstream sp_file(sp_path_, std::ios::binary);
    if (!sp_file) {
        std::cerr << "Failed to open ScratchPad file: " << sp_path_ << "\n";
        return false;
    }
    sp_data_.assign(std::istreambuf_iterator<char>(sp_file), std::istreambuf_iterator<char>());
    // Detect 64-byte DoJa SDK header (e.g. 511,964 or 512,064 bytes)
    sp_header_offset_ = (sp_data_.size() % 100 == 64) ? 64 : 0;
    load_sav_overlay();

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

    // Initialize SDL2 window & renderer (unless headless scale <= 0)
    if (scale > 0) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
            std::cerr << "SDL_Init warning: " << SDL_GetError() << "\n";
        } else {
            SDL_Window* win = SDL_CreateWindow(
                "Rockman EXE Mobile Recomp (PoN / LoN)",
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                240 * window_scale_,
                240 * window_scale_,
                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
            );
            if (win) {
                sdl_window_ = win;
                SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                if (!ren) {
                    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
                }
                sdl_renderer_ = ren;
                if (ren) {
                    SDL_RenderSetLogicalSize(ren, 240, 240);
                    sdl_texture_ = SDL_CreateTexture(
                        ren,
                        SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING,
                        240,
                        240
                    );
                }
            }
        }
    }

    std::fill(std::begin(framebuffer_), std::end(framebuffer_), 0xFF000000u);
    return true;
}

void DojaRuntime::shutdown() {
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
    SDL_Quit();
}

void DojaRuntime::load_sav_overlay() {
    std::ifstream sav(sav_path_, std::ios::binary);
    if (!sav) return;
    // Overlay save header region (first 4096 bytes after sp_header_offset_)
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(sav)), std::istreambuf_iterator<char>());
    if (sp_header_offset_ + buf.size() <= sp_data_.size()) {
        std::memcpy(sp_data_.data() + sp_header_offset_, buf.data(), buf.size());
    }
}

void DojaRuntime::save_sav_overlay() {
    // Save the mutable save segment (first 4096 bytes of ScratchPad) to .sav
    size_t save_len = std::min<size_t>(4096, sp_data_.size() - sp_header_offset_);
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

Ref DojaRuntime::alloc_string(const std::string& utf8) {
    HeapObject obj{};
    obj.kind = ObjKind::String;
    obj.str = utf8;
    Ref r = static_cast<Ref>(heap_.size());
    heap_.push_back(std::move(obj));
    return r;
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
        throw JavaException(1);
    }
    return o.fields_i32[idx];
}

int32_t DojaRuntime::baload(Ref arr, int32_t idx) {
    HeapObject& o = get(arr);
    if (idx < 0 || static_cast<size_t>(idx) >= o.bytes.size()) {
        throw JavaException(1);
    }
    return static_cast<int8_t>(o.bytes[idx]);
}

void DojaRuntime::bastore(Ref arr, int32_t idx, int32_t val) {
    HeapObject& o = get(arr);
    if (idx < 0 || static_cast<size_t>(idx) >= o.bytes.size()) {
        throw JavaException(1);
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
    HeapObject& img_obj = get(img_ref);
    if (!img_obj.img) return;
    ImageData& img = *img_obj.img;

    const PaletteData* pal = nullptr;
    if (img.is_paletted && img.palette_ref > 0) {
        pal = get(img.palette_ref).pal.get();
    }

    uint32_t* dst_buf = framebuffer_;
    int dst_w = 240;
    int dst_h = 240;
    if (gfx.target_image > 0) {
        HeapObject& t_obj = get(gfx.target_image);
        if (t_obj.img && !t_obj.img->argb.empty()) {
            dst_buf = t_obj.img->argb.data();
            dst_w = t_obj.img->width;
            dst_h = t_obj.img->height;
        }
    }

    int base_dx = dx + gfx.origin_x;
    int base_dy = dy + gfx.origin_y;
    bool flip_h = (gfx.flip_mode == 1 || gfx.flip_mode == 3);
    bool flip_v = (gfx.flip_mode == 2 || gfx.flip_mode == 3);

    for (int py = 0; py < dh; ++py) {
        int out_y = base_dy + py;
        if (out_y < 0 || out_y >= dst_h) continue;
        int rel_sy = (py * sh) / dh;
        if (flip_v) rel_sy = sh - 1 - rel_sy;
        int src_y = sy + rel_sy;
        if (src_y < 0 || src_y >= img.height) continue;

        for (int px = 0; px < dw; ++px) {
            int out_x = base_dx + px;
            if (out_x < 0 || out_x >= dst_w) continue;
            int rel_sx = (px * sw) / dw;
            if (flip_h) rel_sx = sw - 1 - rel_sx;
            int src_x = sx + rel_sx;
            if (src_x < 0 || src_x >= img.width) continue;

            if (img.is_paletted) {
                uint8_t idx = img.indices[src_y * img.width + src_x];
                if (img.transparent_enabled && idx == img.transparent_index) continue;
                uint32_t col = pal ? pal->entries[idx] : 0xFFFF00FFu;
                dst_buf[out_y * dst_w + out_x] = col | 0xFF000000u;
            } else {
                uint32_t col = img.argb[src_y * img.width + src_x];
                if ((col >> 24) == 0) continue;
                if (img.transparent_enabled && ((col & 0xFFFFFFu) == (img.transparent_color & 0xFFFFFFu))) {
                    continue;
                }
                dst_buf[out_y * dst_w + out_x] = col | 0xFF000000u;
            }
        }
    }
}

void DojaRuntime::draw_text_internal(HeapObject& gfx, const std::string& text, int x, int y) {
    uint32_t* dst_buf = framebuffer_;
    int dst_w = 240;
    int dst_h = 240;
    if (gfx.target_image > 0) {
        HeapObject& t_obj = get(gfx.target_image);
        if (t_obj.img && !t_obj.img->argb.empty()) {
            dst_buf = t_obj.img->argb.data();
            dst_w = t_obj.img->width;
            dst_h = t_obj.img->height;
        }
    }

    uint32_t color = static_cast<uint32_t>(gfx.aux_i32) | 0xFF000000u;
    int pen_x = x + gfx.origin_x;
    int baseline_y = y + gfx.origin_y;

    auto cps = utf8_to_codepoints(text);
    uint8_t sr = (color >> 16) & 0xFF;
    uint8_t sg = (color >> 8) & 0xFF;
    uint8_t sb = color & 0xFF;

    for (uint32_t cp : cps) {
        bool rendered_ft = false;
        if (g_ft.ft_face && g_ft.load_char) {
            // FT_LOAD_RENDER == 4
            if (g_ft.load_char(g_ft.ft_face, cp, 4) == 0) {
                const uint8_t* face_bytes = reinterpret_cast<const uint8_t*>(g_ft.ft_face);
                const uint8_t* glyph_ptr = *reinterpret_cast<const uint8_t* const*>(face_bytes + 0x98);
                if (glyph_ptr) {
                    int32_t adv_x = *reinterpret_cast<const int32_t*>(glyph_ptr + 0x80) >> 6;
                    uint32_t rows = *reinterpret_cast<const uint32_t*>(glyph_ptr + 0x98);
                    uint32_t width = *reinterpret_cast<const uint32_t*>(glyph_ptr + 0x9c);
                    int32_t pitch = *reinterpret_cast<const int32_t*>(glyph_ptr + 0xa0);
                    const uint8_t* buf = *reinterpret_cast<const uint8_t* const*>(glyph_ptr + 0xa8);
                    int32_t bmp_left = *reinterpret_cast<const int32_t*>(glyph_ptr + 0xc0);
                    int32_t bmp_top = *reinterpret_cast<const int32_t*>(glyph_ptr + 0xc4);

                    if (buf && rows < 64 && width < 64) {
                        for (uint32_t ry = 0; ry < rows; ++ry) {
                            int dy = baseline_y - bmp_top + static_cast<int>(ry);
                            if (dy < 0 || dy >= dst_h) continue;
                            for (uint32_t rx = 0; rx < width; ++rx) {
                                int dx = pen_x + bmp_left + static_cast<int>(rx);
                                if (dx < 0 || dx >= dst_w) continue;
                                uint8_t alpha = buf[ry * pitch + rx];
                                if (alpha > 96) {
                                    // Crisp pixel-art threshold or alpha blend
                                    dst_buf[dy * dst_w + dx] = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
                                }
                            }
                        }
                    }
                    pen_x += (adv_x > 0) ? adv_x : ((cp < 0x80) ? 6 : 12);
                    rendered_ft = true;
                }
            }
        }
        if (!rendered_ft) {
            int adv = (cp < 0x80) ? 6 : 12;
            if (cp > 0x20) {
                for (int gy = 0; gy < 10; ++gy) {
                    int dy = baseline_y - 9 + gy;
                    if (dy < 0 || dy >= dst_h) continue;
                    for (int gx = 0; gx < adv - 1; ++gx) {
                        int dx = pen_x + gx;
                        if (dx < 0 || dx >= dst_w) continue;
                        bool on = ((cp >> ((gx + gy) % 7)) & 1) != 0;
                        if (on) dst_buf[dy * dst_w + dx] = color;
                    }
                }
            }
            pen_x += adv;
        }
    }
}

void DojaRuntime::poll_events() {
    if (!sdl_window_) return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            save_sav_overlay();
            std::exit(0);
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            bool pressed = (ev.type == SDL_KEYDOWN);
            int doja_key = -1;
            switch (ev.key.keysym.sym) {
                case SDLK_LEFT:  case SDLK_a: doja_key = 0x10; break; // KEY_LEFT
                case SDLK_UP:    case SDLK_w: doja_key = 0x11; break; // KEY_UP
                case SDLK_RIGHT: case SDLK_d: doja_key = 0x12; break; // KEY_RIGHT
                case SDLK_DOWN:  case SDLK_s: doja_key = 0x13; break; // KEY_DOWN
                case SDLK_z: case SDLK_j: case SDLK_RETURN: case SDLK_SPACE:
                    doja_key = 0x14; break; // KEY_SELECT (A button)
                case SDLK_x: case SDLK_k:
                    doja_key = 0x0A; break; // KEY_ASTERISK (B button / Buster in battle)
                case SDLK_q: case SDLK_u: case SDLK_c:
                    doja_key = 0x15; break; // KEY_SOFT1 (L / Left Softkey / Custom)
                case SDLK_e: case SDLK_i: case SDLK_v: case SDLK_BACKSPACE:
                    doja_key = 0x16; break; // KEY_SOFT2 (R / Right Softkey / Menu / Cancel)
                case SDLK_0: doja_key = 0x00; break;
                case SDLK_1: doja_key = 0x01; break;
                case SDLK_2: doja_key = 0x02; break;
                case SDLK_3: doja_key = 0x03; break;
                case SDLK_4: doja_key = 0x04; break;
                case SDLK_5: doja_key = 0x05; break;
                case SDLK_6: doja_key = 0x06; break;
                case SDLK_7: doja_key = 0x07; break;
                case SDLK_8: doja_key = 0x08; break;
                case SDLK_9: doja_key = 0x09; break;
                case SDLK_ESCAPE:
                    save_sav_overlay();
                    std::exit(0);
                default: break;
            }
            if (doja_key >= 0) {
                if (pressed) {
                    keypad_state_ |= (1 << doja_key);
                    if (current_canvas_ref_ > 0) {
                        generated_process_event(*this, current_canvas_ref_, 0, doja_key);
                    }
                } else {
                    keypad_state_ &= ~(1 << doja_key);
                    if (current_canvas_ref_ > 0) {
                        generated_process_event(*this, current_canvas_ref_, 1, doja_key);
                    }
                }
            }
        }
    }
}

void DojaRuntime::present_screen() {
    ++frame_count_;
    for (const auto& kv : injected_keys_) {
        if (kv.first == frame_count_) {
            keypad_state_ |= (1 << kv.second);
            if (current_canvas_ref_ > 0) {
                generated_process_event(*this, current_canvas_ref_, 0, kv.second);
            }
        } else if (kv.first + 1 == frame_count_) {
            keypad_state_ &= ~(1 << kv.second);
            if (current_canvas_ref_ > 0) {
                generated_process_event(*this, current_canvas_ref_, 1, kv.second);
            }
        }
    }
    poll_events();
    if (sdl_renderer_ && sdl_texture_) {
        SDL_Renderer* ren = static_cast<SDL_Renderer*>(sdl_renderer_);
        SDL_Texture* tex = static_cast<SDL_Texture*>(sdl_texture_);
        SDL_UpdateTexture(tex, nullptr, framebuffer_, 240 * sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    }
    if (max_frames_ > 0 && frame_count_ >= max_frames_) {
        if (!dump_ppm_path_.empty()) {
            std::ofstream ppm(dump_ppm_path_, std::ios::binary | std::ios::trunc);
            if (ppm) {
                ppm << "P6\n240 240\n255\n";
                for (int i = 0; i < 240 * 240; ++i) {
                    uint32_t c = framebuffer_[i];
                    uint8_t rgb[3] = {
                        static_cast<uint8_t>((c >> 16) & 0xFF),
                        static_cast<uint8_t>((c >> 8) & 0xFF),
                        static_cast<uint8_t>(c & 0xFF)
                    };
                    ppm.write(reinterpret_cast<const char*>(rgb), 3);
                }
            }
        }
        save_sav_overlay();
        std::exit(0);
    }
}

int64_t DojaRuntime::current_time_millis() {
    poll_events();
    if (fast_forward_) {
        fake_time_ms_ += 20;
        return fake_time_ms_;
    }
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void DojaRuntime::sleep_ms(int64_t ms) {
    poll_events();
    if (fast_forward_) return;
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

Slot DojaRuntime::invoke_external(const char* cls, const char* name, const char* desc, Slot* args, int /*num_args*/) {
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
            return Slot(alloc_string(get(args[0].r).aux_i32 ? "true" : "false"));
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
        if (m == "<init>") {
            HeapObject& s_obj = get(args[0].r);
            s_obj.kind = ObjKind::String;
            HeapObject& b_arr = get(args[1].r);
            if (d == "([B)V") {
                s_obj.str = cp932_to_utf8(b_arr.bytes.data(), b_arr.bytes.size());
            } else if (d == "([BII)V") {
                int off = std::max(0, args[2].i);
                int len = std::max(0, args[3].i);
                if (static_cast<size_t>(off + len) <= b_arr.bytes.size()) {
                    s_obj.str = cp932_to_utf8(b_arr.bytes.data() + off, static_cast<size_t>(len));
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
            return Slot(alloc_string(codepoints_to_utf8(cps, b, e)));
        }
        if (m == "getBytes") {
            auto bytes = utf8_to_cp932(s);
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
            return Slot(alloc_string(sb.str));
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
        if (m == "getSourceURL") return Slot(alloc_string("http://localhost/"));
        if (m == "getArgs") {
            std::string ver = "10000";
            if (sp_header_offset_ + 410624 + 5 <= sp_data_.size()) {
                ver.assign(reinterpret_cast<const char*>(sp_data_.data() + sp_header_offset_ + 410624), 5);
            }
            Ref arr = alloc_ref_array(1);
            get(arr).fields_i32[0] = alloc_string(ver);
            return Slot(arr);
        }
        if (m == "launch") return Slot(0);
        if (m == "terminate") {
            save_sav_overlay();
            std::exit(0);
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
            poll_events();
            return Slot(keypad_state_);
        }
        if (m == "setSoftLabel") return Slot(0);
    }

    if (c == "com/nttdocomo/ui/Font") {
        if (m == "getFont") return Slot(default_font_ref_);
        if (m == "getDescent") return Slot(2);
        if (m == "stringWidth") {
            if (args[1].r <= 0) return Slot(0);
            auto cps = utf8_to_codepoints(get(args[1].r).str);
            int w = 0;
            for (uint32_t cp : cps) w += (cp < 0x80) ? 6 : 12;
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
        if (m == "lock") {
            graphics_locked_ = true;
            return Slot(0);
        }
        if (m == "unlock") {
            graphics_locked_ = false;
            present_screen();
            return Slot(0);
        }
        if (m == "fillRect" || m == "drawRect") {
            int rx = args[1].i + gfx.origin_x;
            int ry = args[2].i + gfx.origin_y;
            int rw = args[3].i;
            int rh = args[4].i;
            uint32_t col = static_cast<uint32_t>(gfx.aux_i32) | 0xFF000000u;

            uint32_t* dst_buf = framebuffer_;
            int dst_w = 240;
            int dst_h = 240;
            if (gfx.target_image > 0) {
                HeapObject& t_obj = get(gfx.target_image);
                if (t_obj.img && !t_obj.img->argb.empty()) {
                    dst_buf = t_obj.img->argb.data();
                    dst_w = t_obj.img->width;
                    dst_h = t_obj.img->height;
                }
            }
            if (m == "fillRect") {
                for (int y = std::max(0, ry); y < std::min(dst_h, ry + rh); ++y) {
                    for (int x = std::max(0, rx); x < std::min(dst_w, rx + rw); ++x) {
                        dst_buf[y * dst_w + x] = col;
                    }
                }
            } else {
                for (int x = std::max(0, rx); x <= std::min(dst_w - 1, rx + rw); ++x) {
                    if (ry >= 0 && ry < dst_h) dst_buf[ry * dst_w + x] = col;
                    if (ry + rh >= 0 && ry + rh < dst_h) dst_buf[(ry + rh) * dst_w + x] = col;
                }
                for (int y = std::max(0, ry); y <= std::min(dst_h - 1, ry + rh); ++y) {
                    if (rx >= 0 && rx < dst_w) dst_buf[y * dst_w + rx] = col;
                    if (rx + rw >= 0 && rx + rw < dst_w) dst_buf[y * dst_w + (rx + rw)] = col;
                }
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
            HeapObject ms_o{};
            ms_o.kind = ObjKind::MediaSound;
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
            HeapObject ap{};
            ap.kind = ObjKind::AudioPresenter;
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(ap));
            return Slot(r);
        }
        if (m == "setSound" || m == "setAttribute" || m == "play" || m == "stop") return Slot(0);
        if (m == "getCurrentTime") return Slot(100);
    }

    if (c == "com/nttdocomo/ui/Dialog") {
        if (m == "<init>" || m == "setText") return Slot(0);
        if (m == "show") return Slot(0);
    }

    if (c == "com/nttdocomo/io/HttpConnection") {
        if (m == "setRequestMethod" || m == "setRequestProperty" || m == "connect" || m == "close") return Slot(0);
        if (m == "getHeaderField") return Slot(alloc_string("0"));
        if (m == "openInputStream" || m == "openOutputStream") {
            HeapObject st_obj{};
            st_obj.kind = ObjKind::InputStream;
            st_obj.stream = std::make_shared<StreamData>();
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(st_obj));
            return Slot(r);
        }
    }

    if (c == "com/nttdocomo/net/URLEncoder" && m == "encode") {
        return Slot(args[0].r);
    }

    return Slot(0);
}

} // namespace rmexe
