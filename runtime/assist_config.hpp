#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace rmexe {

struct AssistConfig {
    bool master_enabled = true;           // Global switch (toggled/held by F6)
    int32_t hotkey_mode = 0;              // 0 = Trigger (toggle F6), 1 = Hold (active while holding F6)

    int32_t buster_mult = 1;              // Normal Buster damage multiplier: 1..9999
    int32_t last_nonzero_buster_mult = 100; // Target multiplier for F7 quick toggle

    int32_t damage_taken_pct = 100;       // Incoming player damage %: 0..500 (0 = invincible)
    int32_t encounter_rate_pct = 100;     // Encounter rate %: 0..10000 (0 = none, 10000 = every step)
    bool wild_boss_only = false;          // Wild Boss Only Stealth: 100% boss on boss maps, 0% elsewhere
    int32_t show_ex_boss = 0;             // 0 = Off, 1 = Radar HUD, 2 = Radar + Force Unlock
    int32_t ex_boss_tier = -1;            // -1 = Auto (save navi_flg), 0..3 = Forced tier (V1/V2/V3/SP)

    int32_t zenny_rate_pct = 100;         // Zenny multiplier %: 0..10000
    int32_t run_speed_pct = 100;          // Overworld walk/run speed %: 50..500
    bool auto_full_hp = false;            // Restore full HP after winning/escaping battle
    bool always_escape = false;           // 100% escape chance in escapable battles
    int32_t custom_gauge_pct = 100;       // Custom Gauge fill speed %: 50..10000
    int32_t drop_rank_mode = 0;           // 0 = Off, 1 = Always Drop Chip, 2 = S-Rank + Always Drop

    bool show_assist_hud = true;          // Show compact active-assist badge on screen

    void sanitize() {
        hotkey_mode = (hotkey_mode == 1) ? 1 : 0;
        buster_mult = std::clamp<int32_t>(buster_mult, 1, 9999);
        last_nonzero_buster_mult = std::clamp<int32_t>(last_nonzero_buster_mult, 2, 9999);
        if (buster_mult > 1) {
            last_nonzero_buster_mult = buster_mult;
        }
        damage_taken_pct = std::clamp<int32_t>(damage_taken_pct, 0, 500);
        encounter_rate_pct = std::clamp<int32_t>(encounter_rate_pct, 0, 10000);
        show_ex_boss = std::clamp<int32_t>(show_ex_boss, 0, 2);
        ex_boss_tier = std::clamp<int32_t>(ex_boss_tier, -1, 3);
        zenny_rate_pct = std::clamp<int32_t>(zenny_rate_pct, 0, 10000);
        run_speed_pct = std::clamp<int32_t>(run_speed_pct, 50, 500);
        custom_gauge_pct = std::clamp<int32_t>(custom_gauge_pct, 50, 10000);
        drop_rank_mode = std::clamp<int32_t>(drop_rank_mode, 0, 2);
    }

    void reset_to_vanilla() {
        master_enabled = true;
        buster_mult = 1;
        damage_taken_pct = 100;
        encounter_rate_pct = 100;
        wild_boss_only = false;
        show_ex_boss = 0;
        ex_boss_tier = -1;
        zenny_rate_pct = 100;
        run_speed_pct = 100;
        auto_full_hp = false;
        always_escape = false;
        custom_gauge_pct = 100;
        drop_rank_mode = 0;
        sanitize();
    }

    bool is_effective_on(bool hold_key_down) const {
        return (hotkey_mode == 1) ? hold_key_down : master_enabled;
    }

    int active_cheat_count() const {
        int c = 0;
        if (buster_mult != 1) ++c;
        if (damage_taken_pct != 100) ++c;
        if (encounter_rate_pct != 100) ++c;
        if (wild_boss_only) ++c;
        if (show_ex_boss != 0) ++c;
        if (ex_boss_tier != -1) ++c;
        if (zenny_rate_pct != 100) ++c;
        if (run_speed_pct != 100) ++c;
        if (auto_full_hp) ++c;
        if (always_escape) ++c;
        if (custom_gauge_pct != 100) ++c;
        if (drop_rank_mode != 0) ++c;
        return c;
    }

    bool has_any_non_default() const {
        return active_cheat_count() > 0;
    }

    bool has_ranking_invalidating_cheat(bool hold_key_down, bool en_native_bas_max) const {
        const bool eff = is_effective_on(hold_key_down);
        const bool buster_active = (eff && buster_mult > 1) || en_native_bas_max;
        const bool dmg_reduce_active = (eff && damage_taken_pct < 100);
        return buster_active || dmg_reduce_active;
    }

    std::string build_hud_summary(bool hold_key_down, bool en_native_bas_max) const {
        if (!is_effective_on(hold_key_down)) {
            if (en_native_bas_max) return "原版百倍炮";
            return "";
        }
        std::string s;
        auto append_tag = [&](const std::string& tag) {
            if (!s.empty()) s += ' ';
            s += tag;
        };
        if (buster_mult > 1) {
            append_tag("炮x" + std::to_string(buster_mult));
        } else if (en_native_bas_max) {
            append_tag("原版百倍炮");
        }
        if (damage_taken_pct != 100) {
            append_tag(damage_taken_pct == 0 ? "无伤" : ("伤" + std::to_string(damage_taken_pct) + "%"));
        }
        if (wild_boss_only) {
            append_tag("Boss潜行");
        } else if (encounter_rate_pct != 100) {
            if (encounter_rate_pct == 0) append_tag("不遇敌");
            else if (encounter_rate_pct >= 10000) append_tag("步步遇敌");
            else append_tag("遇" + std::to_string(encounter_rate_pct) + "%");
        }
        if (show_ex_boss == 2) append_tag("EX解锁");
        if (ex_boss_tier >= 0) append_tag("V" + std::to_string(ex_boss_tier + 1));
        if (zenny_rate_pct != 100) append_tag("钱" + std::to_string(zenny_rate_pct) + "%");
        if (run_speed_pct != 100) append_tag("速" + std::to_string(run_speed_pct) + "%");
        if (auto_full_hp) append_tag("战后满血");
        if (always_escape) append_tag("必逃");
        if (custom_gauge_pct != 100) append_tag("槽" + std::to_string(custom_gauge_pct) + "%");
        if (drop_rank_mode == 1) append_tag("必掉");
        else if (drop_rank_mode == 2) append_tag("S必掉");
        return s;
    }
};

struct AssistHudStatus {
    std::string ex_boss_banner;
    uint32_t ex_boss_color = 0xFFFFD874u;
    std::string ranking_warning_banner;
    bool ranking_warning_urgent = false;
    bool en_native_bas_max = false;
};

} // namespace rmexe
