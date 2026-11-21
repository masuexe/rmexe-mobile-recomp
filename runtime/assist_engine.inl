// Included inside namespace rmexe at the bottom of each statically recompiled game C++ file
// (next to pvp_*_engine.inl).
// All state lives strictly outside DojaRuntime::heap_ and sf_app_* so GGPO snapshots and
// single-player Save/Load remain 100% untouched.

namespace {

struct WildBossEntry {
    int32_t req_sina;
    int32_t navi_id;
};

struct AssistLocalState {
    bool initialized = false;
    int32_t prev_scene = -999;
    int32_t prev_rank_flg = 0;
    bool prev_cheat_in_rank_scene = false;
    bool rank_run_tainted = false;
    int32_t run_accum = 0;
    int32_t cus_accum = 0;
    bool in_movechack_passthrough = false;
    std::unordered_map<int32_t, std::string> boss_name_cache;
};

static AssistLocalState g_assist_state{};

static inline bool assist_pvp_blocked(const DojaRuntime& rt) {
    return rt.pvp_match_active() || rt.pvp_self_test();
}

static inline bool assist_effective_active(const DojaRuntime& rt) {
    if (assist_pvp_blocked(rt)) return false;
    return rt.assist_config().is_effective_on(rt.assist_hold_key_down());
}

static inline bool assist_lookup_wild_boss(int32_t map_no, int32_t& req_sina_out, int32_t& navi_id_out) {
#ifdef RMEXE_IS_PON_GAME
    static constexpr WildBossEntry kTable[] = {
        {99, 0}, {2, 0}, {99, 0}, {4, 2}, {99, 0}, {6, 3},
        {3, 1},  {99, 0}, {7, 9}, {99, 0}, {99, 0}, {8, 7},
    };
#else
    static constexpr WildBossEntry kTable[] = {
        {99, 0}, {2, 0}, {99, 0}, {3, 1}, {99, 0}, {5, 3}, {99, 0}, {99, 0},
        {6, 5},  {7, 6}, {99, 0}, {9, 8}, {4, 2},  {99, 0}, {99, 0}, {8, 7},
    };
#endif
    if (map_no < 0 || map_no >= static_cast<int32_t>(sizeof(kTable) / sizeof(kTable[0]))) {
        return false;
    }
    if (kTable[map_no].req_sina >= 99) {
        return false;
    }
    req_sina_out = kTable[map_no].req_sina;
    navi_id_out = kTable[map_no].navi_id;
    return true;
}

static inline int32_t assist_effective_boss_tier(DojaRuntime& rt, int32_t navi_id) {
    const auto& cfg = rt.assist_config();
    if (cfg.ex_boss_tier >= 0) {
        return std::clamp<int32_t>(cfg.ex_boss_tier, 0, 3);
    }
    if (sf_app_CpCanvas_navi_flg <= 0) return 0;
    int32_t len = rt.array_length(sf_app_CpCanvas_navi_flg);
    if (navi_id < 0 || navi_id >= len) return 0;
    return std::clamp<int32_t>(rt.iaload(sf_app_CpCanvas_navi_flg, navi_id), 0, 3);
}

static std::string assist_resolve_boss_name(DojaRuntime& rt, int32_t teki_pt, int32_t navi_id) {
    auto it = g_assist_state.boss_name_cache.find(teki_pt);
    if (it != g_assist_state.boss_name_cache.end()) {
        return it->second;
    }
    std::string fallback = "Boss#" + std::to_string(navi_id);
    if (sf_app_CpCanvas_dat <= 0 || assist_pvp_blocked(rt)) {
        return fallback;
    }
    std::string resolved;
    try {
        Ref s31 = fn_app_CpCanvas_JarGet___I__Ljava_io_InputStream_(rt, 31);
        if (s31 > 0) {
            Ref buf16 = rt.alloc_byte_array(16);
            Slot skip_args[2];
            skip_args[0].r = s31;
            skip_args[1].j = static_cast<int64_t>(teki_pt) * 16LL;
            rt.invoke_external("java/io/InputStream", "skip", "(J)J", skip_args, 2);

            Slot read_args[2];
            read_args[0].r = s31;
            read_args[1].r = buf16;
            rt.invoke_external("java/io/InputStream", "read", "([B)I", read_args, 2);

            Slot close_args[1];
            close_args[0].r = s31;
            rt.invoke_external("java/io/InputStream", "close", "()V", close_args, 1);

            int32_t enemy_id = -1;
            for (int slot = 0; slot < 3; ++slot) {
                int off = 4 + slot * 4;
                uint32_t w = (static_cast<uint32_t>(rt.baload(buf16, off) & 0xFF) << 24) |
                             (static_cast<uint32_t>(rt.baload(buf16, off + 1) & 0xFF) << 16) |
                             (static_cast<uint32_t>(rt.baload(buf16, off + 2) & 0xFF) << 8) |
                             (static_cast<uint32_t>(rt.baload(buf16, off + 3) & 0xFF));
                if (w != 0) {
                    int32_t species = static_cast<int32_t>((w >> 2) & 63u);
                    int32_t tier = static_cast<int32_t>(w & 3u);
                    enemy_id = species * 4 + tier;
                    break;
                }
            }
            if (enemy_id >= 0) {
                Ref s29 = fn_app_CpCanvas_JarGet___I__Ljava_io_InputStream_(rt, 29);
                if (s29 > 0) {
                    Ref nbuf = rt.alloc_byte_array(16);
                    Slot sk2[2];
                    sk2[0].r = s29;
                    sk2[1].j = static_cast<int64_t>(enemy_id) * 16LL;
                    rt.invoke_external("java/io/InputStream", "skip", "(J)J", sk2, 2);

                    Slot rd2[2];
                    rd2[0].r = s29;
                    rd2[1].r = nbuf;
                    rt.invoke_external("java/io/InputStream", "read", "([B)I", rd2, 2);

                    Slot cl2[1];
                    cl2[0].r = s29;
                    rt.invoke_external("java/io/InputStream", "close", "()V", cl2, 1);

                    uint8_t raw_bytes[16];
                    size_t nlen = 0;
                    while (nlen < 16) {
                        int32_t b = rt.baload(nbuf, static_cast<int32_t>(nlen)) & 0xFF;
                        if (b == 0) break;
                        raw_bytes[nlen++] = static_cast<uint8_t>(b);
                    }
                    if (nlen > 0) {
                        resolved = rt.decode_game_bytes_to_utf8(raw_bytes, nlen);
                    }
                }
            }
        }
    } catch (...) {
        resolved.clear();
    }
    if (resolved.empty()) {
        resolved = fallback;
    }
    g_assist_state.boss_name_cache[teki_pt] = resolved;
    return resolved;
}

static inline bool assist_en_native_bas_max_on() {
#ifdef HAS_SF_app_CpCanvas_bas_max_flg
    return sf_app_CpCanvas_bas_max_flg != 0;
#else
    return false;
#endif
}

static inline bool assist_is_ranking_tainted_now(const DojaRuntime& rt) {
#ifdef HAS_SF_app_CpCanvas_rank_flg
    if (assist_pvp_blocked(rt)) return false;
    const auto& cfg = rt.assist_config();
    bool cheat_now = cfg.has_ranking_invalidating_cheat(rt.assist_hold_key_down(), assist_en_native_bas_max_on());
    return cheat_now || g_assist_state.rank_run_tainted;
#else
    (void)rt;
    return false;
#endif
}

} // namespace

static void assist_on_frame_tick(DojaRuntime& rt) {
    if (assist_pvp_blocked(rt)) return;

    const auto& cfg = rt.assist_config();
    const bool eff = assist_effective_active(rt);
    const bool en_nat = assist_en_native_bas_max_on();

    AssistHudStatus st{};
    st.en_native_bas_max = en_nat;

    // 1. EX Boss Radar HUD (when Field scene == 3)
    if (sf_app_CpCanvas_scene == 3 && eff && (cfg.show_ex_boss > 0 || cfg.wild_boss_only)) {
        int32_t req_sina = 99;
        int32_t navi_id = 0;
        if (assist_lookup_wild_boss(sf_app_CpCanvas_map_no, req_sina, navi_id)) {
            int32_t tier = assist_effective_boss_tier(rt, navi_id);
            int32_t teki_pt = 300 + navi_id * 4 + tier;
            std::string bname = assist_resolve_boss_name(rt, teki_pt, navi_id);
            bool unlocked = (cfg.show_ex_boss == 2) || cfg.wild_boss_only || (sf_app_CpCanvas_sina_no >= req_sina);
            if (unlocked) {
                st.ex_boss_banner = "EX: " + bname + (cfg.wild_boss_only ? " [潜行锁定]" : " [可遇]");
                st.ex_boss_color = cfg.wild_boss_only ? 0xFF74F0FFu : 0xFFFFD874u;
            } else {
                st.ex_boss_banner = "EX: " + bname + " (第" + std::to_string(req_sina) + "章解锁)";
                st.ex_boss_color = 0xFFD0A870u;
            }
        } else if (cfg.wild_boss_only) {
            st.ex_boss_banner = "潜行中: 本区无野生Boss";
            st.ex_boss_color = 0xFF74D8FFu;
        } else if (cfg.show_ex_boss > 0) {
            st.ex_boss_banner = "EX雷达: 本区无野生Boss";
            st.ex_boss_color = 0xFF90A4C8u;
        }
    }

    // 2. PoN Virus Busting Ranking Pre-Warning & Run Taint Latch
#ifdef HAS_SF_app_CpCanvas_rank_flg
    bool cheat_now = cfg.has_ranking_invalidating_cheat(rt.assist_hold_key_down(), en_nat);
    int32_t cur_scene = sf_app_CpCanvas_scene;
    int32_t cur_rank_flg = sf_app_CpCanvas_rank_flg;

    if (cur_rank_flg != 0) {
        if (g_assist_state.prev_rank_flg == 0) {
            g_assist_state.rank_run_tainted = cheat_now;
            if (cheat_now) {
                rt.show_ui_toast("警告: 炮强化/减伤已开，本次排行记录无效!", 180);
            }
        } else if (cheat_now && !g_assist_state.rank_run_tainted) {
            g_assist_state.rank_run_tainted = true;
            rt.show_ui_toast("警告: 排行战启用炮强化/减伤，记录已标记无效!", 180);
        }
    } else if (cur_scene != 9 && cur_scene != 1 && cur_scene != 2) {
        g_assist_state.rank_run_tainted = false;
    }

    if (cur_scene == 9 && g_assist_state.prev_scene != 9 && cheat_now) {
        rt.show_ui_toast("提示: 已开炮强化/受伤减免，病毒清扫排行记录将无效!", 180);
    } else if (cur_scene == 9 && cheat_now && !g_assist_state.prev_cheat_in_rank_scene) {
        rt.show_ui_toast("提示: 检测到炮强化/减伤，病毒清扫排行记录将无效!", 180);
    }

    if ((cur_scene == 9 || cur_rank_flg != 0) && (cheat_now || g_assist_state.rank_run_tainted)) {
        st.ranking_warning_urgent = true;
        st.ranking_warning_banner = (cur_rank_flg != 0)
            ? "! 炮强化/减伤已生效: 本次排行记录无效 !"
            : "! 检测到炮强化/减伤: 排行记录将无效 !";
    }

    g_assist_state.prev_scene = cur_scene;
    g_assist_state.prev_rank_flg = cur_rank_flg;
    g_assist_state.prev_cheat_in_rank_scene = (cur_scene == 9 && cheat_now);
#endif

    rt.set_assist_hud_status(st);
}

static bool assist_should_bypass_en_bas_max(DojaRuntime& rt) {
    if (!assist_effective_active(rt)) return false;
    return rt.assist_config().buster_mult > 1;
}

static void assist_adjust_buster_damage(DojaRuntime& rt, Ref attack_obj, int32_t attack_id, int32_t owner_side) {
    if (attack_obj <= 0 || owner_side != 0 || attack_id != 0) return;
    if (!assist_effective_active(rt)) return;
    int32_t mult = rt.assist_config().buster_mult;
    if (mult <= 1) return;
    int32_t cur_pow = rt.field_i32(attack_obj, kFld_Attack_pow);
    int64_t boosted = static_cast<int64_t>(cur_pow) * static_cast<int64_t>(mult);
    rt.field_i32(attack_obj, kFld_Attack_pow) = static_cast<int32_t>(std::clamp<int64_t>(boosted, 1, 99999));
}

static int32_t assist_scale_rock_damage(DojaRuntime& rt, int32_t dmg) {
    if (dmg <= 0 || !assist_effective_active(rt)) return dmg;
    int32_t pct = rt.assist_config().damage_taken_pct;
    if (pct == 100) return dmg;
    if (pct <= 0) return 0;
    int64_t scaled = (static_cast<int64_t>(dmg) * static_cast<int64_t>(pct) + 50LL) / 100LL;
    return static_cast<int32_t>(std::clamp<int64_t>(scaled, 1, 9999));
}

static bool assist_eval_field_encounter(DojaRuntime& rt, int32_t raw_encount, int32_t move_cnt) {
    if (!assist_effective_active(rt)) {
        return raw_encount < move_cnt;
    }
    const auto& cfg = rt.assist_config();
    if (cfg.encounter_rate_pct <= 0) {
        return false;
    }
    if (cfg.wild_boss_only) {
        int32_t req_sina = 99;
        int32_t navi_id = 0;
        if (!assist_lookup_wild_boss(sf_app_CpCanvas_map_no, req_sina, navi_id)) {
            return false; // Wild Boss Only Stealth suppresses normal virus encounters on non-boss maps
        }
    }
    if (cfg.encounter_rate_pct >= 10000) {
        return move_cnt >= 1;
    }
    if (cfg.encounter_rate_pct == 100) {
        return raw_encount < move_cnt;
    }
    int32_t eff_thresh = std::max<int32_t>(
        1,
        static_cast<int32_t>((static_cast<int64_t>(std::max<int32_t>(1, raw_encount)) * 100LL) /
                             static_cast<int64_t>(cfg.encounter_rate_pct))
    );
    return move_cnt > eff_thresh;
}

static void assist_override_wild_boss_encounter(DojaRuntime& rt, int32_t& ghost_loc3, int32_t boss_roll_loc4) {
    if (!assist_effective_active(rt)) return;
    const auto& cfg = rt.assist_config();
    int32_t req_sina = 99;
    int32_t navi_id = 0;
    if (cfg.wild_boss_only && assist_lookup_wild_boss(sf_app_CpCanvas_map_no, req_sina, navi_id)) {
        int32_t tier = assist_effective_boss_tier(rt, navi_id);
        sf_app_CpCanvas_teki_pt = 300 + navi_id * 4 + tier;
        ghost_loc3 = navi_id;
        return;
    }
    if (ghost_loc3 >= 0 && cfg.ex_boss_tier >= 0) {
        int32_t tier = std::clamp<int32_t>(cfg.ex_boss_tier, 0, 3);
        sf_app_CpCanvas_teki_pt = 300 + ghost_loc3 * 4 + tier;
        return;
    }
    if (cfg.show_ex_boss == 2 && ghost_loc3 < 0 && boss_roll_loc4 == 0 &&
        assist_lookup_wild_boss(sf_app_CpCanvas_map_no, req_sina, navi_id)) {
        int32_t tier = assist_effective_boss_tier(rt, navi_id);
        sf_app_CpCanvas_teki_pt = 300 + navi_id * 4 + tier;
        ghost_loc3 = navi_id;
    }
}

static int32_t assist_filter_ene_get(DojaRuntime& rt, int32_t mode, int32_t raw_val) {
    if (mode != 1 || raw_val <= 0 || !assist_effective_active(rt)) return raw_val;
    int32_t pct = rt.assist_config().zenny_rate_pct;
    if (pct == 100) return raw_val;
    int64_t scaled = (static_cast<int64_t>(raw_val) * static_cast<int64_t>(pct)) / 100LL;
    return static_cast<int32_t>(std::clamp<int64_t>(scaled, 0, 9999999));
}

static int32_t assist_scale_zenny_getitem(DojaRuntime& rt, int32_t item_type, int32_t op_mode, int32_t amount) {
    // Only boost positive Zenny pickups outside BattleMain (scene != 1) so battle Zenny from Ene.Get is never double-scaled
    if (item_type != 1 || op_mode != 0 || amount <= 0 || sf_app_CpCanvas_scene == 1) return amount;
    if (!assist_effective_active(rt)) return amount;
    int32_t pct = rt.assist_config().zenny_rate_pct;
    if (pct == 100) return amount;
    int64_t scaled = (static_cast<int64_t>(amount) * static_cast<int64_t>(pct)) / 100LL;
    return static_cast<int32_t>(std::clamp<int64_t>(scaled, 0, 9999999));
}

static int32_t assist_move_check_intercept(DojaRuntime& rt, Ref cp, int32_t a0, int32_t a1) {
    if (g_assist_state.in_movechack_passthrough || !assist_effective_active(rt)) return -1;
    int32_t pct = rt.assist_config().run_speed_pct;
    if (pct == 100) return -1;
    if ((std::abs(a0) + std::abs(a1)) != 1) return -1; // Leave conveyor/special non-unit moves untouched

    int32_t orig_x = sf_app_CpCanvas_map_x;
    int32_t orig_y = sf_app_CpCanvas_map_y;
    int32_t orig_sp = sf_app_CpCanvas_map_sp;

    g_assist_state.run_accum += std::max<int32_t>(1, orig_sp) * pct;
    int32_t total_want = g_assist_state.run_accum / 100;
    g_assist_state.run_accum %= 100;
    if (total_want <= 0) return 0;

    struct MoveGuard {
        int32_t ox, oy, osp;
        ~MoveGuard() {
            sf_app_CpCanvas_map_x = ox;
            sf_app_CpCanvas_map_y = oy;
            sf_app_CpCanvas_map_sp = osp;
            g_assist_state.in_movechack_passthrough = false;
        }
    } guard{orig_x, orig_y, orig_sp};

    g_assist_state.in_movechack_passthrough = true;
    int32_t moved = 0;
    while (moved < total_want) {
        int32_t chunk = std::min<int32_t>(5, total_want - moved);
        sf_app_CpCanvas_map_sp = chunk;
        int32_t step = fn_app_CpCanvas_MoveChack___II__I(rt, cp, a0, a1);
        if (step == 0 && chunk > 1) {
            sf_app_CpCanvas_map_sp = 1;
            step = fn_app_CpCanvas_MoveChack___II__I(rt, cp, a0, a1);
        }
        if (step <= 0) break;
        sf_app_CpCanvas_map_x += a0 * step;
        sf_app_CpCanvas_map_y += a1 * step;
        moved += step;
        if (sf_app_CpCanvas_mes_no > 0) break;
    }
    return moved;
}

static void assist_on_battle_end_hp(DojaRuntime& rt) {
    if (!assist_effective_active(rt)) return;
    if (!rt.assist_config().auto_full_hp) return;
    if (sf_app_CpCanvas_scene == 1 || sf_app_CpCanvas_over_flg != 0) return;
    if (sf_app_Rock_hp > 0 && sf_app_CpCanvas_max_hp > 0) {
        sf_app_Rock_hp = sf_app_CpCanvas_max_hp;
        sf_app_CpCanvas_now_hp = sf_app_CpCanvas_max_hp;
    }
}

static bool assist_should_always_escape(DojaRuntime& rt) {
    if (!assist_effective_active(rt)) return false;
    return rt.assist_config().always_escape;
}

static int32_t assist_custom_gauge_step(DojaRuntime& rt, int32_t base_sp) {
    if (!assist_effective_active(rt)) return base_sp;
    int32_t pct = rt.assist_config().custom_gauge_pct;
    if (pct == 100) return base_sp;
    if (sf_app_CpCanvas_cus_gage <= 0) {
        g_assist_state.cus_accum = 0;
    }
    g_assist_state.cus_accum += std::max<int32_t>(0, base_sp) * pct;
    int32_t step = g_assist_state.cus_accum / 100;
    g_assist_state.cus_accum %= 100;
    return std::clamp<int32_t>(step, 0, 128);
}

static bool assist_should_always_drop_chip(DojaRuntime& rt) {
    if (!assist_effective_active(rt)) return false;
    return rt.assist_config().drop_rank_mode >= 1;
}

static bool assist_should_force_s_rank(DojaRuntime& rt) {
    if (!assist_effective_active(rt)) return false;
    return rt.assist_config().drop_rank_mode == 2;
}

static bool assist_should_block_ranking_record(DojaRuntime& rt) {
#ifdef HAS_SF_app_CpCanvas_rank_flg
    if (!assist_is_ranking_tainted_now(rt)) return false;
    rt.show_ui_toast("因启用炮强化/受伤减免，本次排行成绩无效，未保存/未上传", 180);
    return true;
#else
    (void)rt;
    return false;
#endif
}

static bool assist_should_block_ranking_upload(DojaRuntime& rt) {
#ifdef HAS_SF_app_CpCanvas_rank_flg
    if (!assist_is_ranking_tainted_now(rt)) return false;
    rt.show_ui_toast("因启用炮强化/受伤减免，排行成绩无效，已拦截上传!", 180);
    return true;
#else
    (void)rt;
    return false;
#endif
}

// Saito-style Touchscreen Custom Screen Chip Selection (PoN & LoN, SP & Online PvP)
static void assist_on_battle_tip_touch(DojaRuntime& rt) {
    int gx = -1, gy = -1;
    if (!rt.consume_game_touch_tap(gx, gy)) return;
    if (sf_app_CpCanvas_cas_cnt != 0 || sf_app_CpCanvas_esc_flg != 0 ||
        sf_app_CpCanvas_yes_no_flg != 0 || sf_app_CpCanvas_pa_start != 0) {
        return;
    }

    // 1. Chip tray 5x2 grid: x = 7 + col*16 (w=16), y = 144 + row*24 (h=24)
    if (gx >= 6 && gx < 88 && gy >= 143 && gy < 193) {
        int col = std::clamp((gx - 7) / 16, 0, 4);
        int row = (gy >= 168) ? 1 : 0;
        int slot = row * 5 + col;
        if (slot >= 0 && slot < sf_app_CpCanvas_max_sel) {
            sf_app_CpCanvas_sel_cas_mode = 0;
            sf_app_CpCanvas_sel_cas_tip = slot;
            // Single-tap immediate selection if chip slot is currently selectable (cas_ok[slot] == 1)
            if (sf_app_CpCanvas_cas_ok > 0 && slot < rt.array_length(sf_app_CpCanvas_cas_ok) &&
                rt.iaload(sf_app_CpCanvas_cas_ok, slot) == 1) {
                sf_app_CpCanvas_key |= 1048576; // KEY_SELECT (1 << 20)
            }
        }
        return;
    }

    // 2. OK button: x in [88, 115), y in [155, 179)
    if (gx >= 88 && gx < 115 && gy >= 155 && gy < 179) {
        sf_app_CpCanvas_sel_cas_mode = 1;
        sf_app_CpCanvas_key |= 1048576; // KEY_SELECT
        return;
    }

    // 3. ADD / Shuffle / Arm button: x in [88, 115), y in [179, 202)
    if (gx >= 88 && gx < 115 && gy >= 179 && gy < 202) {
#ifdef RMEXE_IS_PON_GAME
        if (sf_app_CpCanvas_sec_cha != 0 && sf_app_CpCanvas_sel_cnt == 0) {
            sf_app_CpCanvas_sel_cas_mode = 2;
            sf_app_CpCanvas_key |= 1048576;
        }
#else
        if (sf_app_CpCanvas_arm_on != 0 && sf_app_CpCanvas_arm_mode == 0) {
            sf_app_CpCanvas_sel_cas_mode = 2;
            sf_app_CpCanvas_key |= 1048576;
        } else if (sf_app_CpCanvas_sha_cnt != 0) {
            sf_app_CpCanvas_sel_cas_mode = 3;
            sf_app_CpCanvas_key |= 1048576;
        }
#endif
        return;
    }

    // 4. Queued selected chips column: tap to undo last selected chip (KEY_SOFT1 = 1 << 21)
    if (gx >= 94 && gx < 118 && gy >= 64 && gy < 148) {
        if (sf_app_CpCanvas_sel_cnt > 0) {
            sf_app_CpCanvas_key |= 2097152; // KEY_SOFT1
        }
        return;
    }
}

static void assist_run_self_test(DojaRuntime& rt, Ref cp) {
    std::cout << "[AssistSelfTest] Running comprehensive QoL/Assist verification suite...\n";
    auto check = [](bool cond, const char* msg) {
        if (!cond) {
            std::cerr << "[AssistSelfTest FAIL] " << msg << "\n";
            std::exit(1);
        }
    };

    AssistConfig saved_cfg = rt.assist_config();
    auto& cfg = rt.mutable_assist_config();
    cfg.reset_to_vanilla();
    cfg.master_enabled = true;
    cfg.hotkey_mode = 0;

    // Boot a clean map + battle arena so Attack/Rock/Ene/Map objects exist right after SetUp()
    fn_app_CpCanvas_ImgSet2___I__V(rt, cp, 0);
    fn_app_CpCanvas_EnSet_____V(rt, cp);
    fn_app_CpCanvas_MapSet___II__V(rt, cp, 0, 0);
    sf_app_CpCanvas_teki_pt = 0;
    sf_app_CpCanvas_boss_flg = 0;
    sf_app_CpCanvas_ivent_flg = -1;
    fn_app_CpCanvas_BtSet___I__V(rt, cp, 0);

    // 1. Buster Multiplier & EN bas_max_flg de-duplication
    Ref atk_arr = sf_app_CpCanvas_ata;
    check(atk_arr > 0 && rt.array_length(atk_arr) > 0, "sf_app_CpCanvas_ata initialized");
    Ref atk0 = rt.iaload(atk_arr, 0);
    check(atk0 > 0, "atk[0] valid");
    sf_app_CpCanvas_ata_lv = 2;
#ifdef HAS_SF_app_CpCanvas_bas_up
    sf_app_CpCanvas_bas_up = 0;
#endif
#ifdef HAS_SF_app_CpCanvas_bas_max_flg
    sf_app_CpCanvas_bas_max_flg = 0;
#endif
#ifdef HAS_SF_app_CpCanvas_rank_flg
    sf_app_CpCanvas_rank_flg = 0;
#endif
    fn_app_Attack_Set___IIIII__I(rt, atk0, 0, 2, 1, 0, 0);
    int32_t base_pow = rt.field_i32(atk0, kFld_Attack_pow);
    check(base_pow >= 2, "base buster power >= 2 with ata_lv=2");

    cfg.buster_mult = 100;
    fn_app_Attack_Set___IIIII__I(rt, atk0, 0, 2, 1, 0, 0);
    check(rt.field_i32(atk0, kFld_Attack_pow) == base_pow * 100, "buster_mult=100 gives 100x damage");

    cfg.buster_mult = 250;
    fn_app_Attack_Set___IIIII__I(rt, atk0, 0, 2, 1, 0, 0);
    check(rt.field_i32(atk0, kFld_Attack_pow) == base_pow * 250, "custom buster_mult=250 gives 250x damage");

#ifdef HAS_SF_app_CpCanvas_bas_max_flg
    sf_app_CpCanvas_bas_max_flg = 1;
    fn_app_Attack_Set___IIIII__I(rt, atk0, 0, 2, 1, 0, 0);
    check(rt.field_i32(atk0, kFld_Attack_pow) == base_pow * 250, "EN native bas_max_flg does not double-multiply with buster_mult=250");
    sf_app_CpCanvas_bas_max_flg = 0;
#endif
    // Enemy attack (owner_side=1) & chip (attack_id=1) must NOT be scaled
    Ref atk1 = rt.iaload(atk_arr, 1);
    check(atk1 > 0, "atk[1] valid");
    rt.field_i32(atk1, kFld_Attack_pow) = 10;
    fn_app_Attack_Set___IIIII__I(rt, atk1, 0, 2, 1, 0, 1);
    check(rt.field_i32(atk1, kFld_Attack_pow) == 10, "enemy attack not boosted");

    // 2. Incoming damage scaling
    check(sf_app_CpCanvas_rock > 0, "sf_app_CpCanvas_rock initialized");
    sf_app_CpCanvas_max_hp = 500;
    sf_app_Rock_hp = 500;
    sf_app_Rock_muteki_cnt = 0;
    sf_app_Rock_bari = 0;
    sf_app_Rock_zoku = 0;
    sf_app_Rock_pane_id = 0;
    sf_app_CpCanvas_dell_cnt = 0;
    cfg.damage_taken_pct = 40;
    int32_t hit_res = fn_app_Rock_Hit___III__Z(rt, sf_app_CpCanvas_rock, 0, 100, 0);
    check(hit_res == 1 && sf_app_Rock_hp == 460, "damage_taken_pct=40 scales 100 -> 40");
    cfg.damage_taken_pct = 0;
    hit_res = fn_app_Rock_Hit___III__Z(rt, sf_app_CpCanvas_rock, 0, 100, 0);
    check(hit_res == 0 && sf_app_Rock_hp == 460, "damage_taken_pct=0 grants invincibility");

    // 3. Encounter rate, Wild Boss Only Stealth, EX Boss Radar
    cfg.encounter_rate_pct = 0;
    check(!assist_eval_field_encounter(rt, 10, 999), "encounter_rate_pct=0 blocks encounters");
    cfg.encounter_rate_pct = 10000;
    check(assist_eval_field_encounter(rt, 800, 1), "encounter_rate_pct=10000 triggers on step 1");
    cfg.encounter_rate_pct = 100;
    cfg.wild_boss_only = true;
    sf_app_CpCanvas_map_no = 0; // non-boss map in both PoN & LoN
    check(!assist_eval_field_encounter(rt, 10, 999), "wild_boss_only suppresses non-boss map encounters");
    sf_app_CpCanvas_map_no = 1; // boss map (req_sina=2, navi_id=0) in both PoN & LoN
    check(assist_eval_field_encounter(rt, 10, 999), "wild_boss_only allows boss map encounters");
    int32_t ghost_loc3 = -1;
    cfg.ex_boss_tier = 3;
    assist_override_wild_boss_encounter(rt, ghost_loc3, 5);
    check(ghost_loc3 == 0 && sf_app_CpCanvas_teki_pt == 303, "wild_boss_only + ex_boss_tier=3 spawns SP boss");

    sf_app_CpCanvas_scene = 3;
    cfg.show_ex_boss = 1;
    assist_on_frame_tick(rt);
    std::string boss_nm = assist_resolve_boss_name(rt, 303, 0);
    std::cout << "[AssistSelfTest] Resolved boss name (teki_pt=303): " << boss_nm << "\n";
    check(!boss_nm.empty() && boss_nm != "Boss#0", "assist_resolve_boss_name resolves localized boss name");

    // 4. Zenny Multiplier (no double-multiply in BattleMain)
    Ref ene0 = rt.iaload(sf_app_CpCanvas_ene, 0);
    check(ene0 > 0, "ene[0] valid");
    cfg.zenny_rate_pct = 100;
    int32_t z100 = assist_filter_ene_get(rt, 1, 120);
    cfg.zenny_rate_pct = 500;
    int32_t z500 = assist_filter_ene_get(rt, 1, 120);
    check(z100 == 120 && z500 == 600, "zenny_rate_pct=500 scales Ene.Get(1) 5x");

    sf_app_CpCanvas_zenny = 1000;
    sf_app_CpCanvas_scene = 3;
    fn_app_CpCanvas_GetItem___III__I(rt, cp, 1, 0, 100);
    check(sf_app_CpCanvas_zenny == 1500, "GetItem scales Zenny 5x outside BattleMain");
    sf_app_CpCanvas_scene = 1;
    fn_app_CpCanvas_GetItem___III__I(rt, cp, 1, 0, 100);
    check(sf_app_CpCanvas_zenny == 1600, "GetItem does NOT double-scale Zenny inside BattleMain");

    // 5. Overworld Run Speed (preserving map_x/map_y/map_sp)
    cfg.run_speed_pct = 200;
    int32_t saved_mx = sf_app_CpCanvas_map_x;
    int32_t saved_my = sf_app_CpCanvas_map_y;
    int32_t saved_sp = sf_app_CpCanvas_map_sp;
    (void)assist_move_check_intercept(rt, cp, 1, 0);
    check(sf_app_CpCanvas_map_x == saved_mx && sf_app_CpCanvas_map_y == saved_my &&
          sf_app_CpCanvas_map_sp == saved_sp && !g_assist_state.in_movechack_passthrough,
          "MoveChack intercept restores map_x/map_y/map_sp and passthrough flag");

    // 6. Auto Full HP, Always Escape, Custom Gauge Speed, Drop & S-Rank
    cfg.auto_full_hp = true;
    sf_app_CpCanvas_over_flg = 0;
    sf_app_CpCanvas_max_hp = 500;
    sf_app_Rock_hp = 123;
    sf_app_CpCanvas_now_hp = 123;
    sf_app_CpCanvas_scene = 1;
    assist_on_battle_end_hp(rt);
    check(sf_app_Rock_hp == 123, "auto_full_hp does NOT heal mid-wave when scene==1");
    sf_app_CpCanvas_scene = 3;
    assist_on_battle_end_hp(rt);
    check(sf_app_Rock_hp == 500 && sf_app_CpCanvas_now_hp == 500, "auto_full_hp heals when battle ends");

    cfg.always_escape = true;
    check(fn_app_CpCanvas_EscCh_____Z(rt, cp) == 1, "always_escape forces EscCh() == 1");

    cfg.custom_gauge_pct = 500;
    sf_app_CpCanvas_cus_gage = 10;
    g_assist_state.cus_accum = 0;
    check(assist_custom_gauge_step(rt, 1) == 5, "custom_gauge_pct=500 scales step 1 -> 5");

    cfg.drop_rank_mode = 2;
    check(assist_should_always_drop_chip(rt), "drop_rank_mode=2 forces chip drop");
    check(fn_app_CpCanvas_BasLv_____I(rt, cp) == 11, "drop_rank_mode=2 forces S-Rank (11)");

    // 7. PoN Ranking pre-warning & taint latch
#ifdef HAS_SF_app_CpCanvas_rank_flg
    g_assist_state.rank_run_tainted = false;
    g_assist_state.prev_scene = 3;
    g_assist_state.prev_rank_flg = 0;
    cfg.buster_mult = 1;
    cfg.damage_taken_pct = 100;
    cfg.zenny_rate_pct = 500;
    cfg.run_speed_pct = 200;
    sf_app_CpCanvas_scene = 9;
    sf_app_CpCanvas_rank_flg = 0;
    assist_on_frame_tick(rt);
    check(!assist_should_block_ranking_record(rt) && !assist_should_block_ranking_upload(rt),
          "non-combat QoL cheats do not taint ranking");

    cfg.buster_mult = 100;
    sf_app_CpCanvas_rank_flg = 1;
    assist_on_frame_tick(rt);
    check(assist_is_ranking_tainted_now(rt), "buster_mult=100 taints active ranking run");
    cfg.buster_mult = 1; // disabling mid-run must stay tainted!
    assist_on_frame_tick(rt);
    check(assist_should_block_ranking_record(rt) && fn_app_CpCanvas_RankUp_____I(rt) == -999,
          "ranking taint stays latched and RankUp() returns -999");
    sf_app_CpCanvas_rank_flg = 0;
    sf_app_CpCanvas_scene = 3;
    assist_on_frame_tick(rt);
    check(!g_assist_state.rank_run_tainted, "leaving ranking clears taint latch");
#endif

    // 8. Complete PvP Isolation
    cfg.buster_mult = 999;
    cfg.damage_taken_pct = 0;
    cfg.encounter_rate_pct = 0;
    cfg.wild_boss_only = true;
    cfg.zenny_rate_pct = 1000;
    cfg.run_speed_pct = 500;
    cfg.auto_full_hp = true;
    cfg.always_escape = true;
    cfg.custom_gauge_pct = 1000;
    cfg.drop_rank_mode = 2;
    rt.set_pvp_match_active(true);

    check(assist_scale_rock_damage(rt, 100) == 100, "PvP isolates Rock damage");
    check(!assist_should_always_escape(rt), "PvP isolates EscCh");
    check(!assist_should_always_drop_chip(rt), "PvP isolates chip drop");
    check(!assist_should_force_s_rank(rt), "PvP isolates S-Rank");
    check(assist_custom_gauge_step(rt, 1) == 1, "PvP isolates custom gauge");
    check(assist_filter_ene_get(rt, 1, 100) == 100, "PvP isolates Ene.Get zenny");
    check(assist_scale_zenny_getitem(rt, 1, 0, 100) == 100, "PvP isolates GetItem zenny");
    check(assist_move_check_intercept(rt, cp, 1, 0) == -1, "PvP isolates MoveChack");
    fn_app_Attack_Set___IIIII__I(rt, atk0, 0, 2, 1, 0, 0);
    check(rt.field_i32(atk0, kFld_Attack_pow) == base_pow, "PvP isolates Buster power");

    rt.set_pvp_match_active(false);
    rt.mutable_assist_config() = saved_cfg;
    std::cout << "[AssistSelfTest] ALL CHECKS PASSED!\n";
    rt.shutdown();
    std::exit(0);
}
