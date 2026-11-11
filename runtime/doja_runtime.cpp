#include "doja_runtime.hpp"
#include "net_config.hpp"
#include "../common/pvp_protocol.hpp"
#include "../common/ymu762_fm.hpp"

#include <SDL2/SDL.h>
#include <zlib.h>
#include <iconv.h>
#include <dlfcn.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace rmexe {

DojaRuntime* g_runtime = nullptr;

// ============================================================================
// Dynamic FreeType2 loader for crisp CJK (Japanese) + English 12px font rendering
// ============================================================================
namespace {

std::atomic<bool> g_quit_signal{false};

void on_termination_signal(int sig) {
    g_quit_signal.store(true, std::memory_order_relaxed);
    std::signal(sig, SIG_DFL);
}

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
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
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
    ACT_TURBO,
    ACT_COUNT
};

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

static std::string generate_random_uid() {
    std::random_device rd;
    uint32_t v = rd();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "NV%06X", v & 0xFFFFFFu);
    return std::string(buf);
}

struct UiInputManager {
    std::string config_path = "controls.cfg";
    bool show_softkey_bar = true;
    bool high_fps_mode = true; // true = 60 FPS smooth interpolation, false = 15 FPS original flip-phone rate
    bool is_fullscreen = false;
    std::string soft_label_left;
    std::string soft_label_right;

    // Network & PvP settings (persisted in controls.cfg, overridable via CLI)
    bool online_mode = true;
    std::string server_url = "111.229.100.54:8765";
    std::string player_uid = generate_random_uid();
    std::string player_name = "ROCKMAN";
    bool is_lon_game = false;
    int pvp_menu_choice = 0; // 0 = Create Room, 1 = Join Room

    bool has_cli_net_override = false;
    bool cli_online_mode = false;
    bool has_cli_fps_override = false;
    bool cli_high_fps_mode = true;
    std::string cli_server_url;
    std::string cli_player_uid;
    std::string cli_player_name;
    std::string custom_sav_path;
    int cli_pvp_mode = 0;         // 0 = none, 1 = create, 2 = join
    uint32_t cli_pvp_room_id = 0;
    int cli_pvp_join_retries = 0;

    std::string toast_text;
    int toast_frames = 0;

    int64_t paused_duration_ms = 0;
    int64_t turbo_bonus_ms = 0;

    ActionBinding binds[ACT_COUNT];
    std::vector<SDL_GameController*> controllers;
    uint32_t display_buf[240 * 260] = {0};

    UiInputManager() {
        reset_defaults();
    }

    void reset_defaults() {
        binds[ACT_UP]     = {"up",     "上移",     0x11, {SDLK_UP,    SDLK_w}, SDL_CONTROLLER_BUTTON_DPAD_UP};
        binds[ACT_DOWN]   = {"down",   "下移",     0x13, {SDLK_DOWN,  SDLK_s}, SDL_CONTROLLER_BUTTON_DPAD_DOWN};
        binds[ACT_LEFT]   = {"left",   "左移",     0x10, {SDLK_LEFT,  SDLK_a}, SDL_CONTROLLER_BUTTON_DPAD_LEFT};
        binds[ACT_RIGHT]  = {"right",  "右移",     0x12, {SDLK_RIGHT, SDLK_d}, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
        binds[ACT_SELECT] = {"select", "决定/A",   0x14, {SDLK_z,     SDLK_j}, SDL_CONTROLLER_BUTTON_A};
        binds[ACT_CANCEL] = {"cancel", "取消/B",   0x0A, {SDLK_x,     SDLK_k}, SDL_CONTROLLER_BUTTON_B};
        binds[ACT_SOFT1]  = {"soft1",  "左软键/L", 0x15, {SDLK_q,     SDLK_u}, SDL_CONTROLLER_BUTTON_LEFTSHOULDER};
        binds[ACT_SOFT2]  = {"soft2",  "右软键/R", 0x16, {SDLK_e,     SDLK_i}, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
        binds[ACT_POUND]  = {"pound",  "井号/#",   0x0B, {SDLK_c,     SDLK_l}, SDL_CONTROLLER_BUTTON_X};
        binds[ACT_TURBO]  = {"turbo",  "按住倍速", -1,   {SDLK_TAB,   SDLK_BACKQUOTE}, SDL_CONTROLLER_BUTTON_Y};
    }

    void clear_held_states() {
        for (int i = 0; i < ACT_COUNT; ++i) {
            binds[i].key_down[0] = false;
            binds[i].key_down[1] = false;
            binds[i].pad_down = false;
            binds[i].axis_down = false;
            binds[i].effective_down = false;
        }
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
        return static_cast<int>(SDL_GameControllerGetButtonFromString(v.c_str()));
    }

    static std::string format_pad_cfg(int btn) {
        if (btn < 0) return "None";
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
            default: return "---";
        }
    }

    std::string primary_soft_key_label(int act_idx) const {
        if (binds[act_idx].keys[0] != SDLK_UNKNOWN) return short_key_name(binds[act_idx].keys[0]);
        if (binds[act_idx].keys[1] != SDLK_UNKNOWN) return short_key_name(binds[act_idx].keys[1]);
        if (binds[act_idx].pad_btn >= 0) return short_pad_name(binds[act_idx].pad_btn);
        return "-";
    }

    void apply_cli_overrides() {
        if (has_cli_net_override) {
            online_mode = cli_online_mode;
            if (!cli_server_url.empty()) server_url = cli_server_url;
        }
        if (has_cli_fps_override) {
            high_fps_mode = cli_high_fps_mode;
        }
        if (!cli_player_uid.empty()) player_uid = cli_player_uid;
        if (!cli_player_name.empty()) player_name = cli_player_name;
    }

    void load_config() {
        std::ifstream f(config_path);
        if (!f) {
            apply_cli_overrides();
            return;
        }
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
            } else if (key == "online_mode") {
                online_mode = (std::atoi(val.c_str()) != 0);
            } else if (key == "server_url" && !val.empty()) {
                server_url = val;
            } else if (key == "player_uid" && !val.empty()) {
                player_uid = val;
            } else if (key == "player_name" && !val.empty()) {
                player_name = val;
            } else {
                for (int i = 0; i < ACT_COUNT; ++i) {
                    std::string pfx = std::string(binds[i].cfg_id) + "_";
                    if (key == pfx + "key1") binds[i].keys[0] = parse_key_str(val);
                    else if (key == pfx + "key2") binds[i].keys[1] = parse_key_str(val);
                    else if (key == pfx + "pad")  binds[i].pad_btn = parse_pad_str(val);
                }
            }
        }
        apply_cli_overrides();
    }

    void save_config() const {
        std::ofstream f(config_path, std::ios::trunc);
        if (!f) return;
        f << "# rmexe-mobile-recomp keybindings, frame rate, network & emulator settings\n";
        {
            std::lock_guard<std::mutex> lk(g_audio.mtx);
            f << "master_volume=" << g_audio.master_volume << "\n";
            f << "muted=" << (g_audio.muted ? 1 : 0) << "\n";
        }
        f << "high_fps_mode=" << (high_fps_mode ? 1 : 0) << "\n";
        f << "show_softkey_bar=" << (show_softkey_bar ? 1 : 0) << "\n";
        f << "online_mode=" << (online_mode ? 1 : 0) << "\n";
        f << "server_url=" << server_url << "\n";
        f << "player_uid=" << player_uid << "\n";
        f << "player_name=" << player_name << "\n\n";
        for (int i = 0; i < ACT_COUNT; ++i) {
            f << binds[i].cfg_id << "_key1=" << format_key_cfg(binds[i].keys[0]) << "\n";
            f << binds[i].cfg_id << "_key2=" << format_key_cfg(binds[i].keys[1]) << "\n";
            f << binds[i].cfg_id << "_pad="  << format_pad_cfg(binds[i].pad_btn) << "\n";
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

    static int measure_ui_text(const std::string& utf8) {
        auto cps = utf8_to_codepoints(utf8);
        int w = 0;
        for (uint32_t cp : cps) {
            if (g_ft.ft_face && g_ft.load_char && g_ft.load_char(g_ft.ft_face, cp, 4) == 0) {
                const uint8_t* face_bytes = reinterpret_cast<const uint8_t*>(g_ft.ft_face);
                const uint8_t* glyph_ptr = *reinterpret_cast<const uint8_t* const*>(face_bytes + 0x98);
                if (glyph_ptr) {
                    int32_t adv_x = *reinterpret_cast<const int32_t*>(glyph_ptr + 0x80) >> 6;
                    w += (adv_x > 0) ? adv_x : ((cp < 0x80) ? 6 : 12);
                    continue;
                }
            }
            w += (cp < 0x80) ? 6 : 12;
        }
        return w;
    }

    static void draw_ui_text_raw(uint32_t* buf, int bw, int bh, const std::string& utf8, int x, int baseline_y, uint32_t color) {
        auto cps = utf8_to_codepoints(utf8);
        uint32_t a_col = (color >> 24) & 0xFF;
        if (a_col == 0) a_col = 255;
        uint8_t sr = (color >> 16) & 0xFF;
        uint8_t sg = (color >> 8) & 0xFF;
        uint8_t sb = color & 0xFF;
        int pen_x = x;

        for (uint32_t cp : cps) {
            bool ok = false;
            if (g_ft.ft_face && g_ft.load_char && g_ft.load_char(g_ft.ft_face, cp, 4) == 0) {
                const uint8_t* face_bytes = reinterpret_cast<const uint8_t*>(g_ft.ft_face);
                const uint8_t* glyph_ptr = *reinterpret_cast<const uint8_t* const*>(face_bytes + 0x98);
                if (glyph_ptr) {
                    int32_t adv_x = *reinterpret_cast<const int32_t*>(glyph_ptr + 0x80) >> 6;
                    uint32_t rows = *reinterpret_cast<const uint32_t*>(glyph_ptr + 0x98);
                    uint32_t width = *reinterpret_cast<const uint32_t*>(glyph_ptr + 0x9c);
                    int32_t pitch = *reinterpret_cast<const int32_t*>(glyph_ptr + 0xa0);
                    const uint8_t* gbuf = *reinterpret_cast<const uint8_t* const*>(glyph_ptr + 0xa8);
                    int32_t bmp_left = *reinterpret_cast<const int32_t*>(glyph_ptr + 0xc0);
                    int32_t bmp_top = *reinterpret_cast<const int32_t*>(glyph_ptr + 0xc4);

                    if (gbuf && rows < 64 && width < 64) {
                        for (uint32_t ry = 0; ry < rows; ++ry) {
                            int dy = baseline_y - bmp_top + static_cast<int>(ry);
                            if (dy < 0 || dy >= bh) continue;
                            for (uint32_t rx = 0; rx < width; ++rx) {
                                int dx = pen_x + bmp_left + static_cast<int>(rx);
                                if (dx < 0 || dx >= bw) continue;
                                uint32_t ga = (static_cast<uint32_t>(gbuf[ry * pitch + rx]) * a_col) / 255;
                                if (ga > 32) {
                                    uint32_t dst = buf[dy * bw + dx];
                                    uint32_t dr = (dst >> 16) & 0xFF;
                                    uint32_t dg = (dst >> 8) & 0xFF;
                                    uint32_t db = dst & 0xFF;
                                    uint32_t nr = (sr * ga + dr * (255 - ga)) / 255;
                                    uint32_t ng = (sg * ga + dg * (255 - ga)) / 255;
                                    uint32_t nb = (sb * ga + db * (255 - ga)) / 255;
                                    buf[dy * bw + dx] = 0xFF000000u | (nr << 16) | (ng << 8) | nb;
                                }
                            }
                        }
                    }
                    pen_x += (adv_x > 0) ? adv_x : ((cp < 0x80) ? 6 : 12);
                    ok = true;
                }
            }
            if (!ok) {
                pen_x += (cp < 0x80) ? 6 : 12;
            }
        }
    }

    static void draw_ui_text(uint32_t* buf, int bw, int bh, const std::string& utf8, int x, int baseline_y, uint32_t color, bool shadow = true) {
        if (shadow) {
            uint32_t sa = ((color >> 24) & 0xFF);
            if (sa == 0) sa = 255;
            draw_ui_text_raw(buf, bw, bh, utf8, x + 1, baseline_y + 1, (sa << 24) | 0x060810u);
        }
        draw_ui_text_raw(buf, bw, bh, utf8, x, baseline_y, color);
    }

    void render_softkey_bar(int32_t keypad_state) {
        // Metallic flip-phone bottom bar y = 240..259
        fill_rect_alpha(display_buf, 240, 260, 0, 240, 240, 1, 0xFF445684u);
        for (int y = 241; y < 260; ++y) {
            uint32_t r = 26 - (y - 241) * 10 / 19;
            uint32_t g = 32 - (y - 241) * 12 / 19;
            uint32_t b = 50 - (y - 241) * 18 / 19;
            fill_rect_alpha(display_buf, 240, 260, 0, y, 240, 1, 0xFF000000u | (r << 16) | (g << 8) | b);
        }

        // Left Softkey (SOFT_KEY_1 -> DoJa 0x15)
        bool s1_held = (keypad_state & (1 << 0x15)) != 0;
        std::string k1 = primary_soft_key_label(ACT_SOFT1);
        std::string left_txt = soft_label_left.empty() ? ("[" + k1 + "] ---") : ("[" + k1 + "] " + soft_label_left);
        fill_rect_alpha(display_buf, 240, 260, 2, 242, 80, 16, s1_held ? 0xFF2B5EA8u : 0xFF202942u);
        draw_rect_border(display_buf, 240, 260, 2, 242, 80, 16, s1_held ? 0xFF74B2FFu : 0xFF394970u);
        draw_ui_text(display_buf, 240, 260, left_txt, 5, 254, soft_label_left.empty() ? 0xFF7080A0u : 0xFFF0F6FFu);

        // Right Softkey (SOFT_KEY_2 -> DoJa 0x16)
        bool s2_held = (keypad_state & (1 << 0x16)) != 0;
        std::string k2 = primary_soft_key_label(ACT_SOFT2);
        std::string right_txt = soft_label_right.empty() ? ("--- [" + k2 + "]") : (soft_label_right + " [" + k2 + "]");
        fill_rect_alpha(display_buf, 240, 260, 158, 242, 80, 16, s2_held ? 0xFF2B5EA8u : 0xFF202942u);
        draw_rect_border(display_buf, 240, 260, 158, 242, 80, 16, s2_held ? 0xFF74B2FFu : 0xFF394970u);
        int rw = measure_ui_text(right_txt);
        draw_ui_text(display_buf, 240, 260, right_txt, std::max(161, 235 - rw), 254, soft_label_right.empty() ? 0xFF7080A0u : 0xFFF0F6FFu);

        // Center Status / F1 Hint
        if (is_turbo_held() && !g_pvp.active()) {
            std::string c_txt = ">> 倍速";
            int cw = measure_ui_text(c_txt);
            draw_ui_text(display_buf, 240, 260, c_txt, 120 - cw / 2, 254, 0xFFFFD448u);
        } else if (g_pvp.active()) {
            std::string c_txt = "#" + std::to_string(g_pvp.room_id());
            if (g_pvp.match_started() && g_pvp.rtt_ms() > 0) {
                c_txt += " " + std::to_string(g_pvp.rtt_ms()) + "ms";
            }
            int cw = measure_ui_text(c_txt);
            draw_ui_text(display_buf, 240, 260, c_txt, 120 - cw / 2, 254, g_pvp.match_started() ? 0xFF68FF9Au : 0xFF74C4FFu);
        } else if (online_mode) {
            std::string c_txt = "在线:F1";
            int cw = measure_ui_text(c_txt);
            draw_ui_text(display_buf, 240, 260, c_txt, 120 - cw / 2, 254, 0xFF74D8FFu);
        } else {
            std::string c_txt = "F1:设置";
            int cw = measure_ui_text(c_txt);
            draw_ui_text(display_buf, 240, 260, c_txt, 120 - cw / 2, 254, 0xFF8A9CC2u);
        }
    }

    void compose_display_buffer(const uint32_t* game_fb, int32_t keypad_state, bool tick_toast = true) {
        std::memcpy(display_buf, game_fb, 240 * 240 * sizeof(uint32_t));
        if (show_softkey_bar) {
            render_softkey_bar(keypad_state);
        }
        if (overlay_open) {
            render_overlay_onto_display_buf();
        }
        if (toast_frames > 0 && !toast_text.empty()) {
            uint32_t alpha = (toast_frames < 20) ? static_cast<uint32_t>(toast_frames * 255 / 20) : 235u;
            int tw = measure_ui_text(toast_text);
            int bw = std::min(232, tw + 14);
            int bx = (240 - bw) / 2;
            fill_rect_alpha(display_buf, 240, 260, bx, 4, bw, 17, ((alpha * 220 / 255) << 24) | 0x10182Cu);
            draw_rect_border(display_buf, 240, 260, bx, 4, bw, 17, (alpha << 24) | 0x5898E8u);
            draw_ui_text(display_buf, 240, 260, toast_text, bx + (bw - tw) / 2, 16, (alpha << 24) | 0xFFFFFFu);
            if (tick_toast) --toast_frames;
        }
    }

    void apply_softbar_geometry(SDL_Window* win, SDL_Renderer* ren, int scale) {
        if (!ren) return;
        int logical_h = show_softkey_bar ? 260 : 240;
        SDL_RenderSetLogicalSize(ren, 240, logical_h);
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

    void save_screenshot_bmp(const uint32_t* game_fb) {
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
            const_cast<uint32_t*>(game_fb),
            240, 240, 32, 240 * static_cast<int>(sizeof(uint32_t)),
            SDL_PIXELFORMAT_ARGB8888
        );
        if (!surf) return;
        std::time_t t = std::time(nullptr);
        char fname[64];
        std::snprintf(fname, sizeof(fname), "screenshot_%lld.bmp", static_cast<long long>(t));
        if (SDL_SaveBMP(surf, fname) == 0) {
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
    // 1: 服务器地址 (Enter to type/paste IP:Port, Left/Right preset toggle)
    // 2: 玩家昵称 (Enter to type/paste nickname)
    // 3: 通信模式 (Online Server / Local Offline)
    // 4: 主音量
    // 5: 帧率模式 (60 FPS / 15 FPS)
    // 6: 底部软键栏
    // 7..16: Keybindings (ACT_COUNT = 10)
    // 17: 恢复默认按键配置
    // 18: 返回游戏 (Resume)
    // 19: 保存进度并退出
    static constexpr int BIND_START_ROW = 7;
    static constexpr int BIND_END_ROW = BIND_START_ROW + ACT_COUNT; // 17
    static constexpr int TOTAL_ROWS = BIND_END_ROW + 3;             // 20
    static constexpr int VISIBLE_ROWS = 9;

    bool overlay_open = false;
    int overlay_sel_row = 0;
    int overlay_sel_col = 0; // 0 = Key1, 1 = Key2, 2 = Pad
    int overlay_scroll_top = 0;
    bool overlay_waiting_bind = false;
    int overlay_text_edit_mode = 0; // 0 = none, 1 = server_url, 2 = player_name
    std::string overlay_text_edit_buf;
    bool overlay_ignore_next_textinput = false;
    std::vector<pvp::PvpRoomInfo> overlay_open_rooms;

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
        overlay_open_rooms.clear();
        if (online_mode && !server_url.empty() && !g_pvp.active()) {
            pvp::PvpClientSession::list_open_rooms(server_url, is_lon_game ? "lon" : "pon", overlay_open_rooms);
        }
        int max_choices = 2 + static_cast<int>(overlay_open_rooms.size());
        if (pvp_menu_choice >= max_choices) pvp_menu_choice = 0;
    }

    void connect_and_probe_server() {
        online_mode = true;
        save_config();
        std::string stat_msg;
        if (pvp::PvpClientSession::check_server_status(server_url, stat_msg)) {
            show_toast("已连接 " + server_url + " (" + stat_msg + ")", 120);
            refresh_open_rooms();
        } else {
            overlay_open_rooms.clear();
            show_toast("未连上 " + server_url + " (请检查地址或端口)", 120);
        }
    }

    void cycle_server_preset(int dir) {
        if (g_pvp.active()) {
            show_toast("请先退出当前对战房间再修改地址", 90);
            return;
        }
        const std::vector<std::string> presets = {
            "111.229.100.54:8765",
            "127.0.0.1:8765"
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
            g_pvp.close_udp();
            overlay_open_rooms.clear();
            pvp_menu_choice = 0;
            show_toast("通信模式: 本地离线模拟", 90);
        }
    }

    void open_overlay(bool allow_room_probe) {
        // Ensure OS IME preedit composition is stopped so F1/Esc/Arrow/Enter are never swallowed
        SDL_StopTextInput();
        clear_held_states();
        overlay_sel_row = 0;
        overlay_sel_col = 0;
        overlay_scroll_top = 0;
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
        if (pvp_menu_choice == 0) {
            if (g_pvp.create_room(server_url, player_uid, player_name, gm)) {
                show_toast("已创建房间 #" + std::to_string(g_pvp.room_id()) + " 等待挑战者...", 180);
                close_overlay();
            } else {
                show_toast("创建失败: 无法连接 " + server_url, 120);
            }
        } else {
            uint32_t target_room = 0;
            if (pvp_menu_choice >= 2 && static_cast<size_t>(pvp_menu_choice - 2) < overlay_open_rooms.size()) {
                target_room = overlay_open_rooms[static_cast<size_t>(pvp_menu_choice - 2)].room_id;
            }
            if (g_pvp.join_room(server_url, target_room, player_uid, player_name, gm)) {
                show_toast("已加入 #" + std::to_string(g_pvp.room_id()) + " VS " + g_pvp.opponent_name(), 150);
                close_overlay();
            } else {
                refresh_open_rooms();
                show_toast("暂无可加入的对战房间 (" + server_url + ")", 120);
            }
        }
    }

    void handle_overlay_event(const SDL_Event& ev, SDL_Window* win, SDL_Renderer* ren, int scale, bool& quit_game) {
        if (ev.type == SDL_QUIT ||
            (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
            quit_game = true;
            close_overlay();
            return;
        }
        if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            open_controllers();
            return;
        }
        if (overlay_text_edit_mode != 0) {
            size_t max_len = (overlay_text_edit_mode == 1) ? 44 : 18;
            bool no_space = (overlay_text_edit_mode == 1);
            if (ev.type == SDL_TEXTINPUT) {
                if (overlay_ignore_next_textinput) {
                    overlay_ignore_next_textinput = false;
                } else {
                    sanitize_append(overlay_text_edit_buf, ev.text.text, no_space, max_len);
                }
            } else if (ev.type == SDL_KEYDOWN) {
                overlay_ignore_next_textinput = false;
                SDL_Keycode k = ev.key.keysym.sym;
                uint16_t mod = ev.key.keysym.mod;
                if (k == SDLK_ESCAPE || k == SDLK_F1) {
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
                        } else {
                            player_name = trimmed;
                            save_config();
                            overlay_text_edit_mode = 0;
                            SDL_StopTextInput();
                            show_toast("玩家昵称已更新: " + player_name, 90);
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
                            sanitize_append(overlay_text_edit_buf, clip, no_space, max_len);
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
            }
        } else {
            auto adjust_row_lr = [&](int dir) {
                if (overlay_sel_row == 0 && !g_pvp.active()) {
                    if (online_mode) refresh_open_rooms();
                    int max_choices = 2 + static_cast<int>(overlay_open_rooms.size());
                    pvp_menu_choice = (pvp_menu_choice + dir + max_choices) % max_choices;
                } else if (overlay_sel_row == 1) {
                    cycle_server_preset(dir);
                } else if (overlay_sel_row == 3) {
                    toggle_net_mode();
                } else if (overlay_sel_row == 4) {
                    std::lock_guard<std::mutex> lk(g_audio.mtx);
                    g_audio.muted = false;
                    g_audio.master_volume = std::clamp(g_audio.master_volume + dir * 5, 0, 100);
                    save_config();
                } else if (overlay_sel_row == 5) {
                    toggle_fps_mode();
                } else if (overlay_sel_row == 6) {
                    show_softkey_bar = !show_softkey_bar;
                    apply_softbar_geometry(win, ren, scale);
                    save_config();
                } else if (overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW) {
                    overlay_sel_col = (overlay_sel_col + dir + 3) % 3;
                }
            };
            auto activate_row = [&]() {
                if (overlay_sel_row == 0) {
                    exec_pvp_action();
                } else if (overlay_sel_row == 1) {
                    if (g_pvp.active()) {
                        show_toast("请先退出当前对战房间再修改地址", 90);
                    } else {
                        overlay_text_edit_mode = 1;
                        overlay_text_edit_buf = server_url;
                        overlay_ignore_next_textinput = true;
                        SDL_StartTextInput();
                    }
                } else if (overlay_sel_row == 2) {
                    overlay_text_edit_mode = 2;
                    overlay_text_edit_buf = player_name;
                    overlay_ignore_next_textinput = true;
                    SDL_StartTextInput();
                } else if (overlay_sel_row == 3) {
                    toggle_net_mode();
                } else if (overlay_sel_row == 4) {
                    {
                        std::lock_guard<std::mutex> lk(g_audio.mtx);
                        g_audio.muted = !g_audio.muted;
                    }
                    save_config();
                } else if (overlay_sel_row == 5) {
                    toggle_fps_mode();
                } else if (overlay_sel_row == 6) {
                    show_softkey_bar = !show_softkey_bar;
                    apply_softbar_geometry(win, ren, scale);
                    save_config();
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

            if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                SDL_Keycode k = ev.key.keysym.sym;
                bool in_bind_rows = (overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW);
                if (k == SDLK_F1 || k == SDLK_ESCAPE || (!in_bind_rows && matches_act(ACT_CANCEL, k))) {
                    close_overlay();
                } else if (k == SDLK_F3) {
                    toggle_fps_mode();
                } else if (k == SDLK_UP || matches_act(ACT_UP, k)) {
                    overlay_sel_row = (overlay_sel_row - 1 + TOTAL_ROWS) % TOTAL_ROWS;
                } else if (k == SDLK_DOWN || matches_act(ACT_DOWN, k)) {
                    overlay_sel_row = (overlay_sel_row + 1) % TOTAL_ROWS;
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
                        overlay_sel_row = (overlay_sel_row - 1 + TOTAL_ROWS) % TOTAL_ROWS; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                        overlay_sel_row = (overlay_sel_row + 1) % TOTAL_ROWS; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                        adjust_row_lr(-1); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                        adjust_row_lr(1); break;
                    case SDL_CONTROLLER_BUTTON_A:
                        activate_row(); break;
                    case SDL_CONTROLLER_BUTTON_B:
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
        fill_rect_alpha(display_buf, 240, 260, 0, 0, 240, 240, live_pvp ? 0xB8080C18u : 0xDC080C18u);

        // Title bar
        fill_rect_alpha(display_buf, 240, 260, 4, 4, 232, 18, live_pvp ? 0xFF14362Au : 0xFF1A284Au);
        draw_rect_border(display_buf, 240, 260, 4, 4, 232, 18, live_pvp ? 0xFF4CC88Au : 0xFF4F78C8u);
        std::string title = live_pvp
            ? (g_pvp.match_started() ? "联机菜单 — 对战后台运行中 (F1关闭)" : "联机菜单 — 房间等待中 (F1关闭)")
            : "联机对战与模拟器设置 (F1 / Esc)";
        draw_ui_text(display_buf, 240, 260, title, 120 - measure_ui_text(title) / 2, 17, 0xFFFFFFFFu);

        // Dynamic section header
        if (overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW) {
            draw_ui_text(display_buf, 240, 260, "动作",  8,   34, 0xFF8EB0E8u);
            draw_ui_text(display_buf, 240, 260, "键位1", 80,  34, 0xFF8EB0E8u);
            draw_ui_text(display_buf, 240, 260, "键位2", 136, 34, 0xFF8EB0E8u);
            draw_ui_text(display_buf, 240, 260, "手柄",  192, 34, 0xFF8EB0E8u);
        } else {
            draw_ui_text(display_buf, 240, 260, "功能选项", 8, 34, 0xFF8EB0E8u);
            draw_ui_text(display_buf, 240, 260, "当前设置 (Enter确认 / ←→切换)", 74, 34, 0xFF8EB0E8u);
        }
        fill_rect_alpha(display_buf, 240, 260, 6, 37, 228, 1, 0xFF324672u);

        // Rows viewport (y = 40 .. 193, 9 rows * 17px)
        for (int vi = 0; vi < VISIBLE_ROWS; ++vi) {
            int r = overlay_scroll_top + vi;
            if (r >= TOTAL_ROWS) break;
            int ry = 40 + vi * 17;
            bool is_sel = (r == overlay_sel_row);
            if (is_sel) {
                fill_rect_alpha(display_buf, 240, 260, 5, ry, 226, 16, 0xFF223A6Au);
                draw_rect_border(display_buf, 240, 260, 5, ry, 226, 16, 0xFF5A8AE0u);
            }

            if (r == 0) {
                draw_ui_text(display_buf, 240, 260, "联机对战", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFFFE484u);
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
                    pstr = "< #" + std::to_string(rm.room_id) + " " + rm.host + " >";
                    pcol = 0xFF74D8FFu;
                } else {
                    pstr = "< 快速加入配对 >";
                }
                draw_ui_text(display_buf, 240, 260, pstr, 84, ry + 12, pcol);
            } else if (r == 1) {
                draw_ui_text(display_buf, 240, 260, "服务器地址", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string disp_srv = server_url;
                if (disp_srv.size() > 21) disp_srv = disp_srv.substr(0, 20) + "..";
                std::string ustr = "[" + disp_srv + "]";
                draw_ui_text(display_buf, 240, 260, ustr, 76, ry + 12, is_sel ? 0xFF88EEFFu : 0xFF74D8FFu);
            } else if (r == 2) {
                draw_ui_text(display_buf, 240, 260, "玩家昵称", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string nstr = "[" + player_name + "]";
                draw_ui_text(display_buf, 240, 260, nstr, 84, ry + 12, is_sel ? 0xFFFFEC88u : 0xFF88FFAAu);
            } else if (r == 3) {
                draw_ui_text(display_buf, 240, 260, "通信模式", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string nstr = online_mode ? "< 在线服务器 >" : "< 本地离线模拟 >";
                draw_ui_text(display_buf, 240, 260, nstr, 86, ry + 12, online_mode ? 0xFF74D8FFu : 0xFF98A8C0u);
            } else if (r == 4) {
                int vol = 100;
                bool muted = false;
                {
                    std::lock_guard<std::mutex> lk(g_audio.mtx);
                    vol = g_audio.master_volume;
                    muted = g_audio.muted;
                }
                draw_ui_text(display_buf, 240, 260, "主音量", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string vstr = muted ? ("< 静音 (" + std::to_string(vol) + "%) >") : ("< " + std::to_string(vol) + "% >");
                draw_ui_text(display_buf, 240, 260, vstr, 102, ry + 12, muted ? 0xFFFF8888u : 0xFF88FFAAu);
            } else if (r == 5) {
                draw_ui_text(display_buf, 240, 260, "帧率模式", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string fstr = high_fps_mode ? "< 60 FPS 丝滑插值 >" : "< 15 FPS 原版帧率 >";
                draw_ui_text(display_buf, 240, 260, fstr, 84, ry + 12, high_fps_mode ? 0xFF88FFAAu : 0xFFFFD874u);
            } else if (r == 6) {
                draw_ui_text(display_buf, 240, 260, "底部软键栏", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                std::string sstr = show_softkey_bar ? "< 开启 >" : "< 关闭 >";
                draw_ui_text(display_buf, 240, 260, sstr, 110, ry + 12, show_softkey_bar ? 0xFF88FFAAu : 0xFF98A8C0u);
            } else if (r >= BIND_START_ROW && r < BIND_END_ROW) {
                int bi = r - BIND_START_ROW;
                draw_ui_text(display_buf, 240, 260, binds[bi].label_zh, 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
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
                        fill_rect_alpha(display_buf, 240, 260, cx[c], ry + 1, cw[c], 14, 0xFF345898u);
                        draw_rect_border(display_buf, 240, 260, cx[c], ry + 1, cw[c], 14, 0xFFFFD24Cu);
                    }
                    int tw = measure_ui_text(cols[c]);
                    uint32_t tcol = (cols[c] == "---") ? 0xFF687898u : (cell_sel ? 0xFFFFEC88u : 0xFFE8F0FFu);
                    draw_ui_text(display_buf, 240, 260, cols[c], cx[c] + (cw[c] - tw) / 2, ry + 12, tcol);
                }
            } else if (r == BIND_END_ROW) {
                std::string lbl = "[ 恢复默认按键配置 ]";
                draw_ui_text(display_buf, 240, 260, lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFFFFD866u : 0xFFC8D8F8u);
            } else if (r == BIND_END_ROW + 1) {
                std::string lbl = "[ 返回游戏 (Resume) ]";
                draw_ui_text(display_buf, 240, 260, lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFF88FFAAu : 0xFFC8D8F8u);
            } else if (r == BIND_END_ROW + 2) {
                std::string lbl = "[ 保存进度并退出 ]";
                draw_ui_text(display_buf, 240, 260, lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFFFF8888u : 0xFFE8B8B8u);
            }
        }

        // Scrollbar track on right edge
        fill_rect_alpha(display_buf, 240, 260, 233, 40, 3, VISIBLE_ROWS * 17, 0xFF182238u);
        int sb_h = std::max(14, (VISIBLE_ROWS * 17 * VISIBLE_ROWS) / TOTAL_ROWS);
        int sb_y = 40 + ((VISIBLE_ROWS * 17 - sb_h) * overlay_scroll_top) / std::max(1, TOTAL_ROWS - VISIBLE_ROWS);
        fill_rect_alpha(display_buf, 240, 260, 233, sb_y, 3, sb_h, 0xFF6894E0u);

        // Footer instructions
        fill_rect_alpha(display_buf, 240, 260, 6, 196, 228, 1, 0xFF324672u);
        if (overlay_sel_row == 1) {
            draw_ui_text(display_buf, 240, 260, "Enter:手动输入/粘贴地址  ←→:预设切换", 6, 211, 0xFF88EEFFu);
        } else if (overlay_sel_row == 2) {
            draw_ui_text(display_buf, 240, 260, "Enter:输入/粘贴昵称 (自动保存)", 14, 211, 0xFFFFEC88u);
        } else if (overlay_sel_row == 0) {
            if (live_pvp) {
                draw_ui_text(display_buf, 240, 260, "Enter:立即退出房间   F1/Esc:继续对战", 10, 211, 0xFF88FFAAu);
            } else {
                draw_ui_text(display_buf, 240, 260, "←→:选建房/快速加入/房间号  Enter:出发!", 6, 211, 0xFFFFE484u);
            }
        } else {
            draw_ui_text(display_buf, 240, 260, "↑↓:换行  ←→:切换/选槽  Enter:确认/改键", 6, 211, 0xFFA4BCE4u);
        }
        draw_ui_text(display_buf, 240, 260, "↓下滑设置按键 | F3:帧率 F11:全屏 F12:截图", 4, 227, 0xFF849AC0u);

        // Text input modal popup (Server URL / Player Nickname)
        if (overlay_text_edit_mode != 0) {
            fill_rect_alpha(display_buf, 240, 260, 8, 62, 224, 106, 0xF60C162Cu);
            draw_rect_border(display_buf, 240, 260, 8, 62, 224, 106, 0xFF68B8FFu);
            std::string hdr = (overlay_text_edit_mode == 1)
                ? "输入服务器地址 (IP:端口)"
                : "输入联机玩家昵称";
            draw_ui_text(display_buf, 240, 260, hdr, 120 - measure_ui_text(hdr) / 2, 82, 0xFFFFE070u);

            fill_rect_alpha(display_buf, 240, 260, 16, 94, 208, 24, 0xFF060B18u);
            draw_rect_border(display_buf, 240, 260, 16, 94, 208, 24, 0xFF88D8FFu);
            std::string cursor_txt = overlay_text_edit_buf + "_";
            while (measure_ui_text(cursor_txt) > 198 && cursor_txt.size() > 2) {
                cursor_txt.erase(0, 1);
            }
            draw_ui_text(display_buf, 240, 260, cursor_txt, 22, 111, 0xFFFFFFFFu);

            std::string h1 = "支持直接打字或 Ctrl+V 粘贴剪贴板";
            std::string h2 = "Enter:保存确认   Backspace:退格   Esc:取消";
            draw_ui_text(display_buf, 240, 260, h1, 120 - measure_ui_text(h1) / 2, 136, 0xFF88CCFFu);
            draw_ui_text(display_buf, 240, 260, h2, 120 - measure_ui_text(h2) / 2, 155, 0xFFA0B8E0u);
        }

        // Waiting for key modal popup
        if (overlay_waiting_bind && overlay_sel_row >= BIND_START_ROW && overlay_sel_row < BIND_END_ROW) {
            int bi = overlay_sel_row - BIND_START_ROW;
            fill_rect_alpha(display_buf, 240, 260, 12, 72, 216, 86, 0xF4101830u);
            draw_rect_border(display_buf, 240, 260, 12, 72, 216, 86, 0xFFFFD24Cu);
            std::string p1 = std::string("正在设置: ") + binds[bi].label_zh;
            std::string p2 = "请按下键盘按键或手柄按钮...";
            std::string p3 = "Esc:取消   Backspace:清空槽位";
            draw_ui_text(display_buf, 240, 260, p1, 120 - measure_ui_text(p1) / 2, 95,  0xFFFFE070u);
            draw_ui_text(display_buf, 240, 260, p2, 120 - measure_ui_text(p2) / 2, 118, 0xFFFFFFFFu);
            draw_ui_text(display_buf, 240, 260, p3, 120 - measure_ui_text(p3) / 2, 142, 0xFFA0B8E0u);
        }
    }

    // Blocking pause modal used ONLY in single-player (!g_pvp.active()).
    // Returns true if user chose "Save & Quit".
    bool run_pause_config_modal(SDL_Window* win, SDL_Renderer* ren, SDL_Texture* tex, const uint32_t* game_fb, int scale) {
        if (!win || !ren || !tex) return false;
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
            if (g_pvp.poll_udp()) {
                show_toast("对战配对成功! VS " + g_pvp.opponent_name(), 150);
                close_overlay();
                break;
            }
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                handle_overlay_event(ev, win, ren, scale, quit_game);
                if (!overlay_open) break;
            }
            if (!overlay_open) break;

            compose_display_buffer(game_fb, 0, true);
            int logical_h = show_softkey_bar ? 260 : 240;
            SDL_Rect src_rect{0, 0, 240, logical_h};
            SDL_UpdateTexture(tex, nullptr, display_buf, 240 * static_cast<int>(sizeof(uint32_t)));
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, &src_rect, nullptr);
            SDL_RenderPresent(ren);
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
static void blit_rect_buf(uint32_t* dst_buf, int dst_w, int dst_h, bool fill,
                          int rx, int ry, int rw, int rh, uint32_t col) {
    if (rw <= 0 || rh <= 0) return;
    if (fill) {
        int y0 = std::max(0, ry), y1 = std::min(dst_h, ry + rh);
        int x0 = std::max(0, rx), x1 = std::min(dst_w, rx + rw);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                dst_buf[y * dst_w + x] = col;
            }
        }
    } else {
        int x0 = std::max(0, rx), x1 = std::min(dst_w - 1, rx + rw);
        for (int x = x0; x <= x1; ++x) {
            if (ry >= 0 && ry < dst_h) dst_buf[ry * dst_w + x] = col;
            if (ry + rh >= 0 && ry + rh < dst_h) dst_buf[(ry + rh) * dst_w + x] = col;
        }
        int y0 = std::max(0, ry), y1 = std::min(dst_h - 1, ry + rh);
        for (int y = y0; y <= y1; ++y) {
            if (rx >= 0 && rx < dst_w) dst_buf[y * dst_w + rx] = col;
            if (rx + rw >= 0 && rx + rw < dst_w) dst_buf[y * dst_w + (rx + rw)] = col;
        }
    }
}

static void blit_image_buf(DojaRuntime& rt, uint32_t* dst_buf, int dst_w, int dst_h,
                           Ref img_ref, int base_dx, int base_dy,
                           int sx, int sy, int sw, int sh, int dw, int dh, int flip_mode) {
    if (img_ref <= 0 || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    HeapObject& img_obj = rt.get(img_ref);
    if (!img_obj.img) return;
    ImageData& img = *img_obj.img;

    const PaletteData* pal = nullptr;
    if (img.is_paletted && img.palette_ref > 0) {
        pal = rt.get(img.palette_ref).pal.get();
    }

    bool flip_h = (flip_mode == 1 || flip_mode == 3);
    bool flip_v = (flip_mode == 2 || flip_mode == 3);

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

static void blit_text_buf(uint32_t* dst_buf, int dst_w, int dst_h,
                          const std::string& text, int pen_x, int baseline_y, uint32_t color) {
    auto cps = utf8_to_codepoints(text);
    uint8_t sr = (color >> 16) & 0xFF;
    uint8_t sg = (color >> 8) & 0xFF;
    uint8_t sb = color & 0xFF;

    for (uint32_t cp : cps) {
        bool rendered_ft = false;
        if (g_ft.ft_face && g_ft.load_char) {
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

struct FrameInterpolator {
    enum class CmdKind : uint8_t { FillRect, DrawRect, Image, Text };

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
        std::string text;
        int prev_dx = 0, prev_dy = 0;
        bool has_match = false;
    };

    std::unordered_map<Ref, uint32_t> img_gen;
    uint32_t lock_base_fb[240 * 240] = {0};
    uint32_t interp_fb[240 * 240] = {0};

    std::vector<DrawCmd> prev_cmds;
    std::vector<DrawCmd> curr_cmds;
    std::vector<DrawCmd> building_cmds;
    int building_coverage = 0;

    bool can_interpolate = false;
    bool any_motion = false;
    int subframes_presented = 4; // 1..4 across ~62.5 ms 15Hz logic tick
    std::chrono::steady_clock::time_point tick_unlock_tp = std::chrono::steady_clock::now();

    void reset() {
        prev_cmds.clear();
        curr_cmds.clear();
        building_cmds.clear();
        building_coverage = 0;
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

    void on_screen_lock(const uint32_t* current_fb) {
        std::memcpy(lock_base_fb, current_fb, 240 * 240 * sizeof(uint32_t));
        building_cmds.clear();
        building_coverage = 0;
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

    void on_screen_unlock(const uint32_t* final_fb, bool enable_interp) {
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
            std::memcpy(lock_base_fb, final_fb, 240 * 240 * sizeof(uint32_t));
            return;
        }
        // Only interpolate full-viewport scenes (Overworld, Battle, Menus, Title) so tiny
        // incremental wipe patches never leave trails on uncleared backgrounds
        if (cov < 240 * 160) {
            std::memcpy(lock_base_fb, final_fb, 240 * 240 * sizeof(uint32_t));
            return;
        }
        int n_prev = static_cast<int>(prev_cmds.size());
        int n_curr = static_cast<int>(curr_cmds.size());
        if (std::abs(n_curr - n_prev) > std::max(16, (n_curr * 3) / 5)) {
            std::memcpy(lock_base_fb, final_fb, 240 * 240 * sizeof(uint32_t));
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
            if (c.kind == CmdKind::FillRect || c.kind == CmdKind::DrawRect) {
                return c.dw == p.dw && c.dh == p.dh && c.color == p.color;
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
            if (c.has_match) continue;
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
        std::memcpy(interp_fb, lock_base_fb, 240 * 240 * sizeof(uint32_t));
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
                    blit_rect_buf(interp_fb, 240, 240, true, dx, dy, c.dw, c.dh, c.color);
                    break;
                case CmdKind::DrawRect:
                    blit_rect_buf(interp_fb, 240, 240, false, dx, dy, c.dw, c.dh, c.color);
                    break;
                case CmdKind::Image:
                    blit_image_buf(rt, interp_fb, 240, 240, c.img_ref, dx, dy,
                                   c.sx, c.sy, c.sw, c.sh, c.dw, c.dh, c.flip_mode);
                    break;
                case CmdKind::Text:
                    blit_text_buf(interp_fb, 240, 240, c.text, dx, dy, c.color);
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

pvp::PvpClientSession& DojaRuntime::pvp_session() {
    return g_pvp;
}

std::string DojaRuntime::player_nickname() const {
    return g_ui.player_name;
}

int DojaRuntime::take_cli_pvp_action(uint32_t& room_id_out) {
    if (g_ui.cli_pvp_mode == 0) return 0;
    std::string gm = g_ui.is_lon_game ? "lon" : "pon";
    g_ui.online_mode = true;
    if (g_ui.cli_pvp_mode == 1) {
        g_ui.cli_pvp_mode = 0;
        if (g_pvp.create_room(g_ui.server_url, g_ui.player_uid, g_ui.player_name, gm)) {
            room_id_out = g_pvp.room_id();
            g_ui.show_toast("已创建房间 #" + std::to_string(room_id_out) + " 等待加入...", 180);
            return 1;
        }
        return -1;
    }
    if (g_ui.cli_pvp_mode == 2) {
        if (g_pvp.join_room(g_ui.server_url, g_ui.cli_pvp_room_id, g_ui.player_uid, g_ui.player_name, gm)) {
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
    out.paletted_image_refs.clear();
    for (size_t i = 1; i < heap_.size(); ++i) {
        const HeapObject& o = heap_[i];
        if (o.kind == ObjKind::Palette && o.pal) {
            out.palettes.emplace_back(static_cast<Ref>(i), *o.pal);
        } else if (o.kind == ObjKind::Image && o.img && !o.img->argb.empty()) {
            out.offscreen_argb.emplace_back(static_cast<Ref>(i), o.img->argb);
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
        }
    }
    for (const auto& pr : in.paletted_image_refs) {
        if (pr.first > 0 && static_cast<size_t>(pr.first) < heap_.size() && heap_[pr.first].img) {
            heap_[pr.first].img->palette_ref = pr.second;
        }
    }
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

void DojaRuntime::fb_fill_rect_alpha(int rx, int ry, int rw, int rh, uint32_t argb) {
    UiInputManager::fill_rect_alpha(framebuffer_, 240, 240, rx, ry, rw, rh, argb);
}

void DojaRuntime::fb_draw_border(int rx, int ry, int rw, int rh, uint32_t argb) {
    UiInputManager::draw_rect_border(framebuffer_, 240, 240, rx, ry, rw, rh, argb);
}

int DojaRuntime::fb_measure_text(const std::string& utf8) const {
    return UiInputManager::measure_ui_text(utf8);
}

void DojaRuntime::fb_draw_text(const std::string& utf8, int x, int baseline_y, uint32_t color, bool shadow) {
    UiInputManager::draw_ui_text(framebuffer_, 240, 240, utf8, x, baseline_y, color, shadow);
}

void DojaRuntime::show_ui_toast(const std::string& msg, int frames) {
    g_ui.show_toast(msg, frames);
}

void DojaRuntime::set_config_path(const std::string& path) {
    if (!path.empty()) {
        g_ui.config_path = path;
    }
}

bool DojaRuntime::init(const std::string& jar_path, const std::string& sp_path, const std::string& /*jam_path*/, int scale) {
    jar_path_ = jar_path;
    sp_path_ = sp_path;
    sav_path_ = g_ui.custom_sav_path.empty() ? (sp_path + ".sav") : g_ui.custom_sav_path;
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

    // Detect PoN ("10000") vs LoN ("10001") from ScratchPad AppParam and initialize local Capcom offline DB
    bool is_lon = (sp_path_.find("LoN") != std::string::npos);
    if (sp_header_offset_ + 410624 + 5 <= sp_data_.size()) {
        std::string ver(reinterpret_cast<const char*>(sp_data_.data() + sp_header_offset_ + 410624), 5);
        if (ver == "10001") is_lon = true;
    }
    g_ui.is_lon_game = is_lon;
    g_capcom_local.set_db_path("capcom_local.db");
    if (sp_data_.size() > sp_header_offset_) {
        g_capcom_local.register_sp_image(
            is_lon,
            sp_data_.data() + sp_header_offset_,
            sp_data_.size() - sp_header_offset_
        );
    }

    load_sav_overlay();
    g_ui.load_config();

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
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "1");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
            std::cerr << "SDL_Init warning: " << SDL_GetError() << "\n";
        } else {
#ifdef __linux__
            prctl(PR_SET_PDEATHSIG, SIGHUP);
#endif
            std::signal(SIGINT, on_termination_signal);
            std::signal(SIGTERM, on_termination_signal);
            std::signal(SIGHUP, on_termination_signal);
            g_ui.load_config();
            if (!std::ifstream(g_ui.config_path).good()) {
                g_ui.save_config();
            }
            g_ui.open_controllers();
            int init_h = g_ui.show_softkey_bar ? 260 : 240;
            SDL_Window* win = SDL_CreateWindow(
                "Rockman EXE Mobile Recomp (PoN / LoN)",
                SDL_WINDOWPOS_CENTERED,
                SDL_WINDOWPOS_CENTERED,
                240 * window_scale_,
                init_h * window_scale_,
                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
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
                    SDL_RenderSetLogicalSize(ren, 240, init_h);
                    sdl_texture_ = SDL_CreateTexture(
                        ren,
                        SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING,
                        240,
                        260
                    );
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
    g_ui.close_controllers();
    g_audio.shutdown_sdl_audio();
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
        g_interp.mark_offscreen_dirty(gfx.target_image);
    }

    int base_dx = dx + gfx.origin_x;
    int base_dy = dy + gfx.origin_y;
    blit_image_buf(*this, dst_buf, dst_w, dst_h, img_ref, base_dx, base_dy, sx, sy, sw, sh, dw, dh, gfx.flip_mode);

    if (gfx.target_image == 0 && g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_) {
        g_interp.record_image(img_ref, base_dx, base_dy, sx, sy, sw, sh, dw, dh, gfx.flip_mode);
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
        g_interp.mark_offscreen_dirty(gfx.target_image);
    }

    uint32_t color = static_cast<uint32_t>(gfx.aux_i32) | 0xFF000000u;
    int pen_x = x + gfx.origin_x;
    int baseline_y = y + gfx.origin_y;
    blit_text_buf(dst_buf, dst_w, dst_h, text, pen_x, baseline_y, color);

    if (gfx.target_image == 0 && g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_) {
        g_interp.record_text(text, pen_x, baseline_y, color);
    }
}

void DojaRuntime::poll_events() {
    if (g_quit_signal.load(std::memory_order_relaxed)) {
        clean_exit_runtime(*this, sdl_window_ != nullptr, 0);
    }
    if (!sdl_window_) return;
    static bool in_modal = false;

    auto sync_action_edge = [&](int act_idx) {
        ActionBinding& ab = g_ui.binds[act_idx];
        bool now_down = ab.key_down[0] || ab.key_down[1] || ab.pad_down || ab.axis_down;
        if (now_down == ab.effective_down) return;
        ab.effective_down = now_down;
        if (ab.doja_key >= 0) {
            if (now_down) {
                keypad_state_ |= (1 << ab.doja_key);
                if (current_canvas_ref_ > 0 && !pvp_match_active_) {
                    generated_process_event(*this, current_canvas_ref_, 0, ab.doja_key);
                }
            } else {
                keypad_state_ &= ~(1 << ab.doja_key);
                if (current_canvas_ref_ > 0 && !pvp_match_active_) {
                    generated_process_event(*this, current_canvas_ref_, 1, ab.doja_key);
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
                generated_process_event(*this, current_canvas_ref_, 0, doja_key);
            }
        } else {
            keypad_state_ &= ~mask;
            if (current_canvas_ref_ > 0 && !pvp_match_active_) {
                generated_process_event(*this, current_canvas_ref_, 1, doja_key);
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
            framebuffer_,
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
        if (ev.type == SDL_QUIT ||
            (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE)) {
            clean_exit_runtime(*this, true, 0);
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            g_ui.open_controllers();
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            if (ev.key.repeat) continue;
            bool pressed = (ev.type == SDL_KEYDOWN);
            SDL_Keycode sym = ev.key.keysym.sym;

            // Emulator hotkeys on key-down
            if (pressed) {
                if (sym == SDLK_F1 || sym == SDLK_ESCAPE) {
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
                if (sym == SDLK_F11 || (sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                    g_ui.toggle_fullscreen(static_cast<SDL_Window*>(sdl_window_));
                    continue;
                }
                if (sym == SDLK_F12) {
                    g_ui.save_screenshot_bmp(framebuffer_);
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
            }
        }
    }
}

void DojaRuntime::present_screen() {
    ++frame_count_;
    // Support deterministic synthetic UI hotkeys in --inject-key (100=F1, 101=Enter, 102=Down)
    for (const auto& kv : injected_keys_) {
        if (kv.start_frame == frame_count_ && kv.doja_key >= 100) {
            SDL_Keycode sym = (kv.doja_key == 100) ? SDLK_F1 : ((kv.doja_key == 101) ? SDLK_RETURN : SDLK_DOWN);
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
                    else if (g_pvp.active()) { keypad_state_ = 0; g_ui.open_overlay(false); }
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
                    generated_process_event(*this, current_canvas_ref_, 0, kv.doja_key);
                }
            } else if (kv.end_frame + 1 == frame_count_) {
                keypad_state_ &= ~(1 << kv.doja_key);
                if (current_canvas_ref_ > 0) {
                    generated_process_event(*this, current_canvas_ref_, 1, kv.doja_key);
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
    if (sdl_renderer_ && sdl_texture_) {
        SDL_Renderer* ren = static_cast<SDL_Renderer*>(sdl_renderer_);
        SDL_Texture* tex = static_cast<SDL_Texture*>(sdl_texture_);
        bool interp_active = g_ui.high_fps_mode && !fast_forward_ && (!g_ui.is_turbo_held() || g_pvp.active());
        g_interp.on_screen_unlock(framebuffer_, interp_active);

        const uint32_t* present_fb = framebuffer_;
        if (interp_active && g_interp.can_interpolate && g_interp.any_motion) {
            g_interp.rasterize_subframe(*this, 0.25f);
            present_fb = g_interp.interp_fb;
        }
        g_ui.compose_display_buffer(present_fb, keypad_state_, true);
        int logical_h = g_ui.show_softkey_bar ? 260 : 240;
        SDL_Rect src_rect{0, 0, 240, logical_h};
        SDL_UpdateTexture(tex, nullptr, g_ui.display_buf, 240 * sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src_rect, nullptr);
        SDL_RenderPresent(ren);
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
            g_ui.compose_display_buffer(framebuffer_, keypad_state_, false);
            int dump_h = g_ui.show_softkey_bar ? 260 : 240;
            std::ofstream ppm(dump_ppm_path_, std::ios::binary | std::ios::trunc);
            if (ppm) {
                ppm << "P6\n240 " << dump_h << "\n255\n";
                for (int i = 0; i < 240 * dump_h; ++i) {
                    uint32_t c = g_ui.display_buf[i];
                    uint8_t rgb[3] = {
                        static_cast<uint8_t>((c >> 16) & 0xFF),
                        static_cast<uint8_t>((c >> 8) & 0xFF),
                        static_cast<uint8_t>(c & 0xFF)
                    };
                    ppm.write(reinterpret_cast<const char*>(rgb), 3);
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
            // During CpCanvas's ~62.5 ms wait between 15 Hz logic ticks, present smooth
            // 60 FPS interpolated sub-frames (alpha = 0.50, 0.75, 1.00) when 60 FPS mode is active
            if (g_ui.high_fps_mode && sdl_renderer_ && sdl_texture_ && g_interp.subframes_presented < 4) {
                auto now_tp = std::chrono::steady_clock::now();
                double elapsed_ms = std::chrono::duration<double, std::milli>(now_tp - g_interp.tick_unlock_tp).count();
                static constexpr double kSubTargetsMs[4] = {0.0, 15.5, 31.0, 46.5};
                int next_sf = g_interp.subframes_presented; // 1 -> 2 -> 3
                if (elapsed_ms >= kSubTargetsMs[next_sf]) {
                    g_interp.subframes_presented = next_sf + 1;
                    const uint32_t* sub_fb = framebuffer_;
                    if (g_interp.can_interpolate && g_interp.any_motion && next_sf < 3) {
                        float alpha = std::clamp(static_cast<float>(elapsed_ms / 62.0), 0.25f, 1.0f);
                        g_interp.rasterize_subframe(*this, alpha);
                        sub_fb = g_interp.interp_fb;
                    }
                    SDL_Renderer* ren = static_cast<SDL_Renderer*>(sdl_renderer_);
                    SDL_Texture* tex = static_cast<SDL_Texture*>(sdl_texture_);
                    g_ui.compose_display_buffer(sub_fb, keypad_state_, false);
                    int logical_h = g_ui.show_softkey_bar ? 260 : 240;
                    SDL_Rect src_rect{0, 0, 240, logical_h};
                    SDL_UpdateTexture(tex, nullptr, g_ui.display_buf, 240 * sizeof(uint32_t));
                    SDL_RenderClear(ren);
                    SDL_RenderCopy(ren, tex, &src_rect, nullptr);
                    SDL_RenderPresent(ren);
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
        if (suppress_av_) return Slot(0);
        if (m == "lock") {
            graphics_locked_ = true;
            if (gfx.target_image == 0) {
                g_interp.on_screen_lock(framebuffer_);
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
                g_interp.mark_offscreen_dirty(gfx.target_image);
            }
            bool is_fill = (m == "fillRect");
            blit_rect_buf(dst_buf, dst_w, dst_h, is_fill, rx, ry, rw, rh, col);
            if (gfx.target_image == 0 && g_ui.high_fps_mode && sdl_renderer_ && !fast_forward_) {
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
            HeapObject ap{};
            ap.kind = ObjKind::AudioPresenter;
            ap.aux_i32 = port;      // Hardware port (0 = BGM, 1..3 = SE)
            ap.origin_x = 0;        // Bound MediaSound Ref
            ap.origin_y = 100;      // Volume (0..100)
            Ref r = static_cast<Ref>(heap_.size());
            heap_.push_back(std::move(ap));
            return Slot(r);
        }
        if (args[0].r <= 0) return Slot(0);
        HeapObject& ap = get(args[0].r);
        if (m == "setSound") {
            ap.origin_x = args[1].r;
            return Slot(0);
        }
        if (m == "setAttribute") {
            if (suppress_av_) return Slot(0);
            if (args[1].i == 4) {
                ap.origin_y = std::clamp(args[2].i, 0, 100);
                g_audio.set_port_volume(ap.aux_i32, args[0].r, ap.origin_y);
            }
            return Slot(0);
        }
        if (m == "play") {
            if (suppress_av_) return Slot(0);
            if (ap.origin_x > 0) {
                int snd_id = get(ap.origin_x).aux_i32;
                g_audio.play_port(ap.aux_i32, snd_id, ap.origin_y, args[0].r, current_time_millis());
            }
            return Slot(0);
        }
        if (m == "stop") {
            if (suppress_av_) return Slot(0);
            g_audio.stop_port(ap.aux_i32, args[0].r);
            return Slot(0);
        }
        if (m == "getCurrentTime") {
            return Slot(g_audio.get_current_time(ap.aux_i32, args[0].r, current_time_millis()));
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
            return Slot(alloc_string(val));
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
        return Slot(alloc_string(net::url_encode(get(args[0].r).str)));
    }

    return Slot(0);
}

} // namespace rmexe
