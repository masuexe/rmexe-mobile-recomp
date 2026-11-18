#pragma once
#ifndef RMEXE_PVP_COMMON_HPP
#define RMEXE_PVP_COMMON_HPP

// ============================================================================
// runtime/pvp_common.hpp
// Game-agnostic shared PvP engine primitives for Rockman EXE PoN & LoN:
//   - Shared Phase state machine (including TRANCE_CUTSCENE for Stage 3 LoN)
//   - TrillTimeSyncThrottler (drift smoothing ported from HCTOrganization/trill)
//   - ScopedRenderMute (RAII DojaRuntime::suppress_av() guard)
//   - HostRuntimeState & safe array capture/restore helpers
//   - Generic RollbackSlot<CoreT>, input history, confirmation, desync hashing
//   - Shared BO3 session state, seed derivation & match conclusion helpers
//   - Shared HUD / waiting / judge / scoreboard / result overlay drawers
//   - Shared self-test LoopbackRelay & RAII guards
//
// IMPORTANT: Must be #included at global scope OUTSIDE `namespace rmexe { ... }`.
// ============================================================================

#include "doja_runtime.hpp"
#include "pvp_protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace rmexe::pvp_common {

enum class Phase : uint8_t {
    IDLE = 0,
    LOBBY_WAIT,
    HANDSHAKE,
    CUSTOM_SCREEN,
    TRANCE_CUTSCENE, // Stage 3 (LoN); PoN transitions CUSTOM_SCREEN -> ROUND_BANNER directly
    ROUND_BANNER,
    COMBAT,
    JUDGE,
    ROUND_RESULT,
    RESULT,
};

static constexpr size_t kRollbackWindow = 128;
static constexpr uint32_t kMaxPredictionFrames = 12; // 0.75s max speculation cap at 16 Hz (prevents multi-second drift!)
static constexpr size_t kInputHistorySize = 4096;
static constexpr uint32_t kInputDelayFrames = 1;
static constexpr int kCustomTimeoutTicks = 30 * 16; // 30s at 16 Hz
static constexpr uint16_t kBannerTicks = 15;
static constexpr uint16_t kMaxTurns = 15;
static constexpr uint16_t kFinalTurnDurationTicks = 10 * 16; // 160 ticks at 16 Hz
static constexpr uint8_t kOppNonDimChipHudTicks = 16; // 1.0s at 16 Hz
static constexpr int kJudgeTicks = 48;
static constexpr int kRoundResultTicks = 48;
static constexpr int kResultAutoExitTicks = 96;
static constexpr int kStallTimeoutTicks = 16 * 12;

// RAII helper to temporarily mute or unmute DojaRuntime AV output / rendering.
struct ScopedRenderMute {
    DojaRuntime& rt;
    bool prev_sup;

    explicit ScopedRenderMute(DojaRuntime& r, bool mute = true)
        : rt(r), prev_sup(r.suppress_av()) {
        rt.set_suppress_av(mute);
    }
    ~ScopedRenderMute() {
        rt.set_suppress_av(prev_sup);
    }

    ScopedRenderMute(const ScopedRenderMute&) = delete;
    ScopedRenderMute& operator=(const ScopedRenderMute&) = delete;
};

// Rollback clock-drift throttler ported from HCTOrganization/trill
// (getgud::Session local_tick_advantage / skew / speculation_balance + trill-pvp/src/battle/throttler.rs)
// adapted to DoJa's native 62ms (16.129 Hz) CpCanvas.exe() frame clock.
struct TrillTimeSyncThrottler {
    static constexpr float kExpectedFps = 1000.0f / 62.0f; // 16.129032 Hz
    static constexpr float kMaxSlowdown = 0.36f * kExpectedFps;
    static constexpr float kSkewGain = 0.065f * kExpectedFps; // skew = 2 * lead_ticks
    static constexpr float kEngagementSlope = 0.75f * kExpectedFps;
    static constexpr float kBalanceFloor = -(kMaxSlowdown / kEngagementSlope);
    static constexpr float kSkewAlphaRise = 0.22f;
    static constexpr float kSkewAlphaFall = 0.60f;
    static constexpr float kBalanceAlpha = 0.40f;

    float skew_ema = 0.0f;
    float balance_ema = 0.0f;

    void reset() {
        skew_ema = 0.0f;
        balance_ema = 0.0f;
    }

    // Returns extra frame wait in milliseconds (>= 0) applied ONLY to the leading peer's mill timer
    int64_t update_extra_wait_ms(int32_t skew, int32_t speculation_balance) {
        int32_t eff_skew = (std::abs(skew) <= 1) ? 0 : skew;
        float target_skew = static_cast<float>(eff_skew);
        float s_alpha = (target_skew > skew_ema) ? kSkewAlphaRise : kSkewAlphaFall;
        skew_ema = std::max(0.0f, skew_ema + s_alpha * (target_skew - skew_ema));

        balance_ema = std::max(
            kBalanceFloor,
            balance_ema + kBalanceAlpha * (static_cast<float>(speculation_balance) - balance_ema));

        if (balance_ema <= 0.0f || skew_ema <= 0.05f) {
            return 0;
        }
        float gate_cap = balance_ema * kEngagementSlope;
        float slowdown_fps = std::clamp(std::min(skew_ema * kSkewGain, gate_cap), 0.0f, kMaxSlowdown);
        if (slowdown_fps <= 0.05f) {
            return 0;
        }
        float target_fps = std::max(4.0f, kExpectedFps - slowdown_fps);
        int64_t target_period_ms = static_cast<int64_t>(std::lround(1000.0f / target_fps));
        return std::clamp<int64_t>(target_period_ms - 62, 0, 38);
    }
};

struct HostRuntimeState {
    int64_t mill = 0;
    int64_t audio_mill = 0;
    int32_t audio_flg = 0;
    int32_t play_flg = 0;
    int32_t now_bgm = 0;
    std::vector<int32_t> m_audio_se;
    std::vector<int32_t> now_se;
    std::vector<int32_t> now_se_cnt;
    std::vector<int32_t> now_se_flg;
    std::vector<int32_t> se_set;
    std::vector<int32_t> se_flg;
};

template <typename CoreT>
struct RollbackSlot {
    uint32_t turn = UINT32_MAX;
    int32_t p0_mask = 0;
    int32_t p1_mask = 0;
    bool p1_predicted = false;
    uint16_t pre_hash = 0;
    CoreT core{};
    std::vector<int64_t> statics{};
    FastCombatSnapshot heap_snap{};
};

struct TurnEmittedSe {
    uint32_t turn = UINT32_MAX;
    uint8_t count = 0;
    std::array<int32_t, 8> se_ids{};
};

struct RollbackDiag {
    uint32_t total_rollbacks = 0;
    uint32_t last_rollback_from = UINT32_MAX;
    uint32_t max_rollback_depth = 0;
};

template <typename CoreT>
struct PvpCommonState {
    using TurnEmittedSe = rmexe::pvp_common::TurnEmittedSe;

    Phase phase = Phase::IDLE;
    uint32_t active_room_id = 0;
    int local_idx = 0; // 0 = Host (Left/Red), 1 = Joiner (Right/Blue)
    uint32_t match_seed = 0x13579BDFu;
    size_t mutable_heap_min_ref = 1;

    // Pre-match full VM snapshot for clean restoration after match ends
    bool has_pre_match_snap = false;
    FullHeapSnapshot pre_match_heap{};
    std::vector<int64_t> pre_match_statics{};

    // Build sync & Custom Screen state
    pvp::PvpFighterBuild local_build{};
    pvp::PvpFighterBuild builds[2]{};
    bool build_prepared = false;
    int64_t last_sync_send_ms = 0;
    int64_t last_build_keepalive_ms = 0;

    bool local_custom_done = false;
    pvp::PvpCustomCommit local_commit{};
    int custom_timer_ticks = 0;
    std::array<int32_t, 10> local_cas_tip{};
    std::array<int32_t, 10> local_cas_ok{};
    uint32_t local_permanently_consumed_mask = 0;
    CoreT pre_custom_core{};
    std::vector<int64_t> pre_custom_statics{};
    FastCombatSnapshot pre_custom_snap{};

    // Lockstep & GGPO Rollback state
    CoreT core{};
    std::array<int32_t, kInputHistorySize> local_inputs{};
    std::array<int32_t, kInputHistorySize> predicted_remote{};
    std::array<bool, kInputHistorySize> remote_confirmed{};
    std::array<RollbackSlot<CoreT>, kRollbackWindow> rb_ring{};
    std::array<TurnEmittedSe, kRollbackWindow> turn_emitted_se{};
    FastCombatSnapshot render_snap{};
    std::vector<int64_t> render_statics{};
    uint32_t round_start_turn = 0;
    uint32_t first_unconfirmed_turn = 0;
    uint32_t max_scheduled_turn = 0;
    uint32_t last_hash_checked_turn = 0;
    bool round_needs_initial_schedule = true;
    int32_t round_initial_remote_pred = 0;
    int32_t round_held_edge_suppress_mask = 0;
    int desync_warnings = 0;
    int stall_frames = 0;
    int phase_timer = 0;
    int combat_stall_frames = 0;
    RollbackDiag diag{};
    TrillTimeSyncThrottler throttler{};

    // Round banner, Judge banner, Between-game BO3 & Result banner timers + BO3 state
    int banner_ticks = 0;
    int judge_ticks = 0;
    int round_result_ticks = 0;
    int result_ticks = 0;
    uint32_t base_match_seed = 0;
    bool is_triple_battle = false;
    uint8_t bo3_game_index = 0;                 // 0, 1, 2 (Game 1..3)
    uint8_t bo3_wins[2] = {0, 0};               // Game wins for P0, P1
    int8_t bo3_game_results[3] = {-2, -2, -2};  // Per-game winner (-1 Draw, 0 P0, 1 P1)
    int match_winner = -2;

    // Re-entrancy flags for hit hooks
    bool in_combat_step = false;
    int active_side = 0;
    int bot_custom_step = 0;
};

// ----------------------------------------------------------------------------
// Array, Timing & Deterministic BO3 Helpers
// ----------------------------------------------------------------------------

inline std::vector<int32_t> copy_int_array_safe(DojaRuntime& rt, Ref arr) {
    std::vector<int32_t> out;
    if (arr > 0) {
        int32_t n = rt.array_length(arr);
        if (n > 0) {
            out.resize(static_cast<size_t>(n));
            for (int32_t i = 0; i < n; ++i) {
                out[static_cast<size_t>(i)] = rt.iaload(arr, i);
            }
        }
    }
    return out;
}

inline void write_int_array_safe(DojaRuntime& rt, Ref arr, const std::vector<int32_t>& vals) {
    if (arr > 0 && !vals.empty()) {
        int32_t n = std::min<int32_t>(rt.array_length(arr), static_cast<int32_t>(vals.size()));
        for (int32_t i = 0; i < n; ++i) {
            rt.iaload(arr, i) = vals[static_cast<size_t>(i)];
        }
    }
}

inline uint32_t derive_bo3_game_seed(uint32_t base_seed, uint8_t game_idx) {
    if (game_idx == 0) return base_seed ? base_seed : 1u;
    uint32_t s = base_seed ^ (static_cast<uint32_t>(game_idx) * 0x9E3779B9u + 0x7F4A7C15u);
    s ^= (s >> 16);
    s *= 0x85EBCA6Bu;
    s ^= (s >> 13);
    return s | 1u;
}

inline uint16_t make_wire_round_key(uint8_t bo3_game_index, uint16_t custom_round) {
    return static_cast<uint16_t>(static_cast<uint16_t>(bo3_game_index) * 100u + custom_round);
}

inline uint8_t make_match_end_result_code(int winner_idx, int local_idx) {
    if (winner_idx == -1) return 3;
    return (winner_idx == local_idx) ? 1 : 2;
}

inline int64_t steady_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

template <typename CoreT>
inline void reset_bo3_series_state(PvpCommonState<CoreT>& eng, uint32_t base_seed, bool is_triple) {
    eng.base_match_seed = base_seed ? base_seed : 0x2468ACE1u;
    eng.is_triple_battle = is_triple;
    eng.bo3_game_index = 0;
    eng.bo3_wins[0] = 0;
    eng.bo3_wins[1] = 0;
    eng.bo3_game_results[0] = -2;
    eng.bo3_game_results[1] = -2;
    eng.bo3_game_results[2] = -2;
    eng.match_winner = -2;
    eng.round_result_ticks = 0;
    eng.desync_warnings = 0;
    eng.diag = RollbackDiag{};
    eng.match_seed = derive_bo3_game_seed(eng.base_match_seed, 0);
}

template <typename CoreT>
inline void reset_common_exit_fields(PvpCommonState<CoreT>& eng) {
    eng.core = CoreT{};
    eng.phase = Phase::IDLE;
    eng.banner_ticks = 0;
    eng.judge_ticks = 0;
    eng.round_result_ticks = 0;
    eng.phase_timer = 0;
    eng.combat_stall_frames = 0;
    eng.stall_frames = 0;
    eng.last_hash_checked_turn = 0;
    eng.diag = RollbackDiag{};
    eng.base_match_seed = 0;
    eng.is_triple_battle = false;
    eng.bo3_game_index = 0;
    eng.bo3_wins[0] = 0;
    eng.bo3_wins[1] = 0;
    eng.bo3_game_results[0] = -2;
    eng.bo3_game_results[1] = -2;
    eng.bo3_game_results[2] = -2;
    eng.match_winner = -2;
    eng.desync_warnings = 0;
    eng.active_room_id = 0;
    eng.build_prepared = false;
    eng.last_build_keepalive_ms = 0;
}

template <typename CoreT>
inline void apply_bo3_disconnect_win_state(PvpCommonState<CoreT>& eng, pvp::PvpClientSession& net) {
    eng.core.winner = eng.local_idx;
    eng.match_winner = eng.local_idx;
    if (eng.is_triple_battle) {
        if (eng.bo3_game_index < 3 &&
            eng.bo3_game_results[eng.bo3_game_index] == -2) {
            eng.bo3_game_results[eng.bo3_game_index] = static_cast<int8_t>(eng.local_idx);
        }
        eng.bo3_wins[eng.local_idx] = std::min<uint8_t>(
            2, static_cast<uint8_t>(eng.bo3_wins[eng.local_idx] + 1));
    }
    if (net.active()) {
        net.send_match_end(1);
    }
    eng.phase = Phase::RESULT;
    eng.phase_timer = 0;
    eng.result_ticks = 0;
}

// Updates BO3 score/winner and Phase (ROUND_RESULT vs RESULT) + transmits PKT_MATCH_END if series done.
// Returns true if entering Phase::RESULT (final match conclusion), false if entering Phase::ROUND_RESULT.
template <typename CoreT>
inline bool apply_bo3_conclude_game_state(PvpCommonState<CoreT>& eng, pvp::PvpClientSession& net) {
    int game_win = eng.core.winner;
    if (eng.bo3_game_index < 3) {
        eng.bo3_game_results[eng.bo3_game_index] = static_cast<int8_t>(game_win);
    }
    if (game_win == 0) {
        eng.bo3_wins[0]++;
    } else if (game_win == 1) {
        eng.bo3_wins[1]++;
    }

    if (!eng.is_triple_battle) {
        eng.match_winner = game_win;
        eng.phase = Phase::RESULT;
        eng.phase_timer = 0;
        eng.result_ticks = 0;
        if (net.active()) {
            net.send_match_end(make_match_end_result_code(game_win, eng.local_idx));
        }
        return true;
    }

    bool series_done = (eng.bo3_wins[0] >= 2 ||
                        eng.bo3_wins[1] >= 2 ||
                        eng.bo3_game_index >= 2);
    if (!series_done) {
        eng.phase = Phase::ROUND_RESULT;
        eng.phase_timer = 0;
        eng.round_result_ticks = 0;
        // Never send PKT_MATCH_END mid-series (server prunes rooms 6s after PKT_MATCH_END).
        return false;
    }

    if (eng.bo3_wins[0] > eng.bo3_wins[1]) {
        eng.match_winner = 0;
    } else if (eng.bo3_wins[1] > eng.bo3_wins[0]) {
        eng.match_winner = 1;
    } else {
        eng.match_winner = -1;
    }
    eng.core.winner = eng.match_winner;
    eng.phase = Phase::RESULT;
    eng.phase_timer = 0;
    eng.result_ticks = 0;
    if (net.active()) {
        net.send_match_end(make_match_end_result_code(eng.match_winner, eng.local_idx));
    }
    return true;
}

// ----------------------------------------------------------------------------
// Generic Rollback / Input History / Confirmation / Desync Helpers
// ----------------------------------------------------------------------------

template <typename CoreT>
inline void transition_banner_to_combat(DojaRuntime& rt,
                                        PvpCommonState<CoreT>& eng,
                                        bool has_loaded_chip) {
    int32_t held_now = rt.poll_local_pvp_keypad();
    int32_t suppress_bits = (0xF << 16) | (1 << 21) | (1 << 22);
    if (has_loaded_chip) {
        suppress_bits |= (1 << 20);
    }
    eng.round_held_edge_suppress_mask = held_now & suppress_bits;
    eng.stall_frames = 0;
    eng.combat_stall_frames = 0;
    eng.phase_timer = 0;
    eng.throttler.reset();
    eng.phase = Phase::COMBAT;
    rt.reset_interpolator();
}

template <typename CoreT>
inline void schedule_local_combat_inputs(PvpCommonState<CoreT>& eng,
                                         uint32_t cur_t,
                                         int32_t raw_local,
                                         bool pending_transition) {
    const size_t hist_n = eng.local_inputs.size();
    if (!pending_transition && eng.round_needs_initial_schedule) {
        size_t s0 = cur_t % hist_n;
        eng.local_inputs[s0] = raw_local;
        eng.remote_confirmed[s0] = false;
        eng.max_scheduled_turn = cur_t;
        eng.round_needs_initial_schedule = false;
    }
    uint32_t target_sched = pending_transition
        ? (cur_t > eng.round_start_turn ? cur_t - 1 : eng.round_start_turn)
        : (cur_t + kInputDelayFrames);
    while (eng.max_scheduled_turn < target_sched) {
        uint32_t next_s = eng.max_scheduled_turn + 1;
        size_t s_idx = next_s % hist_n;
        eng.local_inputs[s_idx] = raw_local;
        eng.remote_confirmed[s_idx] = false;
        eng.max_scheduled_turn = next_s;
    }
}

template <typename CoreT>
inline uint8_t build_redundant_input_bundle(const PvpCommonState<CoreT>& eng,
                                            uint32_t top_turn,
                                            int32_t out_hist_newest_first[8]) {
    const size_t hist_n = eng.local_inputs.size();
    uint8_t cnt = static_cast<uint8_t>(
        std::min<uint32_t>(8u, top_turn - eng.round_start_turn + 1u));
    for (uint8_t i = 0; i < cnt; ++i) {
        out_hist_newest_first[i] = eng.local_inputs[(top_turn - i) % hist_n];
    }
    return cnt;
}

template <typename CoreT>
inline uint32_t scan_mispredicted_turns(PvpCommonState<CoreT>& eng,
                                        pvp::PvpClientSession& net,
                                        uint16_t rnd,
                                        uint32_t cur_t) {
    const size_t hist_n = eng.local_inputs.size();
    uint32_t oldest_rb = (cur_t >= kRollbackWindow - 2) ? (cur_t - (kRollbackWindow - 2)) : 0;
    oldest_rb = std::max(oldest_rb, eng.round_start_turn);
    uint32_t rollback_from = UINT32_MAX;
    for (uint32_t t = oldest_rb; t < cur_t; ++t) {
        int32_t net_rem = 0;
        if (net.get_remote_turn_input(rnd, t, net_rem)) {
            size_t idx = t % hist_n;
            if (!eng.remote_confirmed[idx]) {
                if (eng.predicted_remote[idx] != net_rem && rollback_from == UINT32_MAX) {
                    rollback_from = t;
                }
                eng.predicted_remote[idx] = net_rem;
                eng.remote_confirmed[idx] = true;
            }
        }
    }
    return rollback_from;
}

template <typename CoreT>
inline void advance_confirmed_frontier(PvpCommonState<CoreT>& eng,
                                       pvp::PvpClientSession& net,
                                       uint16_t rnd,
                                       uint32_t cur_t) {
    const size_t hist_n = eng.local_inputs.size();
    while (eng.first_unconfirmed_turn < cur_t) {
        uint32_t ft = eng.first_unconfirmed_turn;
        size_t idx = ft % hist_n;
        if (!eng.remote_confirmed[idx]) {
            int32_t late_m = 0;
            if (net.get_remote_turn_input(rnd, ft, late_m)) {
                eng.predicted_remote[idx] = late_m;
                eng.remote_confirmed[idx] = true;
            } else {
                break;
            }
        }
        eng.first_unconfirmed_turn++;
    }
}

template <typename CoreT>
inline void verify_confirmed_state_hashes(DojaRuntime& rt,
                                        PvpCommonState<CoreT>& eng,
                                        pvp::PvpClientSession& net,
                                        uint16_t rnd,
                                        uint32_t cur_t) {
    while (eng.last_hash_checked_turn < eng.first_unconfirmed_turn) {
        uint32_t check_t = eng.last_hash_checked_turn;
        if (check_t + kRollbackWindow <= cur_t + 2u) {
            eng.last_hash_checked_turn++;
            continue;
        }
        int32_t dummy_m = 0;
        uint16_t peer_h = 0;
        if (!net.get_remote_turn_input(rnd, check_t + kInputDelayFrames, dummy_m, &peer_h)) {
            break;
        }
        if (peer_h != 0) {
            const RollbackSlot<CoreT>& rbs = eng.rb_ring[check_t % kRollbackWindow];
            if (rbs.turn == check_t && rbs.pre_hash != 0 && rbs.pre_hash != peer_h) {
                if (++eng.desync_warnings <= 8) {
                    std::fprintf(stderr,
                                 "[PvP DESYNC] round=%u turn=%u local_hash=0x%04x remote_hash=0x%04x\n",
                                 static_cast<unsigned>(rnd), check_t,
                                 static_cast<unsigned>(rbs.pre_hash),
                                 static_cast<unsigned>(peer_h));
                    rt.show_ui_toast("警告: 检测到对战状态不同步!", 150);
                }
            }
        }
        eng.last_hash_checked_turn++;
    }
}

template <typename CoreT>
inline void flush_round_tail_bundle(const PvpCommonState<CoreT>& eng,
                                    pvp::PvpClientSession& net,
                                    uint16_t rnd,
                                    uint32_t cur_t) {
    if (cur_t > eng.round_start_turn) {
        uint32_t last_t = cur_t - 1;
        int32_t fhist[8] = {0};
        uint8_t fcnt = build_redundant_input_bundle(eng, last_t, fhist);
        net.send_input_bundle(rnd, last_t, fhist, fcnt, 0);
    }
}

// ----------------------------------------------------------------------------
// Shared HUD / Overlay Drawers (DojaRuntime framebuffer UI primitives)
// ----------------------------------------------------------------------------

inline void render_connecting_screen(DojaRuntime& rt,
                                     uint32_t room_id,
                                     const std::string& opp_name,
                                     bool is_triple_battle) {
    rt.fb_fill_rect_alpha(0, 0, 240, 240, 0xFF0A1428u);
    rt.fb_fill_rect_alpha(16, 68, 208, 104, 0xF0142448u);
    rt.fb_draw_border(16, 68, 208, 104, 0xFF5898FFu);
    std::string t1 = "NETBATTLE 联机对战准备中";
    if (is_triple_battle) {
        t1 += " [三回战]";
    }
    std::string t2 = "房间 #" + std::to_string(room_id) + "  VS  " + opp_name;
    std::string t3 = "正在同步双方存档牌组与领航员改造...";
    rt.fb_draw_text(t1, 120 - rt.fb_measure_text(t1) / 2, 94, 0xFFFFE066u);
    rt.fb_draw_text(t2, 120 - rt.fb_measure_text(t2) / 2, 122, 0xFFFFFFFFu);
    rt.fb_draw_text(t3, 120 - rt.fb_measure_text(t3) / 2, 148, 0xFF88CCFFu);
}

inline void render_waiting_custom_banner(DojaRuntime& rt, bool is_final_turn) {
    if (is_final_turn) {
        rt.fb_fill_rect_alpha(44, 78, 152, 20, 0xEE2C0C18u);
        rt.fb_draw_border(44, 78, 152, 20, 0xFFFF5868u);
        std::string ft_wait = "FINAL TURN · 最终回合";
        rt.fb_draw_text(ft_wait, 120 - rt.fb_measure_text(ft_wait) / 2, 92, 0xFFFFE866u);
    }
    rt.fb_fill_rect_alpha(28, 102, 184, 36, 0xE6101C38u);
    rt.fb_draw_border(28, 102, 184, 36, 0xFF68A8FFu);
    std::string wait_msg = "等待对手选牌确认...";
    rt.fb_draw_text(wait_msg, 120 - rt.fb_measure_text(wait_msg) / 2, 125, 0xFFFFE878u);
}

inline void render_sync_stall_banner(DojaRuntime& rt) {
    std::string sync_txt = "网络同步中...";
    rt.fb_fill_rect_alpha(72, 108, 96, 22, 0xD8101830u);
    rt.fb_draw_border(72, 108, 96, 22, 0xFFFFB84Cu);
    rt.fb_draw_text(sync_txt, 120 - rt.fb_measure_text(sync_txt) / 2, 123, 0xFFFFE088u);
}

inline void render_top_combat_hud(DojaRuntime& rt,
                                  const std::string& loc_name,
                                  int32_t loc_hp,
                                  int32_t loc_max_hp,
                                  const std::string& opp_name,
                                  int32_t opp_hp,
                                  int32_t opp_max_hp,
                                  bool is_triple_battle,
                                  uint8_t bo3_game_index,
                                  uint8_t loc_wins,
                                  uint8_t opp_wins,
                                  bool show_final_countdown,
                                  uint16_t final_turn_ticks_left) {
    rt.fb_fill_rect_alpha(3, 4, 104, 32, 0xE014203Cu);
    rt.fb_draw_border(3, 4, 104, 32, 0xFF4C88FFu);
    rt.fb_draw_text(loc_name, 7, 16, 0xFF88CCFFu);
    std::string loc_hp_str = "HP " + std::to_string(std::max(0, loc_hp)) + "/" + std::to_string(loc_max_hp);
    rt.fb_draw_text(loc_hp_str, 7, 31, (loc_hp * 4 <= loc_max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);

    if (is_triple_battle) {
        rt.fb_draw_text("G" + std::to_string(bo3_game_index + 1), 113, 15, 0xFFFFD84Cu);
        rt.fb_draw_text(std::to_string(loc_wins) + "-" + std::to_string(opp_wins),
                        111, 31, 0xFFFFFFFFu);
    } else {
        rt.fb_draw_text("VS", 113, 24, 0xFFFFD84Cu);
    }

    rt.fb_fill_rect_alpha(133, 4, 104, 32, 0xE0381420u);
    rt.fb_draw_border(133, 4, 104, 32, 0xFFFF5868u);
    rt.fb_draw_text(opp_name, 137, 16, 0xFFFF98A8u);
    std::string opp_hp_str = "HP " + std::to_string(std::max(0, opp_hp)) + "/" + std::to_string(opp_max_hp);
    rt.fb_draw_text(opp_hp_str, 137, 31, (opp_hp * 4 <= opp_max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);

    if (show_final_countdown) {
        int secs = (static_cast<int>(final_turn_ticks_left) + 15) / 16;
        int tenths = (static_cast<int>(final_turn_ticks_left) * 10) / 16;
        std::string cd_txt = "FINAL " + std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "s";
        rt.fb_fill_rect_alpha(74, 37, 92, 13, 0xE81C0C14u);
        rt.fb_draw_border(74, 37, 92, 13, (secs <= 3) ? 0xFFFF4858u : 0xFFFFC84Cu);
        rt.fb_draw_text(cd_txt, 120 - rt.fb_measure_text(cd_txt) / 2, 47, (secs <= 3) ? 0xFFFF7888u : 0xFFFFF088u);
    }
}

inline void render_judge_overlay(DojaRuntime& rt,
                                 const std::string& loc_name,
                                 int32_t loc_dmg,
                                 const std::string& opp_name,
                                 int32_t opp_dmg,
                                 bool is_draw,
                                 bool is_win,
                                 int judge_ticks) {
    rt.fb_fill_rect_alpha(16, 68, 208, 92, 0xF00E1830u);
    rt.fb_draw_border(16, 68, 208, 92, 0xFFFFD84Cu);

    std::string title = "TIME UP! 伤害判定 (JUDGE)";
    rt.fb_draw_text(title, 120 - rt.fb_measure_text(title) / 2, 88, 0xFFFFE866u);

    std::string loc_line = loc_name + " 有效伤害: " + std::to_string(loc_dmg);
    std::string opp_line = opp_name + " 有效伤害: " + std::to_string(opp_dmg);
    rt.fb_draw_text(loc_line, 26, 110, 0xFF88CCFFu);
    rt.fb_draw_text(opp_line, 26, 128, 0xFFFF98A8u);

    if (judge_ticks >= 12) {
        uint32_t vcol = is_draw ? 0xFFFFD866u : (is_win ? 0xFF58FF98u : 0xFFFF6070u);
        std::string verdict = is_draw
            ? "判定结果: 平局 (DRAW)"
            : (is_win ? "判定结果: 己方胜利!" : "判定结果: 对手胜利...");
        rt.fb_draw_text(verdict, 120 - rt.fb_measure_text(verdict) / 2, 150, vcol);
    }
}

inline void render_round_result_overlay(DojaRuntime& rt,
                                        uint8_t bo3_game_index,
                                        int game_win,
                                        int local_idx,
                                        uint8_t loc_wins,
                                        uint8_t opp_wins) {
    uint32_t hdr_col = (game_win == -1) ? 0xFFFFD866u : ((game_win == local_idx) ? 0xFF58FF98u : 0xFFFF6070u);
    std::string line1 = "GAME " + std::to_string(bo3_game_index + 1) +
        (game_win == -1 ? " - DRAW!" : (game_win == local_idx ? " - WIN!" : " - LOSE"));
    std::string line2 = "当前比分: " + std::to_string(loc_wins) + " - " + std::to_string(opp_wins);
    std::string line3 = "即将进入第 " + std::to_string(bo3_game_index + 2) + " 局...";

    rt.fb_fill_rect_alpha(18, 74, 204, 76, 0xEE0E1830u);
    rt.fb_draw_border(18, 74, 204, 76, 0xFF58B8FFu);
    rt.fb_draw_text(line1, 120 - rt.fb_measure_text(line1) / 2, 96, hdr_col);
    rt.fb_draw_text(line2, 120 - rt.fb_measure_text(line2) / 2, 118, 0xFFFFFFFFu);
    rt.fb_draw_text(line3, 120 - rt.fb_measure_text(line3) / 2, 140, 0xFF88CCFFu);
}

inline void render_match_result_overlay(DojaRuntime& rt,
                                        bool is_draw,
                                        bool is_win,
                                        int result_ticks,
                                        bool is_triple_battle,
                                        uint8_t loc_wins,
                                        uint8_t opp_wins) {
    uint32_t border_col = is_draw ? 0xFFFFD866u : (is_win ? 0xFF58FF98u : 0xFFFF6070u);
    std::string headline = is_draw ? "DRAW GAME - 平局!" : (is_win ? "YOU WIN!! 对战胜利!" : "YOU LOSE... 对战落败");
    std::string subline = "按 决定键 返回游戏 (" + std::to_string(std::max(0, (kResultAutoExitTicks - result_ticks) / 16)) + "s)";

    if (is_triple_battle) {
        std::string series_line = "三回战总比分 " + std::to_string(loc_wins) + " - " + std::to_string(opp_wins);
        rt.fb_fill_rect_alpha(20, 82, 200, 66, 0xEE0C162Cu);
        rt.fb_draw_border(20, 82, 200, 66, border_col);
        rt.fb_draw_text(headline, 120 - rt.fb_measure_text(headline) / 2, 104, border_col);
        rt.fb_draw_text(series_line, 120 - rt.fb_measure_text(series_line) / 2, 122, 0xFFFFE866u);
        rt.fb_draw_text(subline, 120 - rt.fb_measure_text(subline) / 2, 140, 0xFFD8E6FFu);
    } else {
        rt.fb_fill_rect_alpha(20, 86, 200, 56, 0xEE0C162Cu);
        rt.fb_draw_border(20, 86, 200, 56, border_col);
        rt.fb_draw_text(headline, 120 - rt.fb_measure_text(headline) / 2, 110, border_col);
        rt.fb_draw_text(subline, 120 - rt.fb_measure_text(subline) / 2, 132, 0xFFD8E6FFu);
    }
}

// ----------------------------------------------------------------------------
// Shared Loopback Relay & RAII Guards for Self-Test Suites
// ----------------------------------------------------------------------------

struct LoopbackRelay {
    plat::sock_t tcp_fd = plat::kInvalidSocket;
    plat::sock_t udp_fd = plat::kInvalidSocket;
    uint16_t port = 0;
    std::atomic<bool> stop_flag{false};
    pvp::PvpServerEngine engine{};
    std::thread worker{};

    bool start() {
        plat::ensure_sockets_init();
        for (int attempt = 0; attempt < 8; ++attempt) {
            plat::sock_t tfd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (tfd == plat::kInvalidSocket) return false;
            plat::set_socket_reuseaddr(tfd);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (::bind(tfd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
                ::listen(tfd, 16) != 0) {
                plat::close_socket(tfd);
                continue;
            }
            sockaddr_in bound{};
            socklen_t blen = sizeof(bound);
            if (::getsockname(tfd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
                plat::close_socket(tfd);
                continue;
            }
            uint16_t p = ntohs(bound.sin_port);

            plat::sock_t ufd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (ufd == plat::kInvalidSocket) {
                plat::close_socket(tfd);
                continue;
            }
            plat::set_socket_reuseaddr(ufd);
            sockaddr_in uaddr{};
            uaddr.sin_family = AF_INET;
            uaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            uaddr.sin_port = htons(p);
            if (::bind(ufd, reinterpret_cast<const sockaddr*>(&uaddr), sizeof(uaddr)) != 0) {
                plat::close_socket(ufd);
                plat::close_socket(tfd);
                continue;
            }
            plat::set_socket_nonblocking(tfd, true);
            plat::set_socket_nonblocking(ufd, true);
            plat::disable_udp_connreset(ufd);
            tcp_fd = tfd;
            udp_fd = ufd;
            port = p;
            engine.set_udp_fd(udp_fd);
            worker = std::thread([this]() { run(); });
            return true;
        }
        return false;
    }

    void run() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            pollfd pfds[2] = {
                pollfd{tcp_fd, POLLIN, 0},
                pollfd{udp_fd, POLLIN, 0},
            };
            int rc = plat::poll_sockets(pfds, 2, 5);
            if (rc <= 0) continue;

            if (pfds[1].revents & POLLIN) {
                uint8_t ubuf[1500];
                while (true) {
                    sockaddr_storage src{};
                    socklen_t slen = sizeof(src);
                    ptrdiff_t n = plat::sock_recvfrom(
                        udp_fd, ubuf, sizeof(ubuf), reinterpret_cast<sockaddr*>(&src), &slen);
                    if (n <= 0) break;
                    engine.handle_udp_datagram(udp_fd, ubuf, static_cast<size_t>(n), src, slen);
                }
            }

            if (pfds[0].revents & POLLIN) {
                while (true) {
                    sockaddr_in caddr{};
                    socklen_t clen = sizeof(caddr);
                    plat::sock_t cfd = ::accept(tcp_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
                    if (cfd == plat::kInvalidSocket) break;
                    std::string req;
                    uint8_t rbuf[2048];
                    for (int w = 0; w < 20; ++w) {
                        pollfd cpfd{cfd, POLLIN, 0};
                        if (plat::poll_sockets(&cpfd, 1, 5) > 0 && (cpfd.revents & POLLIN)) {
                            ptrdiff_t rn = plat::sock_recv(cfd, rbuf, sizeof(rbuf));
                            if (rn > 0) {
                                req.append(reinterpret_cast<const char*>(rbuf), static_cast<size_t>(rn));
                                if (req.find("\r\n\r\n") != std::string::npos) break;
                            } else {
                                break;
                            }
                        }
                    }
                    std::istringstream iss(req);
                    std::string method, uri;
                    if (iss >> method >> uri) {
                        net::HttpResponse resp = engine.handle_http(method, uri);
                        const char* reason = (resp.status_code == 200) ? "OK" :
                                             (resp.status_code == 404) ? "Not Found" : "Bad Request";
                        std::ostringstream out;
                        out << "HTTP/1.1 " << resp.status_code << " " << reason << "\r\n"
                            << "Content-Type: application/json\r\n"
                            << "Content-Length: " << resp.body.size() << "\r\n"
                            << "Connection: close\r\n\r\n";
                        std::string hdr = out.str();
                        std::vector<uint8_t> wire(hdr.begin(), hdr.end());
                        wire.insert(wire.end(), resp.body.begin(), resp.body.end());
                        size_t sent = 0;
                        while (sent < wire.size()) {
                            ptrdiff_t sn = plat::sock_send(cfd, wire.data() + sent, wire.size() - sent);
                            if (sn <= 0) break;
                            sent += static_cast<size_t>(sn);
                        }
                    }
                    plat::close_socket(cfd);
                }
            }
        }
    }

    std::string addr() const { return "127.0.0.1:" + std::to_string(port); }

    ~LoopbackRelay() {
        stop_flag.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
        if (udp_fd != plat::kInvalidSocket) plat::close_socket(udp_fd);
        if (tcp_fd != plat::kInvalidSocket) plat::close_socket(tcp_fd);
    }
};

struct SelfTestFlagGuard {
    DojaRuntime& rt;
    explicit SelfTestFlagGuard(DojaRuntime& r) : rt(r) { rt.set_pvp_self_test(false); }
    ~SelfTestFlagGuard() { rt.set_pvp_self_test(true); }
};

struct DefaultSingleFormatGuard {
    pvp::PvpClientSession& sess;
    uint8_t prev;
    explicit DefaultSingleFormatGuard(pvp::PvpClientSession& s)
        : sess(s), prev(s.preferred_format()) {
        sess.set_preferred_format(pvp::PVP_FORMAT_SINGLE);
    }
    ~DefaultSingleFormatGuard() {
        sess.set_preferred_format(prev);
    }
};

} // namespace rmexe::pvp_common

#endif // RMEXE_PVP_COMMON_HPP
