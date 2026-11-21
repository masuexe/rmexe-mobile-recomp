#pragma once

// ============================================================================
// runtime/touch_controls.hpp
// Pure C++20 / SDL2 Multi-Touch Virtual Gamepad, Dual-Orientation Viewport,
// Interactive Drag-and-Drop Layout Editor, and Game-Screen Touch Tap Mapper.
// Shared across Android (APK), iOS (IPA), and Desktop (--touch preview mode).
// ============================================================================

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace rmexe {

enum class TouchControlId : int {
    None = -1,
    DpadUp = 0,
    DpadDown,
    DpadLeft,
    DpadRight,
    BtnA,       // KEY_SELECT (0x14) — Confirm / Use Chip
    BtnB,       // KEY_ASTERISK (0x0A) — Cancel / Buster
    BtnL,       // KEY_SOFT1 (0x15) — Left Softkey / Custom
    BtnR,       // KEY_SOFT2 (0x16) — Right Softkey / PET Menu
    BtnPound,   // KEY_POUND (0x0B) — Sort / #
    BtnPageUp,  // KEY_1 (0x01) — Page Up / Rival Folder
    BtnPageDn,  // KEY_4 (0x04) — Page Down
    BtnTurbo,   // Hold Fast-Forward
    BtnMenu,    // F1 Pause / Settings Overlay
    Count
};

// Draggable layout groups (D-Pad moves as a single unit; each button moves independently)
enum class TouchGroupId : int {
    Dpad = 0,
    BtnA,
    BtnB,
    BtnPound,
    BtnL,
    BtnR,
    BtnPageUp,
    BtnPageDn,
    BtnTurbo,
    BtnMenu,
    Count
};

struct TouchGroupOverride {
    bool custom = false;
    float cx = 0.5f;    // Normalized center X in [0..1]
    float cy = 0.5f;    // Normalized center Y in [0..1]
    float scale = 1.0f; // Relative size multiplier in [0.60..1.60]
};

struct TouchButtonRect {
    TouchControlId id = TouchControlId::None;
    TouchGroupId group = TouchGroupId::Dpad;
    SDL_Rect rect{0, 0, 0, 0};
    const char* label = "";
    int doja_key = -1; // >= 0 for DoJa keypad bit, -1 for emulator utility
    uint8_t accent_r = 64;
    uint8_t accent_g = 152;
    uint8_t accent_b = 240;
    bool is_round = false;
};

struct TouchActionDelta {
    uint32_t doja_mask = 0; // Bitmask of currently pressed DoJa keys from touch
    bool turbo_down = false;
    bool menu_triggered = false;
    bool layout_saved = false;
    bool has_game_tap = false;
    int game_tap_x = -1; // 0..239 logical screen X
    int game_tap_y = -1; // 0..239 logical screen Y
    // Edge-triggered menu navigation events when overlay is open
    std::vector<SDL_Keycode> overlay_keys;
};

class TouchOverlayController {
public:
    bool enabled() const { return enabled_; }
    void set_enabled(bool en) {
        enabled_ = en;
        if (!en) {
            edit_mode_ = false;
            clear_all_fingers();
        }
    }

    bool in_edit_mode() const { return edit_mode_; }
    void set_edit_mode(bool ed) {
        edit_mode_ = ed;
        dragging_group_ = -1;
        drag_finger_active_ = false;
        if (ed) {
            clear_all_fingers();
            if (selected_group_ < 0) selected_group_ = static_cast<int>(TouchGroupId::Dpad);
        }
    }

    float opacity() const { return opacity_; }
    void set_opacity(float op) { opacity_ = std::clamp(op, 0.25f, 1.0f); }

    bool is_portrait() const { return portrait_; }
    SDL_Rect game_rect() const { return game_rect_; }

    void clear_all_fingers() {
        finger_states_.clear();
        mouse_active_ = false;
        mouse_ctrl_mask_ = 0;
        recompute_pressed_bits();
    }

    static const char* group_cfg_name(TouchGroupId g) {
        switch (g) {
            case TouchGroupId::Dpad:      return "dpad";
            case TouchGroupId::BtnA:      return "a";
            case TouchGroupId::BtnB:      return "b";
            case TouchGroupId::BtnPound:  return "pound";
            case TouchGroupId::BtnL:      return "l";
            case TouchGroupId::BtnR:      return "r";
            case TouchGroupId::BtnPageUp: return "k1";
            case TouchGroupId::BtnPageDn: return "k4";
            case TouchGroupId::BtnTurbo:  return "ff";
            case TouchGroupId::BtnMenu:   return "menu";
            default:                      return "";
        }
    }

    void reset_current_orientation() {
        auto& arr = portrait_ ? portrait_ov_ : landscape_ov_;
        for (int i = 0; i < static_cast<int>(TouchGroupId::Count); ++i) {
            arr[i] = TouchGroupOverride{};
        }
        layout_dirty_ = true;
    }

    void serialize_config(std::ostringstream& out) const {
        out << "touch_opacity=" << static_cast<int>(std::round(opacity_ * 100.0f)) << "\n";
        for (int orient = 0; orient < 2; ++orient) {
            const auto& arr = (orient == 0) ? portrait_ov_ : landscape_ov_;
            const char* pfx = (orient == 0) ? "touch_p_" : "touch_l_";
            for (int i = 0; i < static_cast<int>(TouchGroupId::Count); ++i) {
                if (!arr[i].custom) continue;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%.4f,%.4f,%.2f", arr[i].cx, arr[i].cy, arr[i].scale);
                out << pfx << group_cfg_name(static_cast<TouchGroupId>(i)) << "=" << buf << "\n";
            }
        }
    }

    bool parse_config_line(const std::string& key, const std::string& val) {
        if (key == "touch_opacity") {
            int pct = std::clamp(std::atoi(val.c_str()), 25, 100);
            opacity_ = static_cast<float>(pct) / 100.0f;
            return true;
        }
        bool is_p = (key.rfind("touch_p_", 0) == 0);
        bool is_l = (key.rfind("touch_l_", 0) == 0);
        if (!is_p && !is_l) return false;
        std::string gname = key.substr(8);
        int gidx = -1;
        for (int i = 0; i < static_cast<int>(TouchGroupId::Count); ++i) {
            if (gname == group_cfg_name(static_cast<TouchGroupId>(i))) {
                gidx = i;
                break;
            }
        }
        if (gidx < 0) return false;
        float cx = 0.5f, cy = 0.5f, sc = 1.0f;
        if (std::sscanf(val.c_str(), "%f,%f,%f", &cx, &cy, &sc) >= 2) {
            auto& ov = (is_p ? portrait_ov_ : landscape_ov_)[gidx];
            ov.custom = true;
            ov.cx = std::clamp(cx, 0.04f, 0.96f);
            ov.cy = std::clamp(cy, 0.04f, 0.96f);
            ov.scale = std::clamp(sc, 0.60f, 1.60f);
            layout_dirty_ = true;
            return true;
        }
        return false;
    }

    void update_layout(int out_w, int out_h, int game_logical_h) {
        if (out_w <= 0 || out_h <= 0) return;
        if (!layout_dirty_ && out_w == last_w_ && out_h == last_h_ && game_logical_h == last_logical_h_) {
            return;
        }
        layout_dirty_ = false;
        last_w_ = out_w;
        last_h_ = out_h;
        last_logical_h_ = game_logical_h;
        buttons_.clear();

        const float aspect = static_cast<float>(game_logical_h) / 240.0f;
        portrait_ = (out_h > static_cast<int>(out_w * 1.12f));
        const auto& ovs = portrait_ ? portrait_ov_ : landscape_ov_;

        auto apply_group_rect = [&](TouchGroupId g, SDL_Rect base) -> SDL_Rect {
            const auto& ov = ovs[static_cast<int>(g)];
            if (!ov.custom) return base;
            int w = std::max(24, static_cast<int>(std::round(base.w * ov.scale)));
            int h = std::max(24, static_cast<int>(std::round(base.h * ov.scale)));
            int x = static_cast<int>(std::round(ov.cx * out_w)) - w / 2;
            int y = static_cast<int>(std::round(ov.cy * out_h)) - h / 2;
            x = std::clamp(x, 2, std::max(2, out_w - w - 2));
            y = std::clamp(y, 2, std::max(2, out_h - h - 2));
            return SDL_Rect{x, y, w, h};
        };

        if (portrait_) {
            const int safe_top = std::max(8, out_h / 28);
            const int safe_bottom = std::max(12, out_h / 32);
            const int deck_min_h = std::max(out_h * 38 / 100, 180);
            const int max_game_h = std::max(120, out_h - safe_top - safe_bottom - deck_min_h);

            int gw = out_w;
            int gh = static_cast<int>(std::round(gw * aspect));
            if (gh > max_game_h) {
                gh = max_game_h;
                gw = static_cast<int>(std::round(gh / aspect));
            }
            game_rect_ = SDL_Rect{(out_w - gw) / 2, safe_top, gw, gh};

            const int deck_top = game_rect_.y + game_rect_.h + std::max(6, out_h / 90);
            const int deck_h = std::max(120, out_h - safe_bottom - deck_top);
            const int pad_x = std::max(10, out_w / 28);

            const int util_h = std::clamp(deck_h * 15 / 100, 28, 56);
            const int util_y = deck_top;
            const int shoulder_w = std::clamp(out_w * 20 / 100, 56, 150);
            add_btn(TouchControlId::BtnL, TouchGroupId::BtnL, apply_group_rect(TouchGroupId::BtnL, {pad_x, util_y, shoulder_w, util_h}),
                    "L/CUS", 0x15, 72, 168, 255, false);
            add_btn(TouchControlId::BtnR, TouchGroupId::BtnR, apply_group_rect(TouchGroupId::BtnR, {out_w - pad_x - shoulder_w, util_y, shoulder_w, util_h}),
                    "R/PET", 0x16, 72, 168, 255, false);

            const int mid_x0 = pad_x + shoulder_w + 8;
            const int mid_x1 = out_w - pad_x - shoulder_w - 8;
            const int mid_w = std::max(60, mid_x1 - mid_x0);
            const int u_gap = 6;
            const int u_w = std::max(24, (mid_w - u_gap * 3) / 4);
            add_btn(TouchControlId::BtnPageUp, TouchGroupId::BtnPageUp, apply_group_rect(TouchGroupId::BtnPageUp, {mid_x0, util_y, u_w, util_h}),
                    "1", 0x01, 120, 140, 175, false);
            add_btn(TouchControlId::BtnMenu, TouchGroupId::BtnMenu, apply_group_rect(TouchGroupId::BtnMenu, {mid_x0 + (u_w + u_gap), util_y, u_w, util_h}),
                    "MENU", -1, 240, 176, 64, false);
            add_btn(TouchControlId::BtnTurbo, TouchGroupId::BtnTurbo, apply_group_rect(TouchGroupId::BtnTurbo, {mid_x0 + 2 * (u_w + u_gap), util_y, u_w, util_h}),
                    "FF", -1, 88, 212, 140, false);
            add_btn(TouchControlId::BtnPageDn, TouchGroupId::BtnPageDn, apply_group_rect(TouchGroupId::BtnPageDn, {mid_x0 + 3 * (u_w + u_gap), util_y, u_w, util_h}),
                    "4", 0x04, 120, 140, 175, false);

            const int main_top = util_y + util_h + std::max(10, deck_h / 14);
            const int main_h = std::max(90, out_h - safe_bottom - main_top);
            const int dpad_size = std::clamp(std::min(out_w * 42 / 100, main_h * 90 / 100), 96, 260);
            const int dpad_x = pad_x + std::max(0, (out_w / 2 - pad_x - dpad_size) / 3);
            const int dpad_y = main_top + (main_h - dpad_size) / 2;
            dpad_rect_ = apply_group_rect(TouchGroupId::Dpad, SDL_Rect{dpad_x, dpad_y, dpad_size, dpad_size});

            const int arm = dpad_rect_.w / 3;
            add_btn(TouchControlId::DpadUp,    TouchGroupId::Dpad, {dpad_rect_.x + arm, dpad_rect_.y, arm, arm}, "^", 0x11, 160, 190, 230, false);
            add_btn(TouchControlId::DpadDown,  TouchGroupId::Dpad, {dpad_rect_.x + arm, dpad_rect_.y + arm * 2, arm, arm}, "v", 0x13, 160, 190, 230, false);
            add_btn(TouchControlId::DpadLeft,  TouchGroupId::Dpad, {dpad_rect_.x, dpad_rect_.y + arm, arm, arm}, "<", 0x10, 160, 190, 230, false);
            add_btn(TouchControlId::DpadRight, TouchGroupId::Dpad, {dpad_rect_.x + arm * 2, dpad_rect_.y + arm, arm, arm}, ">", 0x12, 160, 190, 230, false);

            const int btn_sz = std::clamp(dpad_size * 44 / 100, 44, 108);
            const int right_cx = out_w - pad_x - dpad_size / 2;
            const int right_cy = dpad_y + dpad_size / 2;
            const int off_x = btn_sz * 58 / 100;
            const int off_y = btn_sz * 34 / 100;

            add_btn(TouchControlId::BtnA, TouchGroupId::BtnA,
                    apply_group_rect(TouchGroupId::BtnA, {right_cx + off_x - btn_sz / 2, right_cy - off_y - btn_sz / 2, btn_sz, btn_sz}),
                    "A", 0x14, 64, 180, 255, true);
            add_btn(TouchControlId::BtnB, TouchGroupId::BtnB,
                    apply_group_rect(TouchGroupId::BtnB, {right_cx - off_x - btn_sz / 2, right_cy + off_y - btn_sz / 2, btn_sz, btn_sz}),
                    "B", 0x0A, 240, 96, 96, true);
            const int p_sz = btn_sz * 72 / 100;
            add_btn(TouchControlId::BtnPound, TouchGroupId::BtnPound,
                    apply_group_rect(TouchGroupId::BtnPound, {right_cx - off_x - p_sz / 2, right_cy - off_y - btn_sz / 2 - p_sz / 4, p_sz, p_sz}),
                    "#", 0x0B, 176, 140, 240, true);
        } else {
            const int safe_x = std::max(14, out_w / 42);
            const int safe_y = std::max(8, out_h / 36);
            int gh = out_h - safe_y * 2;
            int gw = static_cast<int>(std::round(gh / aspect));
            if (gw > out_w * 72 / 100) {
                gw = out_w * 72 / 100;
                gh = static_cast<int>(std::round(gw * aspect));
            }
            game_rect_ = SDL_Rect{(out_w - gw) / 2, (out_h - gh) / 2, gw, gh};

            const int side_w = std::max(84, game_rect_.x - safe_x - 8);
            const int sh_h = std::clamp(out_h * 13 / 100, 32, 60);
            const int sh_w = std::min(side_w, std::clamp(out_w * 14 / 100, 68, 150));

            add_btn(TouchControlId::BtnL, TouchGroupId::BtnL, apply_group_rect(TouchGroupId::BtnL, {safe_x, safe_y + 4, sh_w, sh_h}),
                    "L/CUS", 0x15, 72, 168, 255, false);

            const int dpad_size = std::clamp(std::min(side_w, out_h * 48 / 100), 96, 230);
            const int dpad_x = safe_x + std::max(0, (side_w - dpad_size) / 2);
            const int dpad_y = out_h - safe_y - dpad_size - std::max(8, out_h / 24);
            dpad_rect_ = apply_group_rect(TouchGroupId::Dpad, SDL_Rect{dpad_x, dpad_y, dpad_size, dpad_size});
            const int arm = dpad_rect_.w / 3;
            add_btn(TouchControlId::DpadUp,    TouchGroupId::Dpad, {dpad_rect_.x + arm, dpad_rect_.y, arm, arm}, "^", 0x11, 160, 190, 230, false);
            add_btn(TouchControlId::DpadDown,  TouchGroupId::Dpad, {dpad_rect_.x + arm, dpad_rect_.y + arm * 2, arm, arm}, "v", 0x13, 160, 190, 230, false);
            add_btn(TouchControlId::DpadLeft,  TouchGroupId::Dpad, {dpad_rect_.x, dpad_rect_.y + arm, arm, arm}, "<", 0x10, 160, 190, 230, false);
            add_btn(TouchControlId::DpadRight, TouchGroupId::Dpad, {dpad_rect_.x + arm * 2, dpad_rect_.y + arm, arm, arm}, ">", 0x12, 160, 190, 230, false);

            const int sm_w = std::max(28, (sh_w - 6) / 2);
            const int sm_h = std::clamp(sh_h * 80 / 100, 26, 44);
            add_btn(TouchControlId::BtnPageUp, TouchGroupId::BtnPageUp, apply_group_rect(TouchGroupId::BtnPageUp, {safe_x, safe_y + sh_h + 12, sm_w, sm_h}),
                    "1", 0x01, 120, 140, 175, false);
            add_btn(TouchControlId::BtnPageDn, TouchGroupId::BtnPageDn, apply_group_rect(TouchGroupId::BtnPageDn, {safe_x + sm_w + 6, safe_y + sh_h + 12, sm_w, sm_h}),
                    "4", 0x04, 120, 140, 175, false);

            const int rx = out_w - safe_x - sh_w;
            add_btn(TouchControlId::BtnR, TouchGroupId::BtnR, apply_group_rect(TouchGroupId::BtnR, {rx, safe_y + 4, sh_w, sh_h}),
                    "R/PET", 0x16, 72, 168, 255, false);
            add_btn(TouchControlId::BtnMenu, TouchGroupId::BtnMenu, apply_group_rect(TouchGroupId::BtnMenu, {rx, safe_y + sh_h + 12, sm_w, sm_h}),
                    "MENU", -1, 240, 176, 64, false);
            add_btn(TouchControlId::BtnTurbo, TouchGroupId::BtnTurbo, apply_group_rect(TouchGroupId::BtnTurbo, {rx + sm_w + 6, safe_y + sh_h + 12, sm_w, sm_h}),
                    "FF", -1, 88, 212, 140, false);

            const int btn_sz = std::clamp(dpad_size * 44 / 100, 46, 102);
            const int right_cx = out_w - safe_x - side_w / 2;
            const int right_cy = dpad_y + dpad_size / 2;
            const int off_x = btn_sz * 54 / 100;
            const int off_y = btn_sz * 34 / 100;
            add_btn(TouchControlId::BtnA, TouchGroupId::BtnA,
                    apply_group_rect(TouchGroupId::BtnA, {right_cx + off_x - btn_sz / 2, right_cy - off_y - btn_sz / 2, btn_sz, btn_sz}),
                    "A", 0x14, 64, 180, 255, true);
            add_btn(TouchControlId::BtnB, TouchGroupId::BtnB,
                    apply_group_rect(TouchGroupId::BtnB, {right_cx - off_x - btn_sz / 2, right_cy + off_y - btn_sz / 2, btn_sz, btn_sz}),
                    "B", 0x0A, 240, 96, 96, true);
            const int p_sz = btn_sz * 70 / 100;
            add_btn(TouchControlId::BtnPound, TouchGroupId::BtnPound,
                    apply_group_rect(TouchGroupId::BtnPound, {right_cx - off_x - p_sz / 2, right_cy - off_y - btn_sz / 2 - p_sz / 4, p_sz, p_sz}),
                    "#", 0x0B, 176, 140, 240, true);
        }

        // Compute overlay-only "EDIT" button and layout-editor toolbar rects
        const int bar_h = std::clamp(out_h / 18, 34, 58);
        const int bar_y = std::max(8, out_h / 36);
        const int bar_w = std::min(out_w - 20, 460);
        const int bar_x = (out_w - bar_w) / 2;
        const int gap = 6;
        const int bw = (bar_w - gap * 4) / 5;
        edit_bar_rects_[0] = {bar_x,                 bar_y, bw, bar_h}; // "-"
        edit_bar_rects_[1] = {bar_x + (bw + gap),    bar_y, bw, bar_h}; // "+"
        edit_bar_rects_[2] = {bar_x + 2 * (bw + gap),bar_y, bw, bar_h}; // "OP"
        edit_bar_rects_[3] = {bar_x + 3 * (bw + gap),bar_y, bw, bar_h}; // "RST"
        edit_bar_rects_[4] = {bar_x + 4 * (bw + gap),bar_y, bw, bar_h}; // "SAVE"

        // Floating "EDIT" button shown ONLY when F1 overlay menu is open (top-right of screen)
        const int eb_w = std::clamp(out_w / 5, 68, 128);
        const int eb_h = std::clamp(out_h / 20, 32, 52);
        overlay_edit_btn_ = {out_w - eb_w - 10, std::max(8, out_h / 36), eb_w, eb_h};
    }

    // Handle an SDL touch or (desktop preview) mouse event. Returns true if consumed.
    bool handle_event(const SDL_Event& ev, int out_w, int out_h, bool overlay_open, TouchActionDelta& out_delta) {
        if (!enabled_ || out_w <= 0 || out_h <= 0) return false;

        // Extract pointer coords for touch or (desktop) mouse
        bool is_down = false, is_move = false, is_up = false;
        int px = 0, py = 0;
        SDL_FingerID fid = 0;
        bool is_finger = (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERUP);
        if (is_finger) {
            px = static_cast<int>(std::round(ev.tfinger.x * out_w));
            py = static_cast<int>(std::round(ev.tfinger.y * out_h));
            fid = ev.tfinger.fingerId;
            is_down = (ev.type == SDL_FINGERDOWN);
            is_move = (ev.type == SDL_FINGERMOTION);
            is_up   = (ev.type == SDL_FINGERUP);
        } else {
#if !defined(__ANDROID__) && !defined(__IPHONEOS__)
            if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                is_down = true; px = ev.button.x; py = ev.button.y;
            } else if (ev.type == SDL_MOUSEMOTION && (mouse_active_ || drag_finger_active_)) {
                is_move = true; px = ev.motion.x; py = ev.motion.y;
            } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT) {
                is_up = true; px = ev.button.x; py = ev.button.y;
            } else {
                return false;
            }
#else
            return false;
#endif
        }

        // 1) Interactive Layout Edit Mode (drag-and-drop buttons & resize/opacity/reset/save)
        if (edit_mode_) {
            return handle_edit_pointer(is_down, is_move, is_up, px, py, out_w, out_h, out_delta);
        }

        // 2) When F1 overlay is open, tapping the floating "EDIT" button enters Layout Edit Mode
        if (overlay_open && is_down && pt_in_rect(px, py, overlay_edit_btn_, 6)) {
            set_edit_mode(true);
            return true;
        }

        uint32_t prev_ctrl_mask = pressed_ctrl_mask_;

        if (is_finger) {
            if (is_up) {
                finger_states_.erase(fid);
            } else {
                uint32_t hit = hit_test_mask(px, py);
                if (is_down && hit == 0u) {
                    // Check for direct game-screen tap (Saito-style Custom Screen chip selection!)
                    if (!overlay_open && map_game_screen_tap(px, py, out_delta)) {
                        return true;
                    }
                    return false;
                }
                if (is_move && finger_states_.find(fid) == finger_states_.end()) {
                    return false;
                }
                finger_states_[fid] = hit;
            }
            recompute_pressed_bits();
            emit_edges(prev_ctrl_mask, overlay_open, out_delta);
            return true;
        } else {
            if (is_down) {
                uint32_t hit = hit_test_mask(px, py);
                if (hit != 0u) {
                    mouse_active_ = true;
                    mouse_ctrl_mask_ = hit;
                    recompute_pressed_bits();
                    emit_edges(prev_ctrl_mask, overlay_open, out_delta);
                    return true;
                }
                if (!overlay_open && map_game_screen_tap(px, py, out_delta)) {
                    return true;
                }
                return false;
            } else if (is_move && mouse_active_) {
                mouse_ctrl_mask_ = hit_test_mask(px, py);
                recompute_pressed_bits();
                emit_edges(prev_ctrl_mask, overlay_open, out_delta);
                return true;
            } else if (is_up && mouse_active_) {
                mouse_active_ = false;
                mouse_ctrl_mask_ = 0;
                recompute_pressed_bits();
                emit_edges(prev_ctrl_mask, overlay_open, out_delta);
                return true;
            }
        }
        return false;
    }

    uint32_t doja_key_mask() const { return pressed_doja_mask_; }
    bool turbo_down() const { return (pressed_ctrl_mask_ & ctrl_bit(TouchControlId::BtnTurbo)) != 0u; }

    void render(SDL_Renderer* ren, bool overlay_open = false) const {
        if (!enabled_ || !ren) return;
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

        // Subtle D-Pad center hub
        if (dpad_rect_.w > 0) {
            const int arm = dpad_rect_.w / 3;
            SDL_Rect center{dpad_rect_.x + arm, dpad_rect_.y + arm, arm, arm};
            uint8_t bg_a = static_cast<uint8_t>(110.0f * opacity_);
            SDL_SetRenderDrawColor(ren, 22, 34, 58, bg_a);
            SDL_RenderFillRect(ren, &center);
        }

        for (const auto& b : buttons_) {
            const bool down = !edit_mode_ && ((pressed_ctrl_mask_ & ctrl_bit(b.id)) != 0u);
            const bool sel_edit = edit_mode_ && (static_cast<int>(b.group) == selected_group_);
            const uint8_t fill_a = static_cast<uint8_t>((down || sel_edit ? 200.0f : 115.0f) * opacity_);
            const uint8_t border_a = static_cast<uint8_t>((down || sel_edit ? 255.0f : 200.0f) * opacity_);
            const uint8_t fr = sel_edit ? 220 : (down ? b.accent_r : static_cast<uint8_t>(18 + b.accent_r / 7));
            const uint8_t fg = sel_edit ? 150 : (down ? b.accent_g : static_cast<uint8_t>(26 + b.accent_g / 7));
            const uint8_t fb = sel_edit ? 40  : (down ? b.accent_b : static_cast<uint8_t>(44 + b.accent_b / 7));

            SDL_SetRenderDrawColor(ren, fr, fg, fb, fill_a);
            SDL_RenderFillRect(ren, &b.rect);

            SDL_SetRenderDrawColor(ren, sel_edit ? 255 : b.accent_r, sel_edit ? 215 : b.accent_g, sel_edit ? 64 : b.accent_b, border_a);
            SDL_RenderDrawRect(ren, &b.rect);
            if (b.rect.w > 8 && b.rect.h > 8) {
                SDL_Rect inner{b.rect.x + 1, b.rect.y + 1, b.rect.w - 2, b.rect.h - 2};
                SDL_RenderDrawRect(ren, &inner);
            }

            if (b.label && *b.label) {
                const uint8_t txt_a = static_cast<uint8_t>((down || sel_edit ? 255.0f : 230.0f) * opacity_);
                draw_mini_label(ren, b.rect, b.label, 240, 248, 255, txt_a);
            }
        }

        // In Edit Mode: highlight D-Pad bounding box & draw top floating editor toolbar
        if (edit_mode_) {
            if (selected_group_ == static_cast<int>(TouchGroupId::Dpad) && dpad_rect_.w > 0) {
                SDL_SetRenderDrawColor(ren, 255, 215, 64, 240);
                SDL_RenderDrawRect(ren, &dpad_rect_);
            }
            static const char* kBarLabels[5] = {"-", "+", "OP", "RST", "OK"};
            static const uint8_t kBarCol[5][3] = {
                {88, 168, 255}, {88, 168, 255}, {176, 140, 240}, {240, 110, 96}, {72, 220, 140}
            };
            for (int i = 0; i < 5; ++i) {
                SDL_SetRenderDrawColor(ren, 14, 22, 42, 235);
                SDL_RenderFillRect(ren, &edit_bar_rects_[i]);
                SDL_SetRenderDrawColor(ren, kBarCol[i][0], kBarCol[i][1], kBarCol[i][2], 255);
                SDL_RenderDrawRect(ren, &edit_bar_rects_[i]);
                draw_mini_label(ren, edit_bar_rects_[i], kBarLabels[i], 245, 250, 255, 255);
            }
        } else if (overlay_open) {
            // Show compact "EDIT" button in top-right corner when pause/settings overlay is open
            SDL_SetRenderDrawColor(ren, 24, 40, 74, 230);
            SDL_RenderFillRect(ren, &overlay_edit_btn_);
            SDL_SetRenderDrawColor(ren, 255, 196, 64, 255);
            SDL_RenderDrawRect(ren, &overlay_edit_btn_);
            draw_mini_label(ren, overlay_edit_btn_, "EDIT", 255, 244, 200, 255);
        }
    }

private:
    bool enabled_ = false;
    bool edit_mode_ = false;
    bool layout_dirty_ = true;
    bool portrait_ = true;
    float opacity_ = 0.85f;
    int last_w_ = 0;
    int last_h_ = 0;
    int last_logical_h_ = 260;
    SDL_Rect game_rect_{0, 0, 240, 240};
    SDL_Rect dpad_rect_{0, 0, 0, 0};
    SDL_Rect edit_bar_rects_[5]{};
    SDL_Rect overlay_edit_btn_{0, 0, 0, 0};
    std::vector<TouchButtonRect> buttons_;

    TouchGroupOverride portrait_ov_[static_cast<int>(TouchGroupId::Count)]{};
    TouchGroupOverride landscape_ov_[static_cast<int>(TouchGroupId::Count)]{};

    int selected_group_ = 0;
    int dragging_group_ = -1;
    bool drag_finger_active_ = false;
    int drag_off_x_ = 0;
    int drag_off_y_ = 0;

    std::unordered_map<SDL_FingerID, uint32_t> finger_states_;
    bool mouse_active_ = false;
    uint32_t mouse_ctrl_mask_ = 0;
    uint32_t pressed_ctrl_mask_ = 0;
    uint32_t pressed_doja_mask_ = 0;

    static constexpr uint32_t ctrl_bit(TouchControlId id) {
        return (static_cast<int>(id) >= 0) ? (1u << static_cast<int>(id)) : 0u;
    }

    static bool pt_in_rect(int px, int py, const SDL_Rect& r, int slop = 0) {
        return r.w > 0 && r.h > 0 &&
               px >= r.x - slop && px <= r.x + r.w + slop &&
               py >= r.y - slop && py <= r.y + r.h + slop;
    }

    void add_btn(TouchControlId id, TouchGroupId group, SDL_Rect r, const char* label, int doja_key,
                 uint8_t ar, uint8_t ag, uint8_t ab, bool is_round) {
        buttons_.push_back(TouchButtonRect{id, group, r, label, doja_key, ar, ag, ab, is_round});
    }

    SDL_Rect group_rect(TouchGroupId g) const {
        if (g == TouchGroupId::Dpad) return dpad_rect_;
        for (const auto& b : buttons_) {
            if (b.group == g) return b.rect;
        }
        return SDL_Rect{0, 0, 0, 0};
    }

    bool map_game_screen_tap(int px, int py, TouchActionDelta& out) const {
        if (game_rect_.w <= 0 || game_rect_.h <= 0) return false;
        if (!pt_in_rect(px, py, game_rect_, 0)) return false;
        float nx = static_cast<float>(px - game_rect_.x) / static_cast<float>(game_rect_.w);
        float ny = static_cast<float>(py - game_rect_.y) / static_cast<float>(game_rect_.h);
        int gx = static_cast<int>(std::floor(nx * 240.0f));
        int gy = static_cast<int>(std::floor(ny * static_cast<float>(last_logical_h_)));
        if (gx >= 0 && gx < 240 && gy >= 0 && gy < 240) {
            out.has_game_tap = true;
            out.game_tap_x = gx;
            out.game_tap_y = gy;
            return true;
        }
        return false;
    }

    bool handle_edit_pointer(bool is_down, bool is_move, bool is_up, int px, int py,
                             int out_w, int out_h, TouchActionDelta& out_delta) {
        auto& ovs = portrait_ ? portrait_ov_ : landscape_ov_;
        if (is_down) {
            // Check toolbar buttons first
            for (int i = 0; i < 5; ++i) {
                if (!pt_in_rect(px, py, edit_bar_rects_[i], 4)) continue;
                if (i == 0 || i == 1) {
                    if (selected_group_ >= 0 && selected_group_ < static_cast<int>(TouchGroupId::Count)) {
                        auto& ov = ovs[selected_group_];
                        if (!ov.custom) {
                            SDL_Rect cur = group_rect(static_cast<TouchGroupId>(selected_group_));
                            ov.custom = true;
                            ov.cx = static_cast<float>(cur.x + cur.w * 0.5f) / static_cast<float>(out_w);
                            ov.cy = static_cast<float>(cur.y + cur.h * 0.5f) / static_cast<float>(out_h);
                            ov.scale = 1.0f;
                        }
                        ov.scale = std::clamp(ov.scale + (i == 1 ? 0.12f : -0.12f), 0.60f, 1.60f);
                        layout_dirty_ = true;
                    }
                } else if (i == 2) {
                    // Cycle opacity presets: 35% -> 55% -> 75% -> 90%
                    if (opacity_ < 0.45f) opacity_ = 0.55f;
                    else if (opacity_ < 0.65f) opacity_ = 0.75f;
                    else if (opacity_ < 0.82f) opacity_ = 0.90f;
                    else opacity_ = 0.35f;
                } else if (i == 3) {
                    reset_current_orientation();
                } else if (i == 4) {
                    set_edit_mode(false);
                    out_delta.layout_saved = true;
                }
                return true;
            }

            // Otherwise check if user grabbed a draggable group (D-Pad or button)
            int hit_g = -1;
            if (pt_in_rect(px, py, dpad_rect_, 8)) {
                hit_g = static_cast<int>(TouchGroupId::Dpad);
            } else {
                for (const auto& b : buttons_) {
                    if (b.group == TouchGroupId::Dpad) continue;
                    if (pt_in_rect(px, py, b.rect, 8)) {
                        hit_g = static_cast<int>(b.group);
                        break;
                    }
                }
            }
            if (hit_g >= 0) {
                selected_group_ = hit_g;
                dragging_group_ = hit_g;
                drag_finger_active_ = true;
                SDL_Rect cur = group_rect(static_cast<TouchGroupId>(hit_g));
                drag_off_x_ = (cur.x + cur.w / 2) - px;
                drag_off_y_ = (cur.y + cur.h / 2) - py;
            }
            return true;
        }
        if (is_move && drag_finger_active_ && dragging_group_ >= 0) {
            auto& ov = ovs[dragging_group_];
            if (!ov.custom) {
                ov.custom = true;
                ov.scale = 1.0f;
            }
            ov.cx = std::clamp(static_cast<float>(px + drag_off_x_) / static_cast<float>(out_w), 0.04f, 0.96f);
            ov.cy = std::clamp(static_cast<float>(py + drag_off_y_) / static_cast<float>(out_h), 0.04f, 0.96f);
            layout_dirty_ = true;
            return true;
        }
        if (is_up) {
            dragging_group_ = -1;
            drag_finger_active_ = false;
            return true;
        }
        return true;
    }

    uint32_t hit_test_mask(int px, int py) const {
        if (dpad_rect_.w > 0) {
            const int pad = std::max(10, dpad_rect_.w / 10);
            if (px >= dpad_rect_.x - pad && px <= dpad_rect_.x + dpad_rect_.w + pad &&
                py >= dpad_rect_.y - pad && py <= dpad_rect_.y + dpad_rect_.h + pad) {
                const float cx = dpad_rect_.x + dpad_rect_.w * 0.5f;
                const float cy = dpad_rect_.y + dpad_rect_.h * 0.5f;
                const float dx = static_cast<float>(px) - cx;
                const float dy = static_cast<float>(py) - cy;
                const float dead = dpad_rect_.w * 0.11f;
                uint32_t m = 0;
                if (dx * dx + dy * dy >= dead * dead) {
                    const float ax = std::fabs(dx);
                    const float ay = std::fabs(dy);
                    if (ax >= ay * 0.45f) {
                        m |= (dx < 0.0f) ? ctrl_bit(TouchControlId::DpadLeft) : ctrl_bit(TouchControlId::DpadRight);
                    }
                    if (ay >= ax * 0.45f) {
                        m |= (dy < 0.0f) ? ctrl_bit(TouchControlId::DpadUp) : ctrl_bit(TouchControlId::DpadDown);
                    }
                }
                return m;
            }
        }

        for (const auto& b : buttons_) {
            if (b.group == TouchGroupId::Dpad) continue;
            if (pt_in_rect(px, py, b.rect, 6)) {
                return ctrl_bit(b.id);
            }
        }
        return 0u;
    }

    void recompute_pressed_bits() {
        uint32_t cm = mouse_active_ ? mouse_ctrl_mask_ : 0u;
        for (const auto& kv : finger_states_) {
            cm |= kv.second;
        }
        pressed_ctrl_mask_ = cm;

        uint32_t dm = 0u;
        for (const auto& b : buttons_) {
            if (b.doja_key >= 0 && (cm & ctrl_bit(b.id)) != 0u) {
                dm |= (1u << b.doja_key);
            }
        }
        pressed_doja_mask_ = dm;
    }

    void emit_edges(uint32_t prev_ctrl_mask, bool overlay_open, TouchActionDelta& out) const {
        const uint32_t newly_pressed = pressed_ctrl_mask_ & ~prev_ctrl_mask;
        out.doja_mask = pressed_doja_mask_;
        out.turbo_down = turbo_down();
        if (newly_pressed & ctrl_bit(TouchControlId::BtnMenu)) {
            if (overlay_open) {
                out.overlay_keys.push_back(SDLK_ESCAPE);
            } else {
                out.menu_triggered = true;
            }
        }
        if (overlay_open) {
            if (newly_pressed & ctrl_bit(TouchControlId::DpadUp))    out.overlay_keys.push_back(SDLK_UP);
            if (newly_pressed & ctrl_bit(TouchControlId::DpadDown))  out.overlay_keys.push_back(SDLK_DOWN);
            if (newly_pressed & ctrl_bit(TouchControlId::DpadLeft))  out.overlay_keys.push_back(SDLK_LEFT);
            if (newly_pressed & ctrl_bit(TouchControlId::DpadRight)) out.overlay_keys.push_back(SDLK_RIGHT);
            if (newly_pressed & ctrl_bit(TouchControlId::BtnA))      out.overlay_keys.push_back(SDLK_RETURN);
            if (newly_pressed & ctrl_bit(TouchControlId::BtnB))      out.overlay_keys.push_back(SDLK_ESCAPE);
            if (newly_pressed & ctrl_bit(TouchControlId::BtnL))      out.overlay_keys.push_back(SDLK_PAGEUP);
            if (newly_pressed & ctrl_bit(TouchControlId::BtnR))      out.overlay_keys.push_back(SDLK_PAGEDOWN);
        }
    }

    static void draw_mini_glyph(SDL_Renderer* ren, char ch, int x, int y, int s) {
        static const struct { char c; uint16_t bits; } kFont[] = {
            {'A', 0b010101111101101}, {'B', 0b110101110101110}, {'C', 0b011100100100011},
            {'D', 0b110101101101110}, {'E', 0b111100110100111}, {'F', 0b111100110100100},
            {'I', 0b111010010010111}, {'K', 0b101101110101101}, {'L', 0b100100100100111},
            {'M', 0b101111101101101}, {'N', 0b110101101101101}, {'O', 0b010101101101010},
            {'P', 0b110101110100100}, {'R', 0b110101110101101}, {'S', 0b011100010001110},
            {'T', 0b111010010010010}, {'U', 0b101101101101111}, {'1', 0b010110010010111},
            {'4', 0b101101111001001}, {'/', 0b001001010100100}, {'#', 0b101111101111101},
            {'^', 0b010101000000000}, {'v', 0b000000000101010}, {'<', 0b001010100010001},
            {'>', 0b100010001010100}, {'-', 0b000000111000000}, {'+', 0b000010111010000},
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

    static void draw_mini_label(SDL_Renderer* ren, const SDL_Rect& r, const char* txt,
                                uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
        size_t len = std::strlen(txt);
        if (len == 0) return;
        int s = std::clamp(std::min(r.h / 10, r.w / static_cast<int>(len * 5 + 2)), 2, 6);
        int total_w = static_cast<int>(len) * (4 * s) - s;
        int total_h = 5 * s;
        int x0 = r.x + (r.w - total_w) / 2;
        int y0 = r.y + (r.h - total_h) / 2;
        SDL_SetRenderDrawColor(ren, cr, cg, cb, ca);
        for (size_t i = 0; i < len; ++i) {
            draw_mini_glyph(ren, txt[i], x0 + static_cast<int>(i) * (4 * s), y0, s);
        }
    }
};

} // namespace rmexe
