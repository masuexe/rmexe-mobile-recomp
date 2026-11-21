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

struct CollectionGameEntry {
    const char* id;
    const char* title;
    const char* subtitle;
    const char* jar_rel;
    const char* sp_rel;
    const GameModule* mod;
    bool assets_ready = false;
};

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

int run_collection_launcher(std::vector<CollectionGameEntry>& games,
                            SDL_Window*& out_win, SDL_Renderer*& out_ren) {
    out_win = nullptr;
    out_ren = nullptr;
    if (games.empty()) return -1;
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("RMEXE Launcher SDL_Init failed: %s", SDL_GetError());
        return 0;
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
        return 0;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        SDL_Log("RMEXE Launcher SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        return 0;
    }

    int selected = 0;
    for (int i = 0; i < static_cast<int>(games.size()); ++i) {
        if (games[i].assets_ready) { selected = i; break; }
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

    int chosen = -1;
    bool running = true;
    int stick_dir = 0;
    std::vector<SDL_Rect> card_rects(games.size());

    while (running) {
        int out_w = 0, out_h = 0;
        SDL_GetRendererOutputSize(ren, &out_w, &out_h);
        if (out_w <= 0 || out_h <= 0) {
            SDL_GetWindowSize(win, &out_w, &out_h);
        }
        if (out_w <= 0) out_w = 720;
        if (out_h <= 0) out_h = 1280;

        const int pad = std::max(18, out_w / 18);
        const int header_h = std::max(72, out_h / 9);
        const int avail_h = std::max(200, out_h - header_h - pad * 2);
        const int n = static_cast<int>(games.size());
        const int gap = std::max(12, avail_h / 28);
        const int card_h = std::clamp((avail_h - gap * (n - 1)) / std::max(1, n), 68, 160);
        const int card_w = std::min(out_w - pad * 2, 760);
        const int card_x = (out_w - card_w) / 2;
        const int start_y = header_h + pad / 2;

        for (int i = 0; i < n; ++i) {
            card_rects[i] = SDL_Rect{card_x, start_y + i * (card_h + gap), card_w, card_h};
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT || ev.type == SDL_APP_TERMINATING) {
                running = false;
                chosen = -1;
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED || ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                refresh_pads();
            } else if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                int b = ev.cbutton.button;
                if (b == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    selected = (selected + n - 1) % n;
                } else if (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    selected = (selected + 1) % n;
                } else if (b == SDL_CONTROLLER_BUTTON_A || b == SDL_CONTROLLER_BUTTON_START) {
                    if (games[selected].assets_ready) {
                        chosen = selected;
                        running = false;
                    }
                } else if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_BACK) {
                    running = false;
                    chosen = -1;
                }
            } else if (ev.type == SDL_CONTROLLERAXISMOTION && ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                int nd = (ev.caxis.value < -16000) ? -1 : ((ev.caxis.value > 16000) ? 1 : 0);
                if (nd != 0 && nd != stick_dir) {
                    selected = (selected + n + nd) % n;
                }
                stick_dir = nd;
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_ESCAPE || k == SDLK_AC_BACK) {
                    running = false;
                    chosen = -1;
                } else if (k == SDLK_UP || k == SDLK_w) {
                    selected = (selected + n - 1) % n;
                } else if (k == SDLK_DOWN || k == SDLK_s) {
                    selected = (selected + 1) % n;
                } else if (k == SDLK_RETURN || k == SDLK_SPACE || k == SDLK_z) {
                    if (games[selected].assets_ready) {
                        chosen = selected;
                        running = false;
                    }
                }
            } else if (ev.type == SDL_FINGERUP || (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT)) {
                int px = (ev.type == SDL_FINGERUP) ? static_cast<int>(ev.tfinger.x * out_w) : ev.button.x;
                int py = (ev.type == SDL_FINGERUP) ? static_cast<int>(ev.tfinger.y * out_h) : ev.button.y;
                for (int i = 0; i < n; ++i) {
                    const SDL_Rect& r = card_rects[i];
                    if (px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h) {
                        selected = i;
                        if (games[i].assets_ready) {
                            chosen = i;
                            running = false;
                        }
                        break;
                    }
                }
            }
        }

        SDL_SetRenderDrawColor(ren, 10, 16, 32, 255);
        SDL_RenderClear(ren);

        int title_s = std::clamp(out_w / 140, 3, 7);
        draw_launcher_text(ren, "RMEXE COLLECTION", pad, std::max(24, header_h / 3), title_s, 96, 200, 255);
        draw_launcher_text(ren, "TAP OR PRESS ENTER TO LAUNCH", pad, std::max(24, header_h / 3) + title_s * 7,
                           std::max(2, title_s - 1), 160, 185, 220);

        for (int i = 0; i < n; ++i) {
            const SDL_Rect& r = card_rects[i];
            const bool sel = (i == selected);
            const bool ok = games[i].assets_ready;
            SDL_SetRenderDrawColor(ren, sel ? 26 : 16, sel ? 48 : 26, sel ? 88 : 50, 255);
            SDL_RenderFillRect(ren, &r);
            SDL_SetRenderDrawColor(ren, sel ? 88 : 48, sel ? 188 : 100, sel ? 255 : 160, 255);
            SDL_RenderDrawRect(ren, &r);
            if (sel) {
                SDL_Rect inner{r.x + 2, r.y + 2, r.w - 4, r.h - 4};
                SDL_RenderDrawRect(ren, &inner);
            }
            int cs = std::clamp(card_h / 24, 2, 5);
            draw_launcher_text(ren, games[i].title, r.x + 18, r.y + card_h / 4, cs,
                               ok ? 245 : 140, ok ? 250 : 145, ok ? 255 : 160);
            draw_launcher_text(ren, ok ? games[i].subtitle : "MISSING JAR/SP IN ASSETS",
                               r.x + 18, r.y + card_h / 4 + cs * 7 + 4, std::max(2, cs - 1),
                               ok ? 110 : 230, ok ? 220 : 100, ok ? 160 : 100);
        }

        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    for (SDL_GameController* gc : pads) {
        if (gc) SDL_GameControllerClose(gc);
    }
    pads.clear();

    if (chosen >= 0) {
        // Keep the existing SDL_Window and SDL_Renderer alive for DojaRuntime
        // (avoids tearing down Android Surface/GLES context between launcher and game).
        out_win = win;
        out_ren = ren;
    } else {
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
    }
    return chosen;
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

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--game" && i + 1 < argc) req_game = argv[++i];
        else if (arg == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (arg == "--asset-dir" && i + 1 < argc) asset_dir = argv[++i];
        else if (arg == "--scale" && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--fps" && i + 1 < argc) { fps = std::max(15, std::atoi(argv[++i])); rmexe::set_cli_fps_override(fps); }
        else if (arg == "--max-frames" && i + 1 < argc) max_frames = std::atoi(argv[++i]);
        else if (arg == "--headless") headless = true;
        else if (arg == "--fast-forward") fast_forward = true;
        else if (arg == "--touch") touch_mode = true;
        else if (arg == "--lang" && i + 1 < argc) rmexe::set_cli_lang_mode(argv[++i]);
    }

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

    std::vector<rmexe::CollectionGameEntry> games;
#ifdef RMEXE_HAVE_PON_JP
    games.push_back({"pon_jp", "ROCKMAN EXE PON (JP/ZH)", "READY - PHANTOM OF NETWORK",
                     "assets/PoN_jp/bin/PoN.jar", "assets/PoN_jp/sp/PoN.sp", &rmexe::game_pon_jp::get_game_module(), false});
#endif
#ifdef RMEXE_HAVE_PON_EN
    games.push_back({"pon_en", "MEGAMAN BN PON (EN/ZH)", "READY - PHANTOM OF NETWORK",
                     "assets/PoN/bin/PoN.jar", "assets/PoN/sp/PoN.sp", &rmexe::game_pon_en::get_game_module(), false});
#endif
#ifdef RMEXE_HAVE_LON_JP
    games.push_back({"lon_jp", "ROCKMAN EXE LON (JP/ZH)", "READY - LEGEND OF NETWORK",
                     "assets/LoN_jp/bin/LoN.jar", "assets/LoN_jp/sp/LoN.sp", &rmexe::game_lon_jp::get_game_module(), false});
#endif
#ifdef RMEXE_HAVE_LON_EN
    games.push_back({"lon_en", "MEGAMAN BN LON (EN/ZH)", "READY - LEGEND OF NETWORK",
                     "assets/LoN/bin/LoN.jar", "assets/LoN/sp/LoN.sp", &rmexe::game_lon_en::get_game_module(), false});
#endif

    std::error_code ec;
    for (auto& g : games) {
        std::string jp = rmexe::resolve_entry_path(g.jar_rel, search_dirs);
        std::string sp = rmexe::resolve_entry_path(g.sp_rel, search_dirs);
        g.assets_ready = std::filesystem::is_regular_file(jp, ec) && std::filesystem::is_regular_file(sp, ec);
        SDL_Log("RMEXE game=%s ready=%d jar=%s sp=%s", g.id, g.assets_ready ? 1 : 0, jp.c_str(), sp.c_str());
    }

    int pick = -1;
    SDL_Window* shared_win = nullptr;
    SDL_Renderer* shared_ren = nullptr;
    if (!req_game.empty()) {
        for (int i = 0; i < static_cast<int>(games.size()); ++i) {
            if (req_game == games[i].id) { pick = i; break; }
        }
    } else if (headless) {
        for (int i = 0; i < static_cast<int>(games.size()); ++i) {
            if (games[i].assets_ready) { pick = i; break; }
        }
    } else {
        pick = rmexe::run_collection_launcher(games, shared_win, shared_ren);
    }

    if (pick < 0 || pick >= static_cast<int>(games.size())) {
        SDL_Log("RMEXE launcher exited without selecting a game (pick=%d)", pick);
        return 0;
    }

    const auto& chosen = games[pick];
    std::string jar_path = rmexe::resolve_entry_path(chosen.jar_rel, search_dirs);
    std::string sp_path = rmexe::resolve_entry_path(chosen.sp_rel, search_dirs);
    SDL_Log("RMEXE launching %s (jar=%s, sp=%s)", chosen.id, jar_path.c_str(), sp_path.c_str());

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
    rt.set_game_module(chosen.mod);

    if (!rt.init(jar_path, sp_path, "", scale, chosen.title)) {
        SDL_Log("RMEXE DojaRuntime::init FAILED for %s", chosen.id);
        return 1;
    }

    try {
        chosen.mod->static_init(rt);
        rmexe::Ref app = chosen.mod->create_app_main(rt);
        chosen.mod->start_app(rt, app);
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
