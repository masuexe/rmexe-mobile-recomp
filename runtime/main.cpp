#include "doja_runtime.hpp"
#include "net_config.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef DEFAULT_JAR_PATH
#define DEFAULT_JAR_PATH "assets/PoN_jp/bin/PoN.jar"
#endif

#ifndef DEFAULT_SP_PATH
#define DEFAULT_SP_PATH "assets/PoN_jp/sp/PoN.sp"
#endif

#ifndef DEFAULT_SAV_PATH
#define DEFAULT_SAV_PATH "assets/PoN_jp/sp/PoN.sav"
#endif

#ifndef DEFAULT_WINDOW_TITLE
#define DEFAULT_WINDOW_TITLE "Rockman EXE Mobile Recomp"
#endif

int main(int argc, char** argv) {
    std::string jar_path = DEFAULT_JAR_PATH;
    std::string sp_path = DEFAULT_SP_PATH;
    std::string sav_path = DEFAULT_SAV_PATH;
    std::string dump_ppm_path;
    int scale = 3;
    int fps = 60;
    int max_frames = 0;
    bool headless = false;
    bool fast_forward = false;
    bool explicit_server = false;

    rmexe::DojaRuntime rt;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--jar" && i + 1 < argc) {
            jar_path = argv[++i];
        } else if (arg == "--sp" && i + 1 < argc) {
            sp_path = argv[++i];
        } else if (arg == "--sav" && i + 1 < argc) {
            sav_path = argv[++i];
            rmexe::set_custom_sav_path(sav_path);
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
        } else if (arg == "--pvp-join" && i + 1 < argc) {
            if (!explicit_server) rmexe::set_network_server("", true);
            uint32_t rid = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            rmexe::set_cli_pvp_action(2, rid);
        } else if (arg == "--uid" && i + 1 < argc) {
            rmexe::set_player_identity(argv[++i], "");
        } else if (arg == "--name" && i + 1 < argc) {
            rmexe::set_player_identity("", argv[++i]);
        } else if (arg == "--scale" && i + 1 < argc) {
            scale = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--fps" && i + 1 < argc) {
            fps = std::max(15, std::atoi(argv[++i]));
            rmexe::set_cli_fps_override(fps);
        } else if (arg == "--max-frames" && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        } else if (arg == "--dump-ppm" && i + 1 < argc) {
            dump_ppm_path = argv[++i];
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--fast-forward") {
            fast_forward = true;
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
                      << "  --pvp-join <room_id>  Auto-join PvP room <room_id> (0 = quick match)\n"
                      << "  --offline             Use built-in offline Capcom i-mode simulation\n"
                      << "  --uid <id>            Override subscriber UID for cloud saves/rankings\n"
                      << "  --name <nick>         Override player nickname   (up to 12 ASCII chars)\n"
                      << "  --scale <int>         Window scale factor (default: 3 -> 720x780)\n"
                      << "  --fps <int>           Target render FPS (default: 60)\n"
                      << "  --headless            Run without opening an SDL window\n"
                      << "  --fast-forward        Disable frame sleep for rapid testing\n"
                      << "  --max-frames <N>      Exit after N rendered frames\n"
                      << "  --dump-ppm <path>     Save 240x240 framebuffer to PPM on exit\n"
                      << "  --inject-key <F:K>    Inject DoJa key K at frame F (or F1-F2:K span)\n";
            return 0;
        }
    }

    rt.set_render_fps(fps);
    rt.set_fast_forward(fast_forward);
    if (max_frames > 0) {
        rt.set_max_frames(max_frames, dump_ppm_path);
    }

    if (!rt.init(jar_path, sp_path, "", headless ? 0 : scale)) {
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
