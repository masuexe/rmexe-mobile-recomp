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

struct AutosaveWorldLoc {
    int32_t world_scene = 0; // 3 = FieldMain, 5 = Machi
    int32_t area_id = -1;    // map_no (if 3) or machi_no (if 5)
    bool valid() const { return (world_scene == 3 || world_scene == 5) && area_id >= 0; }
    bool operator==(const AutosaveWorldLoc& o) const { return world_scene == o.world_scene && area_id == o.area_id; }
    bool operator!=(const AutosaveWorldLoc& o) const { return !(*this == o); }
};

struct AutosaveLocalState {
    bool loc_tables_synced = false;
    bool suppress_next_settle = true;
    AutosaveWorldLoc last_settled{};
};

static AutosaveLocalState g_autosave_state{};

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

static void autosave_sync_name_tables_if_ready(DojaRuntime& rt) {
    if (g_autosave_state.loc_tables_synced) return;
    if (sf_app_CpCanvas_map_str <= 0 || sf_app_CpCanvas_m_name <= 0) return;
    if (rt.get(sf_app_CpCanvas_map_str).kind != ObjKind::RefArray ||
        rt.get(sf_app_CpCanvas_m_name).kind != ObjKind::RefArray) {
        return;
    }
    auto extract_table = [&](Ref arr_ref, std::vector<std::string>& out, bool& any_nonempty) {
        int32_t n = rt.array_length(arr_ref);
        out.clear();
        out.reserve(static_cast<size_t>(std::max<int32_t>(0, n)));
        for (int32_t i = 0; i < n; ++i) {
            Ref sref = rt.iaload(arr_ref, i);
            if (sref > 0 && static_cast<size_t>(sref) < rt.heap_size() && rt.get(sref).kind == ObjKind::String) {
                const std::string& s = rt.get(sref).str;
                if (!s.empty()) any_nonempty = true;
                out.push_back(s);
            } else {
                out.emplace_back();
            }
        }
    };
    bool any_nonempty = false;
    std::vector<std::string> map_names;
    std::vector<std::string> machi_names;
    extract_table(sf_app_CpCanvas_map_str, map_names, any_nonempty);
    extract_table(sf_app_CpCanvas_m_name, machi_names, any_nonempty);
    if (!any_nonempty) return;
    g_autosave_state.loc_tables_synced = true;
    rt.set_location_name_tables(std::move(map_names), std::move(machi_names));
}

static bool autosave_is_safe_playable_world(const DojaRuntime& rt) {
    if (assist_pvp_blocked(rt)) return false;
    const int32_t sc = sf_app_CpCanvas_scene;
    if (sc != 3 && sc != 5) return false;
    if (sf_app_CpCanvas_over_flg != 0 || sf_app_CpCanvas_max_hp <= 0 || sf_app_CpCanvas_now_hp <= 0) return false;
    if (sf_app_CpCanvas_sina_flg < 0) return false;
    if (sf_app_CpCanvas_ivent_flg >= 0 || sf_app_CpCanvas_mes_no > 0 ||
        sf_app_CpCanvas_mes_flg != 0 || sf_app_CpCanvas_eff_cnt > 0) {
        return false;
    }
    if (sc == 3) {
        // Note: FieldMain intentionally leaves machi_flg == 1 after Jack-In, so never gate FieldMain on machi_flg.
        if (sf_app_CpCanvas_warp_cnt != 0 || sf_app_CpCanvas_machi_cnt != 0 || sf_app_CpCanvas_ivent_end != 0) {
            return false;
        }
    } else {
        if (sf_app_CpCanvas_machi_move != 0 || sf_app_CpCanvas_machi_cnt != 0 || sf_app_CpCanvas_machi_flg != 0) {
            return false;
        }
    }
    return true;
}

static bool autosave_perform_now(DojaRuntime& rt, Ref cp) {
    if (cp <= 0 || !autosave_is_safe_playable_world(rt)) return false;
    const int32_t cur_scene = sf_app_CpCanvas_scene; // 3 or 5
    const int32_t saved_back_menu = sf_app_CpCanvas_back_menu;
    const int32_t saved_now_bgm = sf_app_CpCanvas_now_bgm;

    struct SaveFieldGuard {
        DojaRuntime& rt;
        int32_t back_menu;
        int32_t now_bgm;
        ~SaveFieldGuard() {
            sf_app_CpCanvas_back_menu = back_menu;
            sf_app_CpCanvas_now_bgm = now_bgm;
            rt.abort_autosave_capture();
        }
    } guard{rt, saved_back_menu, saved_now_bgm};

    sf_app_CpCanvas_back_menu = cur_scene;
    sf_app_CpCanvas_now_bgm = (cur_scene == 3) ? sf_app_CpCanvas_map_bgm : 3;

    rt.begin_autosave_capture();
#ifdef RMEXE_IS_PON_GAME
    fn_app_CpCanvas_Save___I__V(rt, cp, 0);
#else
    (void)fn_app_CpCanvas_Save___I__I(rt, cp, 0);
#endif

    SaveSlotSummary meta{};
    meta.exists = true;
    meta.is_autosave = true;
    meta.sina_no = sf_app_CpCanvas_sina_no;
    meta.back_menu = cur_scene;
    meta.map_no = sf_app_CpCanvas_map_no;
    meta.machi_no = sf_app_CpCanvas_machi_no;
    meta.now_hp = sf_app_CpCanvas_now_hp;
    meta.max_hp = sf_app_CpCanvas_max_hp;
    meta.zenny = sf_app_CpCanvas_zenny;
    return rt.commit_autosave_capture(meta);
}

static bool autosave_execute_load_slot(DojaRuntime& rt, Ref cp, int slot) {
    if (assist_pvp_blocked(rt) || cp <= 0) return false;
    if (slot != 1 && slot != 2) return false;
    if (slot == 2) {
        if (!rt.arm_oneshot_autosave_read()) {
            rt.show_ui_toast("暂无有效的自动存档", 90);
            return false;
        }
    } else {
        if (!rt.has_manual_save()) {
            rt.show_ui_toast("暂无有效的手动存档", 90);
            return false;
        }
    }

    struct ReadDisarmGuard {
        DojaRuntime& rt;
        ~ReadDisarmGuard() { rt.disarm_oneshot_autosave_read(); }
    } disarm_guard{rt};

    fn_app_CpCanvas_Audio___II__V(rt, 0, 0);
    sf_app_CpCanvas_over_flg = 0;
    sf_app_CpCanvas_over_cnt = 0;
    sf_app_CpCanvas_mes_flg = 0;
    sf_app_CpCanvas_mes_no = 0;
    sf_app_CpCanvas_yes_no_flg = 0;
    sf_app_CpCanvas_ivent_flg = -1;
    sf_app_CpCanvas_ivent_end = 0;
    sf_app_CpCanvas_warp_cnt = 0;
    sf_app_CpCanvas_machi_cnt = 0;
    sf_app_CpCanvas_machi_move = 0;
    sf_app_CpCanvas_machi_flg = 0;
    sf_app_CpCanvas_eff_cnt = 0;
    sf_app_CpCanvas_cas_cnt = 0;
    sf_app_CpCanvas_esc_flg = 0;
    sf_app_CpCanvas_pa_start = 0;
    sf_app_CpCanvas_menu_cnt = 0;
    sf_app_CpCanvas_menu_sel = 0;
    sf_app_CpCanvas_option_flg = 0;
    sf_app_CpCanvas_boss_flg = 0;
    sf_app_CpCanvas_win_flg = 0;
    sf_app_CpCanvas_dell_cnt = 0;
    sf_app_CpCanvas_mode = 0;
    sf_app_CpCanvas_mode2 = 0;
    sf_app_CpCanvas_key = 0;
#ifdef HAS_SF_app_CpCanvas_rank_flg
    sf_app_CpCanvas_rank_flg = 0;
#endif

#ifdef RMEXE_IS_PON_GAME
    fn_app_CpCanvas_Load___I__V(rt, cp, 0);
#else
    (void)fn_app_CpCanvas_Load___I__I(rt, cp, 0);
#endif
    rt.disarm_oneshot_autosave_read();

    fn_app_CpCanvas_MachiSet___II__V(rt, cp, sf_app_CpCanvas_machi_no, 0);
    sf_app_CpCanvas_sort_flg = -1;
    fn_app_CpCanvas_ListSort___I__V(rt, cp, 0);
    fn_app_CpCanvas_SortSkill___I__V(rt, 0);
    sf_app_CpCanvas_scene = (sf_app_CpCanvas_back_menu == 3 || sf_app_CpCanvas_back_menu == 5)
        ? sf_app_CpCanvas_back_menu
        : 5;
    if (sf_app_CpCanvas_scene == 5) {
        fn_app_CpCanvas_MachiSet___II__V(rt, cp, sf_app_CpCanvas_machi_no, 1);
    }
    const int32_t saved_x = sf_app_CpCanvas_map_x;
    const int32_t saved_y = sf_app_CpCanvas_map_y;
    sf_app_CpCanvas_mode = 0;
    fn_app_CpCanvas_MapSet___II__V(rt, cp, sf_app_CpCanvas_map_no, 0);
    sf_app_CpCanvas_map_x = saved_x;
    sf_app_CpCanvas_map_y = saved_y;
    sf_app_CpCanvas_now_bgm = (sf_app_CpCanvas_scene == 3) ? sf_app_CpCanvas_map_bgm : 3;
    if (sf_app_CpCanvas_sina_flg >= 0) {
        fn_app_CpCanvas_Audio___II__V(rt, 1, sf_app_CpCanvas_now_bgm);
    } else {
        fn_app_CpCanvas_IventRead___II__V(rt, cp, 1, 0);
        sf_app_CpCanvas_now_bgm = -1;
        sf_app_CpCanvas_sina_flg = 0;
    }

    rt.reset_interpolator();
    g_autosave_state.suppress_next_settle = true;
    g_autosave_state.last_settled = {};
    rt.show_ui_toast(slot == 2 ? "已读取自动存档" : "已读取手动存档", 90);
    return true;
}

static void autosave_tick(DojaRuntime& rt, Ref cp) {
    if (sf_app_CpCanvas_scene <= 0) {
        g_autosave_state.suppress_next_settle = true;
        g_autosave_state.last_settled = {};
        return;
    }
    if (!autosave_is_safe_playable_world(rt)) {
        return;
    }
    const int32_t sc = sf_app_CpCanvas_scene;
    AutosaveWorldLoc cur{sc, (sc == 3) ? sf_app_CpCanvas_map_no : sf_app_CpCanvas_machi_no};
    if (g_autosave_state.suppress_next_settle || !g_autosave_state.last_settled.valid()) {
        g_autosave_state.last_settled = cur;
        g_autosave_state.suppress_next_settle = false;
        return;
    }
    if (cur != g_autosave_state.last_settled) {
        g_autosave_state.last_settled = cur;
        if (rt.auto_save_enabled()) {
            autosave_perform_now(rt, cp);
        }
    }
}

} // namespace

static int32_t assist_on_title_continue(DojaRuntime& rt, Ref /*cp*/) {
    autosave_sync_name_tables_if_ready(rt);
    if (assist_pvp_blocked(rt) || rt.assist_self_test() || rt.autosave_self_test() || rt.is_headless()) {
        g_autosave_state.suppress_next_settle = true;
        g_autosave_state.last_settled = {};
        return 1;
    }
    rt.reload_autosave_from_disk();
    SaveSlotSummary auto_sum = rt.get_auto_save_summary();
    SaveSlotSummary manual_sum = rt.get_manual_save_summary();
    if (!auto_sum.exists) {
        g_autosave_state.suppress_next_settle = true;
        g_autosave_state.last_settled = {};
        return 1;
    }
    if (!manual_sum.exists && auto_sum.exists) {
        rt.arm_oneshot_autosave_read();
        g_autosave_state.suppress_next_settle = true;
        g_autosave_state.last_settled = {};
        return 2;
    }
    int choice = rt.run_save_slot_chooser_modal(auto_sum, manual_sum);
    if (choice == 0) {
        return 0;
    }
    if (choice == 2) {
        rt.arm_oneshot_autosave_read();
    }
    g_autosave_state.suppress_next_settle = true;
    g_autosave_state.last_settled = {};
    return choice;
}

static void assist_on_frame_tick(DojaRuntime& rt, Ref cp) {
    if (assist_pvp_blocked(rt)) return;

    autosave_sync_name_tables_if_ready(rt);
    int pending_load = rt.consume_pending_save_load_request();
    if (pending_load != 0) {
        autosave_execute_load_slot(rt, cp, pending_load);
    }
    autosave_tick(rt, cp);

    const auto& cfg = rt.assist_config();
    const bool eff = assist_effective_active(rt);
    const bool en_nat = assist_en_native_bas_max_on();

    AssistHudStatus st{};
    st.en_native_bas_max = en_nat;

    // 0. Lock encounter step counter to 0 while encounters are suppressed so disabling assist later never triggers an instant encounter
    if (sf_app_CpCanvas_scene == 3 && eff) {
        bool suppress_enc = (cfg.encounter_rate_pct <= 0);
        if (!suppress_enc && cfg.wild_boss_only) {
            int32_t req_sina = 99;
            int32_t navi_id = 0;
            suppress_enc = !assist_lookup_wild_boss(sf_app_CpCanvas_map_no, req_sina, navi_id);
        }
        if (suppress_enc) {
            sf_app_CpCanvas_move_cnt = 0;
        }
    }

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
        sf_app_CpCanvas_move_cnt = 0;
        return false;
    }
    if (cfg.wild_boss_only) {
        int32_t req_sina = 99;
        int32_t navi_id = 0;
        if (!assist_lookup_wild_boss(sf_app_CpCanvas_map_no, req_sina, navi_id)) {
            sf_app_CpCanvas_move_cnt = 0;
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

        // Prevent high run_speed_pct from overshooting 13x13px warp trigger boxes or floor event tiles
        if (sf_app_CpCanvas_m_data > 0) {
            int32_t mx = sf_app_CpCanvas_map_x;
            int32_t my = sf_app_CpCanvas_map_y;
            if (mx >= 0 && (my + 5) >= 0) {
                int32_t tidx = ((my + 5) / 30) * 24 + (mx / 30);
                if (tidx >= 0 && tidx < rt.array_length(sf_app_CpCanvas_m_data)) {
                    int32_t tile = rt.iaload(sf_app_CpCanvas_m_data, tidx);
                    if ((tile & 128) != 0 && sf_app_CpCanvas_warp_cnt == 0) {
                        break;
                    }
                    if (tile > 6 && tile < 11) {
                        if (sf_app_CpCanvas_warp_flg == 0) {
                            int32_t rx = mx % 30;
                            int32_t ry = (my + 8) % 30;
                            if (rx > 8 && rx < 22 && ry > 8 && ry < 22) {
                                break;
                            }
                        }
                    } else if (sf_app_CpCanvas_warp_flg != 0 && sf_app_CpCanvas_ivent_end == 0) {
                        // Let FieldMain clear warp_flg on stepping off a warp tile so adjacent warps work
                        break;
                    }
                }
            }
        }
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

    // 4. Tap anywhere else on the Custom Screen (queued chips, battlefield, art box, etc.)
    //    to undo the last selected chip (KEY_SOFT1 = 1 << 21) when at least 1 chip is queued.
    if (sf_app_CpCanvas_sel_cnt > 0) {
        sf_app_CpCanvas_key |= 2097152; // KEY_SOFT1
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
    sf_app_CpCanvas_move_cnt = 999;
    check(!assist_eval_field_encounter(rt, 10, sf_app_CpCanvas_move_cnt) && sf_app_CpCanvas_move_cnt == 0,
          "encounter_rate_pct=0 blocks encounters and locks move_cnt to 0");
    cfg.encounter_rate_pct = 100;
    check(!assist_eval_field_encounter(rt, 10, sf_app_CpCanvas_move_cnt),
          "disabling no-encounter does not immediately trigger encounter");
    cfg.encounter_rate_pct = 10000;
    check(assist_eval_field_encounter(rt, 800, 1), "encounter_rate_pct=10000 triggers on step 1");
    cfg.encounter_rate_pct = 100;
    cfg.wild_boss_only = true;
    sf_app_CpCanvas_map_no = 0; // non-boss map in both PoN & LoN
    sf_app_CpCanvas_move_cnt = 999;
    check(!assist_eval_field_encounter(rt, 10, sf_app_CpCanvas_move_cnt) && sf_app_CpCanvas_move_cnt == 0,
          "wild_boss_only suppresses non-boss map encounters and locks move_cnt to 0");
    sf_app_CpCanvas_map_no = 1; // boss map (req_sina=2, navi_id=0) in both PoN & LoN
    check(assist_eval_field_encounter(rt, 10, 999), "wild_boss_only allows boss map encounters");
    int32_t ghost_loc3 = -1;
    cfg.ex_boss_tier = 3;
    assist_override_wild_boss_encounter(rt, ghost_loc3, 5);
    check(ghost_loc3 == 0 && sf_app_CpCanvas_teki_pt == 303, "wild_boss_only + ex_boss_tier=3 spawns SP boss");

    sf_app_CpCanvas_scene = 3;
    cfg.show_ex_boss = 1;
    assist_on_frame_tick(rt, cp);
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

    // 5. Overworld Run Speed (preserving map_x/map_y/map_sp & not overshooting warp tiles)
    cfg.run_speed_pct = 200;
    int32_t saved_mx = sf_app_CpCanvas_map_x;
    int32_t saved_my = sf_app_CpCanvas_map_y;
    int32_t saved_sp = sf_app_CpCanvas_map_sp;
    (void)assist_move_check_intercept(rt, cp, 1, 0);
    check(sf_app_CpCanvas_map_x == saved_mx && sf_app_CpCanvas_map_y == saved_my &&
          sf_app_CpCanvas_map_sp == saved_sp && !g_assist_state.in_movechack_passthrough,
          "MoveChack intercept restores map_x/map_y/map_sp and passthrough flag");

    if (sf_app_CpCanvas_m_data > 0 && rt.array_length(sf_app_CpCanvas_m_data) > 2 * 24 + 3) {
        int32_t t1 = rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 1);
        int32_t t2 = rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 2);
        int32_t t3 = rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 3);
        int32_t saved_wf = sf_app_CpCanvas_warp_flg;
        rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 1) = 1;
        rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 2) = 7; // warp tile
        rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 3) = 1;
        sf_app_CpCanvas_warp_flg = 0;
        sf_app_CpCanvas_map_x = 2 * 30 + 4; // 64 (outside 9..21 trigger window)
        sf_app_CpCanvas_map_y = 2 * 30 + 7; // 67 -> (67+8)%30 == 15 (center Y)
        sf_app_CpCanvas_map_sp = 5;
        g_assist_state.run_accum = 0;
        cfg.run_speed_pct = 500; // would leap 25px (64 -> 89) without warp-stop check
        int32_t warp_moved = assist_move_check_intercept(rt, cp, 1, 0);
        int32_t end_rx = (sf_app_CpCanvas_map_x + warp_moved) % 30;
        check(warp_moved > 0 && warp_moved < 25 && end_rx > 8 && end_rx < 22,
              "run_speed_pct=500 stops inside warp trigger zone instead of overshooting");
        rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 1) = t1;
        rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 2) = t2;
        rt.iaload(sf_app_CpCanvas_m_data, 2 * 24 + 3) = t3;
        sf_app_CpCanvas_warp_flg = saved_wf;
        sf_app_CpCanvas_map_x = saved_mx;
        sf_app_CpCanvas_map_y = saved_my;
        sf_app_CpCanvas_map_sp = saved_sp;
    }

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
    assist_on_frame_tick(rt, cp);
    check(!assist_should_block_ranking_record(rt) && !assist_should_block_ranking_upload(rt),
          "non-combat QoL cheats do not taint ranking");

    cfg.buster_mult = 100;
    sf_app_CpCanvas_rank_flg = 1;
    assist_on_frame_tick(rt, cp);
    check(assist_is_ranking_tainted_now(rt), "buster_mult=100 taints active ranking run");
    cfg.buster_mult = 1; // disabling mid-run must stay tainted!
    assist_on_frame_tick(rt, cp);
    check(assist_should_block_ranking_record(rt) && fn_app_CpCanvas_RankUp_____I(rt) == -999,
          "ranking taint stays latched and RankUp() returns -999");
    sf_app_CpCanvas_rank_flg = 0;
    sf_app_CpCanvas_scene = 3;
    assist_on_frame_tick(rt, cp);
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

static void autosave_run_self_test(DojaRuntime& rt, Ref cp) {
    std::cout << "[AutosaveSelfTest] Running dual-slot Auto-Save / Manual-Save verification suite...\n";
    auto check = [](bool cond, const char* msg) {
        if (!cond) {
            std::cerr << "[AutosaveSelfTest FAIL] " << msg << "\n";
            std::exit(1);
        }
    };

    auto native_save = [&]() {
#ifdef RMEXE_IS_PON_GAME
        fn_app_CpCanvas_Save___I__V(rt, cp, 0);
#else
        (void)fn_app_CpCanvas_Save___I__I(rt, cp, 0);
#endif
    };

    auto clear_transient_gates = [&]() {
        sf_app_CpCanvas_over_flg = 0;
        sf_app_CpCanvas_over_cnt = 0;
        sf_app_CpCanvas_sina_flg = 1;
        sf_app_CpCanvas_sina_no = 2;
        sf_app_CpCanvas_ivent_flg = -1;
        sf_app_CpCanvas_ivent_end = 0;
        sf_app_CpCanvas_mes_no = 0;
        sf_app_CpCanvas_mes_flg = 0;
        sf_app_CpCanvas_eff_cnt = 0;
        sf_app_CpCanvas_warp_cnt = 0;
        sf_app_CpCanvas_machi_cnt = 0;
        sf_app_CpCanvas_machi_move = 0;
        sf_app_CpCanvas_machi_flg = 0;
    };

    fn_app_CpCanvas_ImgSet2___I__V(rt, cp, 0);
    fn_app_CpCanvas_EnSet_____V(rt, cp);
    fn_app_CpCanvas_MapSet___II__V(rt, cp, 0, 0);
    autosave_sync_name_tables_if_ready(rt);

    g_autosave_state.suppress_next_settle = true;
    g_autosave_state.last_settled = {};
    rt.set_auto_save_enabled(true);
    rt.set_pvp_match_active(false);

    // 1. Manual Save baseline in Machi 0
    clear_transient_gates();
    sf_app_CpCanvas_scene = 5;
    sf_app_CpCanvas_back_menu = 5;
    sf_app_CpCanvas_machi_no = 0;
    sf_app_CpCanvas_zenny = 1234;
    sf_app_CpCanvas_max_hp = 2000;
    sf_app_CpCanvas_now_hp = 2000;
    native_save();
    check(rt.has_manual_save(), "manual save exists after native Save(0)");
    check(!rt.has_auto_save(), "auto save absent initially");
    check(rt.get_manual_save_summary().zenny == 1234 && rt.get_manual_save_summary().machi_no == 0,
          "manual save captures machi_no=0 and zenny=1234");

    // 2. Initial settle tick must NOT trigger auto-save
    assist_on_frame_tick(rt, cp);
    check(!rt.has_auto_save(), "initial world settle does not create auto save");

    // 3. Real World location transition (Machi 0 -> Machi 1)
    sf_app_CpCanvas_machi_no = 1;
    sf_app_CpCanvas_zenny = 2500;
    assist_on_frame_tick(rt, cp);
    check(rt.has_auto_save(), "Machi 0 -> Machi 1 creates auto save");
    {
        auto as = rt.get_auto_save_summary();
        auto ms = rt.get_manual_save_summary();
        check(as.back_menu == 5 && as.machi_no == 1 && as.zenny == 2500,
              "auto save records back_menu=5, machi_no=1, zenny=2500");
        check(ms.machi_no == 0 && ms.zenny == 1234,
              "manual save stays isolated after real-world auto save");
    }

    // 4. Jack-In (Machi 1 -> Cyberworld Map 0) with stale back_menu=5 and machi_flg=1
    sf_app_CpCanvas_scene = 3;
    sf_app_CpCanvas_map_no = 0;
    sf_app_CpCanvas_machi_flg = 1;
    sf_app_CpCanvas_back_menu = 5;
    sf_app_CpCanvas_now_bgm = 3;
    sf_app_CpCanvas_map_bgm = 7;
    sf_app_CpCanvas_zenny = 3333;
    sf_app_CpCanvas_now_hp = 1800;
    assist_on_frame_tick(rt, cp);
    {
        auto as = rt.get_auto_save_summary();
        auto ms = rt.get_manual_save_summary();
        check(as.back_menu == 3 && as.map_no == 0 && as.zenny == 3333 && as.now_hp == 1800,
              "Jack-In auto save syncs back_menu=3, map_no=0, zenny=3333, now_hp=1800");
        check(sf_app_CpCanvas_back_menu == 5 && sf_app_CpCanvas_now_bgm == 3,
              "Jack-In auto save restores live back_menu and now_bgm after Save(0)");
        check(ms.machi_no == 0 && ms.zenny == 1234,
              "manual save stays untouched after Jack-In auto save");
    }

    // 5. Normal battle victory returning to same Cyberworld map must NOT auto-save
    sf_app_CpCanvas_scene = 1;
    sf_app_CpCanvas_zenny = 4444;
    assist_on_frame_tick(rt, cp);
    sf_app_CpCanvas_scene = 3;
    sf_app_CpCanvas_map_no = 0;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().zenny == 3333,
          "normal battle victory returning to same map does NOT trigger auto save");

    // 6. Cyberworld warp (Map 0 -> Map 1): blocked mid-fade, saved once settled
    sf_app_CpCanvas_map_no = 1;
    sf_app_CpCanvas_warp_cnt = 5;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 0 && rt.get_auto_save_summary().zenny == 3333,
          "warp transition mid-fade (warp_cnt>0) waits before auto saving");
    sf_app_CpCanvas_warp_cnt = 0;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 1 && rt.get_auto_save_summary().zenny == 4444,
          "settled Cyberworld warp triggers auto save");

    // 7. Unsafe gates (ivent_flg / mes_no / over_flg) must block auto-save
    sf_app_CpCanvas_map_no = 2;
    sf_app_CpCanvas_zenny = 5555;
    sf_app_CpCanvas_ivent_flg = 1;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 1, "ivent_flg>=0 blocks auto save");
    sf_app_CpCanvas_ivent_flg = -1;
    sf_app_CpCanvas_mes_no = 1;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 1, "mes_no>0 blocks auto save");
    sf_app_CpCanvas_mes_no = 0;
    sf_app_CpCanvas_over_flg = 1;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 1, "over_flg!=0 blocks auto save");
    sf_app_CpCanvas_over_flg = 0;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 2 && rt.get_auto_save_summary().zenny == 5555,
          "clearing unsafe gates lets map_no=2 settle and auto save");

    // 8. Disabled toggle suppresses transition save without retroactive save on re-enable
    rt.set_auto_save_enabled(false);
    sf_app_CpCanvas_map_no = 3;
    sf_app_CpCanvas_zenny = 6666;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 2 && rt.get_auto_save_summary().zenny == 5555,
          "auto_save_enabled=false suppresses auto save");
    rt.set_auto_save_enabled(true);
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 2,
          "re-enabling auto_save_enabled does not retroactively auto save on same map");
    sf_app_CpCanvas_map_no = 4;
    sf_app_CpCanvas_zenny = 7777;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 4 && rt.get_auto_save_summary().zenny == 7777,
          "next map transition after re-enable saves normally");

    // 9. PvP isolation blocks both auto save and slot load
    rt.set_pvp_match_active(true);
    sf_app_CpCanvas_map_no = 5;
    sf_app_CpCanvas_zenny = 8888;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().map_no == 4 && rt.get_auto_save_summary().zenny == 7777,
          "PvP blocks auto save");
    check(!autosave_execute_load_slot(rt, cp, 2), "PvP blocks autosave load");
    rt.set_pvp_match_active(false);

    // 10. Load Auto Save from corrupted mid-battle state without touching Manual Save
    sf_app_CpCanvas_scene = 1;
    sf_app_CpCanvas_zenny = 99999;
    sf_app_CpCanvas_over_flg = 1;
    sf_app_CpCanvas_map_no = 0;
    check(autosave_execute_load_slot(rt, cp, 2), "load auto save succeeds");
    check(sf_app_CpCanvas_scene == 3 && sf_app_CpCanvas_map_no == 4 &&
          sf_app_CpCanvas_zenny == 7777 && sf_app_CpCanvas_over_flg == 0,
          "load auto save restores Cyberworld map_no=4, zenny=7777, over_flg=0");
    check(rt.get_manual_save_summary().machi_no == 0 && rt.get_manual_save_summary().zenny == 1234,
          "loading auto save leaves manual save untouched");

    // 11. Load Manual Save without touching Auto Save
    check(autosave_execute_load_slot(rt, cp, 1), "load manual save succeeds");
    check(sf_app_CpCanvas_scene == 5 && sf_app_CpCanvas_machi_no == 0 && sf_app_CpCanvas_zenny == 1234,
          "load manual save restores Real World machi_no=0, zenny=1234");
    check(rt.get_auto_save_summary().back_menu == 3 && rt.get_auto_save_summary().map_no == 4 &&
          rt.get_auto_save_summary().zenny == 7777,
          "loading manual save leaves auto save untouched");

    // 12. Jack-Out (Cyberworld -> Real World) with stale back_menu=3
    clear_transient_gates();
    sf_app_CpCanvas_scene = 3;
    sf_app_CpCanvas_map_no = 4;
    assist_on_frame_tick(rt, cp); // settle in Cyberworld map 4
    sf_app_CpCanvas_scene = 5;
    sf_app_CpCanvas_machi_no = 2;
    sf_app_CpCanvas_back_menu = 3; // stale
    sf_app_CpCanvas_zenny = 9100;
    assist_on_frame_tick(rt, cp);
    check(rt.get_auto_save_summary().back_menu == 5 && rt.get_auto_save_summary().machi_no == 2 &&
          rt.get_auto_save_summary().zenny == 9100,
          "Jack-Out auto save syncs back_menu=5, machi_no=2, zenny=9100");
    sf_app_CpCanvas_scene = 1;
    sf_app_CpCanvas_zenny = 1;
    check(autosave_execute_load_slot(rt, cp, 2) &&
          sf_app_CpCanvas_scene == 5 && sf_app_CpCanvas_machi_no == 2 && sf_app_CpCanvas_zenny == 9100,
          "Jack-Out auto save loads cleanly back into Real World");

    // 13. Location names, disk reload, and CRC corruption rejection
    check(!rt.get_auto_save_summary().location_name.empty(), "auto save resolves non-empty location_name");
    check(!rt.get_manual_save_summary().location_name.empty(), "manual save resolves non-empty location_name");
    check(rt.reload_autosave_from_disk() && rt.has_auto_save(), "valid .autosave reloads from disk");
    {
        std::vector<uint8_t> raw;
        {
            std::ifstream in(rt.autosave_path(), std::ios::binary);
            raw.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        check(!raw.empty(), ".autosave file exists on disk");
        raw.back() ^= 0xFFu;
        {
            std::ofstream out(rt.autosave_path(), std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        }
        check(!rt.reload_autosave_from_disk() && !rt.has_auto_save(),
              "corrupted .autosave payload fails CRC and is rejected cleanly");
        check(rt.has_manual_save(), "manual save remains intact after corrupted autosave reload");
    }

    std::remove(rt.autosave_path().c_str());
    std::cout << "[AutosaveSelfTest] ALL CHECKS PASSED!\n";
    rt.shutdown();
    std::exit(0);
}
