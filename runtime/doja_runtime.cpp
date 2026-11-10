#include "doja_runtime.hpp"

#include <SDL2/SDL.h>
#include <zlib.h>
#include <iconv.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
// MFi2 (.mld) Polyphonic + 4-bit ADPCM Synthesizer & SDL2 Audio Engine
// ============================================================================
struct MldSound {
    std::vector<int16_t> pcm;
    size_t loop_start_sample = 0;
    size_t loop_end_sample = 0;
    bool has_loop = false;
    int32_t duration_ms = 250;
};

struct MldAudioEngine {
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int NUM_PORTS = 4;

    struct PortState {
        int sound_id = -1;
        size_t sample_pos = 0;
        int volume = 100;
        bool playing = false;
        bool loop = false;
        Ref active_presenter = 0;
        int64_t start_time_ms = 0;
        int32_t duration_ms = 0;
    };

    SDL_AudioDeviceID dev_id = 0;
    std::mutex mtx;
    std::vector<MldSound> sounds;
    PortState ports[NUM_PORTS];
    int master_volume = 100;
    bool muted = false;

    void init_sdl_audio() {
        if (dev_id != 0) return;
        SDL_AudioSpec want{}, have{};
        want.freq = SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
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

    void shutdown_sdl_audio() {
        if (dev_id != 0) {
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
        int num_samples = len / static_cast<int>(sizeof(int16_t));
        std::lock_guard<std::mutex> lk(self->mtx);

        for (int i = 0; i < num_samples; ++i) {
            int32_t mix = 0;
            for (int p = 0; p < NUM_PORTS; ++p) {
                PortState& ps = self->ports[p];
                if (!ps.playing || ps.sound_id < 0 || static_cast<size_t>(ps.sound_id) >= self->sounds.size()) {
                    continue;
                }
                const MldSound& snd = self->sounds[ps.sound_id];
                if (snd.pcm.empty()) {
                    ps.playing = false;
                    continue;
                }
                if (ps.loop && snd.has_loop && ps.sample_pos >= snd.loop_end_sample) {
                    ps.sample_pos = snd.loop_start_sample;
                } else if (ps.sample_pos >= snd.pcm.size()) {
                    if (ps.loop) {
                        ps.sample_pos = snd.has_loop ? snd.loop_start_sample : 0;
                    } else {
                        ps.playing = false;
                        continue;
                    }
                }
                int32_t s = snd.pcm[ps.sample_pos++];
                mix += (s * ps.volume) / 100;
            }
            int eff_vol = self->muted ? 0 : self->master_volume;
            mix = (mix * eff_vol) / 100;
            if (mix > 32767) mix = 32767;
            if (mix < -32768) mix = -32768;
            out[i] = static_cast<int16_t>(mix);
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

    int load_mld(const uint8_t* data, size_t size) {
        MldSound snd;
        if (size < 10 || std::memcmp(data, "melo", 4) != 0) {
            std::lock_guard<std::mutex> lk(mtx);
            int id = static_cast<int>(sounds.size());
            sounds.push_back(std::move(snd));
            return id;
        }

        uint32_t file_len = read_be32(data + 4);
        uint16_t hdr_len = read_be16(data + 8);
        size_t p = 10 + hdr_len;
        size_t end = std::min(size, static_cast<size_t>(8 + file_len));

        std::vector<int16_t> adpcm_8k;

        struct TempoChange {
            int32_t tick;
            double sec_per_tick;
        };
        struct PitchChange {
            int32_t tick;
            int ch;
            double semitones;
        };
        struct NoteEv {
            int32_t start_tick;
            int32_t end_tick;
            int ch;
            int midi_note;
            int velocity;
            int prog;
            int ch_vol;
            bool is_drum;
        };
        struct AdpcmEv {
            int32_t tick;
            bool play;
            int vol;
        };

        std::vector<TempoChange> tempos = {{0, 60.0 / (120.0 * 48.0)}};
        std::vector<PitchChange> pitches;
        std::vector<NoteEv> notes;
        std::vector<AdpcmEv> adpcm_evs;

        int32_t loop_start_tick = 0;
        int32_t loop_end_tick = 0;
        bool has_loop = false;
        int32_t max_tick = 0;

        int bank[16] = {0};
        int prog[16] = {0};
        int ch_vol[16];
        int bend_range[16];
        bool is_drum[16] = {false};
        for (int i = 0; i < 16; ++i) {
            ch_vol[i] = 52;
            bend_range[i] = 2;
        }
        is_drum[9] = true; // MFi2 trac 2 voice 1 (ch 9) is standard percussion

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
                            tp += 5 + slen;
                        } else {
                            uint8_t arg = data[tp + 3];
                            if (cmd == 0xDC) {
                                ticks += static_cast<int32_t>(arg) * 256;
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
                            } else if (cmd == 0xE7) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                bend_range[ch] = std::max(1, static_cast<int>(arg & 0x3F));
                            } else if (cmd == 0xE4) {
                                int v = (arg >> 6) & 3;
                                int ch = trac_idx * 4 + v;
                                int bend_val = static_cast<int>(arg & 0x3F) - 32;
                                double semi = (static_cast<double>(bend_val) / 32.0) * bend_range[ch];
                                pitches.push_back({ticks, ch, semi});
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
                            notes.push_back({ticks, ticks + gate, ch, midi_note, vel, prog[ch], ch_vol[ch], is_drum[ch]});
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

        size_t total_samples = static_cast<size_t>(total_sec * SAMPLE_RATE) + 1;
        std::vector<float> mix_buf(total_samples, 0.0f);

        if (has_loop && loop_end_tick > loop_start_tick) {
            snd.has_loop = true;
            snd.loop_start_sample = std::min(total_samples - 1, static_cast<size_t>(tick_to_sec(loop_start_tick) * SAMPLE_RATE));
            snd.loop_end_sample = std::min(total_samples, static_cast<size_t>(tick_to_sec(loop_end_tick) * SAMPLE_RATE));
        }
        snd.duration_ms = std::max<int32_t>(80, static_cast<int32_t>(total_sec * 1000.0));

        // Pre-convert pitch bend events to sample indices
        struct SamplePitch {
            size_t sample;
            int ch;
            double semitones;
        };
        std::vector<SamplePitch> sample_pitches;
        sample_pitches.reserve(pitches.size());
        for (const auto& pe : pitches) {
            sample_pitches.push_back({static_cast<size_t>(tick_to_sec(pe.tick) * SAMPLE_RATE), pe.ch, pe.semitones});
        }

        // Render polyphonic notes
        uint32_t lfsr = 0xACE1u;
        for (const auto& n : notes) {
            size_t s0 = static_cast<size_t>(tick_to_sec(n.start_tick) * SAMPLE_RATE);
            size_t s1 = static_cast<size_t>(tick_to_sec(n.end_tick) * SAMPLE_RATE);
            if (s0 >= total_samples) continue;
            // Allow short release tail
            size_t rel_samples = n.is_drum ? static_cast<size_t>(0.12 * SAMPLE_RATE) : static_cast<size_t>(0.025 * SAMPLE_RATE);
            size_t s_end = std::min(total_samples, s1 + rel_samples);
            if (s_end <= s0) continue;

            float amp = (static_cast<float>(n.velocity) / 63.0f) * (static_cast<float>(n.ch_vol) / 63.0f) * 0.22f;
            if (amp <= 0.001f) continue;

            double base_freq = 440.0 * std::pow(2.0, (n.midi_note - 69) / 12.0);
            double phase = 0.0;

            for (size_t s = s0; s < s_end; ++s) {
                float t_note = static_cast<float>(s - s0) / SAMPLE_RATE;
                float gate_dur = static_cast<float>(std::max<size_t>(1, s1 - s0)) / SAMPLE_RATE;
                float sample_val = 0.0f;

                if (n.is_drum) {
                    lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
                    float noise = (static_cast<float>(lfsr & 0xFFFF) / 32768.0f) - 1.0f;
                    if (n.midi_note <= 37) {
                        // Kick drum: fast pitch drop 150Hz -> 44Hz
                        float env = std::exp(-t_note * 28.0f);
                        double k_freq = 44.0 + 115.0 * std::exp(-t_note * 45.0);
                        phase += k_freq / SAMPLE_RATE;
                        sample_val = static_cast<float>(std::sin(phase * 6.2831853)) * env * 1.6f;
                    } else if (n.midi_note == 38 || n.midi_note == 40) {
                        // Snare drum: body tone + crisp noise decay
                        float env_n = std::exp(-t_note * 22.0f);
                        float env_b = std::exp(-t_note * 38.0f);
                        phase += 185.0 / SAMPLE_RATE;
                        sample_val = (static_cast<float>(std::sin(phase * 6.2831853)) * env_b * 0.6f + noise * env_n * 0.9f) * 1.2f;
                    } else if (n.midi_note == 42 || n.midi_note == 44) {
                        // Closed hi-hat
                        float env = std::exp(-t_note * 55.0f);
                        sample_val = noise * env * 0.75f;
                    } else if (n.midi_note == 46) {
                        // Open hi-hat
                        float env = std::exp(-t_note * 12.0f);
                        sample_val = noise * env * 0.75f;
                    } else if (n.midi_note >= 49) {
                        // Crash / Ride cymbal
                        float env = std::exp(-t_note * 6.5f);
                        phase += 820.0 / SAMPLE_RATE;
                        float ring = static_cast<float>(std::sin(phase * 6.2831853) * std::sin(phase * 14.9));
                        sample_val = (noise * 0.7f + ring * 0.3f) * env * 0.85f;
                    } else {
                        // Tom
                        float env = std::exp(-t_note * 18.0f);
                        double t_freq = base_freq * (0.7 + 0.6 * std::exp(-t_note * 20.0));
                        phase += t_freq / SAMPLE_RATE;
                        sample_val = static_cast<float>(std::sin(phase * 6.2831853)) * env * 1.3f;
                    }
                } else {
                    // Check active pitch bend for this channel
                    double bend_semi = 0.0;
                    for (const auto& sp : sample_pitches) {
                        if (sp.sample > s) break;
                        if (sp.ch == n.ch) bend_semi = sp.semitones;
                    }
                    double freq = base_freq * std::pow(2.0, bend_semi / 12.0);
                    // Gentle vibrato after 180ms on sustained melody notes
                    if (t_note > 0.18f) {
                        freq *= 1.0 + 0.004 * std::sin((t_note - 0.18f) * 36.0);
                    }
                    phase += freq / SAMPLE_RATE;
                    double p_frac = phase - std::floor(phase);

                    // Envelope (ADSR)
                    float env = 1.0f;
                    if (t_note < 0.004f) {
                        env = t_note / 0.004f;
                    } else if (s < s1) {
                        env = 0.72f + 0.28f * std::exp(-(t_note - 0.004f) * 5.0f);
                    } else {
                        float t_rel = t_note - gate_dur;
                        env = 0.72f * std::exp(-t_rel * 80.0f);
                    }

                    // Timbre selection based on GM/MFi2 program
                    float osc = 0.0f;
                    if (n.prog == 38 || n.prog == 39 || n.midi_note < 48) {
                        // Synth Bass: triangle + 25% pulse sub-harmonic
                        float tri = static_cast<float>(1.0 - 4.0 * std::abs(p_frac - 0.5));
                        float pls = (p_frac < 0.25) ? 0.6f : -0.6f;
                        osc = 0.65f * tri + 0.35f * pls;
                    } else if (n.prog == 80 || n.prog == 16 || (n.prog & 1) == 0) {
                        // Square / Pulse Lead (classic Rockman EXE lead)
                        osc = (p_frac < 0.5) ? 0.8f : -0.8f;
                    } else if (n.prog >= 48 && n.prog <= 55) {
                        // Strings / Pad: warm saw + triangle
                        float saw = static_cast<float>(2.0 * p_frac - 1.0);
                        float tri = static_cast<float>(1.0 - 4.0 * std::abs(p_frac - 0.5));
                        osc = 0.5f * saw + 0.5f * tri;
                    } else {
                        // Sawtooth / Brass Lead (25% pulse + saw)
                        float saw = static_cast<float>(2.0 * p_frac - 1.0);
                        float pls = (p_frac < 0.25) ? 0.75f : -0.75f;
                        osc = 0.5f * saw + 0.5f * pls;
                    }
                    sample_val = osc * env;
                }

                mix_buf[s] += sample_val * amp;
            }
        }

        // Mix 8kHz ADPCM stream events (upsampled to 44.1kHz with linear interpolation)
        if (!adpcm_8k.empty()) {
            auto mix_adpcm_at = [&](size_t start_sample, size_t stop_sample, float vol_scale) {
                size_t max_s = std::min(total_samples, stop_sample);
                for (size_t s = start_sample; s < max_s; ++s) {
                    double pos_8k = static_cast<double>(s - start_sample) * 8000.0 / SAMPLE_RATE;
                    size_t idx0 = static_cast<size_t>(pos_8k);
                    if (idx0 >= adpcm_8k.size()) break;
                    size_t idx1 = std::min(adpcm_8k.size() - 1, idx0 + 1);
                    double frac = pos_8k - idx0;
                    double val = (1.0 - frac) * adpcm_8k[idx0] + frac * adpcm_8k[idx1];
                    mix_buf[s] += static_cast<float>(val / 32768.0) * vol_scale;
                }
            };

            if (adpcm_evs.empty()) {
                mix_adpcm_at(0, total_samples, 0.85f);
            } else {
                for (size_t i = 0; i < adpcm_evs.size(); ++i) {
                    if (!adpcm_evs[i].play) continue;
                    size_t s0 = static_cast<size_t>(tick_to_sec(adpcm_evs[i].tick) * SAMPLE_RATE);
                    size_t s1 = total_samples;
                    for (size_t j = i + 1; j < adpcm_evs.size(); ++j) {
                        s1 = static_cast<size_t>(tick_to_sec(adpcm_evs[j].tick) * SAMPLE_RATE);
                        break;
                    }
                    float vscale = (std::max(20, adpcm_evs[i].vol) / 63.0f) * 0.9f;
                    mix_adpcm_at(s0, s1, vscale);
                }
            }
        }

        // Convert float mix buffer to int16 PCM with soft limiting
        snd.pcm.resize(total_samples);
        for (size_t i = 0; i < total_samples; ++i) {
            float x = mix_buf[i];
            // Soft tanh-like limiter
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            snd.pcm[i] = static_cast<int16_t>(x * 28000.0f);
        }

        std::lock_guard<std::mutex> lk(mtx);
        int id = static_cast<int>(sounds.size());
        sounds.push_back(std::move(snd));
        return id;
    }

    void play_port(int port, int sound_id, int volume, Ref presenter_ref, int64_t now_ms) {
        if (port < 0 || port >= NUM_PORTS) return;
        std::lock_guard<std::mutex> lk(mtx);
        if (sound_id < 0 || static_cast<size_t>(sound_id) >= sounds.size()) return;
        const MldSound& snd = sounds[sound_id];
        PortState& ps = ports[port];
        ps.sound_id = sound_id;
        ps.sample_pos = 0;
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
            ps.sample_pos = 0;
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

struct UiInputManager {
    std::string config_path = "controls.cfg";
    bool show_softkey_bar = true;
    bool is_fullscreen = false;
    std::string soft_label_left;
    std::string soft_label_right;

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

    void load_config() {
        std::ifstream f(config_path);
        if (!f) return;
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
            } else {
                for (int i = 0; i < ACT_COUNT; ++i) {
                    std::string pfx = std::string(binds[i].cfg_id) + "_";
                    if (key == pfx + "key1") binds[i].keys[0] = parse_key_str(val);
                    else if (key == pfx + "key2") binds[i].keys[1] = parse_key_str(val);
                    else if (key == pfx + "pad")  binds[i].pad_btn = parse_pad_str(val);
                }
            }
        }
    }

    void save_config() const {
        std::ofstream f(config_path, std::ios::trunc);
        if (!f) return;
        f << "# rmexe-mobile-recomp keybindings & emulator settings\n";
        {
            std::lock_guard<std::mutex> lk(g_audio.mtx);
            f << "master_volume=" << g_audio.master_volume << "\n";
            f << "muted=" << (g_audio.muted ? 1 : 0) << "\n";
        }
        f << "show_softkey_bar=" << (show_softkey_bar ? 1 : 0) << "\n\n";
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
        fill_rect_alpha(display_buf, 240, 260, 2, 242, 92, 16, s1_held ? 0xFF2B5EA8u : 0xFF202942u);
        draw_rect_border(display_buf, 240, 260, 2, 242, 92, 16, s1_held ? 0xFF74B2FFu : 0xFF394970u);
        draw_ui_text(display_buf, 240, 260, left_txt, 5, 254, soft_label_left.empty() ? 0xFF7080A0u : 0xFFF0F6FFu);

        // Center Status / F1 Hint
        if (is_turbo_held()) {
            std::string c_txt = ">> 倍速";
            int cw = measure_ui_text(c_txt);
            draw_ui_text(display_buf, 240, 260, c_txt, 120 - cw / 2, 254, 0xFFFFD448u);
        } else {
            std::string c_txt = "F1:设置";
            int cw = measure_ui_text(c_txt);
            draw_ui_text(display_buf, 240, 260, c_txt, 120 - cw / 2, 254, 0xFF8A9CC2u);
        }

        // Right Softkey (SOFT_KEY_2 -> DoJa 0x16)
        bool s2_held = (keypad_state & (1 << 0x16)) != 0;
        std::string k2 = primary_soft_key_label(ACT_SOFT2);
        std::string right_txt = soft_label_right.empty() ? ("--- [" + k2 + "]") : (soft_label_right + " [" + k2 + "]");
        fill_rect_alpha(display_buf, 240, 260, 146, 242, 92, 16, s2_held ? 0xFF2B5EA8u : 0xFF202942u);
        draw_rect_border(display_buf, 240, 260, 146, 242, 92, 16, s2_held ? 0xFF74B2FFu : 0xFF394970u);
        int rw = measure_ui_text(right_txt);
        draw_ui_text(display_buf, 240, 260, right_txt, std::max(149, 235 - rw), 254, soft_label_right.empty() ? 0xFF7080A0u : 0xFFF0F6FFu);
    }

    void compose_display_buffer(const uint32_t* game_fb, int32_t keypad_state, bool tick_toast = true) {
        std::memcpy(display_buf, game_fb, 240 * 240 * sizeof(uint32_t));
        if (show_softkey_bar) {
            render_softkey_bar(keypad_state);
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

    // Returns true if user chose "Save & Quit"
    bool run_pause_config_modal(SDL_Window* win, SDL_Renderer* ren, SDL_Texture* tex, const uint32_t* game_fb, int scale) {
        if (!win || !ren || !tex) return false;
        auto pause_t0 = std::chrono::steady_clock::now();
        if (g_audio.dev_id != 0) SDL_PauseAudioDevice(g_audio.dev_id, 1);

        constexpr int TOTAL_ROWS = ACT_COUNT + 5; // 10 binds + Vol + SoftBar + Reset + Resume + Quit
        constexpr int VISIBLE_ROWS = 9;
        int sel_row = 0;
        int sel_col = 0; // 0 = Key1, 1 = Key2, 2 = Pad
        int scroll_top = 0;
        bool waiting_bind = false;
        bool menu_open = true;
        bool quit_game = false;

        while (menu_open) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) {
                    quit_game = true;
                    menu_open = false;
                    break;
                } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    open_controllers();
                } else if (waiting_bind) {
                    if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                        SDL_Keycode k = ev.key.keysym.sym;
                        if (k == SDLK_ESCAPE || k == SDLK_F1) {
                            waiting_bind = false;
                        } else if (k == SDLK_BACKSPACE || k == SDLK_DELETE) {
                            if (sel_col < 2) assign_key_unique(sel_row, sel_col, SDLK_UNKNOWN);
                            else assign_pad_unique(sel_row, SDL_CONTROLLER_BUTTON_INVALID);
                            save_config();
                            waiting_bind = false;
                        } else {
                            int target_slot = (sel_col < 2) ? sel_col : 0;
                            assign_key_unique(sel_row, target_slot, k);
                            save_config();
                            waiting_bind = false;
                        }
                    } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                        assign_pad_unique(sel_row, ev.cbutton.button);
                        sel_col = 2;
                        save_config();
                        waiting_bind = false;
                    }
                } else {
                    auto adjust_row_lr = [&](int dir) {
                        if (sel_row < ACT_COUNT) {
                            sel_col = (sel_col + dir + 3) % 3;
                        } else if (sel_row == ACT_COUNT) {
                            std::lock_guard<std::mutex> lk(g_audio.mtx);
                            g_audio.muted = false;
                            g_audio.master_volume = std::clamp(g_audio.master_volume + dir * 5, 0, 100);
                            save_config();
                        } else if (sel_row == ACT_COUNT + 1) {
                            show_softkey_bar = !show_softkey_bar;
                            apply_softbar_geometry(win, ren, scale);
                            save_config();
                        }
                    };
                    auto activate_row = [&]() {
                        if (sel_row < ACT_COUNT) {
                            waiting_bind = true;
                        } else if (sel_row == ACT_COUNT) {
                            {
                                std::lock_guard<std::mutex> lk(g_audio.mtx);
                                g_audio.muted = !g_audio.muted;
                            }
                            save_config();
                        } else if (sel_row == ACT_COUNT + 1) {
                            show_softkey_bar = !show_softkey_bar;
                            apply_softbar_geometry(win, ren, scale);
                            save_config();
                        } else if (sel_row == ACT_COUNT + 2) {
                            reset_defaults();
                            save_config();
                            show_toast("已恢复默认按键配置");
                        } else if (sel_row == ACT_COUNT + 3) {
                            menu_open = false;
                        } else if (sel_row == ACT_COUNT + 4) {
                            quit_game = true;
                            menu_open = false;
                        }
                    };

                    if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                        SDL_Keycode k = ev.key.keysym.sym;
                        if (k == SDLK_F1 || k == SDLK_ESCAPE) {
                            menu_open = false;
                        } else if (k == SDLK_UP) {
                            sel_row = (sel_row - 1 + TOTAL_ROWS) % TOTAL_ROWS;
                        } else if (k == SDLK_DOWN) {
                            sel_row = (sel_row + 1) % TOTAL_ROWS;
                        } else if (k == SDLK_LEFT) {
                            adjust_row_lr(-1);
                        } else if (k == SDLK_RIGHT) {
                            adjust_row_lr(1);
                        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) {
                            activate_row();
                        } else if ((k == SDLK_DELETE || k == SDLK_BACKSPACE) && sel_row < ACT_COUNT) {
                            if (sel_col < 2) assign_key_unique(sel_row, sel_col, SDLK_UNKNOWN);
                            else assign_pad_unique(sel_row, SDL_CONTROLLER_BUTTON_INVALID);
                            save_config();
                        } else if (k == SDLK_F11) {
                            toggle_fullscreen(win);
                        }
                    } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                        switch (ev.cbutton.button) {
                            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                                sel_row = (sel_row - 1 + TOTAL_ROWS) % TOTAL_ROWS; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                                sel_row = (sel_row + 1) % TOTAL_ROWS; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                                adjust_row_lr(-1); break;
                            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                                adjust_row_lr(1); break;
                            case SDL_CONTROLLER_BUTTON_A:
                                activate_row(); break;
                            case SDL_CONTROLLER_BUTTON_B:
                            case SDL_CONTROLLER_BUTTON_START:
                                menu_open = false; break;
                            default: break;
                        }
                    }
                }
            }

            if (sel_row < scroll_top) scroll_top = sel_row;
            if (sel_row >= scroll_top + VISIBLE_ROWS) scroll_top = sel_row - VISIBLE_ROWS + 1;

            // Compose background + translucent modal panel
            compose_display_buffer(game_fb, 0, true);
            fill_rect_alpha(display_buf, 240, 260, 0, 0, 240, 240, 0xDC080C18u);

            // Title bar
            fill_rect_alpha(display_buf, 240, 260, 4, 4, 232, 18, 0xFF1A284Au);
            draw_rect_border(display_buf, 240, 260, 4, 4, 232, 18, 0xFF4F78C8u);
            std::string title = "按键与模拟器设置 (F1 / Esc)";
            draw_ui_text(display_buf, 240, 260, title, 120 - measure_ui_text(title) / 2, 17, 0xFFFFFFFFu);

            // Column headers
            draw_ui_text(display_buf, 240, 260, "动作",  8,   34, 0xFF8EB0E8u);
            draw_ui_text(display_buf, 240, 260, "键位1", 80,  34, 0xFF8EB0E8u);
            draw_ui_text(display_buf, 240, 260, "键位2", 136, 34, 0xFF8EB0E8u);
            draw_ui_text(display_buf, 240, 260, "手柄",  192, 34, 0xFF8EB0E8u);
            fill_rect_alpha(display_buf, 240, 260, 6, 37, 228, 1, 0xFF324672u);

            // Rows viewport (y = 40 .. 193, 9 rows * 17px)
            for (int vi = 0; vi < VISIBLE_ROWS; ++vi) {
                int r = scroll_top + vi;
                if (r >= TOTAL_ROWS) break;
                int ry = 40 + vi * 17;
                bool is_sel = (r == sel_row);
                if (is_sel) {
                    fill_rect_alpha(display_buf, 240, 260, 5, ry, 226, 16, 0xFF223A6Au);
                    draw_rect_border(display_buf, 240, 260, 5, ry, 226, 16, 0xFF5A8AE0u);
                }

                if (r < ACT_COUNT) {
                    draw_ui_text(display_buf, 240, 260, binds[r].label_zh, 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                    std::string cols[3] = {
                        short_key_name(binds[r].keys[0]),
                        short_key_name(binds[r].keys[1]),
                        short_pad_name(binds[r].pad_btn)
                    };
                    int cx[3] = {74, 130, 186};
                    int cw[3] = {52, 52, 42};
                    for (int c = 0; c < 3; ++c) {
                        bool cell_sel = is_sel && (sel_col == c);
                        if (cell_sel) {
                            fill_rect_alpha(display_buf, 240, 260, cx[c], ry + 1, cw[c], 14, 0xFF345898u);
                            draw_rect_border(display_buf, 240, 260, cx[c], ry + 1, cw[c], 14, 0xFFFFD24Cu);
                        }
                        int tw = measure_ui_text(cols[c]);
                        uint32_t tcol = (cols[c] == "---") ? 0xFF687898u : (cell_sel ? 0xFFFFEC88u : 0xFFE8F0FFu);
                        draw_ui_text(display_buf, 240, 260, cols[c], cx[c] + (cw[c] - tw) / 2, ry + 12, tcol);
                    }
                } else if (r == ACT_COUNT) {
                    int vol = 100;
                    bool muted = false;
                    {
                        std::lock_guard<std::mutex> lk(g_audio.mtx);
                        vol = g_audio.master_volume;
                        muted = g_audio.muted;
                    }
                    draw_ui_text(display_buf, 240, 260, "主音量", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                    std::string vstr = muted ? ("< 静音 (" + std::to_string(vol) + "%) >") : ("< " + std::to_string(vol) + "% >");
                    draw_ui_text(display_buf, 240, 260, vstr, 110, ry + 12, muted ? 0xFFFF8888u : 0xFF88FFAAu);
                } else if (r == ACT_COUNT + 1) {
                    draw_ui_text(display_buf, 240, 260, "底部软键栏", 8, ry + 12, is_sel ? 0xFFFFFFFFu : 0xFFD0DCF0u);
                    std::string sstr = show_softkey_bar ? "< 开启 >" : "< 关闭 >";
                    draw_ui_text(display_buf, 240, 260, sstr, 118, ry + 12, show_softkey_bar ? 0xFF88FFAAu : 0xFF98A8C0u);
                } else if (r == ACT_COUNT + 2) {
                    std::string lbl = "[ 恢复默认按键配置 ]";
                    draw_ui_text(display_buf, 240, 260, lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFFFFD866u : 0xFFC8D8F8u);
                } else if (r == ACT_COUNT + 3) {
                    std::string lbl = "[ 返回游戏 (Resume) ]";
                    draw_ui_text(display_buf, 240, 260, lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFF88FFAAu : 0xFFC8D8F8u);
                } else if (r == ACT_COUNT + 4) {
                    std::string lbl = "[ 保存进度并退出 ]";
                    draw_ui_text(display_buf, 240, 260, lbl, 120 - measure_ui_text(lbl) / 2, ry + 12, is_sel ? 0xFFFF8888u : 0xFFE8B8B8u);
                }
            }

            // Scrollbar track on right edge
            fill_rect_alpha(display_buf, 240, 260, 233, 40, 3, VISIBLE_ROWS * 17, 0xFF182238u);
            int sb_h = std::max(14, (VISIBLE_ROWS * 17 * VISIBLE_ROWS) / TOTAL_ROWS);
            int sb_y = 40 + ((VISIBLE_ROWS * 17 - sb_h) * scroll_top) / std::max(1, TOTAL_ROWS - VISIBLE_ROWS);
            fill_rect_alpha(display_buf, 240, 260, 233, sb_y, 3, sb_h, 0xFF6894E0u);

            // Footer instructions
            fill_rect_alpha(display_buf, 240, 260, 6, 196, 228, 1, 0xFF324672u);
            draw_ui_text(display_buf, 240, 260, "↑↓←→:选择  Enter:改键  Del:清空", 8, 211, 0xFFA4BCE4u);
            draw_ui_text(display_buf, 240, 260, "F2:软键栏  F11:全屏  F12:保存截图", 8, 227, 0xFF849AC0u);

            // Waiting for key modal popup
            if (waiting_bind && sel_row < ACT_COUNT) {
                fill_rect_alpha(display_buf, 240, 260, 12, 72, 216, 86, 0xF4101830u);
                draw_rect_border(display_buf, 240, 260, 12, 72, 216, 86, 0xFFFFD24Cu);
                std::string p1 = std::string("正在设置: ") + binds[sel_row].label_zh;
                std::string p2 = "请按下键盘按键或手柄按钮...";
                std::string p3 = "Esc:取消   Backspace:清空槽位";
                draw_ui_text(display_buf, 240, 260, p1, 120 - measure_ui_text(p1) / 2, 95,  0xFFFFE070u);
                draw_ui_text(display_buf, 240, 260, p2, 120 - measure_ui_text(p2) / 2, 118, 0xFFFFFFFFu);
                draw_ui_text(display_buf, 240, 260, p3, 120 - measure_ui_text(p3) / 2, 142, 0xFFA0B8E0u);
            }

            int logical_h = show_softkey_bar ? 260 : 240;
            SDL_Rect src_rect{0, 0, 240, logical_h};
            SDL_UpdateTexture(tex, nullptr, display_buf, 240 * static_cast<int>(sizeof(uint32_t)));
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, &src_rect, nullptr);
            SDL_RenderPresent(ren);
            SDL_Delay(16);
        }

        clear_held_states();
        if (g_audio.dev_id != 0) SDL_PauseAudioDevice(g_audio.dev_id, 0);
        auto pause_t1 = std::chrono::steady_clock::now();
        paused_duration_ms += std::chrono::duration_cast<std::chrono::milliseconds>(pause_t1 - pause_t0).count();
        return quit_game;
    }
};

static UiInputManager g_ui;

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

void DojaRuntime::set_config_path(const std::string& path) {
    if (!path.empty()) {
        g_ui.config_path = path;
    }
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
            g_ui.show_toast("按 F1 / Esc 打开按键与模拟器设置", 120);
        }
    }

    std::fill(std::begin(framebuffer_), std::end(framebuffer_), 0xFF000000u);
    return true;
}

void DojaRuntime::shutdown() {
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
    static bool in_modal = false;

    auto sync_action_edge = [&](int act_idx) {
        ActionBinding& ab = g_ui.binds[act_idx];
        bool now_down = ab.key_down[0] || ab.key_down[1] || ab.pad_down || ab.axis_down;
        if (now_down == ab.effective_down) return;
        ab.effective_down = now_down;
        if (ab.doja_key >= 0) {
            if (now_down) {
                keypad_state_ |= (1 << ab.doja_key);
                if (current_canvas_ref_ > 0) {
                    generated_process_event(*this, current_canvas_ref_, 0, ab.doja_key);
                }
            } else {
                keypad_state_ &= ~(1 << ab.doja_key);
                if (current_canvas_ref_ > 0) {
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
            if (current_canvas_ref_ > 0) {
                generated_process_event(*this, current_canvas_ref_, 0, doja_key);
            }
        } else {
            keypad_state_ &= ~mask;
            if (current_canvas_ref_ > 0) {
                generated_process_event(*this, current_canvas_ref_, 1, doja_key);
            }
        }
    };

    auto open_pause_modal = [&]() {
        if (in_modal) return;
        in_modal = true;
        keypad_state_ = 0;
        g_ui.clear_held_states();
        bool quit = g_ui.run_pause_config_modal(
            static_cast<SDL_Window*>(sdl_window_),
            static_cast<SDL_Renderer*>(sdl_renderer_),
            static_cast<SDL_Texture*>(sdl_texture_),
            framebuffer_,
            window_scale_
        );
        in_modal = false;
        if (quit) {
            save_sav_overlay();
            g_ui.save_config();
            std::exit(0);
        }
    };

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            save_sav_overlay();
            g_ui.save_config();
            std::exit(0);
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            g_ui.open_controllers();
        } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
            if (ev.key.repeat) continue;
            bool pressed = (ev.type == SDL_KEYDOWN);
            SDL_Keycode sym = ev.key.keysym.sym;

            // Emulator hotkeys on key-down
            if (pressed) {
                if (sym == SDLK_F1 || sym == SDLK_ESCAPE) {
                    open_pause_modal();
                    return;
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
                open_pause_modal();
                return;
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
        g_ui.compose_display_buffer(framebuffer_, keypad_state_, true);
        int logical_h = g_ui.show_softkey_bar ? 260 : 240;
        SDL_Rect src_rect{0, 0, 240, logical_h};
        SDL_UpdateTexture(tex, nullptr, g_ui.display_buf, 240 * sizeof(uint32_t));
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, &src_rect, nullptr);
        SDL_RenderPresent(ren);
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
    if (g_ui.is_turbo_held()) {
        g_ui.turbo_bonus_ms += 12;
    }
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return wall_ms - g_ui.paused_duration_ms + g_ui.turbo_bonus_ms;
}

void DojaRuntime::sleep_ms(int64_t ms) {
    poll_events();
    if (fast_forward_) return;
    if (g_ui.is_turbo_held()) {
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
        if (m == "setSoftLabel") {
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
            if (args[1].i == 4) {
                ap.origin_y = std::clamp(args[2].i, 0, 100);
                g_audio.set_port_volume(ap.aux_i32, args[0].r, ap.origin_y);
            }
            return Slot(0);
        }
        if (m == "play") {
            if (ap.origin_x > 0) {
                int snd_id = get(ap.origin_x).aux_i32;
                g_audio.play_port(ap.aux_i32, snd_id, ap.origin_y, args[0].r, current_time_millis());
            }
            return Slot(0);
        }
        if (m == "stop") {
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
