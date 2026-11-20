#pragma once

// ============================================================================
// runtime/touch_controls.hpp
// Pure C++20 / SDL2 Multi-Touch Virtual Gamepad & Dual-Orientation Viewport
// Shared across Android (APK), iOS (IPA), and Desktop (--touch preview mode).
// ============================================================================

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

struct TouchButtonRect {
    TouchControlId id = TouchControlId::None;
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
    // Edge-triggered menu navigation events when overlay is open
    std::vector<SDL_Keycode> overlay_keys;
};

class TouchOverlayController {
public:
    bool enabled() const { return enabled_; }
    void set_enabled(bool en) {
        enabled_ = en;
        if (!en) {
            clear_all_fingers();
        }
    }

    float opacity() const { return opacity_; }
    void set_opacity(float op) { opacity_ = std::clamp(op, 0.2f, 1.0f); }

    bool is_portrait() const { return portrait_; }
    SDL_Rect game_rect() const { return game_rect_; }

    void clear_all_fingers() {
        finger_states_.clear();
        mouse_active_ = false;
        mouse_ctrl_mask_ = 0;
        recompute_pressed_bits();
    }

    void update_layout(int out_w, int out_h, int game_logical_h) {
        if (out_w <= 0 || out_h <= 0) return;
        if (out_w == last_w_ && out_h == last_h_ && game_logical_h == last_logical_h_) {
            return;
        }
        last_w_ = out_w;
        last_h_ = out_h;
        last_logical_h_ = game_logical_h;
        buttons_.clear();

        const float aspect = static_cast<float>(game_logical_h) / 240.0f;
        portrait_ = (out_h > static_cast<int>(out_w * 1.12f));

        if (portrait_) {
            // Portrait: flip-phone / GBA SP ergonomic layout (zero screen occlusion)
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

            // Top utility strip of control deck: L | 1 | MENU | 4 | TURBO | R
            const int util_h = std::clamp(deck_h * 15 / 100, 28, 56);
            const int util_y = deck_top;
            const int shoulder_w = std::clamp(out_w * 20 / 100, 56, 150);
            add_btn(TouchControlId::BtnL, {pad_x, util_y, shoulder_w, util_h}, "L/CUS", 0x15, 72, 168, 255, false);
            add_btn(TouchControlId::BtnR, {out_w - pad_x - shoulder_w, util_y, shoulder_w, util_h}, "R/PET", 0x16, 72, 168, 255, false);

            const int mid_x0 = pad_x + shoulder_w + 8;
            const int mid_x1 = out_w - pad_x - shoulder_w - 8;
            const int mid_w = std::max(60, mid_x1 - mid_x0);
            const int u_gap = 6;
            const int u_w = std::max(24, (mid_w - u_gap * 3) / 4);
            add_btn(TouchControlId::BtnPageUp, {mid_x0, util_y, u_w, util_h}, "1", 0x01, 120, 140, 175, false);
            add_btn(TouchControlId::BtnMenu, {mid_x0 + (u_w + u_gap), util_y, u_w, util_h}, "MENU", -1, 240, 176, 64, false);
            add_btn(TouchControlId::BtnTurbo, {mid_x0 + 2 * (u_w + u_gap), util_y, u_w, util_h}, "FF", -1, 88, 212, 140, false);
            add_btn(TouchControlId::BtnPageDn, {mid_x0 + 3 * (u_w + u_gap), util_y, u_w, util_h}, "4", 0x04, 120, 140, 175, false);

            // Main control zone: D-Pad on left, A/B/# on right
            const int main_top = util_y + util_h + std::max(10, deck_h / 14);
            const int main_h = std::max(90, out_h - safe_bottom - main_top);
            const int dpad_size = std::clamp(std::min(out_w * 42 / 100, main_h * 90 / 100), 96, 260);
            const int dpad_x = pad_x + std::max(0, (out_w / 2 - pad_x - dpad_size) / 3);
            const int dpad_y = main_top + (main_h - dpad_size) / 2;
            dpad_rect_ = SDL_Rect{dpad_x, dpad_y, dpad_size, dpad_size};

            const int arm = dpad_size / 3;
            add_btn(TouchControlId::DpadUp,    {dpad_x + arm, dpad_y, arm, arm}, "^", 0x11, 160, 190, 230, false);
            add_btn(TouchControlId::DpadDown,  {dpad_x + arm, dpad_y + arm * 2, arm, arm}, "v", 0x13, 160, 190, 230, false);
            add_btn(TouchControlId::DpadLeft,  {dpad_x, dpad_y + arm, arm, arm}, "<", 0x10, 160, 190, 230, false);
            add_btn(TouchControlId::DpadRight, {dpad_x + arm * 2, dpad_y + arm, arm, arm}, ">", 0x12, 160, 190, 230, false);

            // Right face buttons: A (bottom-right/primary), B (left-lower), # (top-left)
            const int btn_sz = std::clamp(dpad_size * 44 / 100, 44, 108);
            const int right_cx = out_w - pad_x - dpad_size / 2;
            const int right_cy = dpad_y + dpad_size / 2;
            const int off_x = btn_sz * 58 / 100;
            const int off_y = btn_sz * 34 / 100;

            add_btn(TouchControlId::BtnA, {right_cx + off_x - btn_sz / 2, right_cy - off_y - btn_sz / 2, btn_sz, btn_sz},
                    "A", 0x14, 64, 180, 255, true);
            add_btn(TouchControlId::BtnB, {right_cx - off_x - btn_sz / 2, right_cy + off_y - btn_sz / 2, btn_sz, btn_sz},
                    "B", 0x0A, 240, 96, 96, true);
            const int p_sz = btn_sz * 72 / 100;
            add_btn(TouchControlId::BtnPound, {right_cx - off_x - p_sz / 2, right_cy - off_y - btn_sz / 2 - p_sz / 4, p_sz, p_sz},
                    "#", 0x0B, 176, 140, 240, true);
        } else {
            // Landscape: 1:1 game screen centered full-height, side touch rails
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

            // Left rail: L shoulder + D-Pad + 1/4
            add_btn(TouchControlId::BtnL, {safe_x, safe_y + 4, sh_w, sh_h}, "L/CUS", 0x15, 72, 168, 255, false);

            const int dpad_size = std::clamp(std::min(side_w, out_h * 48 / 100), 96, 230);
            const int dpad_x = safe_x + std::max(0, (side_w - dpad_size) / 2);
            const int dpad_y = out_h - safe_y - dpad_size - std::max(8, out_h / 24);
            dpad_rect_ = SDL_Rect{dpad_x, dpad_y, dpad_size, dpad_size};
            const int arm = dpad_size / 3;
            add_btn(TouchControlId::DpadUp,    {dpad_x + arm, dpad_y, arm, arm}, "^", 0x11, 160, 190, 230, false);
            add_btn(TouchControlId::DpadDown,  {dpad_x + arm, dpad_y + arm * 2, arm, arm}, "v", 0x13, 160, 190, 230, false);
            add_btn(TouchControlId::DpadLeft,  {dpad_x, dpad_y + arm, arm, arm}, "<", 0x10, 160, 190, 230, false);
            add_btn(TouchControlId::DpadRight, {dpad_x + arm * 2, dpad_y + arm, arm, arm}, ">", 0x12, 160, 190, 230, false);

            const int sm_w = std::max(28, (sh_w - 6) / 2);
            const int sm_h = std::clamp(sh_h * 80 / 100, 26, 44);
            add_btn(TouchControlId::BtnPageUp, {safe_x, safe_y + sh_h + 12, sm_w, sm_h}, "1", 0x01, 120, 140, 175, false);
            add_btn(TouchControlId::BtnPageDn, {safe_x + sm_w + 6, safe_y + sh_h + 12, sm_w, sm_h}, "4", 0x04, 120, 140, 175, false);

            // Right rail: R shoulder + MENU/FF + A/B/#
            const int rx = out_w - safe_x - sh_w;
            add_btn(TouchControlId::BtnR, {rx, safe_y + 4, sh_w, sh_h}, "R/PET", 0x16, 72, 168, 255, false);
            add_btn(TouchControlId::BtnMenu, {rx, safe_y + sh_h + 12, sm_w, sm_h}, "MENU", -1, 240, 176, 64, false);
            add_btn(TouchControlId::BtnTurbo, {rx + sm_w + 6, safe_y + sh_h + 12, sm_w, sm_h}, "FF", -1, 88, 212, 140, false);

            const int btn_sz = std::clamp(dpad_size * 44 / 100, 46, 102);
            const int right_cx = out_w - safe_x - side_w / 2;
            const int right_cy = dpad_y + dpad_size / 2;
            const int off_x = btn_sz * 54 / 100;
            const int off_y = btn_sz * 34 / 100;
            add_btn(TouchControlId::BtnA, {right_cx + off_x - btn_sz / 2, right_cy - off_y - btn_sz / 2, btn_sz, btn_sz},
                    "A", 0x14, 64, 180, 255, true);
            add_btn(TouchControlId::BtnB, {right_cx - off_x - btn_sz / 2, right_cy + off_y - btn_sz / 2, btn_sz, btn_sz},
                    "B", 0x0A, 240, 96, 96, true);
            const int p_sz = btn_sz * 70 / 100;
            add_btn(TouchControlId::BtnPound, {right_cx - off_x - p_sz / 2, right_cy - off_y - btn_sz / 2 - p_sz / 4, p_sz, p_sz},
                    "#", 0x0B, 176, 140, 240, true);
        }
    }

    // Handle an SDL touch or (desktop preview) mouse event. Returns true if consumed.
    bool handle_event(const SDL_Event& ev, int out_w, int out_h, bool overlay_open, TouchActionDelta& out_delta) {
        if (!enabled_ || out_w <= 0 || out_h <= 0) return false;

        uint32_t prev_ctrl_mask = pressed_ctrl_mask_;

        if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERMOTION || ev.type == SDL_FINGERUP) {
            const int px = static_cast<int>(std::round(ev.tfinger.x * out_w));
            const int py = static_cast<int>(std::round(ev.tfinger.y * out_h));
            const SDL_FingerID fid = ev.tfinger.fingerId;
            if (ev.type == SDL_FINGERUP) {
                finger_states_.erase(fid);
            } else {
                uint32_t hit = hit_test_mask(px, py);
                if (ev.type == SDL_FINGERDOWN && hit == 0u) {
                    return false;
                }
                if (ev.type == SDL_FINGERMOTION && finger_states_.find(fid) == finger_states_.end()) {
                    return false;
                }
                finger_states_[fid] = hit;
            }
            recompute_pressed_bits();
            emit_edges(prev_ctrl_mask, overlay_open, out_delta);
            return true;
        }

#if !defined(__ANDROID__) && !defined(__IPHONEOS__)
        // Desktop --touch preview: allow testing virtual gamepad with left mouse button
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            uint32_t hit = hit_test_mask(ev.button.x, ev.button.y);
            if (hit != 0u) {
                mouse_active_ = true;
                mouse_ctrl_mask_ = hit;
                recompute_pressed_bits();
                emit_edges(prev_ctrl_mask, overlay_open, out_delta);
                return true;
            }
        } else if (ev.type == SDL_MOUSEMOTION && mouse_active_) {
            mouse_ctrl_mask_ = hit_test_mask(ev.motion.x, ev.motion.y);
            recompute_pressed_bits();
            emit_edges(prev_ctrl_mask, overlay_open, out_delta);
            return true;
        } else if (ev.type == SDL_MOUSEBUTTONUP && ev.button.button == SDL_BUTTON_LEFT && mouse_active_) {
            mouse_active_ = false;
            mouse_ctrl_mask_ = 0;
            recompute_pressed_bits();
            emit_edges(prev_ctrl_mask, overlay_open, out_delta);
            return true;
        }
#endif
        return false;
    }

    uint32_t doja_key_mask() const { return pressed_doja_mask_; }
    bool turbo_down() const { return (pressed_ctrl_mask_ & ctrl_bit(TouchControlId::BtnTurbo)) != 0u; }

    void render(SDL_Renderer* ren) const {
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
            const bool down = (pressed_ctrl_mask_ & ctrl_bit(b.id)) != 0u;
            const uint8_t fill_a = static_cast<uint8_t>((down ? 195.0f : 115.0f) * opacity_);
            const uint8_t border_a = static_cast<uint8_t>((down ? 255.0f : 200.0f) * opacity_);
            const uint8_t fr = down ? b.accent_r : static_cast<uint8_t>(18 + b.accent_r / 7);
            const uint8_t fg = down ? b.accent_g : static_cast<uint8_t>(26 + b.accent_g / 7);
            const uint8_t fb = down ? b.accent_b : static_cast<uint8_t>(44 + b.accent_b / 7);

            SDL_SetRenderDrawColor(ren, fr, fg, fb, fill_a);
            SDL_RenderFillRect(ren, &b.rect);

            SDL_SetRenderDrawColor(ren, b.accent_r, b.accent_g, b.accent_b, border_a);
            SDL_RenderDrawRect(ren, &b.rect);
            if (b.rect.w > 8 && b.rect.h > 8) {
                SDL_Rect inner{b.rect.x + 1, b.rect.y + 1, b.rect.w - 2, b.rect.h - 2};
                SDL_RenderDrawRect(ren, &inner);
            }

            if (b.label && *b.label) {
                const uint8_t txt_a = static_cast<uint8_t>((down ? 255.0f : 230.0f) * opacity_);
                draw_mini_label(ren, b.rect, b.label, 240, 248, 255, txt_a);
            }
        }
    }

private:
    bool enabled_ = false;
    bool portrait_ = true;
    float opacity_ = 0.85f;
    int last_w_ = 0;
    int last_h_ = 0;
    int last_logical_h_ = 0;
    SDL_Rect game_rect_{0, 0, 240, 240};
    SDL_Rect dpad_rect_{0, 0, 0, 0};
    std::vector<TouchButtonRect> buttons_;

    std::unordered_map<SDL_FingerID, uint32_t> finger_states_;
    bool mouse_active_ = false;
    uint32_t mouse_ctrl_mask_ = 0;
    uint32_t pressed_ctrl_mask_ = 0;
    uint32_t pressed_doja_mask_ = 0;

    static constexpr uint32_t ctrl_bit(TouchControlId id) {
        return (static_cast<int>(id) >= 0) ? (1u << static_cast<int>(id)) : 0u;
    }

    void add_btn(TouchControlId id, SDL_Rect r, const char* label, int doja_key,
                 uint8_t ar, uint8_t ag, uint8_t ab, bool is_round) {
        buttons_.push_back(TouchButtonRect{id, r, label, doja_key, ar, ag, ab, is_round});
    }

    uint32_t hit_test_mask(int px, int py) const {
        // Ergonomic D-Pad zone with smooth diagonal & slide-across support
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
                    // Allow clean 8-way diagonals when within ~2:1 ratio
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
            if (b.id == TouchControlId::DpadUp || b.id == TouchControlId::DpadDown ||
                b.id == TouchControlId::DpadLeft || b.id == TouchControlId::DpadRight) {
                continue;
            }
            const int slop = 6;
            if (px >= b.rect.x - slop && px <= b.rect.x + b.rect.w + slop &&
                py >= b.rect.y - slop && py <= b.rect.y + b.rect.h + slop) {
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

    // Minimal 3x5 pixel bitmap font for crisp touch button labels (no external font dep)
    static void draw_mini_glyph(SDL_Renderer* ren, char ch, int x, int y, int s) {
        static const struct { char c; uint16_t bits; } kFont[] = {
            {'A', 0b010101111101101}, {'B', 0b110101110101110}, {'C', 0b011100100100011},
            {'E', 0b111100110100111}, {'F', 0b111100110100100}, {'L', 0b100100100100111},
            {'M', 0b101111101101101}, {'N', 0b110101101101101}, {'P', 0b110101110100100},
            {'R', 0b110101110101101}, {'S', 0b011100010001110}, {'T', 0b111010010010010},
            {'U', 0b101101101101111}, {'1', 0b010110010010111}, {'4', 0b101101111001001},
            {'/', 0b001001010100100}, {'#', 0b101111101111101}, {'^', 0b010101000000000},
            {'v', 0b000000000101010}, {'<', 0b001010100010001}, {'>', 0b100010001010100},
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
