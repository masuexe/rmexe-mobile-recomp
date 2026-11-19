#pragma once

#include <string>

namespace rmexe {

// Configure network mode & remote server endpoint (e.g. "127.0.0.1:8765")
void set_network_server(const std::string& host_port, bool enable_online);

// Configure player subscriber UID and leaderboard/PvP nickname
void set_player_identity(const std::string& uid, const std::string& name);

// Override ScratchPad .sav overlay file path if explicitly passed via --sav
void set_custom_sav_path(const std::string& sav_path);

// Override 60 FPS / 15 FPS presentation mode if explicitly passed via --fps
void set_cli_fps_override(int fps);

// Trigger automatic PvP room creation (mode=1) or join (mode=2) once game boots
void set_cli_pvp_action(int mode, uint32_t room_id);

// Configure localization overlay language ("auto", "zh", "orig") and/or explicit JSON file path
void set_cli_lang_mode(const std::string& mode);
void set_cli_translation_path(const std::string& path);

// Configure font rendering mode (-1 = unset, 0 = pixel, 1 = hd) and optional font file overrides
void set_cli_font_mode(int mode);
void set_cli_font_paths(const std::string& pixel_font, const std::string& hd_font);

} // namespace rmexe
