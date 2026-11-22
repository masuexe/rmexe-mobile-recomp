// ============================================================================
// runtime/collection_main.cpp
// Unified All-in-One Collection Launcher (PoN JP / PoN EN / LoN JP / LoN EN)
// Builds as:
//   - Android APK native library (libmain.so via SDL_main)
//   - iOS IPA app binary (via SDL_main, sideloadable with AltStore)
//   - Desktop unified launcher (rmexe_collection via main)
// ============================================================================

#if !defined(__ANDROID__) && !defined(__IPHONEOS__)
#define SDL_MAIN_HANDLED
#endif

#include "doja_runtime.hpp"
#include "net_config.hpp"

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace rmexe {

#ifdef RMEXE_HAVE_PON_JP
namespace game_pon_jp { const GameModule& get_game_module(); }
#endif
#ifdef RMEXE_HAVE_PON_EN
namespace game_pon_en { const GameModule& get_game_module(); }
#endif
#ifdef RMEXE_HAVE_LON_JP
namespace game_lon_jp { const GameModule& get_game_module(); }
#endif
#ifdef RMEXE_HAVE_LON_EN
namespace game_lon_en { const GameModule& get_game_module(); }
#endif

namespace {

struct RomVariant {
    bool compiled = false;
    bool assets_ready = false;
    const char* jar_rel = "";
    const char* sp_rel = "";
    const GameModule* mod = nullptr;
};

struct CollectionTitle {
    const char* id = "";
    const char* jp_zh_title = "";
    const char* en_title = "";
    const char* subtitle = "";
    const char* zh_pack_name = "";
    bool zh_pack_ready = false;
    RomVariant jp;
    RomVariant en;
};

enum class LauncherLang : int {
    Zh = 0,
    Jp = 1,
    En = 2,
};

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

bool is_lang_ready(const CollectionTitle& t, LauncherLang lang) {
    if (lang == LauncherLang::Zh) return t.jp.compiled && t.jp.assets_ready && t.zh_pack_ready;
    if (lang == LauncherLang::Jp) return t.jp.compiled && t.jp.assets_ready;
    return t.en.compiled && t.en.assets_ready;
}

const RomVariant& variant_for_lang(const CollectionTitle& t, LauncherLang lang) {
    return (lang == LauncherLang::En) ? t.en : t.jp;
}

std::string title_for_lang(const CollectionTitle& t, LauncherLang lang) {
    if (lang == LauncherLang::Zh) return std::string(t.jp_zh_title) + " (ZH)";
    if (lang == LauncherLang::Jp) return std::string(t.jp_zh_title) + " (JP)";
    return std::string(t.en_title) + " (EN)";
}

const char* status_for_lang(const CollectionTitle& t, LauncherLang lang) {
    if (lang == LauncherLang::Zh) {
        if (!t.jp.compiled || !t.jp.assets_ready) return "MISSING JP ROM IN ASSETS";
        if (!t.zh_pack_ready) return "MISSING ZH PATCH IN ASSETS";
        return "READY - JP ROM + ZH PATCH";
    }
    if (lang == LauncherLang::Jp) {
        if (!t.jp.compiled || !t.jp.assets_ready) return "MISSING JP ROM IN ASSETS";
        return "READY - OFFICIAL JP ROM";
    }
    if (!t.en.compiled || !t.en.assets_ready) return "MISSING EN ROM IN ASSETS";
    return "READY - FAN EN ROM";
}

#ifdef __ANDROID__
// On Android, unpack bundled read-only APK assets (fonts, translations, optional .jar/.sp)
// into app-private external storage so DojaRuntime can read them with standard std::ifstream.
void unpack_android_asset_if_missing(const std::string& asset_rel, const std::filesystem::path& dst) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(dst, ec) && std::filesystem::file_size(dst, ec) > 0) {
        return;
    }
    SDL_RWops* rw = SDL_RWFromFile(asset_rel.c_str(), "rb");
    if (!rw) return;
    std::filesystem::create_directories(dst.parent_path(), ec);
    FILE* out = std::fopen(dst.string().c_str(), "wb");
    if (out) {
        char buf[16384];
        size_t n = 0;
        while ((n = SDL_RWread(rw, buf, 1, sizeof(buf))) > 0) {
            std::fwrite(buf, 1, n, out);
        }
        std::fclose(out);
    }
    SDL_RWclose(rw);
}

void bootstrap_android_assets(const std::string& data_dir) {
    if (data_dir.empty()) return;
    std::filesystem::path root(data_dir);
    static const char* kBundledFiles[] = {
        "fonts/wqy-microhei.ttc",
        "translations/pon_zh.json",
        "translations/lon_zh.json",
        "PoN_jp/bin/PoN.jar", "PoN_jp/sp/PoN.sp",
        "PoN/bin/PoN.jar",    "PoN/sp/PoN.sp",
        "LoN_jp/bin/LoN.jar", "LoN_jp/sp/LoN.sp",
        "LoN/bin/LoN.jar",    "LoN/sp/LoN.sp",
    };
    for (const char* rel : kBundledFiles) {
        unpack_android_asset_if_missing(rel, root / "assets" / rel);
    }
}
#endif

void ensure_user_rom_dirs(const std::string& root_dir) {
    if (root_dir.empty()) return;
    std::error_code ec;
    std::filesystem::path root(root_dir);
    static const char* kGames[] = {"PoN_jp", "PoN", "LoN_jp", "LoN"};
    for (const char* g : kGames) {
        std::filesystem::create_directories(root / "assets" / g / "bin", ec);
        std::filesystem::create_directories(root / "assets" / g / "sp", ec);
    }
}

std::string resolve_entry_path(const std::string& rel, const std::vector<std::string>& search_dirs) {
    std::error_code ec;
    for (const auto& dir : search_dirs) {
        if (dir.empty()) continue;
        auto p = std::filesystem::path(dir) / rel;
        if (std::filesystem::is_regular_file(p, ec)) return p.string();
    }
    if (std::filesystem::is_regular_file(rel, ec)) return rel;
    return rel;
}

bool is_translation_ready(const std::string& fname, const std::vector<std::string>& search_dirs) {
    std::error_code ec;
    const std::string rel_assets = "assets/translations/" + fname;
    const std::string rel_plain = "translations/" + fname;
    std::string p1 = resolve_entry_path(rel_assets, search_dirs);
    if (std::filesystem::is_regular_file(p1, ec) && std::filesystem::file_size(p1, ec) > 0) return true;
    std::string p2 = resolve_entry_path(rel_plain, search_dirs);
    if (std::filesystem::is_regular_file(p2, ec) && std::filesystem::file_size(p2, ec) > 0) return true;
    return false;
}

// Simple 5x7 ASCII bitmap text for the pure-SDL2 launcher (zero external font deps)
void draw_launcher_char(SDL_Renderer* ren, char ch, int x, int y, int s) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    static const struct { char c; uint16_t bits; } kFont[] = {
        {'A', 0b010101111101101}, {'B', 0b110101110101110}, {'C', 0b011100100100011},
        {'D', 0b110101101101110}, {'E', 0b111100110100111}, {'F', 0b111100110100100},
        {'G', 0b011100101101011}, {'H', 0b101101111101101}, {'I', 0b111010010010111},
        {'J', 0b001001001101010}, {'K', 0b101101110101101}, {'L', 0b100100100100111},
        {'M', 0b101111101101101}, {'N', 0b110101101101101}, {'O', 0b010101101101010},
        {'P', 0b110101110100100}, {'Q', 0b010101101111011}, {'R', 0b110101110101101},
        {'S', 0b011100010001110}, {'T', 0b111010010010010}, {'U', 0b101101101101111},
        {'V', 0b101101101101010}, {'W', 0b101101101111101}, {'X', 0b101101010101101},
        {'Y', 0b101101010010010}, {'Z', 0b111001010100111}, {'0', 0b111101101101111},
        {'1', 0b010110010010111}, {'2', 0b111001111100111}, {'3', 0b111001111001111},
        {'4', 0b101101111001001}, {'5', 0b111100111001111}, {'6', 0b111100111101111},
        {'7', 0b111001001010010}, {'8', 0b111101111101111}, {'9', 0b111101111001111},
        {'-', 0b000000111000000}, {':', 0b000010000010000}, {'/', 0b001001010100100},
        {'.', 0b000000000000010}, {'(', 0b001010010010001}, {')', 0b100010010010100},
        {'[', 0b110100100100110}, {']', 0b011001001001011}, {'!', 0b010010010000010},
        {',', 0b000000000010100}, {'+', 0b000010111010000}, {'<', 0b001010100010001},
        {'>', 0b100010001010100},
    };
    uint16_t bits = 0;
    for (const auto& g : kFont) {
        if (g.c == ch) { bits = g.bits; break; }
    }
    if (bits == 0) return;
    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
            int shift = (4 - row) * 3 + (2 - col);
            if (bits & (1u << shift)) {
                SDL_Rect px{x + col * s, y + row * s, s, s};
                SDL_RenderFillRect(ren, &px);
            }
        }
    }
}

void draw_launcher_text(SDL_Renderer* ren, const std::string& txt, int x, int y, int s,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    SDL_SetRenderDrawColor(ren, r, g, b, a);
    for (size_t i = 0; i < txt.size(); ++i) {
        draw_launcher_char(ren, txt[i], x + static_cast<int>(i) * (4 * s), y, s);
    }
}

void draw_launcher_text_centered(SDL_Renderer* ren, const std::string& txt, const SDL_Rect& box, int y, int s,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    int tw = txt.empty() ? 0 : static_cast<int>(txt.size()) * (4 * s) - s;
    int x = box.x + std::max(0, (box.w - tw) / 2);
    draw_launcher_text(ren, txt, x, y, s, r, g, b, a);
}

bool run_collection_launcher(const std::vector<CollectionTitle>& titles,
                             int& selected_title,
                             LauncherLang& selected_lang,
                             SDL_Window*& out_win,
                             SDL_Renderer*& out_ren) {
    out_win = nullptr;
    out_ren = nullptr;
    if (titles.empty()) return false;
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("RMEXE Launcher SDL_Init failed: %s", SDL_GetError());
        return false;
    }

#if defined(__ANDROID__) || defined(__IPHONEOS__)
    uint32_t win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    int init_w = 0, init_h = 0;
#else
    uint32_t win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    int init_w = 540, init_h = 900;
#endif
    SDL_Window* win = SDL_CreateWindow("RMEXE Mobile Collection (PoN + LoN)",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       init_w, init_h, win_flags);
    if (!win) {
        SDL_Log("RMEXE Launcher SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        SDL_Log("RMEXE Launcher SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        return false;
    }

    std::vector<SDL_GameController*> pads;
    auto refresh_pads = [&]() {
        for (SDL_GameController* gc : pads) {
            if (gc) SDL_GameControllerClose(gc);
        }
        pads.clear();
        int njoy = SDL_NumJoysticks();
        for (int i = 0; i < njoy; ++i) {
            if (SDL_IsGameController(i)) {
                if (SDL_GameController* gc = SDL_GameControllerOpen(i)) {
                    pads.push_back(gc);
                }
            }
        }
    };
    refresh_pads();

    bool launched = false;
    bool running = true;
    int stick_x_dir = 0;
    int stick_y_dir = 0;
    const int n = static_cast<int>(titles.size());
    std::vector<SDL_Rect> card_rects(n);
    SDL_Rect lang_rects[3]{};

    auto step_lang = [&](int delta) {
        int cur = (static_cast<int>(selected_lang) + delta + 3) % 3;
        selected_lang = static_cast<LauncherLang>(cur);
    };

    while (running) {
        int out_w = 0, out_h = 0;
        SDL_GetRendererOutputSize(ren, &out_w, &out_h);
        if (out_w <= 0 || out_h <= 0) {
            SDL_GetWindowSize(win, &out_w, &out_h);
        }
        if (out_w <= 0) out_w = 720;
        if (out_h <= 0) out_h = 1280;

        const int pad = std::max(14, out_w / 20);
        const int card_w = std::min(out_w - pad * 2, 760);
        const int card_x = (out_w - card_w) / 2;

        const int title_s = std::clamp(out_w / 140, 3, 6);
        const int sub_s = std::max(2, title_s - 1);
        const int title_y = std::max(18, out_h / 26);
        const int sub_y = title_y + title_s * 6 + 4;

        const int lang_y = sub_y + sub_s * 5 + std::max(14, out_h / 36);
        const int lang_h = std::clamp(out_h / 11, 50, 82);
        const int lang_gap = std::max(8, card_w / 40);
        const int btn_w = (card_w - lang_gap * 2) / 3;
        for (int k = 0; k < 3; ++k) {
            lang_rects[k] = SDL_Rect{card_x + k * (btn_w + lang_gap), lang_y, btn_w, lang_h};
        }

        const int fs = std::clamp(card_w / 180, 2, 3);
        const int footer_h = fs * 14 + std::max(18, out_h / 36);
        const int games_top = lang_y + lang_h + std::max(14, out_h / 32);
        const int games_avail = std::max(140, out_h - games_top - footer_h - pad);
        const int gap = std::max(14, games_avail / 18);
        const int card_h = std::clamp((games_avail - gap * std::max(0, n - 1)) / std::max(1, n), 76, 175);
        for (int i = 0; i < n; ++i) {
            card_rects[i] = SDL_Rect{card_x, games_top + i * (card_h + gap), card_w, card_h};
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || ev.type == SDL_APP_TERMINATING) {
                running = false;
                launched = false;
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                refresh_pads();
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                int b = ev.cbutton.button;
                if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    selected_title = (selected_title + n - 1) % n;
                } else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    selected_title = (selected_title + 1) % n;
                } else if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                    step_lang(-1);
                } else if (b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT || b == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                    step_lang(1);
                } else if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START) {
                    if (is_lang_ready(titles[selected_title], selected_lang)) {
                        launched = true;
                        running = false;
                    }
                } else if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_BACK) {
                    running = false;
                    launched = false;
                }
            } else if (ev.type == SDL_CONTROLLERAXISMOTION) {
                if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                    int nd = (ev.caxis.value < -16000) ? -1 : ((ev.caxis.value > 16000) ? 1 : 0);
                    if (nd != 0 && nd != stick_y_dir) {
                        selected_title = (selected_title + n + nd) % n;
                    }
                    stick_y_dir = nd;
                } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                    int nd = (ev.caxis.value < -16000) ? -1 : ((ev.caxis.value > 16000) ? 1 : 0);
                    if (nd != 0 && nd != stick_x_dir) {
                        step_lang(nd);
                    }
                    stick_x_dir = nd;
                }
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE || k == SDLK_AC_BACK) {
                    running = false;
                    launched = false;
                } else if (k == SDLK_UP || k == SDLK_w) {
                    selected_title = (selected_title + n - 1) % n;
                } else if (k == SDLK_DOWN || k == SDLK_s) {
                    selected_title = (selected_title + 1) % n;
                } else if (k == SDLK_LEFT || k == SDLK_a) {
                    step_lang(-1);
                } else if (k == SDLK_RIGHT || k == SDLK_d) {
                    step_lang(1);
                } else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_z) {
                    if (is_lang_ready(titles[selected_title], selected_lang)) {
                        launched = true;
                        running = false;
                    }
                }
            } else if (ev.type == SDL_FINGERUP || (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)) {
                int px = (ev.type == SDL_FINGERUP) ? static_cast<int>(ev.tfinger.x * out_w) : ev.button.x;
                int py = (ev.type == SDL_FINGERUP) ? static_cast<int>(ev.tfinger.y * out_h) : ev.button.y;
                bool handled = false;
                for (int k = 0; k < 3; ++k) {
                    const SDL_Rect& lr = lang_rects[k];
                    if (px >= lr.x && px <= lr.x + lr.w && py >= lr.y && py <= lr.y + lr.h) {
                        selected_lang = static_cast<LauncherLang>(k);
                        handled = true;
                        break;
                    }
                }
                if (!handled) {
                    for (int i = 0; i < n; ++i) {
                        const SDL_Rect& r = card_rects[i];
                        if (px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h) {
                            selected_title = i;
                            if (is_lang_ready(titles[i], selected_lang)) {
                                launched = true;
                                running = false;
                            }
                            break;
                        }
                    }
                }
            }
        }

        SDL_SetRenderDrawColor(ren, 10, 16, 32, 255);
        SDL_RenderClear(ren);

        draw_launcher_text(ren, "RMEXE COLLECTION", card_x, title_y, title_s, 96, 200, 255);
        draw_launcher_text(ren, "L/R: LANG  U/D: GAME  ENTER: PLAY", card_x, sub_y, sub_s, 160, 185, 220);

        static const char* kLangShort[3] = {"ZH", "JP", "EN"};
        static const char* kLangLong[3] = {"CHINESE", "JAPANESE", "ENGLISH"};
        const int ls_top = std::clamp(lang_h / 22, 2, 4);
        const int ls_bot = std::clamp(std::min(btn_w / 44, lang_h / 28), 2, 3);
        const int total_lang_txt_h = ls_top * 5 + 6 + ls_bot * 5;
        const int lang_txt_y = lang_y + std::max(4, (lang_h - total_lang_txt_h) / 2);
        for (int k = 0; k < 3; ++k) {
            const SDL_Rect& lr = lang_rects[k];
            const LauncherLang lval = static_cast<LauncherLang>(k);
            const bool active = (selected_lang == lval);
            const bool ready = is_lang_ready(titles[selected_title], lval);
            SDL_SetRenderDrawColor(ren, active ? 32 : 16, active ? 68 : 26, active ? 116 : 48, 255);
            SDL_RenderFillRect(ren, &lr);
            SDL_SetRenderDrawColor(ren, active ? 110 : 48, active ? 215 : 96, active ? 255 : 150, 255);
            SDL_RenderDrawRect(ren, &lr);
            if (active) {
                SDL_Rect inner{lr.x + 2, lr.y + 2, lr.w - 4, lr.h - 4};
                SDL_RenderDrawRect(ren, &inner);
            }
            uint8_t cr = ready ? (active ? 255 : 210) : 135;
            uint8_t cg = ready ? (active ? 255 : 225) : 140;
            uint8_t cb = ready ? 255 : 155;
            draw_launcher_text_centered(ren, kLangShort[k], lr, lang_txt_y, ls_top, cr, cg, cb);
            draw_launcher_text_centered(ren, kLangLong[k], lr, lang_txt_y + ls_top * 5 + 6, ls_bot,
                                        ready ? (active ? 140 : 150) : 210,
                                        ready ? (active ? 240 : 185) : 105,
                                        ready ? (active ? 190 : 210) : 105);
        }

        const int cs = std::clamp(std::min(card_w / 100, card_h / 26), 2, 4);
        const int sub_cs = std::max(2, cs - 1);
        for (int i = 0; i < n; ++i) {
            const SDL_Rect& r = card_rects[i];
            const bool sel = (i == selected_title);
            const bool ok = is_lang_ready(titles[i], selected_lang);
            SDL_SetRenderDrawColor(ren, sel ? 26 : 16, sel ? 48 : 26, sel ? 88 : 50, 255);
            SDL_RenderFillRect(ren, &r);
            SDL_SetRenderDrawColor(ren, sel ? 88 : 48, sel ? 188 : 100, sel ? 255 : 160, 255);
            SDL_RenderDrawRect(ren, &r);
            if (sel) {
                SDL_Rect inner{r.x + 2, r.y + 2, r.w - 4, r.h - 4};
                SDL_RenderDrawRect(ren, &inner);
            }
            const int text_y = r.y + std::max(10, (card_h - (cs * 5 + 8 + sub_cs * 5 + 6 + sub_cs * 5)) / 2);
            draw_launcher_text(ren, title_for_lang(titles[i], selected_lang), r.x + 18, text_y, cs,
                               ok ? 245 : 140, ok ? 250 : 145, ok ? 255 : 160);
            draw_launcher_text(ren, titles[i].subtitle, r.x + 18, text_y + cs * 5 + 6, sub_cs,
                               160, 190, 230);
            draw_launcher_text(ren, status_for_lang(titles[i], selected_lang),
                               r.x + 18, text_y + cs * 5 + 6 + sub_cs * 5 + 6, sub_cs,
                               ok ? 110 : 230, ok ? 220 : 100, ok ? 160 : 100);
        }

        const int footer_y = out_h - footer_h + 4;
        draw_launcher_text(ren, "NOTE: ZH + JP SHARE ONE SAVE FILE.", card_x, footer_y, fs, 155, 185, 220);
        draw_launcher_text(ren, "EN USES A SEPARATE SAVE (FAN EN ROM).", card_x, footer_y + fs * 5 + 5, fs, 155, 185, 220);

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    for (SDL_GameController* gc : pads) {
        if (gc) SDL_GameControllerClose(gc);
    }
    pads.clear();

    if (launched) {
        // Keep the existing SDL_Window and SDL_Renderer alive for DojaRuntime
        // (avoids tearing down Android Surface/GLES context between launcher and game).
        out_win = win;
        out_ren = ren;
    } else {
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
    }
    return launched;
}

} // namespace
} // namespace rmexe

int main(int argc, char** argv) {
    SDL_Log("RMEXE SDL_main starting...");
    std::string req_game;
    std::string data_dir;
    std::string asset_dir;
    int scale = 3;
    int fps = 60;
    int max_frames = 0;
    bool headless = false;
    bool fast_forward = false;
    bool touch_mode = false;
#if defined(__ANDROID__) || defined(__IPHONEOS__)
    touch_mode = true;
#endif

    std::string req_lang_raw;
    bool has_req_lang = false;
    rmexe::LauncherLang req_lang = rmexe::LauncherLang::Zh;
    bool cli_touch_flag = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--game" && i + 1 < argc) req_game = rmexe::to_lower_ascii(argv[++i]);
        else if (arg == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--asset-dir" && i + 1 < argc) asset_dir = argv[++i];
        else if (arg == "--scale" && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--fps" && i + 1 < argc) { fps = std::max(15, std::atoi(argv[++i])); rmexe::set_cli_fps_override(fps); }
        else if (arg == "--max-frames" && i + 1 < argc) max_frames = std::atoi(argv[++i]);
        else if (arg == "--headless") headless = true;
        else if (arg == "--fast-forward") fast_forward = true;
        else if (arg == "--touch") { touch_mode = true; cli_touch_flag = true; }
        else if (arg == "--lang" && i + 1 < argc) {
            req_lang_raw = rmexe::to_lower_ascii(argv[++i]);
            if (req_lang_raw == "zh" || req_lang_raw == "cn" || req_lang_raw == "chinese") {
                req_lang = rmexe::LauncherLang::Zh;
                has_req_lang = true;
            } else if (req_lang_raw == "jp" || req_lang_raw == "ja" || req_lang_raw == "japanese" ||
                       req_lang_raw == "orig" || req_lang_raw == "original") {
                req_lang = rmexe::LauncherLang::Jp;
                has_req_lang = true;
            } else if (req_lang_raw == "en" || req_lang_raw == "eng" || req_lang_raw == "english") {
                req_lang = rmexe::LauncherLang::En;
                has_req_lang = true;
            }
        }
    }
    (void)cli_touch_flag;

    std::vector<std::string> search_dirs;
#ifdef __ANDROID__
    std::string android_ext_dir;
    if (const char* ext = SDL_AndroidGetExternalStoragePath(); ext && *ext) {
        android_ext_dir = ext;
        rmexe::ensure_user_rom_dirs(android_ext_dir);
        search_dirs.push_back(android_ext_dir);
    }
    if (data_dir.empty()) {
        if (const char* intr = SDL_AndroidGetInternalStoragePath(); intr && *intr) data_dir = intr;
        else if (!android_ext_dir.empty()) data_dir = android_ext_dir;
    }
    SDL_Log("RMEXE Android data_dir=%s ext_dir=%s", data_dir.c_str(), android_ext_dir.c_str());
    rmexe::bootstrap_android_assets(data_dir);
    if (asset_dir.empty()) asset_dir = data_dir;
    search_dirs.push_back(data_dir);
#elif defined(__IPHONEOS__)
    if (data_dir.empty()) {
        // Prefer ~/Documents so files are directly visible & importable via the iPhone Files app (UIFileSharingEnabled)
        if (const char* home = std::getenv("HOME"); home && *home) {
            data_dir = std::string(home) + "/Documents";
        } else if (char* pref = SDL_GetPrefPath("rmexe", "recomp")) {
            data_dir = pref;
            SDL_free(pref);
        }
    }
    rmexe::ensure_user_rom_dirs(data_dir);
    if (asset_dir.empty()) asset_dir = data_dir;
    search_dirs.push_back(data_dir);
#endif

    std::string base_dir;
    if (char* b = SDL_GetBasePath()) {
        base_dir = b;
        SDL_free(b);
    }
    if (!asset_dir.empty()) search_dirs.push_back(asset_dir);
    if (!base_dir.empty()) search_dirs.push_back(base_dir);

    std::vector<rmexe::CollectionTitle> titles;
    {
        rmexe::CollectionTitle pon;
        pon.id = "pon";
        pon.jp_zh_title = "ROCKMAN EXE PON";
        pon.en_title = "MEGAMAN BN PON";
        pon.subtitle = "PHANTOM OF NETWORK";
        pon.zh_pack_name = "pon_zh.json";
#ifdef RMEXE_HAVE_PON_JP
        pon.jp = {true, false, "assets/PoN_jp/bin/PoN.jar", "assets/PoN_jp/sp/PoN.sp", &rmexe::game_pon_jp::get_game_module()};
#endif
#ifdef RMEXE_HAVE_PON_EN
        pon.en = {true, false, "assets/PoN/bin/PoN.jar", "assets/PoN/sp/PoN.sp", &rmexe::game_pon_en::get_game_module()};
#endif
        if (pon.jp.compiled || pon.en.compiled) titles.push_back(pon);
    }
    {
        rmexe::CollectionTitle lon;
        lon.id = "lon";
        lon.jp_zh_title = "ROCKMAN EXE LON";
        lon.en_title = "MEGAMAN BN LON";
        lon.subtitle = "LEGEND OF NETWORK";
        lon.zh_pack_name = "lon_zh.json";
#ifdef RMEXE_HAVE_LON_JP
        lon.jp = {true, false, "assets/LoN_jp/bin/LoN.jar", "assets/LoN_jp/sp/LoN.sp", &rmexe::game_lon_jp::get_game_module()};
#endif
#ifdef RMEXE_HAVE_LON_EN
        lon.en = {true, false, "assets/LoN/bin/LoN.jar", "assets/LoN/sp/LoN.sp", &rmexe::game_lon_en::get_game_module()};
#endif
        if (lon.jp.compiled || lon.en.compiled) titles.push_back(lon);
    }

    std::error_code ec;
    auto check_variant_assets = [&](rmexe::RomVariant& v, const char* tag) {
        if (!v.compiled) return;
        std::string jp = rmexe::resolve_entry_path(v.jar_rel, search_dirs);
        std::string sp = rmexe::resolve_entry_path(v.sp_rel, search_dirs);
        v.assets_ready = std::filesystem::is_regular_file(jp, ec) && std::filesystem::is_regular_file(sp, ec);
        SDL_Log("RMEXE variant=%s ready=%d jar=%s sp=%s", tag, v.assets_ready ? 1 : 0, jp.c_str(), sp.c_str());
    };
    for (auto& t : titles) {
        std::string id_s = t.id;
        check_variant_assets(t.jp, (id_s + "_jp").c_str());
        check_variant_assets(t.en, (id_s + "_en").c_str());
        t.zh_pack_ready = rmexe::is_translation_ready(t.zh_pack_name, search_dirs);
        SDL_Log("RMEXE title=%s zh_pack_ready=%d", t.id, t.zh_pack_ready ? 1 : 0);
    }

    int pick_title = 0;
    rmexe::LauncherLang pick_lang = has_req_lang ? req_lang : rmexe::LauncherLang::Zh;
    bool legacy_explicit_rom = false;

    auto find_first_ready_lang = [&](const rmexe::CollectionTitle& t, rmexe::LauncherLang preferred, rmexe::LauncherLang& out_l) {
        rmexe::LauncherLang order[3] = {
            preferred,
            rmexe::LauncherLang::Zh,
            rmexe::LauncherLang::Jp,
        };
        for (rmexe::LauncherLang l : order) {
            if (rmexe::is_lang_ready(t, l)) { out_l = l; return true; }
        }
        for (rmexe::LauncherLang l : {rmexe::LauncherLang::Zh, rmexe::LauncherLang::Jp, rmexe::LauncherLang::En}) {
            if (rmexe::is_lang_ready(t, l)) { out_l = l; return true; }
        }
        return false;
    };

    bool found_default = false;
    if (!has_req_lang) {
        for (rmexe::LauncherLang l : {rmexe::LauncherLang::Zh, rmexe::LauncherLang::Jp, rmexe::LauncherLang::En}) {
            for (int i = 0; i < static_cast<int>(titles.size()); ++i) {
                if (rmexe::is_lang_ready(titles[i], l)) {
                    pick_title = i;
                    pick_lang = l;
                    found_default = true;
                    break;
                }
            }
            if (found_default) break;
        }
    } else {
        for (int i = 0; i < static_cast<int>(titles.size()); ++i) {
            if (rmexe::is_lang_ready(titles[i], req_lang)) {
                pick_title = i;
                pick_lang = req_lang;
                found_default = true;
                break;
            }
        }
    }

    bool proceed = false;
    SDL_Window* shared_win = nullptr;
    SDL_Renderer* shared_ren = nullptr;
    if (!req_game.empty()) {
        auto find_title_idx = [&](const std::string& tid) -> int {
            for (int i = 0; i < static_cast<int>(titles.size()); ++i) {
                if (tid == titles[i].id) return i;
            }
            return -1;
        };
        if (req_game == "pon_jp" || req_game == "lon_jp") {
            int idx = find_title_idx(req_game.substr(0, 3));
            if (idx >= 0) {
                pick_title = idx;
                pick_lang = (has_req_lang && req_lang == rmexe::LauncherLang::Jp) ? rmexe::LauncherLang::Jp : rmexe::LauncherLang::Zh;
                legacy_explicit_rom = true;
                proceed = titles[idx].jp.compiled;
            }
        } else if (req_game == "pon_en" || req_game == "lon_en") {
            int idx = find_title_idx(req_game.substr(0, 3));
            if (idx >= 0) {
                pick_title = idx;
                pick_lang = rmexe::LauncherLang::En;
                legacy_explicit_rom = true;
                proceed = titles[idx].en.compiled;
            }
        } else {
            int idx = find_title_idx(req_game);
            if (idx >= 0) {
                pick_title = idx;
                if (has_req_lang) {
                    pick_lang = req_lang;
                } else {
                    find_first_ready_lang(titles[idx], rmexe::LauncherLang::Zh, pick_lang);
                }
                proceed = rmexe::variant_for_lang(titles[idx], pick_lang).compiled;
            }
        }
    } else if (headless) {
        proceed = found_default;
    } else {
        proceed = rmexe::run_collection_launcher(titles, pick_title, pick_lang, shared_win, shared_ren);
    }

    if (!proceed || pick_title < 0 || pick_title >= static_cast<int>(titles.size())) {
        SDL_Log("RMEXE launcher exited without selecting a game (proceed=%d, pick_title=%d)", proceed ? 1 : 0, pick_title);
        return 0;
    }

    const auto& chosen_title = titles[pick_title];
    const auto& chosen_var = rmexe::variant_for_lang(chosen_title, pick_lang);
    if (!chosen_var.compiled || !chosen_var.mod) {
        SDL_Log("RMEXE selected variant not compiled (title=%s, lang=%d)", chosen_title.id, static_cast<int>(pick_lang));
        return 1;
    }

    if (legacy_explicit_rom) {
        if (!req_lang_raw.empty()) rmexe::set_cli_lang_mode(req_lang_raw);
    } else if (pick_lang == rmexe::LauncherLang::Zh) {
        rmexe::set_cli_lang_mode("zh");
    } else if (pick_lang == rmexe::LauncherLang::Jp) {
        rmexe::set_cli_lang_mode("orig");
    }

    std::string jar_path = rmexe::resolve_entry_path(chosen_var.jar_rel, search_dirs);
    std::string sp_path = rmexe::resolve_entry_path(chosen_var.sp_rel, search_dirs);
    std::string win_title = rmexe::title_for_lang(chosen_title, pick_lang);
    SDL_Log("RMEXE launching %s lang=%d (jar=%s, sp=%s)", chosen_title.id, static_cast<int>(pick_lang), jar_path.c_str(), sp_path.c_str());

    rmexe::DojaRuntime rt;
    if (shared_win && shared_ren) {
        rt.attach_external_sdl(shared_win, shared_ren);
    }
    if (!data_dir.empty()) rt.set_data_dir(data_dir);
    if (!asset_dir.empty()) rt.set_asset_dir(asset_dir);
    rt.set_touch_controls(touch_mode);
    rt.set_headless(headless);
    rt.set_render_fps(fps);
    rt.set_fast_forward(fast_forward);
    if (max_frames > 0) rt.set_max_frames(max_frames);
    rt.set_game_module(chosen_var.mod);

    if (!rt.init(jar_path, sp_path, "", scale, win_title)) {
        SDL_Log("RMEXE DojaRuntime::init FAILED for %s", chosen_title.id);
        return 1;
    }

    try {
        chosen_var.mod->static_init(rt);
        rmexe::Ref app = chosen_var.mod->create_app_main(rt);
        chosen_var.mod->start_app(rt, app);
    } catch (const rmexe::JavaException& ex) {
        SDL_Log("RMEXE Uncaught JavaException ref=%d", ex.exc_ref);
        std::cerr << "[Uncaught JavaException] ref=" << ex.exc_ref << "\n";
        return 2;
    } catch (const std::exception& ex) {
        SDL_Log("RMEXE Fatal Error: %s", ex.what());
        std::cerr << "[Fatal Error] " << ex.what() << "\n";
        return 3;
    }
    return 0;
}
