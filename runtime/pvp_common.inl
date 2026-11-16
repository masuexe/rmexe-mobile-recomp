// ============================================================================
// runtime/pvp_common.inl
// Shared generated-symbol helpers for Rockman EXE PoN & LoN Online PvP.
//
// IMPORTANT: Must be #included INSIDE `namespace rmexe { ... }` AFTER
// `pvp_common.hpp` has been #included at global scope and AFTER generated
// `sf_app_*` / `fn_app_*` / `generated_save_statics` /
// `generated_restore_statics` declarations in `*_recomp_gen.cpp`.
// ============================================================================

#ifndef RMEXE_PVP_COMMON_HPP
#error "Include pvp_common.hpp outside namespace rmexe before including pvp_common.inl inside namespace rmexe."
#endif

namespace pvp_common {

inline HostRuntimeState capture_host_runtime_state(DojaRuntime& rt) {
    HostRuntimeState h{};
    h.mill = sf_app_CpCanvas_mill;
    h.audio_mill = sf_app_CpCanvas_audio_mill;
    h.audio_flg = sf_app_CpCanvas_audio_flg;
    h.play_flg = sf_app_CpCanvas_play_flg;
    h.now_bgm = sf_app_CpCanvas_now_bgm;
    h.m_audio_se = copy_int_array_safe(rt, sf_app_CpCanvas_m_audio_se);
    h.now_se = copy_int_array_safe(rt, sf_app_CpCanvas_now_se);
    h.now_se_cnt = copy_int_array_safe(rt, sf_app_CpCanvas_now_se_cnt);
    h.now_se_flg = copy_int_array_safe(rt, sf_app_CpCanvas_now_se_flg);
    h.se_set = copy_int_array_safe(rt, sf_app_CpCanvas_se_set);
    h.se_flg = copy_int_array_safe(rt, sf_app_CpCanvas_se_flg);
    return h;
}

inline void restore_host_runtime_state(DojaRuntime& rt,
                                       const HostRuntimeState& h,
                                       bool restore_se_queue = true) {
    sf_app_CpCanvas_mill = h.mill;
    sf_app_CpCanvas_audio_mill = h.audio_mill;
    sf_app_CpCanvas_audio_flg = h.audio_flg;
    sf_app_CpCanvas_play_flg = h.play_flg;
    sf_app_CpCanvas_now_bgm = h.now_bgm;
    write_int_array_safe(rt, sf_app_CpCanvas_m_audio_se, h.m_audio_se);
    write_int_array_safe(rt, sf_app_CpCanvas_now_se, h.now_se);
    write_int_array_safe(rt, sf_app_CpCanvas_now_se_cnt, h.now_se_cnt);
    write_int_array_safe(rt, sf_app_CpCanvas_now_se_flg, h.now_se_flg);
    if (restore_se_queue) {
        write_int_array_safe(rt, sf_app_CpCanvas_se_set, h.se_set);
        write_int_array_safe(rt, sf_app_CpCanvas_se_flg, h.se_flg);
    }
}

template <typename CoreT>
inline void save_rollback_slot(DojaRuntime& rt,
                               PvpCommonState<CoreT>& eng,
                               uint32_t turn,
                               int32_t p0_m,
                               int32_t p1_m,
                               bool p1_pred,
                               uint16_t pre_hash) {
    RollbackSlot<CoreT>& sl = eng.rb_ring[turn % kRollbackWindow];
    sl.turn = turn;
    sl.p0_mask = p0_m;
    sl.p1_mask = p1_m;
    sl.p1_predicted = p1_pred;
    sl.pre_hash = pre_hash;
    sl.core = eng.core;
    generated_save_statics(sl.statics);
    sl.heap_snap.base_heap_size = 0;
    rt.save_combat_snapshot(sl.heap_snap, eng.mutable_heap_min_ref);
}

template <typename CoreT>
inline bool restore_rollback_slot(DojaRuntime& rt,
                                  PvpCommonState<CoreT>& eng,
                                  uint32_t turn) {
    const RollbackSlot<CoreT>& sl = eng.rb_ring[turn % kRollbackWindow];
    if (sl.turn != turn) return false;
    HostRuntimeState host_st = capture_host_runtime_state(rt);
    eng.core = sl.core;
    generated_restore_statics(sl.statics);
    rt.restore_combat_snapshot(sl.heap_snap);
    restore_host_runtime_state(rt, host_st, true);
    return true;
}

template <typename CoreT, typename StepFn, typename HashFn>
inline uint32_t execute_rollback_resim(DojaRuntime& rt,
                                       PvpCommonState<CoreT>& eng,
                                       pvp::PvpClientSession& net,
                                       uint16_t rnd,
                                       uint32_t rollback_from,
                                       uint32_t cur_t,
                                       StepFn&& step_fn,
                                       HashFn&& hash_fn) {
    if (rollback_from == UINT32_MAX || !restore_rollback_slot(rt, eng, rollback_from)) {
        return cur_t;
    }
    const size_t hist_n = eng.local_inputs.size();
    const uint32_t depth = (cur_t > rollback_from) ? (cur_t - rollback_from) : 0u;
    eng.diag.total_rollbacks++;
    eng.diag.last_rollback_from = rollback_from;
    if (depth > eng.diag.max_rollback_depth) {
        eng.diag.max_rollback_depth = depth;
    }

    rt.reset_interpolator();
    uint32_t prev_cur_t = cur_t;
    for (uint32_t t = rollback_from; t < prev_cur_t; ++t) {
        size_t idx = t % hist_n;
        int32_t loc_m = eng.local_inputs[idx];
        int32_t rem_m = 0;
        bool is_pred = false;
        if (eng.remote_confirmed[idx]) {
            rem_m = eng.predicted_remote[idx];
        } else if (net.get_remote_turn_input(rnd, t, rem_m)) {
            eng.predicted_remote[idx] = rem_m;
            eng.remote_confirmed[idx] = true;
        } else {
            rem_m = (t > eng.round_start_turn)
                ? eng.predicted_remote[(t - 1) % hist_n]
                : eng.round_initial_remote_pred;
            eng.predicted_remote[idx] = rem_m;
            is_pred = true;
        }
        int32_t p0_m = (eng.local_idx == 0) ? loc_m : rem_m;
        int32_t p1_m = (eng.local_idx == 1) ? loc_m : rem_m;

        save_rollback_slot(rt, eng, t, p0_m, p1_m, is_pred, hash_fn(rt));
        step_fn(p0_m, p1_m);
        if (eng.core.open_custom_req || eng.core.winner != -2) {
            break;
        }
    }
    uint32_t new_cur_t = eng.core.combat_turn;
    for (uint32_t u = new_cur_t; u < prev_cur_t; ++u) {
        if (u >= eng.round_start_turn) {
            eng.remote_confirmed[u % hist_n] = false;
        }
    }
    return new_cur_t;
}

template <typename CoreT>
inline void apply_trill_clock_sync(DojaRuntime& rt,
                                   PvpCommonState<CoreT>& eng,
                                   pvp::PvpClientSession& net,
                                   uint16_t rnd,
                                   uint32_t send_top) {
    uint32_t max_rem_f = 0;
    bool has_rem_f = net.get_highest_remote_turn_frame(rnd, max_rem_f);
    int32_t fresh_rem_frontier = has_rem_f
        ? static_cast<int32_t>(max_rem_f)
        : static_cast<int32_t>(eng.round_start_turn);
    int32_t fresh_local_adv = static_cast<int32_t>(send_top) - fresh_rem_frontier;
    int16_t rem_adv = 0;
    int32_t skew = 0;
    if (net.get_remote_tick_advantage(rnd, rem_adv)) {
        skew = fresh_local_adv - static_cast<int32_t>(rem_adv);
    } else {
        int32_t one_way_ticks = (net.rtt_ms() > 0) ? ((net.rtt_ms() + 62) / 124) : 0;
        skew = 2 * (fresh_local_adv - one_way_ticks);
    }
    int64_t extra_wait_ms = eng.throttler.update_extra_wait_ms(skew, fresh_local_adv);
    if (extra_wait_ms > 0) {
        sf_app_CpCanvas_mill += extra_wait_ms;
        if (rt.pvp_auto_bot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp<int64_t>(extra_wait_ms / 6, 1, 5)));
        }
    } else if (has_rem_f && fresh_local_adv <= -2 && !rt.fast_forward()) {
        int32_t backlog = -fresh_local_adv;
        int64_t catchup_ms = std::min<int64_t>(42, static_cast<int64_t>(backlog) * 14);
        sf_app_CpCanvas_mill -= catchup_ms;
    }
}

inline void draw_native_battle_start_sprite(DojaRuntime& rt,
                                            int frame_idx,
                                            int32_t center_y_offset = 0,
                                            int image_index = 43) {
    static constexpr int32_t kHeights[15] = {4, 9, 16, 18, 16, 16, 16, 16, 16, 16, 16, 16, 16, 9, 4};
    int32_t h = kHeights[std::clamp(frame_idx, 0, 14)];
    Ref img = (sf_app_CpCanvas_image > 0 && image_index >= 0 &&
               rt.array_length(sf_app_CpCanvas_image) > image_index)
        ? rt.iaload(sf_app_CpCanvas_image, image_index)
        : 0;
    if (img <= 0 || sf_app_CpCanvas_g <= 0) {
        return;
    }
    ScopedRenderMute render_unmute(rt, false);
    int32_t dy = 104 + center_y_offset + (16 - h) / 2;
    Slot args[10];
    args[0].r = sf_app_CpCanvas_g;
    args[1].r = img;
    args[2].i = 52;
    args[3].i = dy;
    args[4].i = 68;
    args[5].i = h;
    args[6].i = 120;
    args[7].i = 0;
    args[8].i = 68;
    args[9].i = 16;
    rt.invoke_external("com/nttdocomo/ui/Graphics", "drawScaledImage", "(Lcom/nttdocomo/ui/Image;IIIIIIII)V", args, 10);

    args[2].i = 120;
    args[7].i = 16;
    rt.invoke_external("com/nttdocomo/ui/Graphics", "drawScaledImage", "(Lcom/nttdocomo/ui/Image;IIIIIIII)V", args, 10);
}

inline void render_round_banner_overlay(DojaRuntime& rt,
                                        int frame_idx,
                                        uint16_t custom_round,
                                        int battle_start_image_index = 43) {
    static constexpr int32_t kHeights[15] = {4, 9, 16, 18, 16, 16, 16, 16, 16, 16, 16, 16, 16, 9, 4};
    int idx = std::clamp(frame_idx, 0, 14);
    int32_t kh = kHeights[idx];

    if (custom_round <= 1) {
        draw_native_battle_start_sprite(rt, idx, 8, battle_start_image_index);
        int32_t bh = std::max<int32_t>(4, (20 * kh) / 16);
        int32_t by = 92 - bh / 2;
        rt.fb_fill_rect_alpha(52, by, 136, bh, 0xE8101C38u);
        rt.fb_draw_border(52, by, 136, bh, 0xFFFFD84Cu);
        if (idx >= 2 && idx <= 12) {
            std::string lbl = "TURN 1 START!";
            rt.fb_draw_text(lbl, 120 - rt.fb_measure_text(lbl) / 2, by + bh - 5, 0xFFFFF088u);
        }
    } else if (custom_round <= 14) {
        int32_t bh = std::max<int32_t>(6, (28 * kh) / 16);
        int32_t by = 112 - bh / 2;
        rt.fb_fill_rect_alpha(34, by, 172, bh, 0xEC101E3Cu);
        rt.fb_draw_border(34, by, 172, bh, 0xFF58B8FFu);
        if (bh >= 16) {
            std::string lbl = "TURN " + std::to_string(custom_round) + " START!";
            rt.fb_draw_text(lbl, 120 - rt.fb_measure_text(lbl) / 2, by + bh / 2 + 5, 0xFFFFF078u);
        }
    } else {
        int32_t bh = std::max<int32_t>(6, (40 * kh) / 16);
        int32_t by = 112 - bh / 2;
        rt.fb_fill_rect_alpha(28, by, 184, bh, 0xF02C0E18u);
        rt.fb_draw_border(28, by, 184, bh, 0xFFFF5868u);
        if (idx >= 2 && idx <= 12 && bh >= 22) {
            std::string line1 = "FINAL TURN!";
            std::string line2 = "最终回合 · 限时 10 秒!";
            rt.fb_draw_text(line1, 120 - rt.fb_measure_text(line1) / 2, by + 16, 0xFFFFE866u);
            rt.fb_draw_text(line2, 120 - rt.fb_measure_text(line2) / 2, by + 33, 0xFFFFA8B0u);
        }
    }
}

inline void play_pvp_result_bgm(DojaRuntime& rt,
                                int winner_idx,
                                int local_idx,
                                int win_bgm_id = 5,
                                int lose_bgm_id = 6) {
    if (winner_idx == local_idx) {
        fn_app_CpCanvas_Audio___II__V(rt, 1, win_bgm_id); // Victory / Enemy Deleted fanfare
    } else if (winner_idx >= 0) {
        fn_app_CpCanvas_Audio___II__V(rt, 1, lose_bgm_id); // Defeat BGM
    } else {
        fn_app_CpCanvas_Audio___II__V(rt, 0, 0); // Draw: stop BGM cleanly
    }
}

template <typename CoreT>
inline void adjudicate_peer_disconnect_win_common(DojaRuntime& rt,
                                                  PvpCommonState<CoreT>& eng,
                                                  int win_bgm_id = 5,
                                                  int lose_bgm_id = 6) {
    pvp::PvpClientSession& net = rt.pvp_session();
    apply_bo3_disconnect_win_state(eng, net);
    play_pvp_result_bgm(rt, eng.core.winner, eng.local_idx, win_bgm_id, lose_bgm_id);
    rt.reset_interpolator();
}

template <typename CoreT>
inline void conclude_current_game_common(DojaRuntime& rt,
                                         PvpCommonState<CoreT>& eng,
                                         int win_bgm_id = 5,
                                         int lose_bgm_id = 6) {
    pvp::PvpClientSession& net = rt.pvp_session();
    (void)apply_bo3_conclude_game_state(eng, net);
    play_pvp_result_bgm(rt, eng.core.winner, eng.local_idx, win_bgm_id, lose_bgm_id);
    rt.reset_interpolator();
}

template <typename CoreT>
inline void capture_pre_match_snapshot(DojaRuntime& rt, PvpCommonState<CoreT>& eng) {
    if (!eng.has_pre_match_snap) {
        rt.save_full_snapshot(eng.pre_match_heap);
        generated_save_statics(eng.pre_match_statics);
        eng.has_pre_match_snap = true;
    }
}

template <typename CoreT>
inline void restore_pre_match_snapshot_for_next_game(DojaRuntime& rt, PvpCommonState<CoreT>& eng) {
    eng.bo3_game_index++;
    if (eng.has_pre_match_snap) {
        int64_t saved_mill = sf_app_CpCanvas_mill;
        fn_app_CpCanvas_Audio___II__V(rt, 0, 0);
        rt.restore_full_snapshot(eng.pre_match_heap);
        generated_restore_statics(eng.pre_match_statics);
        sf_app_CpCanvas_mill = saved_mill;
    }
    eng.match_seed = derive_bo3_game_seed(eng.base_match_seed, eng.bo3_game_index);
    eng.round_result_ticks = 0;
    eng.phase_timer = 0;
    eng.combat_stall_frames = 0;
    rt.reset_interpolator();
}

template <typename CoreT>
inline void cleanup_common_and_restore_singleplayer(DojaRuntime& rt,
                                                    PvpCommonState<CoreT>& eng,
                                                    bool close_net = true) {
    pvp::PvpClientSession& net = rt.pvp_session();
    if (close_net) {
        net.close_udp();
    }
    rt.set_pvp_match_active(false);
    rt.set_suppress_av(false);
    rt.set_keypad_override(false, 0);
    rt.reset_interpolator();
    eng.in_combat_step = false;

    if (eng.has_pre_match_snap) {
        fn_app_CpCanvas_Audio___II__V(rt, 0, 0); // Stop PvP BGM before restoring single-player state
        rt.restore_full_snapshot(eng.pre_match_heap);
        generated_restore_statics(eng.pre_match_statics);
        eng.has_pre_match_snap = false;
        eng.pre_match_heap = FullHeapSnapshot{};
        eng.pre_match_statics.clear();
        sf_app_CpCanvas_play_flg = 1; // Resume pre-match BGM cleanly
        sf_app_CpCanvas_mill = 0;     // Prevent frame-timer skew after restoring snapshot
    }
    sf_app_CpCanvas_key = 0;
    reset_common_exit_fields(eng);
}

} // namespace pvp_common
