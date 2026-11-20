#include "doja_runtime.hpp"
#include "net_config.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef DEFAULT_JAR_PATH
#define DEFAULT_JAR_PATH "assets/PoN_jp/bin/PoN.jar"
#endif

#ifndef DEFAULT_SP_PATH
#define DEFAULT_SP_PATH "assets/PoN_jp/sp/PoN.sp"
#endif

#ifndef DEFAULT_WINDOW_TITLE
#define DEFAULT_WINDOW_TITLE "Rockman EXE Mobile Recomp"
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }
#endif
    std::string jar_path = DEFAULT_JAR_PATH;
    std::string sp_path = DEFAULT_SP_PATH;
    std::string dump_ppm_path;
    int scale = 3;
    int fps = 60;
    int max_frames = 0;
    bool headless = false;
    bool fast_forward = false;
    bool explicit_server = false;
    std::string cli_pixel_font;
    std::string cli_hd_font;

    rmexe::DojaRuntime rt;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--jar" && i + 1 < argc) {
            jar_path = argv[++i];
        } else if (arg == "--sp" && i + 1 < argc) {
            sp_path = argv[++i];
        } else if (arg == "--sav" && i + 1 < argc) {
            rmexe::set_custom_sav_path(argv[++i]);
        } else if (arg == "--config" && i + 1 < argc) {
            rt.set_config_path(argv[++i]);
        } else if (arg == "--server" && i + 1 < argc) {
            rmexe::set_network_server(argv[++i], true);
            explicit_server = true;
        } else if (arg == "--offline") {
            rmexe::set_network_server("", false);
        } else if (arg == "--pvp-create") {
            if (!explicit_server) rmexe::set_network_server("", true);
            rmexe::set_cli_pvp_action(1, 0);
        } else if (arg == "--pvp-bo3") {
            rmexe::set_cli_pvp_bo3(true);
        } else if (arg == "--pvp-join" && i + 1 < argc) {
            if (!explicit_server) rmexe::set_network_server("", true);
            uint32_t rid = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            rmexe::set_cli_pvp_action(2, rid);
        } else if (arg == "--pvp-self-test") {
            headless = true;
            fast_forward = true;
            rt.set_pvp_self_test(true);
        } else if (arg == "--pvp-auto-bot") {
            rt.set_pvp_auto_bot(true);
        } else if (arg == "--assist-self-test") {
            headless = true;
            fast_forward = true;
            rt.set_assist_self_test(true);
        } else if (arg == "--buster-mult" && i + 1 < argc) {
            rmexe::set_cli_assist_buster_mult(std::atoi(argv[++i]));
        } else if (arg == "--damage-pct" && i + 1 < argc) {
            rmexe::set_cli_assist_damage_pct(std::atoi(argv[++i]));
        } else if (arg == "--encounter-pct" && i + 1 < argc) {
            rmexe::set_cli_assist_encounter_pct(std::atoi(argv[++i]));
        } else if (arg == "--wild-boss-only") {
            rmexe::set_cli_assist_wild_boss_only(true);
        } else if (arg == "--show-ex-boss" && i + 1 < argc) {
            rmexe::set_cli_assist_show_ex_boss(std::atoi(argv[++i]));
        } else if (arg == "--ex-boss-tier" && i + 1 < argc) {
            rmexe::set_cli_assist_ex_boss_tier(std::atoi(argv[++i]));
        } else if (arg == "--zenny-pct" && i + 1 < argc) {
            rmexe::set_cli_assist_zenny_pct(std::atoi(argv[++i]));
        } else if (arg == "--run-speed-pct" && i + 1 < argc) {
            rmexe::set_cli_assist_run_speed_pct(std::atoi(argv[++i]));
        } else if (arg == "--auto-full-hp") {
            rmexe::set_cli_assist_auto_full_hp(true);
        } else if (arg == "--always-escape") {
            rmexe::set_cli_assist_always_escape(true);
        } else if (arg == "--custom-gauge-pct" && i + 1 < argc) {
            rmexe::set_cli_assist_custom_gauge_pct(std::atoi(argv[++i]));
        } else if (arg == "--drop-rank-mode" && i + 1 < argc) {
            rmexe::set_cli_assist_drop_rank_mode(std::atoi(argv[++i]));
        } else if (arg == "--uid" && i + 1 < argc) {
            rmexe::set_player_identity(argv[++i], "");
        } else if (arg == "--name" && i + 1 < argc) {
            rmexe::set_player_identity("", argv[++i]);
        } else if (arg == "--scale" && i + 1 < argc) {
            scale = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--fps" && i + 1 < argc) {
            fps = std::max(15, std::atoi(argv[++i]));
            rmexe::set_cli_fps_override(fps);
        } else if (arg == "--lang" && i + 1 < argc) {
            rmexe::set_cli_lang_mode(argv[++i]);
        } else if (arg == "--translation" && i + 1 < argc) {
            rmexe::set_cli_translation_path(argv[++i]);
        } else if (arg == "--font-mode" && i + 1 < argc) {
            std::string fm = argv[++i];
            for (char& ch : fm) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            }
            if (fm == "pixel" || fm == "12px" || fm == "0" || fm == "pix") {
                rmexe::set_cli_font_mode(0);
            } else if (fm == "hd" || fm == "smooth" || fm == "1") {
                rmexe::set_cli_font_mode(1);
            }
        } else if (arg == "--font" && i + 1 < argc) {
            cli_pixel_font = argv[++i];
            cli_hd_font = cli_pixel_font;
        } else if (arg == "--pixel-font" && i + 1 < argc) {
            cli_pixel_font = argv[++i];
        } else if (arg == "--hd-font" && i + 1 < argc) {
            cli_hd_font = argv[++i];
        } else if (arg == "--max-frames" && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        } else if (arg == "--dump-ppm" && i + 1 < argc) {
            dump_ppm_path = argv[++i];
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--fast-forward") {
            fast_forward = true;
        } else if (arg == "--touch") {
            rt.set_touch_controls(true);
        } else if (arg == "--inject-key" && i + 1 < argc) {
            std::string kv = argv[++i];
            auto colon = kv.find(':');
            if (colon != std::string::npos) {
                std::string fspec = kv.substr(0, colon);
                int k = std::atoi(kv.substr(colon + 1).c_str());
                auto dash = fspec.find('-');
                if (dash != std::string::npos) {
                    int f0 = std::atoi(fspec.substr(0, dash).c_str());
                    int f1 = std::atoi(fspec.substr(dash + 1).c_str());
                    rt.inject_key_span(f0, f1, k);
                } else {
                    int f = std::atoi(fspec.c_str());
                    rt.inject_key_at_frame(f, k);
                }
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --jar <path>          Path to original .jar file (default: " << DEFAULT_JAR_PATH << ")\n"
                      << "  --sp <path>           Path to original .sp file  (default: " << DEFAULT_SP_PATH << ")\n"
                      << "  --sav <path>          Path to .sav overlay file  (default: <sp>.sav)\n"
                      << "  --config <path>       Path to keybinding config  (default: controls.cfg)\n"
                      << "  --server <host:port>  Connect to rmexe_server    (e.g. 127.0.0.1:8765)\n"
                      << "  --pvp-create          Auto-create an online PvP NetBattle room on boot\n"
                      << "  --pvp-bo3             Use Triple Battle (BO3 / Best-of-3) format when creating a room\n"
                      << "  --pvp-join <room_id>  Auto-join PvP room <room_id> (0 = quick match)\n"
                      << "  --pvp-self-test       Run headless 213-chip PvP symmetry & GGPO rollback suite\n"
                      << "  --pvp-auto-bot        Drive automated PvP inputs in --pvp-create/--pvp-join\n"
                      << "  --assist-self-test    Run headless Assist/QoL cheat & PvP isolation suite\n"
                      << "  --buster-mult <N>     Buster damage multiplier (1..9999)\n"
                      << "  --damage-pct <N>      Incoming damage percentage (0..500)\n"
                      << "  --encounter-pct <N>   Encounter rate percentage (0..10000)\n"
                      << "  --wild-boss-only      Enable Wild Boss Only Stealth\n"
                      << "  --show-ex-boss <0..2> 0=Off, 1=Radar HUD, 2=Radar + Force Unlock\n"
                      << "  --ex-boss-tier <-1..3> -1=Auto, 0..3=Force V1..SP tier\n"
                      << "  --zenny-pct <N>       Zenny reward percentage (0..10000)\n"
                      << "  --run-speed-pct <N>   Overworld run speed percentage (50..500)\n"
                      << "  --auto-full-hp        Restore full HP after battle win/escape\n"
                      << "  --always-escape       100% escape success in escapable battles\n"
                      << "  --custom-gauge-pct <N> Custom gauge fill speed percentage (50..10000)\n"
                      << "  --drop-rank-mode <0..2> 0=Off, 1=Always Drop, 2=S-Rank + Always Drop\n"
                      << "  --offline             Use built-in offline Capcom i-mode simulation\n"
                      << "  --uid <id>            Override subscriber UID for cloud saves/rankings\n"
                      << "  --name <nick>         Override player nickname   (up to 12 ASCII chars)\n"
                      << "  --scale <int>         Window scale factor (default: 3 -> 720x780)\n"
                      << "  --fps <int>           Target render FPS (default: 60)\n"
                      << "  --lang <mode>         Language overlay mode: auto, zh, orig\n"
                      << "  --translation <path>  Path to Chinese translation JSON file\n"
                      << "  --font-mode <mode>    Font rendering mode: hd (default), pixel\n"
                      << "  --font <path>         Override font file for both pixel and HD modes\n"
                      << "  --pixel-font <path>   Override 12px pixel font file (.ttf/.ttc/.otf)\n"
                      << "  --hd-font <path>      Override HD smooth font file (.ttf/.ttc/.otf)\n"
                      << "  --headless            Run without opening an SDL window\n"
                      << "  --fast-forward        Disable frame sleep for rapid testing\n"
                      << "  --max-frames <N>      Exit after N rendered frames\n"
                      << "  --dump-ppm <path>     Save 240x240 framebuffer to PPM on exit\n"
                      << "  --inject-key <F:K>    Inject DoJa key K at frame F (or F1-F2:K span)\n";
            return 0;
        }
    }

    if (!cli_pixel_font.empty() || !cli_hd_font.empty()) {
        rmexe::set_cli_font_paths(cli_pixel_font, cli_hd_font);
    }
    rt.set_headless(headless);
    rt.set_render_fps(fps);
    rt.set_fast_forward(fast_forward);
    if (max_frames > 0) {
        rt.set_max_frames(max_frames, dump_ppm_path);
    }

    static const rmexe::GameModule kSingleGameModule{
        rmexe::generated_static_init,
        rmexe::generated_create_app_main,
        rmexe::generated_start_app,
        rmexe::generated_process_event
    };
    rt.set_game_module(&kSingleGameModule);

    if (!rt.init(jar_path, sp_path, "", scale, DEFAULT_WINDOW_TITLE)) {
        return 1;
    }

    try {
        rmexe::generated_static_init(rt);
        rmexe::Ref app = rmexe::generated_create_app_main(rt);
        rmexe::generated_start_app(rt, app);
    } catch (const rmexe::JavaException& ex) {
        std::cerr << "[Uncaught JavaException] ref=" << ex.exc_ref << "\n";
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "[Fatal Error] " << ex.what() << "\n";
        return 3;
    }

    return 0;
}
