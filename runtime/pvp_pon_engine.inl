// ============================================================================
// runtime/pvp_pon_engine.inl
// Symmetric 2-Player Online PvP NetBattle Engine for Rockman EXE PoN (JP/EN)
// Included inside namespace rmexe at the bottom of *_gen.cpp when is_pon_pvp.
// ============================================================================

} // namespace rmexe

#include "pvp_protocol.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace rmexe {
static bool pvp_pon_tick_override_impl(DojaRuntime& rt, Ref cp);

namespace pvp_pon {

enum class Phase : uint8_t {
    IDLE = 0,
    LOBBY_WAIT,
    HANDSHAKE,
    CUSTOM_SCREEN,
    ROUND_BANNER,
    COMBAT,
    JUDGE,
    ROUND_RESULT,
    RESULT,
};

struct SideContext {
    Ref rock_ref = 0;
    Ref ata_arr = 0;
    Ref fol_ref = 0;
    Ref tip_list_ref = 0;
    Ref skill_kouka_ref = 0;
    Ref rock_pow_ref = 0;
    Ref rock_ani_id_ref = 0;
    Ref rock_ani_pt_ref = 0;

    // CpCanvas per-player combat stats
    int32_t max_hp = 100;
    int32_t now_hp = 100;
    int32_t ata_lv = 1;
    int32_t cha_lv = 1;
    int32_t r_zoku = 0;
    int32_t max_sel = 5;
    int32_t regu_flg = 0;
    int32_t sec_cha = 0;
    int32_t pa_no = -1;
    int32_t key_cnt = 0;
    int32_t cus_sp = 1;
    int32_t combo = 1;
    int32_t combo_time = 0;
    int32_t ata_cnt = 0;
    int32_t rika_cnt = 0;
    int32_t over_cnt = 0;
    int32_t combo_cnt = 0;
    int32_t wana_pow0 = 0;

    // Static app/Rock.* fields swapped per side
    int32_t air = 0;
    int32_t ani_cnt = 0;
    int32_t ani_cnt_max = 0;
    int32_t ani_max = 0;
    int32_t ani_ok = 0;
    int32_t ani_wait_cnt = 0;
    int32_t bari = 0;
    int32_t bari_id = -1;
    int32_t bas_no = -1;
    int32_t bas_pow = 1;
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t e_wait = 0;
    int32_t han = 0;
    int32_t han_id = -1;
    int32_t hit_cnt = 0;
    int32_t hp = 100;
    int32_t joutai = 0;
    int32_t mahi_id = -1;
    int32_t move = 0;
    int32_t move_cnt = 0;
    int32_t move_flg = 0;
    int32_t muteki_cnt = 0;
    int32_t next_tip = 0;
    int32_t noke_cnt = 0;
    int32_t non_bas = 0;
    int32_t non_cnt = 0;
    int32_t non_cnt2 = 0;
    int32_t pane_id = 0;
    int32_t pos_x = 1;
    int32_t pos_y = 1;
    int32_t r_e = 0;
    int32_t r_s = 0;
    int32_t r_suu = 0;
    int32_t s_wait = 0;
    int32_t syoumetu_cnt = 0;
    int32_t tip_no = -1;
    int32_t tmp_ani_cnt = 0;
    int32_t tmp_ata = -1;
    int32_t tmp_move = 0;
    int32_t tmp_muteki_cnt = 0;
    int32_t tmp_x = 1;
    int32_t tmp_y = 1;
    int32_t wana = 0;
    int32_t waza_flg = -1;
    int32_t waza_wait = 0;
    int32_t zoku = 0;

    uint32_t fol_used_mask = 0;
    int32_t prev_input_mask = 0;
    int16_t nondim_tip_id = -1;
    int16_t nondim_base_pow = -1; // >= 0 when Tip.field_1 < 3 (damaging), -1 for support
    int16_t nondim_plus_pow = 0;  // Captured Rock_pow[0] (+Atk boost) when nondim_base_pow >= 0
    uint8_t nondim_ticks = 0;     // Remaining 16Hz combat ticks
};

struct PvpCombatCore {
    SideContext sides[2];
    int board_orient = 0;       // 0 = P0 canonical left, 1 = P1 mirrored left
    int stop_owner = -1;        // Side index currently owning time-freeze
    uint32_t combat_turn = 0;   // Deterministic 16Hz combat tick counter
    uint16_t custom_round = 0;  // Increments each Custom Screen phase (1, 2, ...)
    int32_t shared_cus_sp = 1;  // Combined Custom Gauge fill speed
    bool open_custom_req = false;
    int winner = -2;            // -2 = ongoing, -1 = draw, 0 = P0 win, 1 = P1 win
    int32_t damage_dealt[2] = {0, 0};
    uint16_t final_turn_ticks_left = 0;
    bool ended_by_judge = false;
};

struct RollbackSlot {
    uint32_t turn = UINT32_MAX;
    int32_t p0_mask = 0;
    int32_t p1_mask = 0;
    bool p1_predicted = false;
    uint16_t pre_hash = 0;
    PvpCombatCore core;
    std::vector<int64_t> statics;
    FastCombatSnapshot heap_snap;
};

static constexpr size_t kRollbackWindow = 128;
static constexpr uint32_t kMaxPredictionFrames = 12; // 0.75s max speculation cap at 16 Hz (prevents multi-second drift!)
static constexpr size_t kInputHistorySize = 4096;
static constexpr uint32_t kInputDelayFrames = 1;
static constexpr uint16_t kFinalTurnDurationTicks = 10 * 16; // 160 ticks at 16 Hz
static constexpr uint8_t kOppNonDimChipHudTicks = 16; // 1.0s at 16 Hz

// Rollback clock-drift throttler ported from HCTOrganization/trill
// (getgud::Session local_tick_advantage / skew / speculation_balance + trill-pvp/src/battle/throttler.rs)
// adapted to PoN's native 62ms (16.129 Hz) CpCanvas.exe() frame clock.
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

struct PvpEngineState {
    Phase phase = Phase::IDLE;
    uint32_t active_room_id = 0;
    int local_idx = 0; // 0 = Host (Left/Red), 1 = Joiner (Right/Blue)
    uint32_t match_seed = 0x13579BDFu;
    size_t mutable_heap_min_ref = 1;
    Ref trap_mask_str = 0;

    // Pre-match full VM snapshot for clean restoration after match ends
    bool has_pre_match_snap = false;
    FullHeapSnapshot pre_match_heap;
    std::vector<int64_t> pre_match_statics;

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
    PvpCombatCore pre_custom_core{};
    std::vector<int64_t> pre_custom_statics{};
    FastCombatSnapshot pre_custom_snap{};

    // Lockstep & GGPO Rollback state
    PvpCombatCore core{};
    std::array<int32_t, kInputHistorySize> local_inputs{};
    std::array<int32_t, kInputHistorySize> predicted_remote{};
    std::array<bool, kInputHistorySize> remote_confirmed{};
    std::array<RollbackSlot, kRollbackWindow> rb_ring{};
    struct TurnEmittedSe {
        uint32_t turn = UINT32_MAX;
        uint8_t count = 0;
        std::array<int32_t, 8> se_ids{};
    };
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

    // Re-entrancy flags for Ene.Hit hook
    bool in_combat_step = false;
    int active_side = 0;
    Ref active_ata_ref = 0;
    int bot_custom_step = 0;
};

static PvpEngineState g_pvp_eng;

static uint32_t derive_bo3_game_seed(uint32_t base_seed, uint8_t game_idx) {
    if (game_idx == 0) return base_seed ? base_seed : 1u;
    uint32_t s = base_seed ^ (static_cast<uint32_t>(game_idx) * 0x9E3779B9u + 0x7F4A7C15u);
    s ^= (s >> 16);
    s *= 0x85EBCA6Bu;
    s ^= (s >> 13);
    return s | 1u;
}

static inline uint16_t current_wire_round_key() {
    return static_cast<uint16_t>(static_cast<uint16_t>(g_pvp_eng.bo3_game_index) * 100u +
                                 g_pvp_eng.core.custom_round);
}

static int64_t steady_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

static uint32_t read_folder_used_mask(DojaRuntime& rt, Ref fol_obj) {
    if (fol_obj <= 0) {
        return 0;
    }
    Ref used_arr = rt.field_i32(fol_obj, 1);
    if (used_arr <= 0) {
        return 0;
    }
    int32_t n = std::min<int32_t>(30, rt.array_length(used_arr));
    uint32_t mask = 0;
    for (int32_t i = 0; i < n; ++i) {
        if (rt.iaload(used_arr, i) != 0) {
            mask |= (1u << i);
        }
    }
    return mask;
}

static void sync_folder_used_from_mask(DojaRuntime& rt, Ref fol_obj, uint32_t mask) {
    if (fol_obj <= 0) {
        return;
    }
    mask &= 0x3FFFFFFFu;
    Ref used_arr = rt.field_i32(fol_obj, 1);
    if (used_arr <= 0) {
        return;
    }
    int32_t n = std::min<int32_t>(30, rt.array_length(used_arr));
    int32_t zan = 30;
    for (int32_t i = 0; i < n; ++i) {
        int32_t u = ((mask >> i) & 1u) ? 1 : 0;
        rt.iaload(used_arr, i) = u;
        if (u) {
            --zan;
        }
    }
    rt.field_i32(fol_obj, 2) = zan;
}

static uint32_t compute_custom_tray_slot_mask(DojaRuntime& rt) {
    if (sf_app_CpCanvas_cas_tip <= 0) {
        return 0;
    }
    int32_t clen = std::min<int32_t>(10, rt.array_length(sf_app_CpCanvas_cas_tip));
    uint32_t mask = 0;
    for (int32_t i = 0; i < clen; ++i) {
        int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, i);
        if (ct >= 1 && ct <= 30) {
            mask |= (1u << (ct - 1));
        }
    }
    return mask;
}

// Keep cas_tip[0..9] (1..30 or -1), fol_tip_no[0..9] (0..29 or -1), cas_ok[0..9], and Folder.used[0..29]
// strictly synchronized so empty tray slots can never be selected or shuffled back into Folder.used,
// and permanently consumed chips can never reappear across rounds or Shuffle.
static void sanitize_custom_tray_and_folder(DojaRuntime& rt, uint32_t& perm_consumed_mask) {
    if (sf_app_CpCanvas_cas_tip > 0) {
        int32_t clen = rt.array_length(sf_app_CpCanvas_cas_tip);
        int32_t flen = (sf_app_CpCanvas_fol_tip_no > 0) ? rt.array_length(sf_app_CpCanvas_fol_tip_no) : 0;
        int32_t olen = (sf_app_CpCanvas_cas_ok > 0) ? rt.array_length(sf_app_CpCanvas_cas_ok) : 0;
        for (int32_t i = 0; i < clen; ++i) {
            int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, i);
            if (i < 10 && ct >= 1 && ct <= 30) {
                if (i < flen) {
                    rt.iaload(sf_app_CpCanvas_fol_tip_no, i) = ct - 1;
                }
            } else {
                rt.iaload(sf_app_CpCanvas_cas_tip, i) = -1;
                if (i < flen) {
                    rt.iaload(sf_app_CpCanvas_fol_tip_no, i) = -1;
                }
                if (i < olen) {
                    rt.iaload(sf_app_CpCanvas_cas_ok, i) = 0;
                }
            }
        }
    }
    uint32_t tray_mask = compute_custom_tray_slot_mask(rt);
    Ref fol_obj = (sf_app_CpCanvas_fol > 0) ? rt.iaload(sf_app_CpCanvas_fol, 0) : 0;
    if (sf_app_CpCanvas_cas_cnt > 0 || sf_app_CpCanvas_pa_cnt2 > 0 || sf_app_CpCanvas_scene == 1) {
        perm_consumed_mask = (perm_consumed_mask | (read_folder_used_mask(rt, fol_obj) & ~tray_mask)) & 0x3FFFFFFFu;
    }
    uint32_t authoritative_used = (perm_consumed_mask | tray_mask) & 0x3FFFFFFFu;
    sync_folder_used_from_mask(rt, fol_obj, authoritative_used);
    if (sf_app_CpCanvas_regu_flg != 0 && fol_obj > 0) {
        int32_t regu_slot = rt.field_i32(fol_obj, 8);
        if (regu_slot < 0 || regu_slot >= 30 || ((authoritative_used >> regu_slot) & 1u) != 0) {
            sf_app_CpCanvas_regu_flg = 0;
        }
    }
}

static void save_side(DojaRuntime& rt, int idx) {
    SideContext& s = g_pvp_eng.core.sides[idx];
    s.rock_ref = sf_app_CpCanvas_rock;
    s.ata_arr = sf_app_CpCanvas_ata;
    s.tip_list_ref = sf_app_CpCanvas_tip_list;
    s.skill_kouka_ref = sf_app_CpCanvas_skill_kouka;
    if (sf_app_CpCanvas_fol > 0) {
        s.fol_ref = rt.iaload(sf_app_CpCanvas_fol, 0);
    }
    if (s.fol_ref > 0) {
        s.fol_used_mask = read_folder_used_mask(rt, s.fol_ref);
    }
    s.rock_pow_ref = sf_app_Rock_pow;
    s.rock_ani_id_ref = sf_app_Rock_ani_id;
    s.rock_ani_pt_ref = sf_app_Rock_ani_pt;

    s.max_hp = sf_app_CpCanvas_max_hp;
    s.now_hp = sf_app_Rock_hp;
    s.ata_lv = sf_app_CpCanvas_ata_lv;
    s.cha_lv = sf_app_CpCanvas_cha_lv;
    s.r_zoku = sf_app_Edit_r_zoku;
    s.max_sel = sf_app_CpCanvas_max_sel;
    s.regu_flg = sf_app_CpCanvas_regu_flg;
    s.sec_cha = sf_app_CpCanvas_sec_cha;
    s.pa_no = sf_app_CpCanvas_pa_no;
    s.key_cnt = sf_app_CpCanvas_key_cnt;
    s.combo = sf_app_CpCanvas_combo;
    s.combo_time = sf_app_CpCanvas_combo_time;
    s.ata_cnt = sf_app_CpCanvas_ata_cnt;
    s.rika_cnt = sf_app_CpCanvas_rika_cnt;
    s.over_cnt = sf_app_CpCanvas_over_cnt;
    s.combo_cnt = sf_app_CpCanvas_combo_cnt;
    if (sf_app_CpCanvas_wana_pow > 0 && rt.array_length(sf_app_CpCanvas_wana_pow) > 0) {
        s.wana_pow0 = rt.iaload(sf_app_CpCanvas_wana_pow, 0);
    }

    s.air = sf_app_Rock_air;
    s.ani_cnt = sf_app_Rock_ani_cnt;
    s.ani_cnt_max = sf_app_Rock_ani_cnt_max;
    s.ani_max = sf_app_Rock_ani_max;
    s.ani_ok = sf_app_Rock_ani_ok;
    s.ani_wait_cnt = sf_app_Rock_ani_wait_cnt;
    s.bari = sf_app_Rock_bari;
    s.bari_id = sf_app_Rock_bari_id;
    s.bas_no = sf_app_Rock_bas_no;
    s.bas_pow = sf_app_Rock_bas_pow;
    s.dx = sf_app_Rock_dx;
    s.dy = sf_app_Rock_dy;
    s.e_wait = sf_app_Rock_e_wait;
    s.han = sf_app_Rock_han;
    s.han_id = sf_app_Rock_han_id;
    s.hit_cnt = sf_app_Rock_hit_cnt;
    s.hp = sf_app_Rock_hp;
    s.joutai = sf_app_Rock_joutai;
    s.mahi_id = sf_app_Rock_mahi_id;
    s.move = sf_app_Rock_move;
    s.move_cnt = sf_app_Rock_move_cnt;
    s.move_flg = sf_app_Rock_move_flg;
    s.muteki_cnt = sf_app_Rock_muteki_cnt;
    s.next_tip = sf_app_Rock_next_tip;
    s.noke_cnt = sf_app_Rock_noke_cnt;
    s.non_bas = sf_app_Rock_non_bas;
    s.non_cnt = sf_app_Rock_non_cnt;
    s.non_cnt2 = sf_app_Rock_non_cnt2;
    s.pane_id = sf_app_Rock_pane_id;
    s.pos_x = sf_app_Rock_pos_x;
    s.pos_y = sf_app_Rock_pos_y;
    s.r_e = sf_app_Rock_r_e;
    s.r_s = sf_app_Rock_r_s;
    s.r_suu = sf_app_Rock_r_suu;
    s.s_wait = sf_app_Rock_s_wait;
    s.syoumetu_cnt = sf_app_Rock_syoumetu_cnt;
    s.tip_no = sf_app_Rock_tip_no;
    s.tmp_ani_cnt = sf_app_Rock_tmp_ani_cnt;
    s.tmp_ata = sf_app_Rock_tmp_ata;
    s.tmp_move = sf_app_Rock_tmp_move;
    s.tmp_muteki_cnt = sf_app_Rock_tmp_muteki_cnt;
    s.tmp_x = sf_app_Rock_tmp_x;
    s.tmp_y = sf_app_Rock_tmp_y;
    s.wana = sf_app_Rock_wana;
    s.waza_flg = sf_app_Rock_waza_flg;
    s.waza_wait = sf_app_Rock_waza_wait;
    s.zoku = sf_app_Rock_zoku;
}

static void load_side(DojaRuntime& rt, int idx) {
    const SideContext& s = g_pvp_eng.core.sides[idx];
    sf_app_CpCanvas_rock = s.rock_ref;
    sf_app_CpCanvas_ata = s.ata_arr;
    sf_app_CpCanvas_tip_list = s.tip_list_ref;
    sf_app_CpCanvas_skill_kouka = s.skill_kouka_ref;
    sf_app_CpCanvas_now_fol = 0;
    if (sf_app_CpCanvas_fol > 0 && s.fol_ref > 0) {
        rt.iaload(sf_app_CpCanvas_fol, 0) = s.fol_ref;
    }
    if (s.fol_ref > 0) {
        sync_folder_used_from_mask(rt, s.fol_ref, s.fol_used_mask);
    }
    sf_app_Rock_pow = s.rock_pow_ref;
    sf_app_Rock_ani_id = s.rock_ani_id_ref;
    sf_app_Rock_ani_pt = s.rock_ani_pt_ref;

    sf_app_CpCanvas_max_hp = s.max_hp;
    sf_app_CpCanvas_now_hp = s.hp;
    sf_app_CpCanvas_ata_lv = s.ata_lv;
    sf_app_CpCanvas_cha_lv = s.cha_lv;
    sf_app_Edit_r_zoku = s.r_zoku;
    sf_app_CpCanvas_max_sel = s.max_sel;
    sf_app_CpCanvas_regu_flg = s.regu_flg;
    sf_app_CpCanvas_sec_cha = s.sec_cha;
    sf_app_CpCanvas_pa_no = s.pa_no;
    sf_app_CpCanvas_key_cnt = s.key_cnt;
    sf_app_CpCanvas_cus_sp = g_pvp_eng.core.shared_cus_sp;
    sf_app_CpCanvas_combo = s.combo;
    sf_app_CpCanvas_combo_time = s.combo_time;
    sf_app_CpCanvas_ata_cnt = s.ata_cnt;
    sf_app_CpCanvas_rika_cnt = s.rika_cnt;
    sf_app_CpCanvas_over_cnt = s.over_cnt;
    sf_app_CpCanvas_combo_cnt = s.combo_cnt;
    if (sf_app_CpCanvas_wana_pow > 0 && rt.array_length(sf_app_CpCanvas_wana_pow) > 0) {
        rt.iaload(sf_app_CpCanvas_wana_pow, 0) = s.wana_pow0;
    }

    sf_app_Rock_air = s.air;
    sf_app_Rock_ani_cnt = s.ani_cnt;
    sf_app_Rock_ani_cnt_max = s.ani_cnt_max;
    sf_app_Rock_ani_max = s.ani_max;
    sf_app_Rock_ani_ok = s.ani_ok;
    sf_app_Rock_ani_wait_cnt = s.ani_wait_cnt;
    sf_app_Rock_bari = s.bari;
    sf_app_Rock_bari_id = s.bari_id;
    sf_app_Rock_bas_no = s.bas_no;
    sf_app_Rock_bas_pow = s.bas_pow;
    sf_app_Rock_dx = s.dx;
    sf_app_Rock_dy = s.dy;
    sf_app_Rock_e_wait = s.e_wait;
    sf_app_Rock_han = s.han;
    sf_app_Rock_han_id = s.han_id;
    sf_app_Rock_hit_cnt = s.hit_cnt;
    sf_app_Rock_hp = s.hp;
    sf_app_Rock_joutai = s.joutai;
    sf_app_Rock_mahi_id = s.mahi_id;
    sf_app_Rock_move = s.move;
    sf_app_Rock_move_cnt = s.move_cnt;
    sf_app_Rock_move_flg = s.move_flg;
    sf_app_Rock_muteki_cnt = s.muteki_cnt;
    sf_app_Rock_next_tip = s.next_tip;
    sf_app_Rock_noke_cnt = s.noke_cnt;
    sf_app_Rock_non_bas = s.non_bas;
    sf_app_Rock_non_cnt = s.non_cnt;
    sf_app_Rock_non_cnt2 = s.non_cnt2;
    sf_app_Rock_pane_id = s.pane_id;
    sf_app_Rock_pos_x = s.pos_x;
    sf_app_Rock_pos_y = s.pos_y;
    sf_app_Rock_r_e = s.r_e;
    sf_app_Rock_r_s = s.r_s;
    sf_app_Rock_r_suu = s.r_suu;
    sf_app_Rock_s_wait = s.s_wait;
    sf_app_Rock_syoumetu_cnt = s.syoumetu_cnt;
    sf_app_Rock_tip_no = s.tip_no;
    sf_app_Rock_tmp_ani_cnt = s.tmp_ani_cnt;
    sf_app_Rock_tmp_ata = s.tmp_ata;
    sf_app_Rock_tmp_move = s.tmp_move;
    sf_app_Rock_tmp_muteki_cnt = s.tmp_muteki_cnt;
    sf_app_Rock_tmp_x = s.tmp_x;
    sf_app_Rock_tmp_y = s.tmp_y;
    sf_app_Rock_wana = s.wana;
    sf_app_Rock_waza_flg = s.waza_flg;
    sf_app_Rock_waza_wait = s.waza_wait;
    sf_app_Rock_zoku = s.zoku;
}

// Mirror 6x3 battle grid & field obstacles horizontally between P0 and P1 perspectives
static void mirror_board(DojaRuntime& rt) {
    if (sf_app_CpCanvas_panel > 0) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            for (int x = 0; x < 3; ++x) {
                int mx = 5 - x;
                Ref pL = rt.iaload(row, x);
                Ref pR = rt.iaload(row, mx);
                rt.iaload(row, x) = pR;
                rt.iaload(row, mx) = pL;

                Ref pair[2] = {pR, pL};
                int xs[2] = {x, mx};
                for (int k = 0; k < 2; ++k) {
                    Ref p = pair[k];
                    int nx = xs[k];
                    rt.field_i32(p, 0) = y * 6 + nx; // id
                    rt.field_i32(p, 1) = nx;         // pos_x
                    rt.field_i32(p, 3) = 1 - rt.field_i32(p, 3); // jin (Red <-> Blue)
                    int32_t oc = rt.field_i32(p, 9);
                    if (oc == 1) rt.field_i32(p, 9) = 2;
                    else if (oc == 2) rt.field_i32(p, 9) = 1;
                    else if (oc >= 10 && oc < 14) rt.field_i32(p, 9) = 10 + ((oc - 10) ^ 2);
                }
            }
        }
    }
    if (sf_app_CpCanvas_oki > 0) {
        // Swap owner obstacle slots oki[0..1] <-> oki[2..3] so each player owns oki[0..1] in their own view
        for (int k = 0; k < 2; ++k) {
            Ref okA = rt.iaload(sf_app_CpCanvas_oki, k);
            Ref okB = rt.iaload(sf_app_CpCanvas_oki, k + 2);
            rt.iaload(sf_app_CpCanvas_oki, k) = okB;
            rt.iaload(sf_app_CpCanvas_oki, k + 2) = okA;
        }
        for (int i = 0; i < 4; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
            if (ok <= 0) continue;
            rt.field_i32(ok, 0) = i; // oki_id
            int32_t nx_rep = rt.field_i32(ok, 20);
            if (nx_rep >= 0 && nx_rep < 4) {
                rt.field_i32(ok, 20) = nx_rep ^ 2;
            }
        }
        for (int i = 0; i < 6; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
            if (ok <= 0 || rt.field_i32(ok, 1) == 0) continue;
            rt.field_i32(ok, 9) = 5 - rt.field_i32(ok, 9);    // pos_x (CpCanvas.drawImg2 already reverses pos_dx via flp)
            rt.field_i32(ok, 13) = 1 - rt.field_i32(ok, 13);  // flp
            int32_t r_ata = rt.field_i32(ok, 5);
            int32_t t_ata = rt.field_i32(ok, 6);
            rt.field_i32(ok, 5) = t_ata;
            rt.field_i32(ok, 6) = r_ata;
        }
    }
}

static void ensure_board_orient(DojaRuntime& rt, int target_side) {
    if (g_pvp_eng.core.board_orient != target_side) {
        mirror_board(rt);
        g_pvp_eng.core.board_orient = target_side;
    }
}

static void sync_panel_occupancy_and_proxy(DojaRuntime& rt, int active_idx) {
    const SideContext& act = g_pvp_eng.core.sides[active_idx];
    const SideContext& opp = g_pvp_eng.core.sides[1 - active_idx];

    int opp_mx = std::clamp(5 - opp.pos_x, 0, 5);
    int opp_my = std::clamp(opp.pos_y, 0, 2);

    // Refresh panel[y][x].on_chara cleanly
    if (sf_app_CpCanvas_panel > 0) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                rt.field_i32(p, 9) = 0;
            }
        }
        if (sf_app_CpCanvas_oki > 0) {
            for (int i = 0; i < 6; ++i) {
                Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                if (ok > 0 && rt.field_i32(ok, 1) != 0) {
                    int ox = rt.field_i32(ok, 9);
                    int oy = rt.field_i32(ok, 10);
                    if (ox >= 0 && ox < 6 && oy >= 0 && oy < 3) {
                        rt.field_i32(rt.iaload(rt.iaload(sf_app_CpCanvas_panel, oy), ox), 9) = 10 + i;
                    }
                }
            }
        }
        if (opp.hp > 0) {
            rt.field_i32(rt.iaload(rt.iaload(sf_app_CpCanvas_panel, opp_my), opp_mx), 9) = 2;
        }
        int ax = std::clamp(sf_app_Rock_pos_x, 0, 5);
        int ay = std::clamp(sf_app_Rock_pos_y, 0, 2);
        if (act.hp > 0) {
            rt.field_i32(rt.iaload(rt.iaload(sf_app_CpCanvas_panel, ay), ax), 9) = 1;
        }
    }

    // Configure ene[0] as hitboxes/targeting proxy for opponent Rock
    if (sf_app_CpCanvas_ene > 0) {
        for (int i = 1; i < 3; ++i) {
            Ref e = rt.iaload(sf_app_CpCanvas_ene, i);
            if (e > 0) rt.field_i32(e, 23) = 0; // on = 0
        }
        Ref e0 = rt.iaload(sf_app_CpCanvas_ene, 0);
        if (e0 > 0) {
            rt.field_i32(e0, 23) = (opp.hp > 0) ? 1 : 0; // on
            rt.field_i32(e0, 3)  = std::max(0, opp.hp);  // hp
            rt.field_i32(e0, 4)  = opp.max_hp;           // max_hp
            rt.field_i32(e0, 5)  = 0;                    // zoku=0 (Rock.Hit applies opp.zoku once)
            rt.field_i32(e0, 11) = opp_mx;               // pos_x
            rt.field_i32(e0, 12) = opp_my;               // pos_y
            rt.field_i32(e0, 13) = -opp.dx;              // dx (mirrored)
            rt.field_i32(e0, 14) = opp.dy;               // dy
            rt.field_i32(e0, 15) = std::clamp(5 - opp.tmp_x, 0, 5);
            rt.field_i32(e0, 16) = std::clamp(opp.tmp_y, 0, 2);
            rt.field_i32(e0, 19) = (opp.move_flg != 0) ? 1 : 0; // move_flg (Wind/Fan push guard)
            rt.field_i32(e0, 24) = opp.joutai;
            rt.field_i32(e0, 25) = (opp.han > 0) ? 0 : opp.muteki_cnt; // Allow Ene.Hit intercept during Guard/CrsShld/Curse
            rt.field_i32(e0, 26) = opp.tmp_muteki_cnt;
            rt.field_i32(e0, 41) = 10;                   // Navi-class enemy id
            rt.field_i32(e0, 45) = 0;                    // bari handled in Rock.Hit
            rt.field_i32(e0, 46) = -1;
            rt.field_i32(e0, 47) = -1;
            rt.field_i32(e0, 48) = 0;                    // han handled in intercept
            rt.field_i32(e0, 49) = 0;                    // pane_id handled in Rock.Hit
            rt.field_i32(e0, 53) = opp.air;
            rt.field_i32(e0, 56) = 1;                    // navi_flg = 1
            rt.field_i32(e0, 58) = (opp.syoumetu_cnt > 0) ? 1 : 0;
        }
    }
}

static pvp::PvpFighterBuild extract_local_build_from_sav(DojaRuntime& rt, Ref cp) {
    bool prev_sup = rt.suppress_av();
    rt.set_suppress_av(true);

    if (sf_app_CpCanvas_max_hp <= 0) {
        fn_app_CpCanvas_init___I__V(rt, cp, 1);
        fn_app_CpCanvas_Load___I__V(rt, cp, 0);
        if (sf_app_CpCanvas_max_hp <= 0) {
            fn_app_CpCanvas_init___I__V(rt, cp, 1);
        }
    }

    pvp::PvpFighterBuild b{};
    b.max_hp = static_cast<uint16_t>(std::clamp(sf_app_CpCanvas_max_hp, 100, 1000));
    b.ata_lv = static_cast<uint8_t>(std::clamp(sf_app_CpCanvas_ata_lv, 1, 5));
    b.cha_lv = static_cast<uint8_t>(std::clamp(sf_app_CpCanvas_cha_lv, 1, 5));
    b.r_zoku = static_cast<uint8_t>(std::clamp(sf_app_Edit_r_zoku, 0, 4));

    if (sf_app_CpCanvas_skill_kouka > 0) {
        int32_t slen = std::min<int32_t>(29, rt.array_length(sf_app_CpCanvas_skill_kouka));
        for (int i = 0; i < slen; ++i) {
            b.skill_kouka[i] = static_cast<int8_t>(std::clamp(rt.iaload(sf_app_CpCanvas_skill_kouka, i), -127, 127));
        }
    }

    int32_t fidx = std::clamp(sf_app_CpCanvas_now_fol, 0, 2);
    Ref fol_obj = (sf_app_CpCanvas_fol > 0 && fidx < rt.array_length(sf_app_CpCanvas_fol))
        ? rt.iaload(sf_app_CpCanvas_fol, fidx)
        : 0;
    if (fol_obj <= 0 && sf_app_CpCanvas_fol > 0) {
        fol_obj = rt.iaload(sf_app_CpCanvas_fol, 0);
    }

    bool folder_valid = false;
    if (fol_obj > 0 && sf_app_CpCanvas_tip_list > 0) {
        Ref tip_id_arr = rt.field_i32(fol_obj, 0);
        int32_t tlist_len = rt.array_length(sf_app_CpCanvas_tip_list);
        if (tip_id_arr > 0 && rt.array_length(tip_id_arr) >= 30) {
            int valid_cnt = 0;
            for (int i = 0; i < 30; ++i) {
                int32_t slot_idx = rt.iaload(tip_id_arr, i);
                int32_t packed = (slot_idx >= 0 && slot_idx < tlist_len)
                    ? rt.iaload(sf_app_CpCanvas_tip_list, slot_idx)
                    : 0;
                int32_t cid = (packed >> 8) & 0xFF;
                if (cid <= 0 || cid >= 213) {
                    packed = (2 << 8) | 0; // Cannon A fallback
                } else {
                    ++valid_cnt;
                }
                b.folder_packed[i] = packed;
            }
            folder_valid = (valid_cnt > 0);
            int32_t reg_slot = rt.field_i32(fol_obj, 8);
            if (rt.field_i32(fol_obj, 7) != 0 && reg_slot >= 0 && reg_slot < 30) {
                b.regu_flg = 1;
                b.regu_id = reg_slot;
            } else {
                b.regu_flg = 0;
                b.regu_id = 0;
            }
        }
    }

    if (!folder_valid) {
        // Balanced starter folder fallback if save has no initialized folder
        static const int32_t kDefaultChips[30] = {
            (2 << 8) | 0,  (2 << 8) | 0,  (2 << 8) | 1,  (2 << 8) | 1,
            (3 << 8) | 2,  (3 << 8) | 2,  (5 << 8) | 26, (5 << 8) | 26,
            (8 << 8) | 18, (8 << 8) | 18, (9 << 8) | 18, (10 << 8) | 18,
            (18 << 8) | 11,(18 << 8) | 11,(25 << 8) | 26,(25 << 8) | 26,
            (46 << 8) | 26,(46 << 8) | 26,(89 << 8) | 26,(89 << 8) | 26,
            (95 << 8) | 26,(95 << 8) | 26,(101 << 8)| 26,(107 << 8)| 26,
            (115 << 8)| 26,(121 << 8)| 26,(129 << 8)| 26,(130 << 8)| 26,
            (136 << 8)| 26,(138 << 8)| 26
        };
        for (int i = 0; i < 30; ++i) b.folder_packed[i] = kDefaultChips[i];
        b.regu_flg = 0;
        b.regu_id = 0;
    }
    if (rt.pvp_auto_bot()) {
        b.max_hp = 250;
        b.ata_lv = 5;
        b.cha_lv = 5;
    }

    std::string nick = rt.player_nickname();
    b.player_name = nick.empty() ? "NAVI" : nick.substr(0, 24);

    rt.set_suppress_av(prev_sup);
    return b;
}

static void init_side_context(DojaRuntime& rt, Ref cp, int idx, const pvp::PvpFighterBuild& b) {
    SideContext& s = g_pvp_eng.core.sides[idx];
    s = SideContext{};

    // Allocate per-side Folder, 1-based tip_list[1..30], skill_kouka[29], Rock, Attack[30], and Rock static arrays
    // (1-based indices ensure every drawn cas_tip[i] is 1..30 > 0, matching BattleTip's `cas_tip <= 0` empty check)
    s.tip_list_ref = rt.alloc_int_array(31);
    rt.iaload(s.tip_list_ref, 0) = 0;
    for (int i = 0; i < 30; ++i) {
        rt.iaload(s.tip_list_ref, i + 1) = b.folder_packed[i];
    }

    s.fol_ref = rt.alloc_object(8, 9, 0); // app/Folder (class_id=8, 9 fields)
    fn_app_Folder__init______V(rt, s.fol_ref);
    fn_app_Folder_init_____V(rt, s.fol_ref);
    Ref fol_tip_id = rt.field_i32(s.fol_ref, 0);
    Ref fol_used = rt.field_i32(s.fol_ref, 1);
    for (int i = 0; i < 30; ++i) {
        rt.iaload(fol_tip_id, i) = i + 1;
        rt.iaload(fol_used, i) = 0;
    }
    rt.field_i32(s.fol_ref, 2) = 30; // tip_zan
    rt.field_i32(s.fol_ref, 7) = b.regu_flg ? 1 : 0;
    rt.field_i32(s.fol_ref, 8) = b.regu_flg ? std::clamp<int32_t>(b.regu_id, 0, 29) : -1;

    s.skill_kouka_ref = rt.alloc_int_array(29);
    for (int i = 0; i < 29; ++i) {
        rt.iaload(s.skill_kouka_ref, i) = b.skill_kouka[i];
    }

    s.rock_pow_ref = rt.alloc_int_array(10);
    s.rock_ani_id_ref = rt.alloc_int_array(8);
    s.rock_ani_pt_ref = rt.alloc_int_array(8);
    s.move = rt.alloc_multi_int_array(22, 7);

    s.ata_arr = rt.alloc_ref_array(30);
    for (int i = 0; i < 30; ++i) {
        Ref a = rt.alloc_object(2, 51, 0); // app/Attack (class_id=2, 51 fields)
        fn_app_Attack__init____I__V(rt, a, i);
        fn_app_Attack_init_____V(rt, a);
        rt.iaload(s.ata_arr, i) = a;
    }

    s.rock_ref = rt.alloc_object(11, 5, 0); // app/Rock (class_id=11, 5 fields)
    fn_app_Rock__init______V(rt, s.rock_ref);

    // Bind globals to this side and run native Rock.init() + SkillRun
    sf_app_CpCanvas_rock = s.rock_ref;
    sf_app_CpCanvas_ata = s.ata_arr;
    sf_app_CpCanvas_tip_list = s.tip_list_ref;
    sf_app_CpCanvas_skill_kouka = s.skill_kouka_ref;
    sf_app_CpCanvas_now_fol = 0;
    rt.iaload(sf_app_CpCanvas_fol, 0) = s.fol_ref;
    sf_app_Rock_pow = s.rock_pow_ref;
    sf_app_Rock_ani_id = s.rock_ani_id_ref;
    sf_app_Rock_ani_pt = s.rock_ani_pt_ref;
    sf_app_Rock_move = s.move;

    sf_app_CpCanvas_max_hp = std::clamp<int32_t>(b.max_hp, 100, 1000);
    sf_app_CpCanvas_now_hp = sf_app_CpCanvas_max_hp;
    sf_app_CpCanvas_ata_lv = std::clamp<int32_t>(b.ata_lv, 1, 5);
    sf_app_CpCanvas_cha_lv = std::clamp<int32_t>(b.cha_lv, 1, 5);
    sf_app_Edit_r_zoku = std::clamp<int32_t>(b.r_zoku, 0, 4);
    sf_app_CpCanvas_max_sel = std::clamp<int32_t>(5 + b.skill_kouka[0] + b.skill_kouka[1] * 2, 1, 10);
    sf_app_CpCanvas_regu_flg = b.regu_flg ? 1 : 0;
    sf_app_CpCanvas_sec_cha = b.skill_kouka[15];
    sf_app_CpCanvas_pa_no = -1;
    sf_app_CpCanvas_key_cnt = 0;
    sf_app_CpCanvas_cus_sp = 1;
    sf_app_CpCanvas_combo = 1;
    sf_app_CpCanvas_combo_time = 0;
    sf_app_CpCanvas_ata_cnt = 0;
    sf_app_CpCanvas_rika_cnt = 0;
    sf_app_CpCanvas_over_cnt = 0;
    sf_app_CpCanvas_combo_cnt = 0;
    if (sf_app_CpCanvas_wana_pow > 0 && rt.array_length(sf_app_CpCanvas_wana_pow) > 0) {
        rt.iaload(sf_app_CpCanvas_wana_pow, 0) = 0;
    }

    // Rock.init() applies skill_kouka[6] (FstBarr) and skill_kouka[7] (AirShoes) without wiping stats
    // (Never call CpCanvas.SkillRun(0) here — SkillRun(0) zeroes skill_kouka[0..28] and resets ata_lv/cha_lv=1!)
    fn_app_Rock_init_____V(rt, s.rock_ref);
    sf_app_Rock_hp = sf_app_CpCanvas_max_hp;
    sf_app_Rock_zoku = sf_app_Edit_r_zoku;
    sf_app_Rock_pos_x = 1;
    sf_app_Rock_pos_y = 1;
    sf_app_Rock_tmp_x = 1;
    sf_app_Rock_tmp_y = 1;
    sf_app_Rock_dx = 0;
    sf_app_Rock_dy = 0;
    sf_app_Rock_tip_no = -1;
    sf_app_Rock_tmp_ata = -1;
    sf_app_Rock_non_cnt = 0;
    sf_app_Rock_non_cnt2 = 0;
    sf_app_Rock_muteki_cnt = 0;
    sf_app_Rock_joutai = 0;
    s.cus_sp = std::clamp<int32_t>(sf_app_CpCanvas_cus_sp, 1, 4);

    save_side(rt, idx);
}

static void init_pvp_battle_arena(DojaRuntime& rt, Ref cp) {
    // Load Battle sprite sheets & animation tables (EnSet -> ImgSet2(0)), then stage setup
    sf_app_CpCanvas_R = 155;
    sf_app_CpCanvas_G = 255;
    sf_app_CpCanvas_B = 210;
    sf_app_CpCanvas_rank_flg = 0;
    sf_app_CpCanvas_ren_flg = 0;
    sf_app_CpCanvas_ivent_flg = -1;
    sf_app_CpCanvas_boss_flg = 1;
    sf_app_CpCanvas_deba_flg = 0;
#ifdef HAS_SF_app_CpCanvas_bas_max_flg
    sf_app_CpCanvas_bas_max_flg = 0;
#endif

    fn_app_CpCanvas_ImgSet2___I__V(rt, cp, 0);
    fn_app_CpCanvas_EnSet_____V(rt, cp);
    fn_app_CpCanvas_BtSet___I__V(rt, cp, 0);

    // Preload all 5 Rockman elemental palettes (0=Null..4=Wood) into sf_app_CpCanvas_pal[0..4]
    for (int z = 0; z < 5; ++z) {
        fn_app_CpCanvas_PalSet___II__V(rt, 0, z);
    }
    fn_app_CpCanvas_PalSet___II__V(rt, 0, 0);

    // Clear all stage-0 enemies, enemy name labels, and obstacles
    sf_app_CpCanvas_ene_cnt = 0;
    for (int i = 0; i < 3; ++i) {
        Ref e = rt.iaload(sf_app_CpCanvas_ene, i);
        if (e > 0) rt.field_i32(e, 23) = 0;
    }
    for (int i = 0; i < 6; ++i) {
        Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
        if (ok > 0) fn_app_Okimono_init_____V(rt, ok);
    }

    // Reset 6x3 battle panels cleanly: jou=1 (normal walkable ground; jou=0 is Hole!); BtPanel.init sets jin=0 (Red) / jin=1 (Blue)
    for (int y = 0; y < 3; ++y) {
        Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
        for (int x = 0; x < 6; ++x) {
            Ref p = rt.iaload(row, x);
            fn_app_BtPanel_init___III__V(rt, p, x, y, 1);
            fn_app_BtPanel_Draw___IZ__V(rt, p, -1, 1);
        }
    }

    g_pvp_eng.core = PvpCombatCore{};
    g_pvp_eng.core.board_orient = 0;
    g_pvp_eng.core.combat_turn = 0;
    g_pvp_eng.core.custom_round = 0;
    g_pvp_eng.core.winner = -2;
    g_pvp_eng.trap_mask_str = rt.alloc_string("????");

    // Record heap watermark right before allocating mutable PvP combat objects
    FastCombatSnapshot watermark_probe{};
    rt.save_combat_snapshot(watermark_probe, 1);
    g_pvp_eng.mutable_heap_min_ref = 1;

    bool prev_sup = rt.suppress_av();
    rt.set_suppress_av(true);
    init_side_context(rt, cp, 0, g_pvp_eng.builds[0]);
    init_side_context(rt, cp, 1, g_pvp_eng.builds[1]);
    rt.set_suppress_av(prev_sup);

    int32_t sp0 = g_pvp_eng.core.sides[0].cus_sp;
    int32_t sp1 = g_pvp_eng.core.sides[1].cus_sp;
    g_pvp_eng.core.shared_cus_sp = std::clamp<int32_t>(1 + (sp0 - 1) + (sp1 - 1), 1, 4);

    sf_app_CpCanvas_stop_cnt = 0;
    sf_app_CpCanvas_stop_time = 0;
    sf_app_CpCanvas_wana_flg = 0;
    sf_app_CpCanvas_dell_cnt = 0;
    sf_app_CpCanvas_over_cnt = 0;
    sf_app_CpCanvas_cus_gage = 128;
    sf_app_CpCanvas_boss_flg = 1; // BtSet(0) resets boss_flg=0; restore boss_flg=1 for Navi Battle!

    if (sf_app_CpCanvas_se_set > 0) {
        int32_t n = rt.array_length(sf_app_CpCanvas_se_set);
        for (int32_t i = 0; i < n; ++i) rt.iaload(sf_app_CpCanvas_se_set, i) = 0;
    }
    if (sf_app_CpCanvas_se_flg > 0) {
        int32_t n = rt.array_length(sf_app_CpCanvas_se_flg);
        for (int32_t i = 0; i < n; ++i) rt.iaload(sf_app_CpCanvas_se_flg, i) = 0;
    }

    // Start authentic Navi / Boss Battle BGM (mode=1 play, bgm_id=2 = 1 + boss_flg)
    fn_app_CpCanvas_Audio___II__V(rt, 1, 2);

    g_pvp_eng.local_inputs.fill(0);
    g_pvp_eng.predicted_remote.fill(0);
    g_pvp_eng.remote_confirmed.fill(false);
    g_pvp_eng.local_cas_tip.fill(-1);
    g_pvp_eng.local_cas_ok.fill(0);
    g_pvp_eng.local_permanently_consumed_mask = 0;
    for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
    for (auto& em : g_pvp_eng.turn_emitted_se) {
        em.turn = UINT32_MAX;
        em.count = 0;
    }
    g_pvp_eng.round_start_turn = 0;
    g_pvp_eng.first_unconfirmed_turn = 0;
    g_pvp_eng.max_scheduled_turn = 0;
    g_pvp_eng.last_hash_checked_turn = 0;
    g_pvp_eng.round_needs_initial_schedule = true;
    g_pvp_eng.round_initial_remote_pred = 0;
    g_pvp_eng.round_held_edge_suppress_mask = 0;
    g_pvp_eng.stall_frames = 0;
    g_pvp_eng.banner_ticks = 0;
    g_pvp_eng.judge_ticks = 0;
    g_pvp_eng.round_result_ticks = 0;
}

static int32_t compute_round_initial_remote_buster_pred(int rem_idx) {
    const SideContext& rem = g_pvp_eng.core.sides[rem_idx];
    if (rem.key_cnt <= 0) {
        return 0;
    }
    constexpr int32_t kPureBusterBits = (1 << 11) | (1 << 9) | (1 << 10) | (1 << 7);
    int32_t held_buster = rem.prev_input_mask & kPureBusterBits;
    if (held_buster != 0) {
        return held_buster;
    }
    if (rem.tip_no < 0 && (rem.prev_input_mask & (1 << 20)) != 0) {
        return (1 << 20);
    }
    return (1 << 10);
}

static void set_rock_palette_fast(DojaRuntime& rt, int32_t zoku);

// Deterministic stateless replica of CpCanvas.Ann(II)V backdrop darkening/brightening curve.
// Computing (R, G, B) directly from (stop_cnt, stop_time) guarantees 100% rollback immunity
// even if a network rollback rewinds into or out of a mid-transition Time-Freeze frame.
static void compute_ann_backdrop_rgb(int32_t stop_cnt, int32_t stop_time,
                                     int32_t& out_r, int32_t& out_g, int32_t& out_b) {
    const bool has_stage_rgb = (sf_app_CpCanvas_R > 0 || sf_app_CpCanvas_G > 0 || sf_app_CpCanvas_B > 0);
    const int32_t base_r = has_stage_rgb ? sf_app_CpCanvas_R : 155;
    const int32_t base_g = has_stage_rgb ? sf_app_CpCanvas_G : 255;
    const int32_t base_b = has_stage_rgb ? sf_app_CpCanvas_B : 210;

    int32_t r = base_r;
    int32_t g = base_g;
    int32_t b = base_b;
    if (stop_cnt >= 2 && stop_time > 0) {
        int dark_steps = std::min<int>(stop_cnt - 1, 3); // Ann(0, 0..2) on stop_cnt 1->2, 2->3, 3->4
        for (int ds = 0; ds < dark_steps; ++ds) {
            r = (r * 4) / 5;
            g = (g * 4) / 5;
            b = (b * 4) / 5;
        }
        if (stop_cnt >= stop_time) {
            int diff = stop_cnt - stop_time; // Ann(1, 4 - diff) on stop_cnt == stop_time .. stop_time + 4
            if (diff >= 4) {
                r = base_r;
                g = base_g;
                b = base_b;
            } else {
                int bright_steps = diff + 1;
                for (int bs = 0; bs < bright_steps; ++bs) {
                    r = std::min<int32_t>(255, (r * 45) / 40);
                    g = std::min<int32_t>(255, (g * 45) / 40);
                    b = std::min<int32_t>(255, (b * 45) / 40);
                }
            }
        }
    }
    out_r = r;
    out_g = g;
    out_b = b;
}

static void refresh_arena_imgmap_backdrop(DojaRuntime& rt, int32_t stop_cnt, int32_t stop_time,
                                          bool allow_recovery_blink = true) {
    int32_t bg_r = 155, bg_g = 255, bg_b = 210;
    compute_ann_backdrop_rgb(stop_cnt, stop_time, bg_r, bg_g, bg_b);
    sf_app_BtPanel_R = bg_r;
    sf_app_BtPanel_G = bg_g;
    sf_app_BtPanel_B = bg_b;

    if (sf_app_CpCanvas_graMap > 0) {
        Slot c_args[2];
        c_args[0].r = sf_app_CpCanvas_graMap;
        c_args[1].i = static_cast<int32_t>(
            0xFF000000u |
            (static_cast<uint32_t>(bg_r & 0xFF) << 16) |
            (static_cast<uint32_t>(bg_g & 0xFF) << 8) |
            static_cast<uint32_t>(bg_b & 0xFF));
        rt.invoke_external("com/nttdocomo/ui/Graphics", "setColor", "(I)V", c_args, 2);
        Slot r_args[5];
        r_args[0].r = sf_app_CpCanvas_graMap;
        r_args[1].i = 0; r_args[2].i = 0; r_args[3].i = 240; r_args[4].i = 240;
        rt.invoke_external("com/nttdocomo/ui/Graphics", "fillRect", "(IIII)V", r_args, 5);
    }

    if (sf_app_CpCanvas_panel > 0) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0) continue;
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p <= 0) continue;
                int32_t saved_jin = rt.field_i32(p, 3);
                bool flip_for_blink = false;
                if (allow_recovery_blink) {
                    int32_t pos_x = rt.field_i32(p, 1);
                    int32_t aria_cnt = rt.field_i32(p, 8);
                    if (aria_cnt > 0 && aria_cnt <= 45 && (pos_x / 3) == saved_jin && ((aria_cnt / 2) % 2) == 0) {
                        flip_for_blink = true;
                        rt.field_i32(p, 3) = saved_jin ^ 1;
                    }
                }
                fn_app_BtPanel_Draw___IZ__V(rt, p, -1, 1);
                if (flip_for_blink) {
                    rt.field_i32(p, 3) = saved_jin;
                }
            }
        }
    }
}

static void enter_custom_screen_phase(DojaRuntime& rt, Ref cp) {
    g_pvp_eng.phase = Phase::CUSTOM_SCREEN;
    g_pvp_eng.core.custom_round++;
    g_pvp_eng.core.open_custom_req = false;
    g_pvp_eng.local_custom_done = false;
    g_pvp_eng.custom_timer_ticks = 30 * 16; // 30 seconds at 16 Hz
    g_pvp_eng.last_sync_send_ms = 0;

    // Snapshot canonical combat state BEFORE any local-perspective Custom Screen UI / RNG / CharaDraw runs!
    // Note: Keep each side's remaining queued chips (tip_no / next_tip / rock_tips) intact so pressing OK
    // without selecting new chips preserves the previous round's unused chips just like native PoN virus battles!
    ensure_board_orient(rt, 0);
    load_side(rt, 0);
    g_pvp_eng.pre_custom_core = g_pvp_eng.core;
    generated_save_statics(g_pvp_eng.pre_custom_statics);
    g_pvp_eng.pre_custom_snap.base_heap_size = 0;
    rt.save_combat_snapshot(g_pvp_eng.pre_custom_snap, g_pvp_eng.mutable_heap_min_ref);

    int loc_i = g_pvp_eng.local_idx;
    ensure_board_orient(rt, loc_i);
    load_side(rt, loc_i);
    set_rock_palette_fast(rt, sf_app_Rock_zoku);
    // Prevent local Navi from appearing invisible if Custom Screen opened on an odd blink frame
    sf_app_Rock_muteki_cnt = 0;
    sf_app_Rock_hit_cnt = 0;
    sf_app_Rock_non_cnt2 = 0;
    sync_panel_occupancy_and_proxy(rt, loc_i);

    // Redraw full-brightness backdrop + all 18 panels into imgMap for local Custom Screen
    refresh_arena_imgmap_backdrop(rt, 0, 0, false);

    // Seed RNG deterministically for this player's folder draw
    if (sf_app_CpCanvas_rand > 0) {
        uint64_t seed = (static_cast<uint64_t>(g_pvp_eng.match_seed) ^
                         (static_cast<uint64_t>(g_pvp_eng.core.custom_round) * 0x9E3779B97F4A7C15ULL) ^
                         (static_cast<uint64_t>(loc_i) * 0xBF58476D1CE4E5B9ULL)) &
                        ((1ULL << 48) - 1ULL);
        rt.get(sf_app_CpCanvas_rand).rng_seed = seed;
    }

    sf_app_CpCanvas_cus_gage = 128;
    sf_app_CpCanvas_key = 0;
    sf_app_CpCanvas_sel_cas_mode = 0;
    sf_app_CpCanvas_sel_cas_tip = 0;
    sf_app_CpCanvas_sel_cnt = 0;
    sf_app_CpCanvas_cas_cnt = 0;
    sf_app_CpCanvas_pa_cnt2 = 0;
    sf_app_CpCanvas_pa_no = -1;
    sf_app_CpCanvas_bas_cnt = 0;
    sf_app_CpCanvas_yes_no_flg = 0;
    sf_app_CpCanvas_esc_flg = 0;
    sf_app_CpCanvas_non_esc = 1; // Disable running away in PvP!
    sf_app_CpCanvas_scene = 2;

    if (sf_app_CpCanvas_set_tip > 0) {
        int32_t slen = rt.array_length(sf_app_CpCanvas_set_tip);
        for (int32_t i = 0; i < slen; ++i) rt.iaload(sf_app_CpCanvas_set_tip, i) = -1;
    }
    if (sf_app_CpCanvas_cas_tip > 0) {
        int32_t clen = rt.array_length(sf_app_CpCanvas_cas_tip);
        if (g_pvp_eng.core.custom_round == 1) {
            g_pvp_eng.local_cas_tip.fill(-1);
            g_pvp_eng.local_cas_ok.fill(0);
            g_pvp_eng.local_permanently_consumed_mask = 0;
        }
        for (int32_t i = 0; i < clen; ++i) {
            int32_t ct = (i < 10) ? g_pvp_eng.local_cas_tip[i] : -1;
            bool valid_ct = (ct >= 1 && ct <= 30);
            rt.iaload(sf_app_CpCanvas_cas_tip, i) = valid_ct ? ct : -1;
            if (sf_app_CpCanvas_cas_ok > 0 && i < rt.array_length(sf_app_CpCanvas_cas_ok)) {
                rt.iaload(sf_app_CpCanvas_cas_ok, i) = valid_ct ? 1 : 0;
            }
            if (sf_app_CpCanvas_fol_tip_no > 0 && i < rt.array_length(sf_app_CpCanvas_fol_tip_no)) {
                rt.iaload(sf_app_CpCanvas_fol_tip_no, i) = valid_ct ? (ct - 1) : -1;
            }
        }
    }

    g_pvp_eng.local_permanently_consumed_mask =
        (g_pvp_eng.local_permanently_consumed_mask |
         (g_pvp_eng.core.sides[loc_i].fol_used_mask & ~compute_custom_tray_slot_mask(rt))) &
        0x3FFFFFFFu;
    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);

    Ref fol_obj = rt.iaload(sf_app_CpCanvas_fol, 0);
    fn_app_Folder_RandSet___I__V(rt, fol_obj, sf_app_CpCanvas_max_sel);
    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
    if (sf_app_CpCanvas_max_sel <= 0) {
        sf_app_CpCanvas_sel_cas_mode = 1;
        sf_app_CpCanvas_sel_cas_tip = 0;
    }
}

static pvp::PvpCustomCommit capture_local_custom_commit(DojaRuntime& rt) {
    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
    if (sf_app_CpCanvas_cas_tip > 0) {
        int32_t clen = std::min<int32_t>(10, rt.array_length(sf_app_CpCanvas_cas_tip));
        for (int32_t i = 0; i < 10; ++i) {
            int32_t ct = (i < clen) ? rt.iaload(sf_app_CpCanvas_cas_tip, i) : -1;
            bool valid_ct = (ct >= 1 && ct <= 30);
            g_pvp_eng.local_cas_tip[i] = valid_ct ? ct : -1;
            g_pvp_eng.local_cas_ok[i] = valid_ct ? 1 : 0;
        }
    }

    pvp::PvpCustomCommit c{};
    c.turn_no = current_wire_round_key();
    c.tip_no = static_cast<int8_t>(std::clamp(sf_app_Rock_tip_no, -1, 8));
    c.max_sel = static_cast<uint8_t>(std::clamp(sf_app_CpCanvas_max_sel, 0, 10));
    c.regu_flg = static_cast<uint8_t>(sf_app_CpCanvas_regu_flg ? 1 : 0);
    c.sec_cha = static_cast<uint8_t>(std::clamp(sf_app_CpCanvas_sec_cha, 0, 255));
    c.pa_no = static_cast<int16_t>(sf_app_CpCanvas_pa_no);

    Ref rock_tips = rt.field_i32(sf_app_CpCanvas_rock, 0);
    for (int i = 0; i < 9; ++i) {
        c.rock_tip_ids[i] = -1;
        if (rock_tips > 0 && i < rt.array_length(rock_tips)) {
            Ref t_obj = rt.iaload(rock_tips, i);
            if (t_obj > 0) {
                c.rock_tip_ids[i] = static_cast<int16_t>(rt.field_i32(t_obj, 0)); // Tip.id
            }
        }
    }

    Ref fol_obj = rt.iaload(sf_app_CpCanvas_fol, 0);
    c.fol_used_mask = read_folder_used_mask(rt, fol_obj);
    return c;
}

static void clear_side_nondim_hud(SideContext& sc);

static void apply_custom_commit_to_side(DojaRuntime& rt, int idx, const pvp::PvpCustomCommit& c) {
    load_side(rt, idx);
    clear_side_nondim_hud(g_pvp_eng.core.sides[idx]);
    sf_app_Rock_tip_no = c.tip_no;
    sf_app_CpCanvas_max_sel = std::clamp<int32_t>(c.max_sel, 0, 10);
    sf_app_CpCanvas_regu_flg = c.regu_flg ? 1 : 0;
    sf_app_CpCanvas_sec_cha = c.sec_cha;
    sf_app_CpCanvas_pa_no = c.pa_no;

    Ref rock_tips = rt.field_i32(sf_app_CpCanvas_rock, 0);
    int32_t tip_table_len = rt.array_length(sf_app_CpCanvas_tip);
    int32_t rock_tips_len = rt.array_length(rock_tips);
    Ref default_tip = (tip_table_len > 0) ? rt.iaload(sf_app_CpCanvas_tip, 0) : 0;
    for (int32_t i = 0; i < rock_tips_len; ++i) {
        rt.iaload(rock_tips, i) = default_tip;
    }
    int32_t want_cnt = std::clamp<int32_t>(static_cast<int32_t>(c.tip_no) + 1, 0, std::min<int32_t>(9, rock_tips_len));
    int32_t valid_cnt = 0;
    for (int32_t i = 0; i < want_cnt; ++i) {
        int16_t tid = c.rock_tip_ids[i];
        if (tid >= 0 && tid < tip_table_len) {
            Ref tref = rt.iaload(sf_app_CpCanvas_tip, tid);
            if (tref > 0) {
                rt.iaload(rock_tips, valid_cnt++) = tref;
            }
        }
    }
    sf_app_Rock_tip_no = valid_cnt - 1;

    Ref fol_obj = rt.iaload(sf_app_CpCanvas_fol, 0);
    sync_folder_used_from_mask(rt, fol_obj, c.fol_used_mask);

    sf_app_Rock_tmp_ata = -1;
    sf_app_Rock_waza_flg = -1;
    sf_app_Rock_waza_wait = 0;
    if (sf_app_Rock_tip_no >= 0) {
        fn_app_Rock_TipNext_____V(rt, sf_app_CpCanvas_rock);
    } else {
        sf_app_Rock_next_tip = 0;
        if (sf_app_Rock_pow > 0 && rt.array_length(sf_app_Rock_pow) > 0) {
            rt.iaload(sf_app_Rock_pow, 0) = 0;
        }
    }
    save_side(rt, idx);
}

// Compute 16-bit FNV-1a hash of canonical combat state (100% orientation-invariant)
static uint16_t compute_combat_state_hash(DojaRuntime& rt) {
    uint32_t h = 2166136261u;
    auto mix = [&](int32_t v) {
        uint32_t u = static_cast<uint32_t>(v);
        for (int b = 0; b < 4; ++b) {
            h ^= (u & 0xFFu);
            h *= 16777619u;
            u >>= 8;
        }
    };
    for (int s = 0; s < 2; ++s) {
        const SideContext& sc = g_pvp_eng.core.sides[s];
        mix(sc.hp);
        mix(sc.pos_x);
        mix(sc.pos_y);
        mix(sc.move_flg);
        mix(sc.tip_no);
        mix(sc.next_tip);
        mix(sc.key_cnt);
        mix(static_cast<int32_t>(sc.fol_used_mask));
        mix(sc.bari);
        mix(sc.han);
        mix(sc.wana);
        mix(sc.wana_pow0);
        mix(sc.joutai);
        mix(sc.muteki_cnt);
        mix(sc.syoumetu_cnt);
        mix(sc.combo);
        mix(sc.rika_cnt);
        mix(sc.ata_cnt);
        mix(sc.nondim_tip_id);
        mix(sc.nondim_base_pow);
        mix(sc.nondim_plus_pow);
        mix(sc.nondim_ticks);
        if (sc.ata_arr > 0) {
            for (int i = 0; i < 30; ++i) {
                Ref a = rt.iaload(sc.ata_arr, i);
                int32_t on = (a > 0) ? rt.field_i32(a, 38) : 0;
                mix(on);
                if (on != 0) {
                    mix(rt.field_i32(a, 1));  // waza_id
                    mix(rt.field_i32(a, 3));  // pos_x (owner-local)
                    mix(rt.field_i32(a, 4));  // pos_y
                    mix(rt.field_i32(a, 10)); // pow
                    mix(rt.field_i32(a, 11)); // zoku
                    mix(rt.field_i32(a, 30)); // move_cnt
                }
            }
        }
    }
    mix(g_pvp_eng.core.shared_cus_sp);
    mix(sf_app_CpCanvas_cus_gage);
    mix(sf_app_CpCanvas_stop_cnt);
    mix(sf_app_CpCanvas_stop_time);
    mix(sf_app_CpCanvas_wana_flg);
    mix(g_pvp_eng.core.stop_owner);
    mix(g_pvp_eng.core.damage_dealt[0]);
    mix(g_pvp_eng.core.damage_dealt[1]);
    mix(static_cast<int32_t>(g_pvp_eng.core.custom_round));
    mix(static_cast<int32_t>(g_pvp_eng.core.final_turn_ticks_left));
    mix(g_pvp_eng.core.ended_by_judge ? 1 : 0);
    if (sf_app_CpCanvas_rand > 0) {
        uint64_t rseed = rt.get(sf_app_CpCanvas_rand).rng_seed;
        mix(static_cast<int32_t>(rseed & 0xFFFFFFFFu));
        mix(static_cast<int32_t>((rseed >> 32) & 0xFFFFFFFFu));
    }

    const bool mirrored = (g_pvp_eng.core.board_orient != 0);
    if (sf_app_CpCanvas_panel > 0) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            for (int cx = 0; cx < 6; ++cx) {
                int bx = mirrored ? (5 - cx) : cx;
                Ref p = rt.iaload(row, bx);
                int32_t jin = rt.field_i32(p, 3);
                mix(mirrored ? (1 - jin) : jin);
                mix(rt.field_i32(p, 4));  // jou
                mix(rt.field_i32(p, 6));  // ana_cnt
                mix(rt.field_i32(p, 7));  // mag_cnt
                mix(rt.field_i32(p, 8));  // aria_cnt
                mix(rt.field_i32(p, 10)); // bom_pow
            }
        }
    }
    if (sf_app_CpCanvas_oki > 0) {
        for (int ci = 0; ci < 4; ++ci) {
            int bi = mirrored ? (ci ^ 2) : ci;
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, bi);
            int32_t on = (ok > 0) ? rt.field_i32(ok, 1) : 0;
            mix(on);
            if (on != 0) {
                mix(rt.field_i32(ok, 2));  // syu
                mix(rt.field_i32(ok, 3));  // hp
                mix(rt.field_i32(ok, 4));  // time
                int32_t ox = rt.field_i32(ok, 9);
                mix(mirrored ? (5 - ox) : ox);
                mix(rt.field_i32(ok, 10)); // pos_y
                mix(rt.field_i32(ok, 21)); // move_cnt
            }
        }
    }
    return static_cast<uint16_t>((h >> 16) ^ (h & 0xFFFFu));
}

// Host frame-pacing & sound-driver state that belongs to the local OS/audio frontend rather than
// deterministic combat simulation. Preserving these across rollback / custom / render snapshots
// prevents rollbacks from rewinding sf_app_CpCanvas_mill into the past (which caused 4x turbo clock
// runaway!) or corrupting active CpCanvas.sePlay() AudioPresenter channels during Buster Charging.
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

static std::vector<int32_t> copy_int_array_safe(DojaRuntime& rt, Ref arr) {
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

static void write_int_array_safe(DojaRuntime& rt, Ref arr, const std::vector<int32_t>& vals) {
    if (arr > 0 && !vals.empty()) {
        int32_t n = std::min<int32_t>(rt.array_length(arr), static_cast<int32_t>(vals.size()));
        for (int32_t i = 0; i < n; ++i) {
            rt.iaload(arr, i) = vals[static_cast<size_t>(i)];
        }
    }
}

static HostRuntimeState capture_host_runtime_state(DojaRuntime& rt) {
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

static void restore_host_runtime_state(DojaRuntime& rt, const HostRuntimeState& h, bool restore_se_queue = true) {
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

static void save_rollback_slot(DojaRuntime& rt, uint32_t turn, int32_t p0_m, int32_t p1_m, bool p1_pred) {
    RollbackSlot& sl = g_pvp_eng.rb_ring[turn % kRollbackWindow];
    sl.turn = turn;
    sl.p0_mask = p0_m;
    sl.p1_mask = p1_m;
    sl.p1_predicted = p1_pred;
    sl.pre_hash = compute_combat_state_hash(rt);
    sl.core = g_pvp_eng.core;
    generated_save_statics(sl.statics);
    sl.heap_snap.base_heap_size = 0;
    rt.save_combat_snapshot(sl.heap_snap, g_pvp_eng.mutable_heap_min_ref);
}

static bool restore_rollback_slot(DojaRuntime& rt, uint32_t turn) {
    const RollbackSlot& sl = g_pvp_eng.rb_ring[turn % kRollbackWindow];
    if (sl.turn != turn) return false;
    HostRuntimeState host_st = capture_host_runtime_state(rt);
    g_pvp_eng.core = sl.core;
    generated_restore_statics(sl.statics);
    rt.restore_combat_snapshot(sl.heap_snap);
    restore_host_runtime_state(rt, host_st, true);
    return true;
}

static void sync_muramasa_power(DojaRuntime& rt) {
    if (sf_app_CpCanvas_tip > 0 && rt.array_length(sf_app_CpCanvas_tip) > 131) {
        Ref mura = rt.iaload(sf_app_CpCanvas_tip, 131);
        if (mura > 0) {
            rt.field_i32(mura, 3) = std::clamp<int32_t>(sf_app_CpCanvas_max_hp - sf_app_Rock_hp, 0, 999);
        }
    }
}

static bool is_navi_summon_tip(int32_t tip_id) {
    return (tip_id >= 137 && tip_id <= 180) || tip_id == 207 || tip_id == 208;
}

// Trigger opponent's elemental trap (wana 1..4 = AntiFire/Aqua/Elec/Wood) via Time-Freeze waza 95..98
static void trigger_opponent_elemental_trap(DojaRuntime& rt, int att_idx, int32_t wana_elem) {
    int opp_idx = 1 - att_idx;
    int32_t opp_wpow = (g_pvp_eng.core.sides[opp_idx].wana_pow0 > 0)
        ? g_pvp_eng.core.sides[opp_idx].wana_pow0
        : 150;
    int32_t tx = std::clamp(5 - g_pvp_eng.core.sides[att_idx].pos_x, 0, 5);
    int32_t ty = std::clamp(g_pvp_eng.core.sides[att_idx].pos_y, 0, 2);

    g_pvp_eng.active_side = opp_idx;
    ensure_board_orient(rt, opp_idx);
    load_side(rt, opp_idx);
    sf_app_Rock_wana = 0;
    sync_panel_occupancy_and_proxy(rt, opp_idx);

    int32_t s1 = fn_app_CpCanvas_AtaNo___I__I(rt, 0);
    Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s1);
    fn_app_Attack_Set___IIIII__I(rt, c_ata, 94 + wana_elem, tx, ty, 1, -2);
    rt.field_i32(c_ata, 39) = 0; // chara_flg = 0 so HitGo targets ene[0]
    rt.field_i32(c_ata, 10) = opp_wpow;
    rt.field_i32(c_ata, 11) = wana_elem;
    sf_app_CpCanvas_stop_time = rt.field_i32(c_ata, 20) + 20;

    int32_t s2 = fn_app_CpCanvas_AtaNo___I__I(rt, 0);
    Ref fx_ata = rt.iaload(sf_app_CpCanvas_ata, s2);
    fn_app_Attack_Set___IIIII__I(rt, fx_ata, 52, tx, ty, 0, 2);
    rt.field_i32(fx_ata, 20) = 1;

    sf_app_CpCanvas_stop_cnt = 1;
    sf_app_CpCanvas_wana_flg = 100 + wana_elem;
    if (sf_app_CpCanvas_tip > 0 && 114 + wana_elem < rt.array_length(sf_app_CpCanvas_tip)) {
        Ref t_trap = rt.iaload(sf_app_CpCanvas_tip, 114 + wana_elem);
        if (t_trap > 0) sf_app_CpCanvas_stop_name = rt.field_i32(t_trap, 9);
    }
    g_pvp_eng.core.stop_owner = opp_idx;
    save_side(rt, opp_idx);
}

// Trigger opponent's AntiNavi trap (wana == 6): steal Navi summon and execute from opponent's side!
static void trigger_opponent_antinavi_trap(DojaRuntime& rt, int att_idx, Ref stolen_tip_obj, int32_t stolen_bonus) {
    int opp_idx = 1 - att_idx;
    int32_t tip_id = rt.field_i32(stolen_tip_obj, 0);

    g_pvp_eng.active_side = opp_idx;
    ensure_board_orient(rt, opp_idx);
    load_side(rt, opp_idx);
    sf_app_Rock_wana = 0;
    sync_panel_occupancy_and_proxy(rt, opp_idx);

    int32_t saved_pow3 = rt.iaload(sf_app_Rock_pow, 3);
    rt.iaload(sf_app_Rock_pow, 3) = stolen_bonus;

    int32_t s1 = fn_app_CpCanvas_AtaNo___I__I(rt, 0);
    Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s1);
    fn_app_Attack_Set___IIIII__I(rt, c_ata, tip_id, sf_app_Rock_pos_x, sf_app_Rock_pos_y, 1, 0);
    rt.iaload(sf_app_Rock_pow, 3) = saved_pow3;

    fn_app_Rock_AniSet___I__V(rt, sf_app_CpCanvas_rock, 21);
    sf_app_CpCanvas_stop_time = std::max<int32_t>(12, rt.field_i32(c_ata, 20)) + 20;
    sf_app_CpCanvas_stop_cnt = 1;
    sf_app_CpCanvas_stop_name = rt.field_i32(stolen_tip_obj, 9);
    g_pvp_eng.core.stop_owner = opp_idx;
    if (sf_app_CpCanvas_cus_sp > g_pvp_eng.core.shared_cus_sp) {
        g_pvp_eng.core.shared_cus_sp = std::clamp<int32_t>(sf_app_CpCanvas_cus_sp, 1, 4);
    }
    save_side(rt, opp_idx);
}

static int32_t sum_pending_mine_bom_pow(DojaRuntime& rt) {
    int32_t total = 0;
    if (sf_app_CpCanvas_oki > 0) {
        int32_t n = std::min<int32_t>(6, rt.array_length(sf_app_CpCanvas_oki));
        for (int32_t i = 0; i < n; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
            if (ok > 0 && rt.field_i32(ok, 1) != 0 && rt.field_i32(ok, 2) == 0) {
                int32_t p = rt.field_i32(ok, 14);
                if (p > 0) total += p;
            }
        }
    }
    if (sf_app_CpCanvas_panel > 0) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0) continue;
            for (int x = 0; x < 6; ++x) {
                Ref pan = rt.iaload(row, x);
                if (pan > 0) {
                    int32_t bpow = rt.field_i32(pan, 10);
                    if (bpow > 0) total += bpow;
                }
            }
        }
    }
    return total;
}

static void clear_side_nondim_hud(SideContext& sc) {
    sc.nondim_tip_id = -1;
    sc.nondim_base_pow = -1;
    sc.nondim_plus_pow = 0;
    sc.nondim_ticks = 0;
}

static void record_nondim_chip_if_applicable(DojaRuntime& rt, SideContext& s, Ref t_obj, int32_t captured_plus_pow) {
    if (t_obj <= 0 || sf_app_CpCanvas_tip <= 0 || sf_app_CpCanvas_waza <= 0) return;
    int32_t tip_id = rt.field_i32(t_obj, 0);
    if (tip_id < 0 || tip_id >= rt.array_length(sf_app_CpCanvas_tip)) return;
    int32_t waza_idx = rt.field_i32(t_obj, 8);
    if (waza_idx < 0 || waza_idx >= rt.array_length(sf_app_CpCanvas_waza)) return;
    Ref waza_obj = rt.iaload(sf_app_CpCanvas_waza, waza_idx);
    if (waza_obj <= 0) return;
    // Attack.WazaSet sets stop_time strictly from waza.field_11; 0 == non-dimming.
    if (rt.field_i32(waza_obj, 11) != 0) return;

    int32_t tip_kind = rt.field_i32(t_obj, 1);
    s.nondim_tip_id = static_cast<int16_t>(tip_id);
    if (tip_kind < 3) {
        s.nondim_base_pow = static_cast<int16_t>(std::max<int32_t>(0, rt.field_i32(t_obj, 3)));
        s.nondim_plus_pow = static_cast<int16_t>(std::max<int32_t>(0, captured_plus_pow));
    } else {
        s.nondim_base_pow = -1;
        s.nondim_plus_pow = 0;
    }
    s.nondim_ticks = kOppNonDimChipHudTicks;
}

static void step_single_side(DojaRuntime& rt, Ref cp, int s_idx, int32_t mask) {
    (void)cp;
    g_pvp_eng.active_side = s_idx;
    ensure_board_orient(rt, s_idx);
    load_side(rt, s_idx);
    sync_panel_occupancy_and_proxy(rt, s_idx);
    sync_muramasa_power(rt);

    SideContext& s = g_pvp_eng.core.sides[s_idx];
    SideContext& opp = g_pvp_eng.core.sides[1 - s_idx];
    int32_t newly_pressed = mask & ~s.prev_input_mask;
    s.prev_input_mask = mask;

    // Edge-triggered movement & BattleChip activation (mirrors BattleMain L_3744..L_3950)
    if (s.hp > 0 && sf_app_CpCanvas_stop_cnt == 0) {
        if (newly_pressed & (1 << 16)) { // LEFT
            if (sf_app_Rock_joutai == 0 && sf_app_Rock_move_flg == 0) sf_app_Rock_move_flg = 1;
        } else if (newly_pressed & (1 << 18)) { // RIGHT
            if (sf_app_Rock_joutai == 0 && sf_app_Rock_move_flg == 0) sf_app_Rock_move_flg = 3;
        } else if (newly_pressed & (1 << 17)) { // UP
            if (sf_app_Rock_joutai == 0 && sf_app_Rock_move_flg == 0) sf_app_Rock_move_flg = 2;
        } else if (newly_pressed & (1 << 19)) { // DOWN
            if (sf_app_Rock_joutai == 0 && sf_app_Rock_move_flg == 0) sf_app_Rock_move_flg = 4;
        }

        if ((newly_pressed & (1 << 20)) && sf_app_Rock_tip_no >= 0) { // KEY_SELECT -> Use Chip
            if (sf_app_Rock_tmp_ata < 0) {
                // Check opponent AntiNavi (wana==6) or AntiFire..Wood (wana==1..4) when chip fires immediately
                if (sf_app_Rock_joutai == 0 && sf_app_Rock_non_cnt <= 0 &&
                    sf_app_Rock_non_cnt2 <= 0 && sf_app_Rock_move_flg == 0) {
                    Ref rock_tips = rt.field_i32(sf_app_CpCanvas_rock, 0);
                    Ref t_obj = (rock_tips > 0 && sf_app_Rock_tip_no < rt.array_length(rock_tips))
                        ? rt.iaload(rock_tips, sf_app_Rock_tip_no)
                        : 0;
                    if (t_obj > 0) {
                        int32_t tip_id = rt.field_i32(t_obj, 0);
                        int32_t tip_zoku = rt.field_i32(t_obj, 4);
                        int32_t cur_bonus = (sf_app_Rock_pow > 0) ? rt.iaload(sf_app_Rock_pow, 0) : 0;
                        if (opp.hp > 0 && opp.wana == 6 && is_navi_summon_tip(tip_id)) {
                            if (sf_app_Rock_bas_no >= 0) {
                                fn_app_Attack_init_____V(rt, rt.iaload(sf_app_CpCanvas_ata, sf_app_Rock_bas_no));
                                sf_app_Rock_bas_no = -1;
                                sf_app_CpCanvas_key_cnt = 0;
                            }
                            sf_app_Rock_tip_no -= sf_app_Rock_next_tip;
                            fn_app_Rock_TipNext_____V(rt, sf_app_CpCanvas_rock);
                            sf_app_Rock_tmp_ata = -1;
                            sf_app_Rock_non_bas = 1;
                            rt.set_keypad_override(false, 0);
                            save_side(rt, s_idx);
                            trigger_opponent_antinavi_trap(rt, s_idx, t_obj, cur_bonus);
                            return;
                        }
                        if (opp.hp > 0 && opp.wana >= 1 && opp.wana <= 4 && tip_zoku == opp.wana) {
                            if (sf_app_Rock_bas_no >= 0) {
                                fn_app_Attack_init_____V(rt, rt.iaload(sf_app_CpCanvas_ata, sf_app_Rock_bas_no));
                                sf_app_Rock_bas_no = -1;
                                sf_app_CpCanvas_key_cnt = 0;
                            }
                            sf_app_Rock_tip_no -= sf_app_Rock_next_tip;
                            fn_app_Rock_TipNext_____V(rt, sf_app_CpCanvas_rock);
                            sf_app_Rock_tmp_ata = -1;
                            sf_app_Rock_non_bas = 1;
                            rt.set_keypad_override(false, 0);
                            save_side(rt, s_idx);
                            trigger_opponent_elemental_trap(rt, s_idx, tip_zoku);
                            return;
                        }
                    }
                }
                int32_t pre_act_tip_no = sf_app_Rock_tip_no;
                Ref rock_tips_arr = rt.field_i32(sf_app_CpCanvas_rock, 0);
                Ref pre_act_tip_obj = (rock_tips_arr > 0 && pre_act_tip_no >= 0 && pre_act_tip_no < 5 &&
                                       pre_act_tip_no < rt.array_length(rock_tips_arr))
                    ? rt.iaload(rock_tips_arr, pre_act_tip_no)
                    : 0;
                int32_t pre_act_pow0 = (sf_app_Rock_pow > 0 && rt.array_length(sf_app_Rock_pow) > 0)
                    ? rt.iaload(sf_app_Rock_pow, 0)
                    : 0;
                fn_app_Rock_Action___I__V(rt, sf_app_CpCanvas_rock, 0);
                if (pre_act_tip_no >= 0 && pre_act_tip_no < 5 &&
                    sf_app_Rock_tip_no < pre_act_tip_no && pre_act_tip_obj > 0) {
                    record_nondim_chip_if_applicable(rt, s, pre_act_tip_obj, pre_act_pow0);
                }
            }
        }

        // Buster & Elemental Charge Shot logic (mirrors BattleMain L_4286..L_4582)
        rt.set_keypad_override(true, mask);
        if (sf_app_Rock_tmp_ata < 0 && sf_app_Rock_non_cnt2 == 0) {
            int32_t cha_thresh = 42 - sf_app_CpCanvas_cha_lv * 5;
            bool buster_held =
                ((mask & ((1 << 11) | (1 << 9) | (1 << 10) | (1 << 7))) != 0) ||
                (((mask & (1 << 20)) != 0) && sf_app_Rock_tip_no < 0);

            if (sf_app_CpCanvas_key_cnt < 0) {
                sf_app_CpCanvas_key_cnt += 1;
            } else if (buster_held) {
                if (sf_app_Rock_non_bas == 0 && sf_app_Rock_joutai == 0 && sf_app_CpCanvas_key_cnt <= cha_thresh) {
                    if (sf_app_CpCanvas_key_cnt == 4) {
                        sf_app_CpCanvas_key_cnt -= fn_app_Rock_Chage___I__I(rt, sf_app_CpCanvas_rock, 0);
                    } else if (sf_app_CpCanvas_key_cnt == cha_thresh) {
                        fn_app_Rock_Chage___I__I(rt, sf_app_CpCanvas_rock, 1);
                    }
                    sf_app_CpCanvas_key_cnt += 1;
                }
            } else if (sf_app_CpCanvas_key_cnt > 0) {
                int32_t buster_tip_id = 0;
                int32_t buster_zoku = 0;
                if (sf_app_CpCanvas_key_cnt > cha_thresh) {
                    static const int32_t kChargeTips[5] = {1, 209, 210, 211, 212};
                    int32_t rz = std::clamp(sf_app_Edit_r_zoku, 0, 4);
                    buster_tip_id = kChargeTips[rz];
                    buster_zoku = rz;
                }
                if (buster_zoku >= 1 && buster_zoku <= 4 &&
                    sf_app_Rock_joutai == 0 && sf_app_Rock_non_cnt <= 0 && sf_app_Rock_move_flg == 0 &&
                    opp.hp > 0 && opp.wana == buster_zoku) {
                    if (sf_app_Rock_bas_no >= 0) {
                        fn_app_Attack_init_____V(rt, rt.iaload(sf_app_CpCanvas_ata, sf_app_Rock_bas_no));
                        sf_app_Rock_bas_no = -1;
                    }
                    sf_app_CpCanvas_key_cnt = -4;
                    sf_app_Rock_non_bas = 0;
                    rt.set_keypad_override(false, 0);
                    save_side(rt, s_idx);
                    trigger_opponent_elemental_trap(rt, s_idx, buster_zoku);
                    return;
                }
                int32_t saved_tip_no = sf_app_Rock_tip_no;
                int32_t saved_pow0 = rt.iaload(sf_app_Rock_pow, 0);
                int32_t saved_pow3 = rt.iaload(sf_app_Rock_pow, 3);
                rt.iaload(sf_app_Rock_pow, 0) = 0;
                sf_app_Rock_tip_no = 5;
                rt.iaload(rt.field_i32(sf_app_CpCanvas_rock, 0), 5) = rt.iaload(sf_app_CpCanvas_tip, buster_tip_id);
                fn_app_Rock_Action___I__V(rt, sf_app_CpCanvas_rock, 0);
                sf_app_Rock_tip_no = saved_tip_no;
                rt.iaload(sf_app_Rock_pow, 0) = saved_pow0;
                rt.iaload(sf_app_Rock_pow, 3) = saved_pow3;
                sf_app_CpCanvas_key_cnt = -4;
            }
        }
        sf_app_Rock_non_bas = 0;
    }

    rt.set_keypad_override(true, mask);
    sf_app_CpCanvas_key = 0;
    sync_muramasa_power(rt);

    // Step Rock movement & chip execution (Rock.Move calls Rock.PaneCh internally; never call overworld CpCanvas.PaneHit!)
    int32_t pre_hp = std::max<int32_t>(0, sf_app_Rock_hp);
    int32_t pre_bari = sf_app_Rock_bari;
    int32_t pre_muteki = sf_app_Rock_muteki_cnt;
    int32_t pre_dell = sf_app_CpCanvas_dell_cnt;
    int32_t pre_mine_sum = sum_pending_mine_bom_pow(rt);

    int32_t pre_move_tmp_ata = sf_app_Rock_tmp_ata;
    int32_t pre_move_tip_no = sf_app_Rock_tip_no;
    Ref rock_tips_move = rt.field_i32(sf_app_CpCanvas_rock, 0);
    Ref pre_move_tip_obj = (pre_move_tmp_ata >= 0 && rock_tips_move > 0 &&
                            pre_move_tip_no >= 0 && pre_move_tip_no < 5 &&
                            pre_move_tip_no < rt.array_length(rock_tips_move))
        ? rt.iaload(rock_tips_move, pre_move_tip_no)
        : 0;
    int32_t pre_move_pow0 = (sf_app_Rock_pow > 0 && rt.array_length(sf_app_Rock_pow) > 0)
        ? rt.iaload(sf_app_Rock_pow, 0)
        : 0;

    fn_app_Rock_Move_____V(rt, sf_app_CpCanvas_rock);

    if (pre_move_tmp_ata >= 0 && sf_app_Rock_tmp_ata < 0 &&
        pre_move_tip_no >= 0 && pre_move_tip_no < 5 &&
        sf_app_Rock_tip_no < pre_move_tip_no && pre_move_tip_obj > 0) {
        record_nondim_chip_if_applicable(rt, s, pre_move_tip_obj, pre_move_pow0);
    }

    int32_t consumed_mine_pow = pre_mine_sum - sum_pending_mine_bom_pow(rt);
    if (consumed_mine_pow > 0 && pre_hp > 0 && pre_dell <= 0 && pre_muteki <= 0 && pre_bari <= 0) {
        int32_t mine_dmg = consumed_mine_pow;
        if (sf_app_Rock_pane_id == 7) {
            mine_dmg = (mine_dmg + 1) / 2;
        }
        int32_t post_mine_hp = pre_hp - mine_dmg;
        if (post_mine_hp <= 0) {
            bool has_undershirt = (sf_app_CpCanvas_skill_kouka > 0 &&
                                   rt.array_length(sf_app_CpCanvas_skill_kouka) > 8 &&
                                   rt.iaload(sf_app_CpCanvas_skill_kouka, 8) != 0);
            post_mine_hp = (has_undershirt && pre_hp > 1) ? 1 : 0;
        }
        if (post_mine_hp < pre_hp) {
            g_pvp_eng.core.damage_dealt[1 - s_idx] += (pre_hp - post_mine_hp);
        }
    }

    // Check if this side just triggered a Time-Freeze chip (stop_time > 0).
    // Mirrors CpCanvas.BattleMain L_4975..L_4994: set stop_time += 20, stop_cnt = 1, and return immediately
    // so Attack.Move(0) advances field_30 during the 13-frame banner (stop_cnt = 2..14) before
    // Attack.Draw starts advancing sprite frames (field_27) at stop_cnt == 15.
    if (sf_app_CpCanvas_stop_time > 0 && sf_app_CpCanvas_stop_cnt == 0) {
        sf_app_CpCanvas_stop_time += 20;
        sf_app_CpCanvas_stop_cnt = 1;
        g_pvp_eng.core.stop_owner = s_idx;
        if (sf_app_CpCanvas_cus_sp > g_pvp_eng.core.shared_cus_sp) {
            g_pvp_eng.core.shared_cus_sp = std::clamp<int32_t>(sf_app_CpCanvas_cus_sp, 1, 4);
        }
        rt.set_keypad_override(false, 0);
        save_side(rt, s_idx);
        return;
    }

    // Step all 30 Attack slots for this side (HitGo(-1) -> Move(1) -> HitGo(0) + NorthWind check)
    for (int i = 0; i < 30; ++i) {
        Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
        if (rt.field_i32(a, 38) != 0) {
            // NorthWind (param[7] == 18): strip opponent's Barrier/Aura!
            Ref param_arr = rt.field_i32(a, 23);
            if (param_arr > 0 && rt.array_length(param_arr) > 7 && rt.iaload(param_arr, 7) == 18) {
                if (opp.bari > 0) {
                    opp.bari = 0;
                    if (opp.bari_id >= 0 && opp.bari_id < 30) {
                        rt.field_i32(rt.iaload(opp.ata_arr, opp.bari_id), 38) = 0;
                        opp.bari_id = -1;
                    }
                }
            }
            g_pvp_eng.active_ata_ref = a;
            fn_app_Attack_HitGo___I__V(rt, a, -1);
            g_pvp_eng.active_ata_ref = 0;
        }
    }
    for (int i = 0; i < 30; ++i) {
        Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
        if (rt.field_i32(a, 38) != 0) {
            g_pvp_eng.active_ata_ref = a;
            fn_app_Attack_Move___I__V(rt, a, 1);
            g_pvp_eng.active_ata_ref = 0;
        }
    }
    for (int i = 0; i < 30; ++i) {
        Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
        if (rt.field_i32(a, 38) != 0) {
            // Guard Attack.HitCheck L_10..L_24 which writes panel[field_13][field_12].field_5 before checking bounds
            if (rt.field_i32(a, 8) == 1) {
                int32_t hx = rt.field_i32(a, 12);
                int32_t hy = rt.field_i32(a, 13);
                if (hx < 0 || hx > 5 || hy < 0 || hy > 2) continue;
            }
            g_pvp_eng.active_ata_ref = a;
            fn_app_Attack_HitGo___I__V(rt, a, 0);
            g_pvp_eng.active_ata_ref = 0;
        }
    }
    if (sf_app_CpCanvas_cus_sp > g_pvp_eng.core.shared_cus_sp) {
        g_pvp_eng.core.shared_cus_sp = std::clamp<int32_t>(sf_app_CpCanvas_cus_sp, 1, 4);
    }

    // Advance Rock & Attack sprite frame counters (with AV suppressed so zero pixels are drawn)
    fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 1);
    for (int i = 0; i < 30; ++i) {
        Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
        if (rt.field_i32(a, 38) != 0) {
            fn_app_Attack_Draw___II__V(rt, a, 1, 1);
            fn_app_Attack_Draw___II__V(rt, a, 2, 1);
        }
    }

    if (sf_app_CpCanvas_combo_time > 0) {
        sf_app_CpCanvas_combo_time -= 1;
        if (sf_app_CpCanvas_combo_time == 0) sf_app_CpCanvas_combo = 1;
    }

    rt.set_keypad_override(false, 0);
    save_side(rt, s_idx);
}

static void step_pvp_combat_tick(DojaRuntime& rt, Ref cp, int32_t p0_mask, int32_t p1_mask) {
    bool prev_sup = rt.suppress_av();
    rt.set_suppress_av(true);
    g_pvp_eng.in_combat_step = true;
    const uint32_t tick_turn = g_pvp_eng.core.combat_turn;
    std::vector<int32_t> pre_step_se_set = copy_int_array_safe(rt, sf_app_CpCanvas_se_set);
    std::vector<int32_t> pre_step_se_flg = copy_int_array_safe(rt, sf_app_CpCanvas_se_flg);
    if (sf_app_CpCanvas_se_set > 0) {
        int32_t n = rt.array_length(sf_app_CpCanvas_se_set);
        for (int32_t i = 0; i < n; ++i) rt.iaload(sf_app_CpCanvas_se_set, i) = 0;
    }
    if (sf_app_CpCanvas_se_flg > 0) {
        int32_t n = rt.array_length(sf_app_CpCanvas_se_flg);
        for (int32_t i = 0; i < n; ++i) rt.iaload(sf_app_CpCanvas_se_flg, i) = 0;
    }

    auto commit_tick_audio_and_finish = [&]() {
        std::vector<int32_t> tick_se_set = copy_int_array_safe(rt, sf_app_CpCanvas_se_set);
        std::vector<int32_t> tick_se_flg = copy_int_array_safe(rt, sf_app_CpCanvas_se_flg);
        write_int_array_safe(rt, sf_app_CpCanvas_se_set, pre_step_se_set);
        write_int_array_safe(rt, sf_app_CpCanvas_se_flg, pre_step_se_flg);

        // Emit each one-shot AniSe / gauge sound at most once per combat_turn so rollback
        // resimulation never double-plays already-heard SEs, while newly-arrived remote attack
        // SEs on a rolled-back turn still play immediately!
        PvpEngineState::TurnEmittedSe& em = g_pvp_eng.turn_emitted_se[tick_turn % kRollbackWindow];
        if (em.turn != tick_turn) {
            em.turn = tick_turn;
            em.count = 0;
        }
        for (size_t k = 0; k < tick_se_set.size(); ++k) {
            int32_t sid = tick_se_set[k];
            if (sid <= 0) continue;
            bool already = false;
            for (uint8_t j = 0; j < em.count; ++j) {
                if (em.se_ids[j] == sid) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                if (em.count < em.se_ids.size()) {
                    em.se_ids[em.count++] = sid;
                }
                int32_t flg = (k < tick_se_flg.size()) ? tick_se_flg[k] : 0;
                fn_app_CpCanvas_seSet___II__V(rt, sid - 11, flg);
            }
        }
        g_pvp_eng.in_combat_step = false;
        rt.set_suppress_av(prev_sup);
    };

    // Sync game_cnt & seed RNG deterministically per tick so panel ticks and random attacks stay 100% in sync
    sf_app_CpCanvas_game_cnt = static_cast<int32_t>(g_pvp_eng.core.combat_turn);
    if (sf_app_CpCanvas_rand > 0) {
        uint64_t seed = (static_cast<uint64_t>(g_pvp_eng.match_seed) +
                         static_cast<uint64_t>(g_pvp_eng.core.combat_turn) * 0x9E3779B97F4A7C15ULL) &
                        ((1ULL << 48) - 1ULL);
        rt.get(sf_app_CpCanvas_rand).rng_seed = seed;
    }

    // Check simultaneous Custom Screen request when Custom Gauge is full (128) & no Time-Freeze (blocked on Turn 15+)
    if (g_pvp_eng.core.custom_round < 15 &&
        sf_app_CpCanvas_cus_gage >= 128 && sf_app_CpCanvas_stop_cnt == 0) {
        int32_t p0_new = p0_mask & ~g_pvp_eng.core.sides[0].prev_input_mask;
        int32_t p1_new = p1_mask & ~g_pvp_eng.core.sides[1].prev_input_mask;
        constexpr int32_t kCustomTriggerBits = (1 << 21) | (1 << 22); // SOFT1 or SOFT2
        if ((p0_new & kCustomTriggerBits) || (p1_new & kCustomTriggerBits)) {
            g_pvp_eng.core.sides[0].prev_input_mask = p0_mask;
            g_pvp_eng.core.sides[1].prev_input_mask = p1_mask;
            g_pvp_eng.core.open_custom_req = true;
            g_pvp_eng.core.combat_turn++;
            ensure_board_orient(rt, 0);
            commit_tick_audio_and_finish();
            return;
        }
    }

    // Clear per-tick yellow attack warning highlights (BtPanel.field_5) before stepping attacks;
    // active HitCheck calls on this tick re-assert field_5 = 1, matching single-player BtPanel.Draw2 reset.
    if (sf_app_CpCanvas_panel > 0) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0) continue;
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p > 0) rt.field_i32(p, 5) = 0;
            }
        }
    }

    // Handle active Time-Freeze (stop_cnt > 0) — mirrors CpCanvas.BattleMain L_2998..L_3715
    if (sf_app_CpCanvas_stop_cnt > 0) {
        clear_side_nondim_hud(g_pvp_eng.core.sides[0]);
        clear_side_nondim_hud(g_pvp_eng.core.sides[1]);
        int owner = (g_pvp_eng.core.stop_owner == 1) ? 1 : 0;

        if (sf_app_CpCanvas_stop_cnt == 1) {
            // Rewind mid-frame Rock animation counters (MotionBack) and snapshot active i-frames /
            // Invis into tmp_muteki_cnt on both fighters WITHOUT clearing muteki_cnt (matching
            // CpCanvas.BattleMain L_3023..L_3052):
            //  - Pre-existing Invis / mercy i-frames (muteki_cnt > 0) stay > 0 during Time-Freeze
            //    so Attack.Hit L_389 blocks non-Invis-piercing Time-Freeze attacks (e.g. Roll),
            //    and restore cleanly without losing duration when Time-Freeze ends.
            //  - If a fighter enters Time-Freeze vulnerable (muteki_cnt == 0), Rock.Hit L_276..L_290
            //    moves Hit #1's newly-granted mercy i-frames into tmp_muteki_cnt and zeroes
            //    muteki_cnt so subsequent hits of multi-hit Time-Freeze chips still connect.
            for (int s = 0; s < 2; ++s) {
                load_side(rt, s);
                if (sf_app_Rock_ani_cnt > 0 && sf_app_CpCanvas_rock > 0) {
                    sf_app_Rock_ani_cnt = rt.field_i32(sf_app_CpCanvas_rock, 1);
                    sf_app_Rock_s_wait  = rt.field_i32(sf_app_CpCanvas_rock, 2);
                    sf_app_Rock_r_suu   = rt.field_i32(sf_app_CpCanvas_rock, 3);
                    sf_app_Rock_e_wait  = rt.field_i32(sf_app_CpCanvas_rock, 4);
                }
                sf_app_Rock_tmp_muteki_cnt = sf_app_Rock_muteki_cnt;
                save_side(rt, s);
            }
        }

        g_pvp_eng.active_side = owner;
        ensure_board_orient(rt, owner);
        load_side(rt, owner);
        sync_panel_occupancy_and_proxy(rt, owner);

        sf_app_CpCanvas_stop_cnt += 1;

        SideContext& opp = g_pvp_eng.core.sides[1 - owner];
        // 1. Step Attack.Move(0) on EVERY tick (stop_cnt >= 2) in its own pass
        for (int i = 0; i < 30; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (rt.field_i32(a, 38) != 0) {
                g_pvp_eng.active_ata_ref = a;
                fn_app_Attack_Move___I__V(rt, a, 0);
                g_pvp_eng.active_ata_ref = 0;
            }
        }
        // 2. Step Attack.HitGo(0) in its own pass (+ SuperNorthWind param[7]==18 barrier strip)
        for (int i = 0; i < 30; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (rt.field_i32(a, 38) != 0) {
                if (rt.field_i32(a, 20) != 0) {
                    Ref param_arr = rt.field_i32(a, 23);
                    if (param_arr > 0 && rt.array_length(param_arr) > 7 && rt.iaload(param_arr, 7) == 18) {
                        if (opp.bari > 0) {
                            opp.bari = 0;
                            if (opp.bari_id >= 0 && opp.bari_id < 30) {
                                rt.field_i32(rt.iaload(opp.ata_arr, opp.bari_id), 38) = 0;
                                opp.bari_id = -1;
                            }
                        }
                    }
                }
                if (rt.field_i32(a, 8) == 1) {
                    int32_t hx = rt.field_i32(a, 12);
                    int32_t hy = rt.field_i32(a, 13);
                    if (hx < 0 || hx > 5 || hy < 0 || hy > 2) continue;
                }
                g_pvp_eng.active_ata_ref = a;
                fn_app_Attack_HitGo___I__V(rt, a, 0);
                g_pvp_eng.active_ata_ref = 0;
            }
        }
        if (sf_app_CpCanvas_cus_sp > g_pvp_eng.core.shared_cus_sp) {
            g_pvp_eng.core.shared_cus_sp = std::clamp<int32_t>(sf_app_CpCanvas_cus_sp, 1, 4);
        }

        // 2.5. Elemental Trap (AntiFire..Wood, wana_flg == 101..104) strike on stop_cnt == 16
        //      Mirrors CpCanvas.BattleMain L_3262..L_3520 (Grass/Ice 2x bonus + panel reset + Ene.Hit)
        if (sf_app_CpCanvas_wana_flg >= 101 && sf_app_CpCanvas_wana_flg <= 104 &&
            sf_app_CpCanvas_stop_cnt == 16) {
            int32_t elem = sf_app_CpCanvas_wana_flg - 100;
            Ref e0 = (sf_app_CpCanvas_ene > 0) ? rt.iaload(sf_app_CpCanvas_ene, 0) : 0;
            if (e0 > 0 && rt.field_i32(e0, 23) != 0) {
                int32_t ex = std::clamp(rt.field_i32(e0, 11), 0, 5);
                int32_t ey = std::clamp(rt.field_i32(e0, 12), 0, 2);
                Ref epan = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, ey), ex);
                int32_t pjou = rt.field_i32(epan, 4);
                int32_t wpow = (g_pvp_eng.core.sides[owner].wana_pow0 > 0)
                    ? g_pvp_eng.core.sides[owner].wana_pow0
                    : 150;
                if ((elem == 1 && pjou == 4) || (elem == 3 && pjou == 5)) {
                    wpow *= 2;
                }
                if ((elem == 1 && pjou == 4) || (elem == 2 && pjou == 3)) {
                    fn_app_BtPanel_Henka___II__V(rt, epan, 1, 0);
                }
                g_pvp_eng.active_ata_ref = 0;
                fn_app_Ene_Hit___III__Z(rt, e0, 1, wpow, elem);
            }
            sf_app_CpCanvas_wana_flg = 0;
        }

        // 3. Advance sprite state matching CharaDraw(0): Rock.Draw(0) keeps AniSet(21) hidden during
        //    Navi summons, while Attack.Draw(1/2, 0) advances field_27 once per tick for stop_cnt >= 15
        fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 0);
        for (int i = 0; i < 30; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (rt.field_i32(a, 38) != 0) {
                fn_app_Attack_Draw___II__V(rt, a, 1, 0);
                fn_app_Attack_Draw___II__V(rt, a, 2, 0);
            }
        }

        if (sf_app_CpCanvas_stop_cnt >= sf_app_CpCanvas_stop_time + 4) {
            for (int i = 0; i < 30; ++i) {
                Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                if (rt.field_i32(a, 38) != 0 && rt.field_i32(a, 20) > 0) {
                    fn_app_Attack_init_____V(rt, a);
                }
            }
            save_side(rt, owner);
            // Restore mercy invincibility from tmp_muteki_cnt on both fighters
            for (int s = 0; s < 2; ++s) {
                load_side(rt, s);
                sf_app_Rock_muteki_cnt = sf_app_Rock_tmp_muteki_cnt;
                sf_app_Rock_tmp_muteki_cnt = 0;
                save_side(rt, s);
            }
            sf_app_CpCanvas_stop_cnt = 0;
            sf_app_CpCanvas_stop_time = 0;
            sf_app_CpCanvas_wana_flg = 0;
            sf_app_CpCanvas_key = 0;
            g_pvp_eng.core.stop_owner = -1;
        } else {
            save_side(rt, owner);
        }
    } else {
        for (int s_i = 0; s_i < 2; ++s_i) {
            SideContext& sc = g_pvp_eng.core.sides[s_i];
            if (sc.nondim_ticks > 0) {
                --sc.nondim_ticks;
                if (sc.nondim_ticks == 0) {
                    clear_side_nondim_hud(sc);
                }
            }
        }

        // Alternate execution priority each tick for 100% frame-tie fairness
        int first = static_cast<int>(g_pvp_eng.core.combat_turn & 1u);
        int second = 1 - first;
        int32_t masks[2] = {p0_mask, p1_mask};

        step_single_side(rt, cp, first, masks[first]);
        if (sf_app_CpCanvas_stop_cnt == 0) {
            step_single_side(rt, cp, second, masks[second]);
        }
        if (sf_app_CpCanvas_stop_cnt > 0) {
            clear_side_nondim_hud(g_pvp_eng.core.sides[0]);
            clear_side_nondim_hud(g_pvp_eng.core.sides[1]);
        }

        if (sf_app_CpCanvas_stop_cnt == 0) {
            // Step each player's owned obstacles (oki[0..1]) in their own perspective so:
            //  - Satelit1..3 spawn attacks into their owner's ata_arr targeting opponent (ene[0])
            //  - Wind / Fan push/pull ene[0].pos_x and write back to opponent's pos_x
            int order[2] = {first, second};
            for (int oi = 0; oi < 2; ++oi) {
                int s = order[oi];
                SideContext& opp = g_pvp_eng.core.sides[1 - s];
                g_pvp_eng.active_side = s;
                ensure_board_orient(rt, s);
                load_side(rt, s);
                sync_panel_occupancy_and_proxy(rt, s);

                Ref e0 = (sf_app_CpCanvas_ene > 0) ? rt.iaload(sf_app_CpCanvas_ene, 0) : 0;
                for (int i = 0; i < 2; ++i) {
                    Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                    if (ok > 0 && rt.field_i32(ok, 1) != 0) {
                        int32_t prev_e0_x = (e0 > 0) ? rt.field_i32(e0, 11) : -1;
                        g_pvp_eng.active_ata_ref = 0;
                        fn_app_Okimono_Move_____V(rt, ok);
                        fn_app_Okimono_Draw_____V(rt, ok);
                        if (e0 > 0 && opp.hp > 0 && opp.move_flg == 0) {
                            int32_t new_e0_x = rt.field_i32(e0, 11);
                            if (new_e0_x != prev_e0_x) {
                                opp.pos_x = std::clamp(5 - new_e0_x, 0, 5);
                                opp.tmp_x = opp.pos_x;
                                sync_panel_occupancy_and_proxy(rt, s);
                            }
                        }
                    }
                }
                save_side(rt, s);
            }

            // Step right-half panels (x = 5..3) in each player's perspective so both halves execute
            // BtPanel.JinCh's clean x >= 3 branch (avoiding Capcom's x < 3 on_chara == 0 bug!)
            for (int oi = 0; oi < 2; ++oi) {
                int s = order[oi];
                g_pvp_eng.active_side = s;
                ensure_board_orient(rt, s);
                load_side(rt, s);
                sync_panel_occupancy_and_proxy(rt, s);
                for (int x = 5; x >= 3; --x) {
                    for (int y = 0; y < 3; ++y) {
                        fn_app_BtPanel_Move_____V(rt, rt.iaload(rt.iaload(sf_app_CpCanvas_panel, y), x));
                    }
                }
                save_side(rt, s);
            }

            // Advance Custom Gauge (or forced 10s Turn 15 countdown when custom_round >= 15)
            if (g_pvp_eng.core.custom_round >= 15) {
                if (g_pvp_eng.core.final_turn_ticks_left > 0) {
                    g_pvp_eng.core.final_turn_ticks_left--;
                }
                sf_app_CpCanvas_cus_gage = static_cast<int32_t>(
                    (static_cast<uint32_t>(g_pvp_eng.core.final_turn_ticks_left) * 128u) /
                    kFinalTurnDurationTicks);
            } else if (sf_app_CpCanvas_cus_gage < 128) {
                sf_app_CpCanvas_cus_gage = std::min<int32_t>(128, sf_app_CpCanvas_cus_gage + g_pvp_eng.core.shared_cus_sp);
                if (sf_app_CpCanvas_cus_gage >= 128) {
                    fn_app_CpCanvas_seSet___II__V(rt, 11, 99);
                }
            }
        }
    }

    ensure_board_orient(rt, 0);
    if (sf_app_CpCanvas_soft_id > 0) {
        int32_t sid = (g_pvp_eng.core.custom_round < 15 && sf_app_CpCanvas_cus_gage >= 128) ? 1 : 0;
        rt.iaload(sf_app_CpCanvas_soft_id, 0) = sid;
        rt.iaload(sf_app_CpCanvas_soft_id, 1) = sid;
    }

    // Check KO / Simultaneous KO
    int32_t hp0 = g_pvp_eng.core.sides[0].hp;
    int32_t hp1 = g_pvp_eng.core.sides[1].hp;
    if (hp0 <= 0 && hp1 <= 0) {
        g_pvp_eng.core.winner = -1; // Draw
    } else if (hp0 <= 0) {
        g_pvp_eng.core.winner = 1;  // P1 wins
    } else if (hp1 <= 0) {
        g_pvp_eng.core.winner = 0;  // P0 wins
    }
    if (g_pvp_eng.core.winner == -2 &&
        g_pvp_eng.core.custom_round >= 15 &&
        sf_app_CpCanvas_stop_cnt == 0 &&
        g_pvp_eng.core.final_turn_ticks_left == 0) {
        g_pvp_eng.core.ended_by_judge = true;
        int32_t d0 = g_pvp_eng.core.damage_dealt[0];
        int32_t d1 = g_pvp_eng.core.damage_dealt[1];
        if (d0 > d1) {
            g_pvp_eng.core.winner = 0;
        } else if (d1 > d0) {
            g_pvp_eng.core.winner = 1;
        } else {
            g_pvp_eng.core.winner = -1;
        }
    }

    g_pvp_eng.core.combat_turn++;
    commit_tick_audio_and_finish();
}

static void set_rock_palette_fast(DojaRuntime& rt, int32_t zoku) {
    int32_t z = std::clamp<int32_t>(zoku, 0, 4);
    if (sf_app_CpCanvas_image > 0 && sf_app_CpCanvas_pal > 0) {
        Ref img0 = rt.iaload(sf_app_CpCanvas_image, 0);
        Ref pal_ref = rt.iaload(sf_app_CpCanvas_pal, z);
        if (img0 > 0 && pal_ref > 0) {
            Slot args[2];
            args[0].r = img0;
            args[1].r = pal_ref;
            rt.invoke_external("com/nttdocomo/ui/PalettedImage", "setPalette", "(Lcom/nttdocomo/ui/Palette;)V", args, 2);
            return;
        }
    }
    fn_app_CpCanvas_PalSet___II__V(rt, 0, z);
}

// Render an Attack slot in read-only mode using the exact animation frame (field_43) resolved by
// step_pvp_combat_tick / step_single_side, avoiding double-advancing Attack.Loop or blanking the final frame.
static void draw_attack_readonly(DojaRuntime& rt, Ref a, int layer) {
    int32_t stop_t = rt.field_i32(a, 20);
    if (stop_t != 0 && sf_app_CpCanvas_stop_cnt < 15) {
        return;
    }
    int32_t saved_20 = stop_t;
    int32_t saved_27 = rt.field_i32(a, 27);
    if (stop_t != 0 || sf_app_CpCanvas_stop_cnt == 0) {
        rt.field_i32(a, 27) = rt.field_i32(a, 43);
    }
    rt.field_i32(a, 20) = 0;
    fn_app_Attack_Draw___II__V(rt, a, layer, 0);
    rt.field_i32(a, 20) = saved_20;
    rt.field_i32(a, 27) = saved_27;
}

static void draw_mirrored_opponent_attacks(DojaRuntime& rt, int opp_idx, int layer, int target_row) {
    load_side(rt, opp_idx);
    set_rock_palette_fast(rt, sf_app_Rock_zoku);
    for (int i = 0; i < 30; ++i) {
        Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
        if (rt.field_i32(a, 38) == 0) continue;
        if (target_row >= 0 && rt.field_i32(a, 4) != target_row) continue;

        int32_t orig_px = rt.field_i32(a, 3);
        int32_t orig_flp = rt.field_i32(a, 40);

        // CpCanvas.drawImg2 already reverses Attack.dx (field 5) when flp flips (0 <-> 1);
        // keep Attack.dx unchanged so parabolic / sliding projectiles never double-negate dx!
        rt.field_i32(a, 3) = 5 - orig_px;
        rt.field_i32(a, 40) = 1 - orig_flp;

        draw_attack_readonly(rt, a, layer);

        rt.field_i32(a, 3) = orig_px;
        rt.field_i32(a, 40) = orig_flp;
    }
}

static Ref get_trap_mask_str_ref(DojaRuntime& rt) {
    if (g_pvp_eng.trap_mask_str > 0) {
        return g_pvp_eng.trap_mask_str;
    }
    return rt.alloc_string("????");
}

// Returns true when `owner_idx` is currently executing a trap-setting Time-Freeze chip
// (Attack.Move param[8] == 32 for AntiFire..AntiWood, or param[8] == 33 for AntiDmg / AntiNavi).
// Triggered traps (waza 95..98 elemental punish or stolen Navi summons) do NOT use param[8]==32/33,
// so triggered traps still reveal their real chip name on both clients.
static bool is_active_time_freeze_trap_arm(DojaRuntime& rt, int owner_idx) {
    if (owner_idx < 0 || owner_idx > 1) {
        return false;
    }
    Ref ata_arr = g_pvp_eng.core.sides[owner_idx].ata_arr;
    if (ata_arr <= 0) {
        return false;
    }
    int32_t len = std::min<int32_t>(30, rt.array_length(ata_arr));
    for (int32_t i = 0; i < len; ++i) {
        Ref a = rt.iaload(ata_arr, i);
        if (a > 0 && rt.field_i32(a, 38) != 0 && rt.field_i32(a, 20) > 0) {
            Ref param_arr = rt.field_i32(a, 23);
            if (param_arr > 0 && rt.array_length(param_arr) > 8) {
                int32_t p8 = rt.iaload(param_arr, 8);
                if (p8 == 32 || p8 == 33) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Draw active trap "????" HUD indicators matching native Rock.WanaDraw (pure-white DoJa font at y=75):
//  - Local active trap at left (x = 5, y = 75)
//  - Opponent active trap mirrored at right (x = 240 - 5 - stringWidth("????"), y = 75)
static void draw_active_trap_hud_indicators(DojaRuntime& rt, bool show_local, bool show_opponent) {
    if (!show_local && !show_opponent) {
        return;
    }
    Ref qmark_ref = get_trap_mask_str_ref(rt);
    Slot c_args[2];
    c_args[0].r = sf_app_CpCanvas_g;
    c_args[1].i = static_cast<int32_t>(0xFFFFFFFFu);
    rt.invoke_external("com/nttdocomo/ui/Graphics", "setColor", "(I)V", c_args, 2);

    if (show_local) {
        fn_app_CpCanvas_strDraw___Ljava_lang_String_II__V(rt, qmark_ref, 5, 75);
    }
    if (show_opponent) {
        int32_t qmark_w = 24;
        if (sf_app_CpCanvas_f > 0) {
            Slot w_args[2];
            w_args[0].r = sf_app_CpCanvas_f;
            w_args[1].r = qmark_ref;
            qmark_w = rt.invoke_external("com/nttdocomo/ui/Font", "stringWidth", "(Ljava/lang/String;)I", w_args, 2).i;
        }
        int32_t rx = std::clamp<int32_t>(240 - 5 - qmark_w, 0, std::max<int32_t>(0, 240 - qmark_w));
        fn_app_CpCanvas_strDraw___Ljava_lang_String_II__V(rt, qmark_ref, rx, 75);
    }
}

static void draw_opponent_nondim_chip_hud(DojaRuntime& rt, const SideContext& opp_sc) {
    if (sf_app_CpCanvas_tip <= 0 || opp_sc.nondim_tip_id < 0 ||
        opp_sc.nondim_tip_id >= rt.array_length(sf_app_CpCanvas_tip)) {
        return;
    }
    Ref tip_obj = rt.iaload(sf_app_CpCanvas_tip, opp_sc.nondim_tip_id);
    if (tip_obj <= 0) return;
    Ref name_ref = rt.field_i32(tip_obj, 9);
    if (name_ref <= 0) return;

    auto measure_str = [&](Ref sref) -> int32_t {
        if (sref <= 0 || sf_app_CpCanvas_f <= 0) return 0;
        Slot w_args[2];
        w_args[0].r = sf_app_CpCanvas_f;
        w_args[1].r = sref;
        return rt.invoke_external("com/nttdocomo/ui/Font", "stringWidth", "(Ljava/lang/String;)I", w_args, 2).i;
    };

    int32_t name_w = measure_str(name_ref);
    Ref dmg_ref = 0;
    int32_t dmg_w = 0;
    if (opp_sc.nondim_base_pow >= 0) {
        std::string dmg_str = " " + std::to_string(opp_sc.nondim_base_pow);
        if (opp_sc.nondim_plus_pow > 0) {
            dmg_str += "+" + std::to_string(opp_sc.nondim_plus_pow);
        }
        dmg_ref = rt.alloc_string(dmg_str);
        dmg_w = measure_str(dmg_ref);
    }

    int32_t total_w = name_w + dmg_w;
    int32_t raw_tx = 120 + (120 - total_w) / 2;
    int32_t max_tx = std::max<int32_t>(0, 238 - total_w);
    int32_t min_tx = std::min<int32_t>(120, max_tx);
    int32_t tx = std::clamp<int32_t>(raw_tx, min_tx, max_tx);

    int32_t ty = 80;
    if (opp_sc.wana != 0) {
        int32_t qmark_w = 24;
        if (sf_app_CpCanvas_f > 0) {
            qmark_w = measure_str(get_trap_mask_str_ref(rt));
        }
        int32_t trap_rx = std::clamp<int32_t>(240 - 5 - qmark_w, 0, std::max<int32_t>(0, 240 - qmark_w));
        if (tx + total_w > trap_rx - 2) {
            ty = 89;
        }
    }

    Slot c_args[2];
    c_args[0].r = sf_app_CpCanvas_g;
    c_args[1].i = static_cast<int32_t>(0xFFFFFFFFu);
    rt.invoke_external("com/nttdocomo/ui/Graphics", "setColor", "(I)V", c_args, 2);
    fn_app_CpCanvas_strDraw___Ljava_lang_String_II__V(rt, name_ref, tx, ty);

    if (dmg_ref > 0) {
        c_args[1].i = static_cast<int32_t>(0xFFFFFF00u);
        rt.invoke_external("com/nttdocomo/ui/Graphics", "setColor", "(I)V", c_args, 2);
        fn_app_CpCanvas_strDraw___Ljava_lang_String_II__V(rt, dmg_ref, tx + name_w, ty);
    }
}

static void render_pvp_combat_frame(DojaRuntime& rt, Ref cp) {
    // Snapshot canonical simulation state so PoN Draw methods cannot mutate any combat state
    HostRuntimeState host_st = capture_host_runtime_state(rt);
    PvpCombatCore saved_core = g_pvp_eng.core;
    generated_save_statics(g_pvp_eng.render_statics);
    g_pvp_eng.render_snap.base_heap_size = 0;
    rt.save_combat_snapshot(g_pvp_eng.render_snap, g_pvp_eng.mutable_heap_min_ref);

    rt.set_suppress_av(false);
    int loc_i = g_pvp_eng.local_idx;
    int opp_i = 1 - loc_i;

    ensure_board_orient(rt, loc_i);
    load_side(rt, loc_i);
    sync_panel_occupancy_and_proxy(rt, loc_i);

    // 1. Refresh arena backdrop (including CpCanvas.Ann Time-Freeze darkening/brightening) + all 18 battle panels
    bool allow_recovery_blink = (g_pvp_eng.phase == Phase::COMBAT && sf_app_CpCanvas_stop_cnt == 0);
    refresh_arena_imgmap_backdrop(rt, sf_app_CpCanvas_stop_cnt, sf_app_CpCanvas_stop_time, allow_recovery_blink);
    {
        Slot args[4];
        args[0].r = sf_app_CpCanvas_g;
        args[1].r = sf_app_CpCanvas_imgMap;
        args[2].i = 0;
        args[3].i = 0;
        rt.invoke_external("com/nttdocomo/ui/Graphics", "drawImage", "(Lcom/nttdocomo/ui/Image;II)V", args, 4);
    }

    // 2. Panel warning / effect overlays (BtPanel.Draw2)
    for (int y = 0; y < 3; ++y) {
        Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
        for (int x = 0; x < 6; ++x) {
            fn_app_BtPanel_Draw2_____V(rt, rt.iaload(row, x));
        }
    }

    // 3. Y-sorted 2.5D row rendering (row = 0, 1, 2): Local Rock, Mirrored Opponent Rock, Obstacles, Layer-1 Attacks
    for (int row = 0; row < 3; ++row) {
        // Opponent Rock (mirrored horizontally on right side, facing left)
        const SideContext& opp_sc = g_pvp_eng.core.sides[opp_i];
        if (opp_sc.hp > 0 && opp_sc.pos_y == row) {
            load_side(rt, opp_i);
            set_rock_palette_fast(rt, sf_app_Rock_zoku);
            int32_t saved_px = sf_app_Rock_pos_x;
            sf_app_Rock_pos_x = 5 - saved_px;
            g_pvp_rock_flip_override = 0; // Face Left (drawImg2 flips Rock.dx via flp=0)
            fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 0);
            g_pvp_rock_flip_override = -1;
            sf_app_Rock_pos_x = saved_px;
        }

        // Local Rock (left side, facing right)
        const SideContext& loc_sc = g_pvp_eng.core.sides[loc_i];
        if (loc_sc.hp > 0 && loc_sc.pos_y == row) {
            load_side(rt, loc_i);
            set_rock_palette_fast(rt, sf_app_Rock_zoku);
            g_pvp_rock_flip_override = 1; // Face Right!
            fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 0);
            g_pvp_rock_flip_override = -1;
        }

        // Obstacles on this row
        for (int i = 0; i < 6; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
            if (ok > 0 && rt.field_i32(ok, 1) != 0 && rt.field_i32(ok, 10) == row) {
                fn_app_Okimono_Draw_____V(rt, ok);
            }
        }

        // Local Layer-1 Attacks on this row
        load_side(rt, loc_i);
        set_rock_palette_fast(rt, sf_app_Rock_zoku);
        for (int i = 0; i < 30; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (rt.field_i32(a, 38) != 0 && rt.field_i32(a, 4) == row) {
                draw_attack_readonly(rt, a, 1);
            }
        }

        // Opponent Mirrored Layer-1 Attacks on this row
        draw_mirrored_opponent_attacks(rt, opp_i, 1, row);
    }

    // 4. Layer-2 overhead effects for both players
    load_side(rt, loc_i);
    set_rock_palette_fast(rt, sf_app_Rock_zoku);
    for (int i = 0; i < 30; ++i) {
        Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
        if (rt.field_i32(a, 38) != 0) {
            draw_attack_readonly(rt, a, 2);
        }
    }
    draw_mirrored_opponent_attacks(rt, opp_i, 2, -1);

    // 4b. Opponent foot HP digits (mirrors CpCanvas.CharaDraw -> Ene.HpDraw)
    if (sf_app_CpCanvas_ene > 0) {
        Ref e0 = rt.iaload(sf_app_CpCanvas_ene, 0);
        if (e0 > 0 && rt.field_i32(e0, 23) != 0) {
            fn_app_Ene_HpDraw_____V(rt, e0);
        }
    }

    // 5. Native Custom Gauge bar + Local HP + Local Queued Chips HUD
    //    Mirrors CpCanvas.BattleMain L_3178..L_3203 & L_3523 (during active Time-Freeze stop_cnt >= 2,
    //    only the Custom Gauge frame is drawn while its internal fill bar is hidden; active trap "????"
    //    stays visible in both normal combat and Time-Freeze).
    load_side(rt, loc_i);
    set_rock_palette_fast(rt, sf_app_Rock_zoku);
    fn_app_CpCanvas_drawImg3___IIIIZ__V(rt, 43, 0, 48, 40, 0);
    if (sf_app_CpCanvas_stop_cnt < 2) {
        uint32_t bar_rgb = 0xFFC6D2EAu;
        if (g_pvp_eng.core.custom_round >= 15) {
            if (g_pvp_eng.core.final_turn_ticks_left <= 48) {
                bar_rgb = (((sf_app_CpCanvas_game_cnt / 2) % 2) == 0) ? 0xFFFF4050u : 0xFFFFC858u;
            } else {
                bar_rgb = 0xFFFFB838u;
            }
        } else if (sf_app_CpCanvas_cus_gage >= 128) {
            int flash = ((sf_app_CpCanvas_game_cnt / 3) % 2) * 248;
            bar_rgb = 0xFF000000u | (static_cast<uint32_t>(flash) << 16) | (248u << 8) | static_cast<uint32_t>(flash);
        }
        Slot c_args[2];
        c_args[0].r = sf_app_CpCanvas_g;
        c_args[1].i = static_cast<int32_t>(bar_rgb);
        rt.invoke_external("com/nttdocomo/ui/Graphics", "setColor", "(I)V", c_args, 2);
        Slot r_args[5];
        r_args[0].r = sf_app_CpCanvas_g;
        r_args[1].i = 56;
        r_args[2].i = 50;
        r_args[3].i = std::clamp<int32_t>(sf_app_CpCanvas_cus_gage, 0, 128);
        r_args[4].i = 6;
        rt.invoke_external("com/nttdocomo/ui/Graphics", "fillRect", "(IIII)V", r_args, 5);
    }
    fn_app_Rock_HpDraw___II__V(rt, sf_app_CpCanvas_rock, 0, 0);

    // Top & bottom black letterbox bars + queued chip icons
    {
        Slot c_args[2];
        c_args[0].r = sf_app_CpCanvas_g;
        c_args[1].i = static_cast<int32_t>(0xFF000000u);
        rt.invoke_external("com/nttdocomo/ui/Graphics", "setColor", "(I)V", c_args, 2);
        Slot r_args[5];
        r_args[0].r = sf_app_CpCanvas_g;
        r_args[1].i = 0; r_args[2].i = 0; r_args[3].i = 240; r_args[4].i = 40;
        rt.invoke_external("com/nttdocomo/ui/Graphics", "fillRect", "(IIII)V", r_args, 5);
        r_args[1].i = 0; r_args[2].i = 200; r_args[3].i = 240; r_args[4].i = 40;
        rt.invoke_external("com/nttdocomo/ui/Graphics", "fillRect", "(IIII)V", r_args, 5);
    }
    fn_app_Rock_TipDraw_____V(rt, sf_app_CpCanvas_rock);

    const SideContext& loc_sc = g_pvp_eng.core.sides[loc_i];
    const SideContext& opp_sc = g_pvp_eng.core.sides[opp_i];

    // Native virus/boss battle Time-Freeze chip name announcement (mirrors CpCanvas.BattleMain L_3203..L_3259):
    // Unboxed pure-white DoJa font text at y=80, centered over left half (stop_ch=0) for local fighter
    // or right half (stop_ch=1) for opponent, shown strictly during stop_cnt == 2..14 before attack frames start.
    // When the opponent arms a trap chip (Kawarimi / AntiDmg, AntiFire..Wood, AntiNavi), mask its name as "????"
    // on our client while the trap user's own client still sees the real chip name.
    if (sf_app_CpCanvas_stop_cnt >= 2 && sf_app_CpCanvas_stop_cnt < 15 && sf_app_CpCanvas_stop_name > 0) {
        int32_t stop_ch = (g_pvp_eng.core.stop_owner == opp_i) ? 1 : 0;
        sf_app_CpCanvas_stop_ch = stop_ch;

        Ref disp_name = sf_app_CpCanvas_stop_name;
        if (stop_ch == 1 && is_active_time_freeze_trap_arm(rt, opp_i)) {
            disp_name = get_trap_mask_str_ref(rt);
        }

        Slot c_args[2];
        c_args[0].r = sf_app_CpCanvas_g;
        c_args[1].i = static_cast<int32_t>(0xFFFFFFFFu);
        rt.invoke_external("com/nttdocomo/ui/Graphics", "setColor", "(I)V", c_args, 2);

        int32_t str_w = 0;
        if (sf_app_CpCanvas_f > 0) {
            Slot w_args[2];
            w_args[0].r = sf_app_CpCanvas_f;
            w_args[1].r = disp_name;
            str_w = rt.invoke_external("com/nttdocomo/ui/Font", "stringWidth", "(Ljava/lang/String;)I", w_args, 2).i;
        }
        int32_t tx = (120 - str_w) / 2 + 120 * stop_ch;
        tx = std::clamp<int32_t>(tx, 0, std::max<int32_t>(0, 240 - str_w));
        fn_app_CpCanvas_strDraw___Ljava_lang_String_II__V(rt, disp_name, tx, 80);
    }

    if (g_pvp_eng.phase == Phase::COMBAT && sf_app_CpCanvas_stop_cnt == 0 &&
        opp_sc.nondim_ticks > 0 && opp_sc.nondim_tip_id >= 0) {
        draw_opponent_nondim_chip_hud(rt, opp_sc);
    }

    // Active trap "????" HUD indicators (left x=5,y=75 for local; mirrored right y=75 for opponent)
    draw_active_trap_hud_indicators(rt, loc_sc.wana != 0, opp_sc.wana != 0);

    // 6. Top HUD Bar: Local Name + HP vs Opponent Name + HP + Live Ping
    std::string loc_name = g_pvp_eng.builds[loc_i].player_name;
    std::string opp_name = g_pvp_eng.builds[opp_i].player_name;

    rt.fb_fill_rect_alpha(3, 4, 104, 32, 0xE014203Cu);
    rt.fb_draw_border(3, 4, 104, 32, 0xFF4C88FFu);
    rt.fb_draw_text(loc_name, 7, 16, 0xFF88CCFFu);
    std::string loc_hp_str = "HP " + std::to_string(std::max(0, loc_sc.hp)) + "/" + std::to_string(loc_sc.max_hp);
    rt.fb_draw_text(loc_hp_str, 7, 31, (loc_sc.hp * 4 <= loc_sc.max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);

    if (g_pvp_eng.is_triple_battle) {
        rt.fb_draw_text("G" + std::to_string(g_pvp_eng.bo3_game_index + 1), 113, 15, 0xFFFFD84Cu);
        rt.fb_draw_text(std::to_string(g_pvp_eng.bo3_wins[loc_i]) + "-" +
                        std::to_string(g_pvp_eng.bo3_wins[opp_i]),
                        111, 31, 0xFFFFFFFFu);
    } else {
        rt.fb_draw_text("VS", 113, 24, 0xFFFFD84Cu);
    }

    rt.fb_fill_rect_alpha(133, 4, 104, 32, 0xE0381420u);
    rt.fb_draw_border(133, 4, 104, 32, 0xFFFF5868u);
    rt.fb_draw_text(opp_name, 137, 16, 0xFFFF98A8u);
    std::string opp_hp_str = "HP " + std::to_string(std::max(0, opp_sc.hp)) + "/" + std::to_string(opp_sc.max_hp);
    rt.fb_draw_text(opp_hp_str, 137, 31, (opp_sc.hp * 4 <= opp_sc.max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);

    if (g_pvp_eng.core.custom_round >= 15 && sf_app_CpCanvas_stop_cnt < 2) {
        int secs = (static_cast<int>(g_pvp_eng.core.final_turn_ticks_left) + 15) / 16;
        int tenths = (static_cast<int>(g_pvp_eng.core.final_turn_ticks_left) * 10) / 16;
        std::string cd_txt = "FINAL " + std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "s";
        rt.fb_fill_rect_alpha(74, 37, 92, 13, 0xE81C0C14u);
        rt.fb_draw_border(74, 37, 92, 13, (secs <= 3) ? 0xFFFF4858u : 0xFFFFC84Cu);
        rt.fb_draw_text(cd_txt, 120 - rt.fb_measure_text(cd_txt) / 2, 47, (secs <= 3) ? 0xFFFF7888u : 0xFFFFF088u);
    }

    // Restore canonical simulation state (100% read-only rendering, zero heap growth)
    // Restore exact simulation state and pre-render SE queue (live AniSe cues were already queued by step_pvp_combat_tick)
    g_pvp_eng.core = std::move(saved_core);
    generated_restore_statics(g_pvp_eng.render_statics);
    rt.restore_combat_snapshot(g_pvp_eng.render_snap);
    restore_host_runtime_state(rt, host_st, true);
}

static void draw_native_battle_start_sprite(DojaRuntime& rt, int frame_idx, int32_t center_y_offset = 0) {
    static constexpr int32_t kHeights[15] = {4, 9, 16, 18, 16, 16, 16, 16, 16, 16, 16, 16, 16, 9, 4};
    int32_t h = kHeights[std::clamp(frame_idx, 0, 14)];
    Ref img43 = (sf_app_CpCanvas_image > 0 && rt.array_length(sf_app_CpCanvas_image) > 43)
        ? rt.iaload(sf_app_CpCanvas_image, 43)
        : 0;
    if (img43 <= 0 || sf_app_CpCanvas_g <= 0) {
        return;
    }
    bool prev_sup = rt.suppress_av();
    rt.set_suppress_av(false);
    int32_t dy = 104 + center_y_offset + (16 - h) / 2;
    Slot args[10];
    args[0].r = sf_app_CpCanvas_g;
    args[1].r = img43;
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
    rt.set_suppress_av(prev_sup);
}

static void render_round_banner_overlay(DojaRuntime& rt, int frame_idx, uint16_t custom_round) {
    static constexpr int32_t kHeights[15] = {4, 9, 16, 18, 16, 16, 16, 16, 16, 16, 16, 16, 16, 9, 4};
    int idx = std::clamp(frame_idx, 0, 14);
    int32_t kh = kHeights[idx];

    if (custom_round <= 1) {
        draw_native_battle_start_sprite(rt, idx, 8);
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

static void play_pvp_result_bgm(DojaRuntime& rt) {
    if (g_pvp_eng.core.winner == g_pvp_eng.local_idx) {
        fn_app_CpCanvas_Audio___II__V(rt, 1, 5); // Victory / Enemy Deleted fanfare
    } else if (g_pvp_eng.core.winner >= 0) {
        fn_app_CpCanvas_Audio___II__V(rt, 1, 6); // Defeat BGM
    } else {
        fn_app_CpCanvas_Audio___II__V(rt, 0, 0); // Draw: stop BGM cleanly
    }
}

static void conclude_current_game(DojaRuntime& rt, Ref cp) {
    (void)cp;
    pvp::PvpClientSession& net = rt.pvp_session();
    int game_win = g_pvp_eng.core.winner;
    if (g_pvp_eng.bo3_game_index < 3) {
        g_pvp_eng.bo3_game_results[g_pvp_eng.bo3_game_index] = static_cast<int8_t>(game_win);
    }
    if (game_win == 0) {
        g_pvp_eng.bo3_wins[0]++;
    } else if (game_win == 1) {
        g_pvp_eng.bo3_wins[1]++;
    }

    if (!g_pvp_eng.is_triple_battle) {
        g_pvp_eng.match_winner = game_win;
        g_pvp_eng.phase = Phase::RESULT;
        g_pvp_eng.result_ticks = 0;
        play_pvp_result_bgm(rt);
        uint8_t res_code = (game_win == -1)
            ? 3
            : ((game_win == g_pvp_eng.local_idx) ? 1 : 2);
        if (net.active()) {
            net.send_match_end(res_code);
        }
        return;
    }

    bool series_done = (g_pvp_eng.bo3_wins[0] >= 2 ||
                        g_pvp_eng.bo3_wins[1] >= 2 ||
                        g_pvp_eng.bo3_game_index >= 2);
    if (!series_done) {
        g_pvp_eng.phase = Phase::ROUND_RESULT;
        g_pvp_eng.round_result_ticks = 0;
        play_pvp_result_bgm(rt);
        // Never send PKT_MATCH_END mid-series (server prunes rooms 6s after PKT_MATCH_END).
        return;
    }

    if (g_pvp_eng.bo3_wins[0] > g_pvp_eng.bo3_wins[1]) {
        g_pvp_eng.match_winner = 0;
    } else if (g_pvp_eng.bo3_wins[1] > g_pvp_eng.bo3_wins[0]) {
        g_pvp_eng.match_winner = 1;
    } else {
        g_pvp_eng.match_winner = -1;
    }
    g_pvp_eng.core.winner = g_pvp_eng.match_winner;
    g_pvp_eng.phase = Phase::RESULT;
    g_pvp_eng.result_ticks = 0;
    play_pvp_result_bgm(rt);
    uint8_t res_code = (g_pvp_eng.match_winner == -1)
        ? 3
        : ((g_pvp_eng.match_winner == g_pvp_eng.local_idx) ? 1 : 2);
    if (net.active()) {
        net.send_match_end(res_code);
    }
}

static void start_next_bo3_game(DojaRuntime& rt, Ref cp) {
    g_pvp_eng.bo3_game_index++;
    if (g_pvp_eng.has_pre_match_snap) {
        int64_t saved_mill = sf_app_CpCanvas_mill;
        fn_app_CpCanvas_Audio___II__V(rt, 0, 0);
        rt.restore_full_snapshot(g_pvp_eng.pre_match_heap);
        generated_restore_statics(g_pvp_eng.pre_match_statics);
        sf_app_CpCanvas_mill = saved_mill;
    }
    g_pvp_eng.match_seed = derive_bo3_game_seed(g_pvp_eng.base_match_seed, g_pvp_eng.bo3_game_index);
    g_pvp_eng.round_result_ticks = 0;
    init_pvp_battle_arena(rt, cp);
    enter_custom_screen_phase(rt, cp);
}

static void cleanup_and_restore_singleplayer(DojaRuntime& rt, bool close_net = true) {
    pvp::PvpClientSession& net = rt.pvp_session();
    if (close_net) {
        net.close_udp();
    }
    rt.set_pvp_match_active(false);
    rt.set_suppress_av(false);
    rt.set_keypad_override(false, 0);
    g_pvp_rock_flip_override = -1;
    g_pvp_eng.in_combat_step = false;

    if (g_pvp_eng.has_pre_match_snap) {
        fn_app_CpCanvas_Audio___II__V(rt, 0, 0); // Stop PvP BGM before restoring single-player state
        rt.restore_full_snapshot(g_pvp_eng.pre_match_heap);
        generated_restore_statics(g_pvp_eng.pre_match_statics);
        g_pvp_eng.has_pre_match_snap = false;
        g_pvp_eng.pre_match_heap = FullHeapSnapshot{};
        g_pvp_eng.pre_match_statics.clear();
        sf_app_CpCanvas_play_flg = 1; // Resume pre-match BGM cleanly
        sf_app_CpCanvas_mill = 0;     // Prevent frame-timer skew after restoring snapshot
    }
    sf_app_CpCanvas_key = 0;
    g_pvp_eng.phase = Phase::IDLE;
    g_pvp_eng.banner_ticks = 0;
    g_pvp_eng.judge_ticks = 0;
    g_pvp_eng.round_result_ticks = 0;
    g_pvp_eng.base_match_seed = 0;
    g_pvp_eng.is_triple_battle = false;
    g_pvp_eng.bo3_game_index = 0;
    g_pvp_eng.bo3_wins[0] = 0;
    g_pvp_eng.bo3_wins[1] = 0;
    g_pvp_eng.bo3_game_results[0] = -2;
    g_pvp_eng.bo3_game_results[1] = -2;
    g_pvp_eng.bo3_game_results[2] = -2;
    g_pvp_eng.match_winner = -2;
    g_pvp_eng.desync_warnings = 0;
    g_pvp_eng.active_room_id = 0;
    g_pvp_eng.build_prepared = false;
    g_pvp_eng.last_build_keepalive_ms = 0;
}

} // namespace pvp_pon

// ============================================================================
// Intercept Ene.Hit(III)Z on ene[0] proxy and route directly to defender Rock.Hit
// Authentically implements Guard / CrsShld / Curse shields, Breaking shield shatter,
// and AntiDmg (Kawarimi Magic waza 99 shuriken) matching Attack.Hit L_816..L_1375.
// ============================================================================
static int32_t pvp_pon_intercept_ene_hit(DojaRuntime& rt, Ref ene_thiz, int32_t hit_kou, int32_t pow, int32_t zoku) {
    using namespace pvp_pon;
    if (!g_pvp_eng.in_combat_step || sf_app_CpCanvas_ene <= 0) {
        return -1; // Normal single-player execution
    }
    if (ene_thiz != rt.iaload(sf_app_CpCanvas_ene, 0)) {
        return -1;
    }

    int att_idx = g_pvp_eng.active_side;
    int def_idx = 1 - att_idx;
    Ref att_ata = g_pvp_eng.active_ata_ref;
    bool is_breaking = (att_ata > 0) && (rt.field_i32(att_ata, 19) != 0);

    save_side(rt, att_idx);
    ensure_board_orient(rt, def_idx);
    load_side(rt, def_idx);
    sync_panel_occupancy_and_proxy(rt, def_idx);
    int32_t prev_def_hp = std::max<int32_t>(0, sf_app_Rock_hp);

    const SideContext& att_sc = g_pvp_eng.core.sides[att_idx];
    int32_t hit_res = 0;
    bool terminate_att_ata = false;
    bool mark_att_ata_hit = false;
    bool kawarimi_triggered = false;

    // 1. Shield / Reflect check (Guard han==1, CrsShld han==2, Curse PA han==3)
    if (sf_app_Rock_han > 0 && pow > 0 && sf_app_Rock_han_id >= 0 && sf_app_Rock_han_id < 30) {
        Ref shield_ata = rt.iaload(sf_app_CpCanvas_ata, sf_app_Rock_han_id);
        if (is_breaking) {
            // Breaking attack shatters active shield and deals full hit!
            if (shield_ata > 0) fn_app_Attack_init_____V(rt, shield_ata);
            sf_app_Rock_han = 0;
            sf_app_Rock_han_id = -1;
            sf_app_Rock_muteki_cnt = 0;
            if (sf_app_CpCanvas_stop_cnt != 0) sf_app_Rock_tmp_muteki_cnt = 0;
            hit_res = fn_app_Rock_Hit___III__Z(rt, sf_app_CpCanvas_rock, hit_kou, pow, zoku);
        } else {
            int32_t han_mode = sf_app_Rock_han;
            int32_t shield_pow = (shield_ata > 0) ? rt.field_i32(shield_ata, 10) : 50;
            if (han_mode == 1) {
                int32_t s_no = fn_app_CpCanvas_AtaNo___I__I(rt, sf_app_Rock_han_id);
                Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s_no);
                fn_app_Attack_Set___IIIII__I(rt, c_ata, 52, sf_app_Rock_pos_x, sf_app_Rock_pos_y, 1, 0);
                rt.field_i32(c_ata, 10) = shield_pow;
            } else if (han_mode == 2) {
                int32_t s_no = fn_app_CpCanvas_AtaNo___I__I(rt, sf_app_Rock_han_id);
                Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s_no);
                fn_app_Attack_Set___IIIII__I(rt, c_ata, 89, sf_app_Rock_pos_x, sf_app_Rock_pos_y, 1, -2);
                rt.field_i32(c_ata, 39) = 0;
                rt.field_i32(c_ata, 10) = shield_pow;
            } else if (han_mode == 3) {
                for (int k = 0; k < 3; ++k) {
                    int32_t cy = sf_app_Rock_pos_y + k - 1;
                    if (cy < 0 || cy > 2) continue;
                    int32_t s_no = fn_app_CpCanvas_AtaNo___I__I(rt, sf_app_Rock_han_id);
                    Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s_no);
                    fn_app_Attack_Set___IIIII__I(rt, c_ata, 89, sf_app_Rock_pos_x, cy, 1, -2);
                    rt.field_i32(c_ata, 39) = 0;
                    rt.field_i32(c_ata, 10) = shield_pow;
                }
            }
            fn_app_Rock_EffSet___I__V(rt, sf_app_CpCanvas_rock, 12);
            if (han_mode != 1) {
                if (shield_ata > 0) fn_app_Attack_init_____V(rt, shield_ata);
                sf_app_Rock_han = 0;
                sf_app_Rock_han_id = -1;
                sf_app_Rock_muteki_cnt = 0;
                if (sf_app_CpCanvas_stop_cnt != 0) sf_app_Rock_tmp_muteki_cnt = 0;
            }
            terminate_att_ata = true;
            hit_res = 0;
        }
    }
    // 2. AntiDmg / Kawarimi Magic trap (wana == 5)
    else if (sf_app_Rock_wana == 5 && sf_app_Rock_muteki_cnt == 0 && sf_app_Rock_bari == 0 && pow >= 10) {
        int32_t wpow = (g_pvp_eng.core.sides[def_idx].wana_pow0 > 0)
            ? g_pvp_eng.core.sides[def_idx].wana_pow0
            : 100;
        sf_app_Rock_wana = 0;
        // Set muteki_cnt = 10 immediately even during Time-Freeze (matching Attack.HitGo L_1233..L_1235)
        // so e0.field_25 != 0 blocks ALL subsequent hits of multi-hit Time-Freeze attacks (e.g. Roll),
        // and preserve >= 10 post-Time-Freeze i-frames in tmp_muteki_cnt when Time-Freeze ends.
        sf_app_Rock_muteki_cnt = 10;
        if (sf_app_CpCanvas_stop_cnt != 0) {
            sf_app_Rock_tmp_muteki_cnt = std::max<int32_t>(sf_app_Rock_tmp_muteki_cnt, 10);
        }
        sf_app_Rock_hit_cnt = 10;
        // Spawn authentic Kawarimi smoke puff (effect 59, mode 2) matching Attack.HitGo L_1248..L_1260
        // instead of Rock.EffSet(39) (which is the 2x elemental weakness "!!" icon).
        {
            int32_t eff_no = fn_app_CpCanvas_AtaNo___I__I(rt, 0);
            Ref eff_ata = rt.iaload(sf_app_CpCanvas_ata, eff_no);
            fn_app_Attack_Set___IIIII__I(rt, eff_ata, 59, sf_app_Rock_pos_x, sf_app_Rock_pos_y, 0, 2);
        }

        if (att_sc.hp > 0) {
            int32_t tx = std::clamp(5 - att_sc.pos_x, 0, 5);
            int32_t ty = std::clamp(att_sc.pos_y, 0, 2);
            int32_t s_no = fn_app_CpCanvas_AtaNo___I__I(rt, 0);
            Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s_no);
            fn_app_Attack_Set___IIIII__I(rt, c_ata, 99, tx, ty, 1, -2);
            rt.field_i32(c_ata, 39) = 0;
            rt.field_i32(c_ata, 10) = wpow;
            rt.field_i32(c_ata, 11) = 0;
        }
        mark_att_ata_hit = true;
        kawarimi_triggered = true;
        hit_res = 0;
    } else if (sf_app_Rock_muteki_cnt > 0 && att_ata > 0) {
        // All Attack-based chips/summons (including Time-Freeze attacks like Roll) cannot pierce
        // Invis / active i-frames (matching Attack.Hit L_389 & L_1175); only direct Elemental Trap
        // strikes (AntiFire..AntiWood, wana_flg == 101..104 where att_ata == 0) bypass muteki_cnt
        // during Time-Freeze via Rock.Hit L_20..L_23 (matching CpCanvas.BattleMain L_3483 -> Ene.Hit).
        hit_res = 0;
    } else {
        hit_res = fn_app_Rock_Hit___III__Z(rt, sf_app_CpCanvas_rock, hit_kou, pow, zoku);
    }

    int32_t post_def_hp = std::max<int32_t>(0, sf_app_Rock_hp);
    if (post_def_hp < prev_def_hp) {
        g_pvp_eng.core.damage_dealt[att_idx] += (prev_def_hp - post_def_hp);
    }
    sf_app_CpCanvas_now_hp = sf_app_Rock_hp;
    save_side(rt, def_idx);

    // Restore attacker context & proxy
    ensure_board_orient(rt, att_idx);
    load_side(rt, att_idx);
    if (att_ata > 0) {
        if (terminate_att_ata) {
            fn_app_Attack_init_____V(rt, att_ata);
            rt.field_i32(att_ata, 47) = 1;
        } else if (mark_att_ata_hit) {
            rt.field_i32(att_ata, 41) |= 4;
            rt.field_i32(att_ata, 47) = 1;
            if (kawarimi_triggered) {
                rt.field_i32(att_ata, 15) = 0;
            }
            Ref p_arr = rt.field_i32(att_ata, 23);
            if (p_arr > 0 && rt.array_length(p_arr) > 3 && rt.iaload(p_arr, 3) == 2) {
                fn_app_Attack_init_____V(rt, att_ata);
            }
        }
    }
    save_side(rt, att_idx);
    sync_panel_occupancy_and_proxy(rt, att_idx);

    return hit_res;
}

// ============================================================================
// Intercept Ene.Draw(I)V in PvP (e.g. during Custom Screen CharaDraw(0))
// Renders the opponent's Rockman mirrored facing left instead of Mettaur!
// ============================================================================
static bool pvp_pon_intercept_ene_draw(DojaRuntime& rt, Ref ene_thiz, int32_t adv) {
    (void)adv;
    using namespace pvp_pon;
    if (g_pvp_eng.phase < Phase::CUSTOM_SCREEN || sf_app_CpCanvas_ene <= 0) {
        return false; // Normal single-player execution (including LOBBY_WAIT)
    }
    int opp_i = 1 - g_pvp_eng.local_idx;
    Ref opp_rock = g_pvp_eng.core.sides[opp_i].rock_ref;
    if (opp_rock <= 0) {
        return false;
    }
    if (ene_thiz != rt.iaload(sf_app_CpCanvas_ene, 0)) {
        return true; // Skip unused stage-0 enemy slots
    }

    PvpCombatCore saved_core = g_pvp_eng.core;
    std::vector<int64_t> saved_st;
    generated_save_statics(saved_st);
    std::vector<int32_t> saved_opp_fields = rt.get(opp_rock).fields_i32;

    if (g_pvp_eng.core.sides[opp_i].hp > 0) {
        load_side(rt, opp_i);
        set_rock_palette_fast(rt, sf_app_Rock_zoku);
        sf_app_Rock_pos_x = 5 - sf_app_Rock_pos_x;
        sf_app_Rock_dx = -sf_app_Rock_dx;
        if (g_pvp_eng.phase == Phase::CUSTOM_SCREEN) {
            sf_app_Rock_muteki_cnt = 0;
            sf_app_Rock_hit_cnt = 0;
            sf_app_Rock_non_cnt2 = 0;
        }
        g_pvp_rock_flip_override = 0; // Face Left!
        fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 0);
        g_pvp_rock_flip_override = -1;
    }

    rt.get(opp_rock).fields_i32 = std::move(saved_opp_fields);
    g_pvp_eng.core = std::move(saved_core);
    generated_restore_statics(saved_st);
    set_rock_palette_fast(rt, sf_app_Rock_zoku);
    return true;
}

// ============================================================================
// Automated Headless PvP Self-Test (--pvp-self-test) & Live Bot (--pvp-auto-bot)
// ============================================================================
namespace pvp_pon {

static int32_t compute_auto_bot_combat_input(DojaRuntime& rt) {
    const int loc_i = g_pvp_eng.local_idx;
    const int opp_i = 1 - loc_i;
    const SideContext& me = g_pvp_eng.core.sides[loc_i];
    const SideContext& opp = g_pvp_eng.core.sides[opp_i];
    const uint32_t t = g_pvp_eng.core.combat_turn;

    // Open Custom Screen once gauge is full and hand is spent (up to 6 rounds, never on Turn 15+) to exercise multi-round sync
    if (g_pvp_eng.core.custom_round < 15 &&
        sf_app_CpCanvas_cus_gage >= 128 && sf_app_CpCanvas_stop_cnt == 0 &&
        me.tip_no < 0 && g_pvp_eng.core.custom_round < 6 && (t % 8u) == static_cast<uint32_t>(loc_i * 4)) {
        return (1 << 21); // SOFT1
    }

    int32_t mask = 0;
    uint32_t phase_t = t + static_cast<uint32_t>(loc_i * 11);

    // Align row with opponent so chips and buster shots connect reliably, with periodic strafing
    if ((phase_t % 10u) == 0u) {
        if (me.pos_y < opp.pos_y) {
            mask |= (1 << 19); // DOWN
        } else if (me.pos_y > opp.pos_y) {
            mask |= (1 << 17); // UP
        } else if ((phase_t % 30u) == 0u) {
            mask |= (me.pos_y == 0) ? (1 << 19) : (1 << 17);
        }
    } else if ((phase_t % 14u) == 5u) {
        mask |= (me.pos_x < 2) ? (1 << 18) : (1 << 16); // RIGHT / LEFT
    }

    // Use queued BattleChips when aligned or periodically
    if (me.tip_no >= 0 && ((phase_t % 12u) == 2u || (me.pos_y == opp.pos_y && (phase_t % 6u) == 2u))) {
        mask |= (1 << 20); // SELECT (Use Chip)
    } else {
        // Alternate rapid Buster taps and Charge Shots
        uint32_t cyc = phase_t % 32u;
        if (cyc < 22u) {
            mask |= (1 << 11); // Hold '#' to charge
        } else if ((cyc & 1u) == 1u && cyc >= 25u) {
            mask |= (1 << 11); // Rapid Buster tap
        }
    }
    return mask;
}

static int run_pvp_headless_self_test(DojaRuntime& rt, Ref cp) {
    std::printf("[PvP SELF-TEST] Starting comprehensive PoN PvP verification suite...\n");
    std::fflush(stdout);

    FullHeapSnapshot base_heap;
    std::vector<int64_t> base_statics;
    rt.save_full_snapshot(base_heap);
    generated_save_statics(base_statics);

    int failures = 0;
    auto fail_msg = [&](const char* suite, const std::string& detail) {
        ++failures;
        std::fprintf(stderr, "[PvP SELF-TEST FAIL] [%s] %s\n", suite, detail.c_str());
    };

    auto tip_name_of = [&](int tid) -> std::string {
        if (sf_app_CpCanvas_tip <= 0 || tid < 0 || tid >= rt.array_length(sf_app_CpCanvas_tip)) {
            return "?";
        }
        Ref t = rt.iaload(sf_app_CpCanvas_tip, tid);
        if (t <= 0) return "?";
        Ref sref = rt.field_i32(t, 9);
        return (sref > 0) ? rt.get(sref).str : "?";
    };

    auto reset_test_arena = [&](int32_t hp0, int32_t hp1, int32_t zoku0 = 0, int32_t zoku1 = 0, int32_t ata_lv = 1) {
        rt.restore_full_snapshot(base_heap);
        generated_restore_statics(base_statics);
        g_pvp_eng = PvpEngineState{};
        g_pvp_eng.local_idx = 0;
        g_pvp_eng.match_seed = 0x13579BDFu;
        g_pvp_eng.phase = Phase::COMBAT;

        for (int s = 0; s < 2; ++s) {
            pvp::PvpFighterBuild& b = g_pvp_eng.builds[s];
            b = pvp::PvpFighterBuild{};
            b.player_name = (s == 0) ? "TEST_P0" : "TEST_P1";
            b.max_hp = 500;
            b.ata_lv = static_cast<uint8_t>(std::clamp(ata_lv, 1, 5));
            b.cha_lv = 5;
            b.r_zoku = static_cast<uint8_t>((s == 0) ? zoku0 : zoku1);
            b.regu_flg = 0;
            b.regu_id = 0;
            for (int i = 0; i < 30; ++i) {
                b.folder_packed[i] = (2 << 8) | 0;
            }
        }

        init_pvp_battle_arena(rt, cp);
        for (int s = 0; s < 2; ++s) {
            load_side(rt, s);
            sf_app_Rock_hp = (s == 0) ? hp0 : hp1;
            sf_app_CpCanvas_now_hp = sf_app_Rock_hp;
            save_side(rt, s);
        }
        ensure_board_orient(rt, 0);
        sf_app_CpCanvas_cus_gage = 0;
        sf_app_CpCanvas_scene = 1;
        sf_app_CpCanvas_stop_cnt = 0;
        sf_app_CpCanvas_stop_time = 0;
    };

    auto queue_tips_fifo = [&](int side_idx, const std::vector<int16_t>& play_order) {
        pvp::PvpCustomCommit c{};
        c.turn_no = 1;
        c.max_sel = 5;
        c.pa_no = -1;
        for (int i = 0; i < 9; ++i) c.rock_tip_ids[i] = -1;
        int n = std::min<int>(static_cast<int>(play_order.size()), 9);
        c.tip_no = static_cast<int8_t>(n - 1);
        // Rock.TipNext reads from tip_no downward (LIFO), so reverse play_order into rock_tip_ids[0..n-1]
        for (int k = 0; k < n; ++k) {
            c.rock_tip_ids[n - 1 - k] = play_order[k];
        }
        ensure_board_orient(rt, side_idx);
        apply_custom_commit_to_side(rt, side_idx, c);
        ensure_board_orient(rt, 0);
    };

    // ------------------------------------------------------------------------
    // Suite A: Full 213-Tip (0..212) Solo Mirror Symmetry + Clash + Render Test
    // ------------------------------------------------------------------------
    reset_test_arena(400, 400);
    const int32_t tip_len = rt.array_length(sf_app_CpCanvas_tip);
    if (tip_len < 213) {
        fail_msg("SuiteA", "Expected >= 213 Tip entries, got " + std::to_string(tip_len));
    }

    // Auto-discovered canonical Tip IDs for Suite B dedicated mechanics assertions
    int tid_atk10 = -1;
    int tid_guard = -1;
    int tid_crsshld = -1;
    int tid_antifire = -1;
    int tid_antidmg = -1;
    int tid_antinavi = -1;
    int tid_fire_atk = -1;
    int tid_bari = -1;
    int tid_nrthwind = -1;
    int tid_fstgauge = -1;
    int tid_fullcust = -1;
    int tid_rockcube = -1;
    int tid_wind = -1;
    int tid_fan = -1;
    int tid_satelit = -1;
    int tid_areagrab = -1;
    int tid_mine = -1;
    int tid_break = -1;
    int tid_invis = -1;
    int tid_multihit_stop = -1;
    int tid_multihit_navi = -1;
    int tid_throw_bomb = -1;

    struct PanelSnap {
        int32_t jin, jou, ana, mag, aria, bom;
    };
    struct OkiSnap {
        int32_t on, syu, hp, x, y;
    };

    for (int tid = 0; tid < tip_len; ++tid) {
        reset_test_arena(400, 400);
        Ref t_obj = rt.iaload(sf_app_CpCanvas_tip, tid);
        if (t_obj <= 0) {
            fail_msg("SuiteA", "Null Tip object at id=" + std::to_string(tid));
            continue;
        }
        int32_t t_type = rt.field_i32(t_obj, 1);
        int32_t t_pow = rt.field_i32(t_obj, 3);
        int32_t t_zoku = rt.field_i32(t_obj, 4);
        if (t_type == 4 && t_pow == 10 && tid_atk10 < 0) {
            tid_atk10 = tid;
        }

        std::vector<int16_t> hand;
        if (t_type == 4) {
            hand = {2, static_cast<int16_t>(tid)};
        } else if (t_type == 5) {
            hand = {137, static_cast<int16_t>(tid)};
        } else {
            hand = {static_cast<int16_t>(tid)};
        }

        // --- Run 1: P0 activates `hand`, P1 idle ---
        queue_tips_fifo(0, hand);
        queue_tips_fifo(1, {});
        bool saw_p7_16 = false, saw_p7_18 = false, saw_p7_19 = false, saw_p7_20 = false;
        bool saw_break_ata = false;
        int32_t min_p1_hp_r1 = 400;
        int32_t prev_p1_hp_r1 = 400;
        int stop_hit_events = 0;

        try {
            for (int k = 0; k < 96; ++k) {
                int32_t m0 = (k == 0) ? (1 << 20) : 0;
                step_pvp_combat_tick(rt, cp, m0, 0);

                const SideContext& s0 = g_pvp_eng.core.sides[0];
                const SideContext& s1 = g_pvp_eng.core.sides[1];
                if (sf_app_CpCanvas_stop_cnt > 0 && s1.hp <= prev_p1_hp_r1 - 10) {
                    ++stop_hit_events;
                }
                prev_p1_hp_r1 = s1.hp;
                min_p1_hp_r1 = std::min(min_p1_hp_r1, s1.hp);
                if (t_type < 4 && tid >= 2 && tid <= 208) {
                    if (stop_hit_events >= 2 && tid_multihit_stop < 0) tid_multihit_stop = tid;
                    if (t_type == 1 && stop_hit_events >= 2 && tid_multihit_navi < 0) tid_multihit_navi = tid;
                    if (s0.han == 1 && tid_guard < 0) tid_guard = tid;
                    if (s0.han == 2 && tid_crsshld < 0) tid_crsshld = tid;
                    if (s0.wana == 1 && tid_antifire < 0) tid_antifire = tid;
                    if (s0.wana == 5 && tid_antidmg < 0) tid_antidmg = tid;
                    if (s0.wana == 6 && tid_antinavi < 0) tid_antinavi = tid;
                    if (s0.bari > 0 && s0.bari <= 100 && tid_bari < 0) tid_bari = tid;
                    for (int ai = 0; ai < 30; ++ai) {
                        Ref a = rt.iaload(s0.ata_arr, ai);
                        if (a > 0 && rt.field_i32(a, 38) != 0) {
                            if (sf_app_CpCanvas_stop_cnt == 0 && rt.field_i32(a, 1) == 13 &&
                                rt.field_i32(a, 5) != 0 && tid_throw_bomb < 0) {
                                tid_throw_bomb = tid;
                            }
                            if (rt.field_i32(a, 19) != 0) {
                                saw_break_ata = true;
                            }
                            Ref pa = rt.field_i32(a, 23);
                            if (pa > 0 && rt.array_length(pa) > 7) {
                                int32_t p7 = rt.iaload(pa, 7);
                                if (p7 == 16) saw_p7_16 = true;
                                if (p7 == 18) saw_p7_18 = true;
                                if (p7 == 19) saw_p7_19 = true;
                                if (p7 == 20) saw_p7_20 = true;
                            }
                        }
                    }
                    ensure_board_orient(rt, 0);
                    for (int oi = 0; oi < 2; ++oi) {
                        Ref ok = rt.iaload(sf_app_CpCanvas_oki, oi);
                        if (ok > 0 && rt.field_i32(ok, 1) != 0) {
                            int32_t syu = rt.field_i32(ok, 2);
                            if (syu == 0 && tid_mine < 0) tid_mine = tid;
                            if (syu == 1 && tid_rockcube < 0) tid_rockcube = tid;
                            if (syu == 3 && tid_satelit < 0) tid_satelit = tid;
                            if (syu == 6 && tid_wind < 0) tid_wind = tid;
                            if (syu == 7 && tid_fan < 0) tid_fan = tid;
                        }
                    }
                }
            }
        } catch (const JavaException& ex) {
            fail_msg("SuiteA-Run1", "Tip " + std::to_string(tid) + " (" + tip_name_of(tid) + ") threw JavaException");
            continue;
        }

        if ((t_type == 0 || t_type == 3) && tid >= 2 && tid <= 136) {
            if (saw_p7_16 && tid_invis < 0) tid_invis = tid;
            if (saw_p7_18 && tid_nrthwind < 0) tid_nrthwind = tid;
            if ((saw_p7_19 || g_pvp_eng.core.shared_cus_sp >= 2) && tid_fstgauge < 0) tid_fstgauge = tid;
            if (saw_p7_20 && tid_fullcust < 0) tid_fullcust = tid;
            if (t_zoku == 1 && min_p1_hp_r1 < 400 && tid_fire_atk < 0) tid_fire_atk = tid;
            if (saw_break_ata && min_p1_hp_r1 < 400 &&
                g_pvp_eng.core.sides[0].pos_x == 1 && tid_break < 0) {
                tid_break = tid;
            }
        }

        // Verify orientation-invariance of compute_combat_state_hash(rt)
        ensure_board_orient(rt, 0);
        uint16_t h_orient0 = compute_combat_state_hash(rt);
        ensure_board_orient(rt, 1);
        uint16_t h_orient1 = compute_combat_state_hash(rt);
        ensure_board_orient(rt, 0);
        if (h_orient0 != h_orient1) {
            fail_msg("SuiteA-HashOrient", "Tip " + std::to_string(tid) + " hash orient mismatch");
        }

        const SideContext r1_s0 = g_pvp_eng.core.sides[0];
        const SideContext r1_s1 = g_pvp_eng.core.sides[1];
        const int32_t r1_cus_sp = g_pvp_eng.core.shared_cus_sp;
        PanelSnap r1_pan[3][6]{};
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                r1_pan[y][x] = {
                    rt.field_i32(p, 3), rt.field_i32(p, 4), rt.field_i32(p, 6),
                    rt.field_i32(p, 7), rt.field_i32(p, 8), rt.field_i32(p, 10)
                };
            }
        }
        if ((t_type == 0 || t_type == 3) && tid >= 2 && tid <= 136 && tid_areagrab < 0) {
            if (r1_pan[0][3].jin == 0 && r1_pan[1][3].jin == 0 && r1_pan[2][3].jin == 0 &&
                r1_pan[1][4].jin == 1 && r1_s1.hp == 400) {
                tid_areagrab = tid;
            }
        }
        if ((t_type == 0 || t_type == 3) && tid >= 2 && tid <= 136 && tid_mine < 0) {
            for (int y = 0; y < 3; ++y) {
                for (int x = 3; x < 6; ++x) {
                    if (r1_pan[y][x].bom > 0) tid_mine = tid;
                }
            }
        }
        OkiSnap r1_oki[4]{};
        for (int i = 0; i < 4; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
            r1_oki[i] = {
                rt.field_i32(ok, 1), rt.field_i32(ok, 2), rt.field_i32(ok, 3),
                rt.field_i32(ok, 9), rt.field_i32(ok, 10)
            };
        }

        // --- Run 2: P1 activates `hand`, P0 idle (must mirror Run 1!) ---
        reset_test_arena(400, 400);
        queue_tips_fifo(0, {});
        queue_tips_fifo(1, hand);
        try {
            for (int k = 0; k < 96; ++k) {
                int32_t m1 = (k == 0) ? (1 << 20) : 0;
                step_pvp_combat_tick(rt, cp, 0, m1);
            }
        } catch (const JavaException& ex) {
            fail_msg("SuiteA-Run2", "Tip " + std::to_string(tid) + " (" + tip_name_of(tid) + ") threw JavaException on P1");
            continue;
        }

        ensure_board_orient(rt, 0);
        const SideContext& r2_s0 = g_pvp_eng.core.sides[0];
        const SideContext& r2_s1 = g_pvp_eng.core.sides[1];
        bool sym_ok =
            (r1_s0.hp == r2_s1.hp) && (r1_s1.hp == r2_s0.hp) &&
            (r1_s0.pos_x == r2_s1.pos_x) && (r1_s0.pos_y == r2_s1.pos_y) &&
            (r1_s1.pos_x == r2_s0.pos_x) && (r1_s1.pos_y == r2_s0.pos_y) &&
            (r1_s0.bari == r2_s1.bari) && (r1_s1.bari == r2_s0.bari) &&
            (r1_s0.han == r2_s1.han) && (r1_s1.han == r2_s0.han) &&
            (r1_s0.wana == r2_s1.wana) && (r1_s1.wana == r2_s0.wana) &&
            (r1_s0.joutai == r2_s1.joutai) && (r1_s1.joutai == r2_s0.joutai) &&
            (r1_cus_sp == g_pvp_eng.core.shared_cus_sp);

        for (int y = 0; y < 3 && sym_ok; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, 5 - x);
                const PanelSnap& ps = r1_pan[y][x];
                if (rt.field_i32(p, 3) != (1 - ps.jin) ||
                    rt.field_i32(p, 4) != ps.jou ||
                    std::abs(rt.field_i32(p, 6) - ps.ana) > 1 ||
                    std::abs(rt.field_i32(p, 7) - ps.mag) > 1 ||
                    std::abs(rt.field_i32(p, 8) - ps.aria) > 1 ||
                    rt.field_i32(p, 10) != ps.bom) {
                    sym_ok = false;
                    break;
                }
            }
        }
        for (int i = 0; i < 4 && sym_ok; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i ^ 2);
            const OkiSnap& os = r1_oki[i];
            if (rt.field_i32(ok, 1) != os.on) {
                sym_ok = false;
            } else if (os.on != 0) {
                if (rt.field_i32(ok, 2) != os.syu ||
                    rt.field_i32(ok, 3) != os.hp ||
                    rt.field_i32(ok, 9) != (5 - os.x) ||
                    rt.field_i32(ok, 10) != os.y) {
                    sym_ok = false;
                }
            }
        }

        if (!sym_ok) {
            fail_msg("SuiteA-MirrorSym",
                     "Tip " + std::to_string(tid) + " (" + tip_name_of(tid) + ") P0/P1 asymmetry: "
                     "R1(hp=" + std::to_string(r1_s0.hp) + "/" + std::to_string(r1_s1.hp) +
                     " pos=" + std::to_string(r1_s0.pos_x) + "," + std::to_string(r1_s0.pos_y) + " vs " +
                     std::to_string(r1_s1.pos_x) + "," + std::to_string(r1_s1.pos_y) + ") != "
                     "R2(hp=" + std::to_string(r2_s1.hp) + "/" + std::to_string(r2_s0.hp) +
                     " pos=" + std::to_string(r2_s1.pos_x) + "," + std::to_string(r2_s1.pos_y) + " vs " +
                     std::to_string(r2_s0.pos_x) + "," + std::to_string(r2_s0.pos_y) + ")");
        }

        // --- Run 3: Simultaneous Clash + Read-Only Render Invariance ---
        reset_test_arena(400, 400);
        queue_tips_fifo(0, hand);
        queue_tips_fifo(1, hand);
        try {
            for (int k = 0; k < 36; ++k) {
                int32_t m = (k == 0) ? (1 << 20) : 0;
                step_pvp_combat_tick(rt, cp, m, m);
                if (k == 1 || k == 8 || k == 20) {
                    uint16_t pre_h = compute_combat_state_hash(rt);
                    g_pvp_eng.local_idx = 0;
                    render_pvp_combat_frame(rt, cp);
                    uint16_t post_h0 = compute_combat_state_hash(rt);
                    g_pvp_eng.local_idx = 1;
                    render_pvp_combat_frame(rt, cp);
                    g_pvp_eng.local_idx = 0;
                    uint16_t post_h1 = compute_combat_state_hash(rt);
                    if (pre_h != post_h0 || pre_h != post_h1) {
                        fail_msg("SuiteA-RenderReadonly",
                                 "Tip " + std::to_string(tid) + " (" + tip_name_of(tid) + ") render mutated combat hash");
                        break;
                    }
                }
            }
        } catch (const JavaException& ex) {
            fail_msg("SuiteA-Clash", "Tip " + std::to_string(tid) + " (" + tip_name_of(tid) + ") clash threw JavaException");
        }
    }
    std::printf("[PvP SELF-TEST] Suite A complete: %d Tips swept (failures=%d)\n", tip_len, failures);
    std::fflush(stdout);

    // ------------------------------------------------------------------------
    // Suite B: Dedicated Mechanics & Regression Assertions (14 Core Checks)
    // ------------------------------------------------------------------------
    auto require_tid = [&](int tid, const char* label) -> bool {
        if (tid < 0) {
            fail_msg("SuiteB-Lookup", std::string("Could not auto-discover Tip for ") + label);
            return false;
        }
        return true;
    };

    // B1 & B3: Uncharged Buster (EN Buster MAX off -> 1 dmg, NOT 100!) + Atk+10 isolation from Buster
    if (require_tid(tid_atk10, "Atk+10")) {
        reset_test_arena(500, 500, 0, 0, /*ata_lv=*/1);
        queue_tips_fifo(0, {2, static_cast<int16_t>(tid_atk10)}); // Cannon + Atk+10 queued
        // Tap '#' (1 << 11) on tick 0, release on tick 1 -> fires normal Buster while Cannon+Atk10 stays queued!
        step_pvp_combat_tick(rt, cp, (1 << 11), 0);
        for (int k = 0; k < 20; ++k) step_pvp_combat_tick(rt, cp, 0, 0);
        if (g_pvp_eng.core.sides[1].hp != 499) {
            fail_msg("SuiteB-BusterAndAtkIsolation",
                     "Expected uncharged Buster to deal 1 dmg (P1 hp=499), got " +
                     std::to_string(g_pvp_eng.core.sides[1].hp));
        }
        // Wait out any brief i-frames, then fire queued Cannon + Atk+10 -> must deal 40 + 10 = 50 dmg (499 -> 449)!
        for (int k = 0; k < 45; ++k) step_pvp_combat_tick(rt, cp, 0, 0);
        step_pvp_combat_tick(rt, cp, (1 << 20), 0);
        for (int k = 0; k < 30; ++k) step_pvp_combat_tick(rt, cp, 0, 0);
        if (g_pvp_eng.core.sides[1].hp != 449) {
            fail_msg("SuiteB-CannonPlusAtk10",
                     "Expected Cannon+Atk10 to deal 50 dmg (499->449), got " +
                     std::to_string(g_pvp_eng.core.sides[1].hp));
        }
    }

    // B2: Muramasa (Tip 131) dynamic missing-HP scaling for both P0 and P1
    for (int active_s = 0; active_s < 2; ++active_s) {
        reset_test_arena(active_s == 0 ? 200 : 500, active_s == 1 ? 200 : 500);
        // Place attacker at local pos_x=2, opponent at local pos_x=2 (adjacent across border: world x=2 and x=3)
        for (int s = 0; s < 2; ++s) {
            load_side(rt, s);
            sf_app_Rock_pos_x = 2;
            sf_app_Rock_tmp_x = 2;
            save_side(rt, s);
        }
        queue_tips_fifo(active_s, {131});
        for (int k = 0; k < 30; ++k) {
            int32_t m0 = (active_s == 0 && k == 0) ? (1 << 20) : 0;
            int32_t m1 = (active_s == 1 && k == 0) ? (1 << 20) : 0;
            step_pvp_combat_tick(rt, cp, m0, m1);
        }
        int def_hp = g_pvp_eng.core.sides[1 - active_s].hp;
        if (def_hp != 200) {
            fail_msg("SuiteB-Muramasa",
                     "Side " + std::to_string(active_s) + " at 200/500 HP should deal 300 dmg (500->200), got " +
                     std::to_string(def_hp));
        }
    }

    // B4: FstGauge & FullCust
    if (require_tid(tid_fstgauge, "FstGauge") && require_tid(tid_fullcust, "FullCust")) {
        reset_test_arena(500, 500);
        queue_tips_fifo(1, {static_cast<int16_t>(tid_fstgauge)});
        for (int k = 0; k < 40; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        if (g_pvp_eng.core.shared_cus_sp < 2) {
            fail_msg("SuiteB-FstGauge", "Expected shared_cus_sp >= 2, got " + std::to_string(g_pvp_eng.core.shared_cus_sp));
        }

        reset_test_arena(500, 500);
        sf_app_CpCanvas_cus_gage = 10;
        queue_tips_fifo(0, {static_cast<int16_t>(tid_fullcust)});
        step_pvp_combat_tick(rt, cp, (1 << 20), 0);
        for (int k = 0; k < 35; ++k) step_pvp_combat_tick(rt, cp, 0, 0);
        if (sf_app_CpCanvas_cus_gage < 128) {
            fail_msg("SuiteB-FullCust", "Expected cus_gage == 128, got " + std::to_string(sf_app_CpCanvas_cus_gage));
        }
    }

    // B5: Barrier blocks Cannon, and NrthWind strips opponent's Barrier
    if (require_tid(tid_bari, "Barrier") && require_tid(tid_nrthwind, "NrthWind")) {
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {static_cast<int16_t>(tid_nrthwind)});
        queue_tips_fifo(1, {static_cast<int16_t>(tid_bari)});
        // P1 puts up Barrier
        for (int k = 0; k < 25; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        if (g_pvp_eng.core.sides[1].bari <= 0) {
            fail_msg("SuiteB-BarrierSetup", "Expected P1 bari > 0");
        }
        // P0 uses NrthWind -> must strip P1's Barrier to 0!
        for (int k = 0; k < 40; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        if (g_pvp_eng.core.sides[1].bari != 0) {
            fail_msg("SuiteB-NrthWind", "Expected NrthWind to strip P1 bari to 0, got " +
                     std::to_string(g_pvp_eng.core.sides[1].bari));
        }
    }

    // Helper to measure exact hit tick of a P0 chip and exact han>0 start tick of a P1 shield chip
    auto measure_hit_tick = [&](int16_t att_tid) -> int {
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {att_tid});
        for (int k = 0; k < 60; ++k) {
            step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
            if (g_pvp_eng.core.sides[1].hp < 500) return k;
        }
        return 10;
    };
    auto measure_han_start = [&](int16_t shld_tid) -> int {
        reset_test_arena(500, 500);
        queue_tips_fifo(1, {shld_tid});
        for (int k = 0; k < 40; ++k) {
            step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
            if (g_pvp_eng.core.sides[1].han > 0) return k;
        }
        return 0;
    };

    // B6: Guard reflects 50-dmg ShockWav on non-Breaking hit, and shatters against Breaking attack!
    if (require_tid(tid_guard, "Guard") && require_tid(tid_break, "BreakingChip")) {
        int cannon_hit_k = measure_hit_tick(2);
        int break_hit_k = measure_hit_tick(static_cast<int16_t>(tid_break));
        int guard_start_k = measure_han_start(static_cast<int16_t>(tid_guard));
        int g_press_vs_cannon = std::max(0, cannon_hit_k - guard_start_k - 2);
        int a_press_vs_cannon = std::max(0, guard_start_k + 2 - cannon_hit_k);

        reset_test_arena(500, 500);
        queue_tips_fifo(0, {2, static_cast<int16_t>(tid_break)});
        queue_tips_fifo(1, {static_cast<int16_t>(tid_guard), static_cast<int16_t>(tid_guard)});
        for (int k = 0; k < 55; ++k) {
            int32_t m0 = (k == a_press_vs_cannon) ? (1 << 20) : 0;
            int32_t m1 = (k == g_press_vs_cannon) ? (1 << 20) : 0;
            step_pvp_combat_tick(rt, cp, m0, m1);
        }
        if (g_pvp_eng.core.sides[1].hp != 500 || g_pvp_eng.core.sides[0].hp != 440) {
            fail_msg("SuiteB-GuardReflect",
                     "Expected Guard to block Cannon (P1=500) and reflect 60 dmg ShockWav (P0=440), got P0=" +
                     std::to_string(g_pvp_eng.core.sides[0].hp) + " P1=" + std::to_string(g_pvp_eng.core.sides[1].hp));
        }

        // Now P1 activates second Guard timed 2 ticks before Breaking chip arrives -> shatters Guard and damages P1!
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {static_cast<int16_t>(tid_break)});
        queue_tips_fifo(1, {static_cast<int16_t>(tid_guard)});
        int g_press_vs_break = std::max(0, break_hit_k - guard_start_k - 2);
        int a_press_vs_break = std::max(0, guard_start_k + 2 - break_hit_k);
        bool saw_guard_up = false;
        for (int k = 0; k < 60; ++k) {
            int32_t m0 = (k == a_press_vs_break) ? (1 << 20) : 0;
            int32_t m1 = (k == g_press_vs_break) ? (1 << 20) : 0;
            step_pvp_combat_tick(rt, cp, m0, m1);
            if (g_pvp_eng.core.sides[1].han == 1) saw_guard_up = true;
        }
        if (!saw_guard_up || g_pvp_eng.core.sides[1].hp >= 500 || g_pvp_eng.core.sides[1].han != 0) {
            fail_msg("SuiteB-GuardBreak",
                     "Expected Breaking chip to shatter active Guard and damage P1, got P1 hp=" +
                     std::to_string(g_pvp_eng.core.sides[1].hp) + " han=" + std::to_string(g_pvp_eng.core.sides[1].han));
        }
    }

    // B7: CrsShld blocks attack, consumes shield, and reflects homing bite damaging attacker
    if (require_tid(tid_crsshld, "CrsShld")) {
        int cannon_hit_k = measure_hit_tick(2);
        int crs_start_k = measure_han_start(static_cast<int16_t>(tid_crsshld));
        int c_press = std::max(0, cannon_hit_k - crs_start_k - 2);
        int a_press = std::max(0, crs_start_k + 2 - cannon_hit_k);
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {2});
        queue_tips_fifo(1, {static_cast<int16_t>(tid_crsshld)});
        for (int k = 0; k < 75; ++k) {
            int32_t m0 = (k == a_press) ? (1 << 20) : 0;
            int32_t m1 = (k == c_press) ? (1 << 20) : 0;
            step_pvp_combat_tick(rt, cp, m0, m1);
        }
        if (g_pvp_eng.core.sides[1].hp != 500 || g_pvp_eng.core.sides[0].hp >= 500) {
            fail_msg("SuiteB-CrsShld",
                     "Expected CrsShld to protect P1 (500) and bite P0 (<500), got P0=" +
                     std::to_string(g_pvp_eng.core.sides[0].hp) + " P1=" + std::to_string(g_pvp_eng.core.sides[1].hp));
        }
    }

    // B8: AntiDmg blocks >=10 dmg hit and throws 100-dmg shuriken
    if (require_tid(tid_antidmg, "AntiDmg")) {
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {2});
        queue_tips_fifo(1, {static_cast<int16_t>(tid_antidmg)});
        // Step 65 ticks so AntiDmg's Time-Freeze banner completes (stop_cnt returns to 0) before P0 fires!
        for (int k = 0; k < 65; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        for (int k = 0; k < 55; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        if (g_pvp_eng.core.sides[1].hp != 500 || g_pvp_eng.core.sides[0].hp != 400 || g_pvp_eng.core.sides[1].wana != 0) {
            fail_msg("SuiteB-AntiDmg",
                     "Expected AntiDmg P1=500, P0=400, wana=0; got P0=" +
                     std::to_string(g_pvp_eng.core.sides[0].hp) + " P1=" + std::to_string(g_pvp_eng.core.sides[1].hp));
        }
    }

    // B9: AntiFire triggers on opponent's Fire chip AND on opponent's Fire Charge Shot!
    if (require_tid(tid_antifire, "AntiFire") && require_tid(tid_fire_atk, "FireChip")) {
        // (a) Fire chip trigger
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {static_cast<int16_t>(tid_fire_atk)});
        queue_tips_fifo(1, {static_cast<int16_t>(tid_antifire)});
        for (int k = 0; k < 65; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        for (int k = 0; k < 70; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        if (g_pvp_eng.core.sides[0].hp != 350 || g_pvp_eng.core.sides[1].hp != 500 || g_pvp_eng.core.sides[1].wana != 0) {
            fail_msg("SuiteB-AntiFireChip",
                     "Expected AntiFire to deal 150 dmg to P0 (350) & protect P1 (500), got P0=" +
                     std::to_string(g_pvp_eng.core.sides[0].hp) + " P1=" + std::to_string(g_pvp_eng.core.sides[1].hp));
        }
        // (b) Fire Elemental Charge Shot (zoku0 = 1) trigger!
        reset_test_arena(500, 500, /*zoku0=*/1, /*zoku1=*/0);
        queue_tips_fifo(1, {static_cast<int16_t>(tid_antifire)});
        for (int k = 0; k < 65; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        // Hold '#' for 22 ticks to charge Fire Buster, then release!
        for (int k = 0; k < 22; ++k) step_pvp_combat_tick(rt, cp, (1 << 11), 0);
        for (int k = 0; k < 70; ++k) step_pvp_combat_tick(rt, cp, 0, 0);
        if (g_pvp_eng.core.sides[0].hp != 350 || g_pvp_eng.core.sides[1].wana != 0) {
            fail_msg("SuiteB-AntiFireCharge",
                     "Expected AntiFire to punish Fire Charge Shot for 150 dmg (P0=350), got P0=" +
                     std::to_string(g_pvp_eng.core.sides[0].hp));
        }
    }

    // B10: AntiNavi steals opponent's Navi chip (Roll = Tip 137), healing trap owner & hitting caller!
    if (require_tid(tid_antinavi, "AntiNavi")) {
        reset_test_arena(500, 350);
        queue_tips_fifo(0, {137}); // Roll
        queue_tips_fifo(1, {static_cast<int16_t>(tid_antinavi)});
        for (int k = 0; k < 65; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        for (int k = 0; k < 95; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        if (g_pvp_eng.core.sides[1].hp <= 350 || g_pvp_eng.core.sides[0].hp >= 500 || g_pvp_eng.core.sides[1].wana != 0) {
            fail_msg("SuiteB-AntiNavi",
                     "Expected P1 AntiNavi to steal Roll (P1>350, P0<500), got P0=" +
                     std::to_string(g_pvp_eng.core.sides[0].hp) + " P1=" + std::to_string(g_pvp_eng.core.sides[1].hp));
        }
    }

    // B11: Dual RockCube coexistence + cover blocking + Breaking instant shatter + Mine detonation
    if (require_tid(tid_rockcube, "RockCube") && require_tid(tid_break, "BreakingChip") &&
        require_tid(tid_mine, "Mine")) {
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {static_cast<int16_t>(tid_rockcube)});
        queue_tips_fifo(1, {static_cast<int16_t>(tid_rockcube)});
        // (a) P0 spawns RockCube (wait out Time-Freeze), then P1 spawns RockCube -> both oki[0] and oki[2] coexist!
        for (int k = 0; k < 70; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        for (int k = 0; k < 70; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        ensure_board_orient(rt, 0);
        int32_t on0 = rt.field_i32(rt.iaload(sf_app_CpCanvas_oki, 0), 1);
        int32_t on2 = rt.field_i32(rt.iaload(sf_app_CpCanvas_oki, 2), 1);
        if (on0 == 0 || on2 == 0) {
            fail_msg("SuiteB-DualRockCube",
                     "Expected both players' RockCubes to coexist (on0=" + std::to_string(on0) +
                     " on2=" + std::to_string(on2) + ")");
        }

        // (b) P1 spawns RockCube (world x=3, y=1, 200 HP) in front of P1 (world x=4, y=1):
        //     - P0 at (x=0, y=1) fires Cannon (40 dmg) -> hits RockCube (200 -> 160 HP), keeping P1 at 500 HP!
        //     - P0 at (x=0, y=1) fires CannBall (tid_break, 3 panels ahead -> x=3, y=1) -> instantly shatters P1's RockCube!
        reset_test_arena(500, 500);
        load_side(rt, 0);
        sf_app_Rock_pos_x = 0;
        sf_app_Rock_tmp_x = 0;
        save_side(rt, 0);
        queue_tips_fifo(0, {2, static_cast<int16_t>(tid_break)});
        queue_tips_fifo(1, {static_cast<int16_t>(tid_rockcube)});
        for (int k = 0; k < 70; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        for (int k = 0; k < 35; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        ensure_board_orient(rt, 0);
        Ref p1_cube = rt.iaload(sf_app_CpCanvas_oki, 2);
        if (rt.field_i32(p1_cube, 1) == 0 || rt.field_i32(p1_cube, 3) != 160 || g_pvp_eng.core.sides[1].hp != 500) {
            fail_msg("SuiteB-RockCubeCover",
                     "Expected P1 RockCube to absorb 40-dmg Cannon (hp=160, P1=500), got cube_on=" +
                     std::to_string(rt.field_i32(p1_cube, 1)) + " cube_hp=" +
                     std::to_string(rt.field_i32(p1_cube, 3)) + " P1=" +
                     std::to_string(g_pvp_eng.core.sides[1].hp));
        }
        for (int k = 0; k < 45; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        ensure_board_orient(rt, 0);
        if (rt.field_i32(rt.iaload(sf_app_CpCanvas_oki, 2), 1) != 0) {
            fail_msg("SuiteB-RockCubeBreak", "Expected Breaking chip to instantly destroy P1 RockCube");
        }

        // (c) P1 plants Mine (oki[2], syu==0) -> hidden bomb lands on P0's red 3x3 area and detonates for Mine's chip power (200 dmg)!
        reset_test_arena(500, 500);
        int32_t mine_pow = rt.field_i32(rt.iaload(sf_app_CpCanvas_tip, tid_mine), 3);
        queue_tips_fifo(1, {static_cast<int16_t>(tid_mine)});
        for (int k = 0; k < 96; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        ensure_board_orient(rt, 0);
        Ref p1_mine = rt.iaload(sf_app_CpCanvas_oki, 2);
        int mine_on = rt.field_i32(p1_mine, 1);
        int mine_x = rt.field_i32(p1_mine, 9);
        int mine_y = rt.field_i32(p1_mine, 10);
        if (mine_on == 0 || rt.field_i32(p1_mine, 2) != 0 || mine_x < 0 || mine_x > 2 || mine_y < 0 || mine_y > 2) {
            fail_msg("SuiteB-MinePlant",
                     "Expected P1 Mine in oki[2] on P0's red 3x3 area, got on=" + std::to_string(mine_on) +
                     " x=" + std::to_string(mine_x) + " y=" + std::to_string(mine_y));
        } else {
            for (int k = 0; k < 60 && g_pvp_eng.core.sides[0].hp == 500; ++k) {
                int32_t m0 = 0;
                if ((k % 6) == 0) {
                    int px = g_pvp_eng.core.sides[0].pos_x;
                    int py = g_pvp_eng.core.sides[0].pos_y;
                    if (px < mine_x) m0 = (1 << 18);
                    else if (px > mine_x) m0 = (1 << 16);
                    else if (py < mine_y) m0 = (1 << 19);
                    else if (py > mine_y) m0 = (1 << 17);
                }
                step_pvp_combat_tick(rt, cp, m0, 0);
            }
            ensure_board_orient(rt, 0);
            int32_t post_on = rt.field_i32(rt.iaload(sf_app_CpCanvas_oki, 2), 1);
            if (mine_pow <= 0 || g_pvp_eng.core.sides[0].hp != 500 - mine_pow ||
                g_pvp_eng.core.sides[1].hp != 500 || post_on != 0) {
                fail_msg("SuiteB-MineDetonate",
                         "Expected P1 Mine to deal " + std::to_string(mine_pow) + " dmg to P0 (" +
                         std::to_string(500 - mine_pow) + ") and keep P1=500, got P0=" +
                         std::to_string(g_pvp_eng.core.sides[0].hp) + " P1=" +
                         std::to_string(g_pvp_eng.core.sides[1].hp) + " oki_on=" + std::to_string(post_on));
            }
        }
    }

    // B12: Wind pushes opponent to back column (pos_x=0), Fan pulls opponent to front column (pos_x=2)
    if (require_tid(tid_wind, "Wind") && require_tid(tid_fan, "Fan")) {
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {static_cast<int16_t>(tid_wind)});
        for (int k = 0; k < 45; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        if (g_pvp_eng.core.sides[1].pos_x != 0) {
            fail_msg("SuiteB-WindPush", "Expected Wind to push P1 to pos_x=0, got " +
                     std::to_string(g_pvp_eng.core.sides[1].pos_x));
        }

        reset_test_arena(500, 500);
        queue_tips_fifo(1, {static_cast<int16_t>(tid_fan)});
        for (int k = 0; k < 45; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        if (g_pvp_eng.core.sides[0].pos_x != 2) {
            fail_msg("SuiteB-FanPull", "Expected P1's Fan to pull P0 to pos_x=2, got " +
                     std::to_string(g_pvp_eng.core.sides[0].pos_x));
        }
    }

    // B13: Satelit1 spawned by P1 tracks & hits P0 without hurting P1
    if (require_tid(tid_satelit, "Satelit1")) {
        reset_test_arena(500, 500);
        queue_tips_fifo(1, {static_cast<int16_t>(tid_satelit)});
        for (int k = 0; k < 110; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
        if (g_pvp_eng.core.sides[0].hp >= 500 || g_pvp_eng.core.sides[1].hp != 500) {
            fail_msg("SuiteB-Satelit1",
                     "Expected P1 Satelit1 to damage P0 (<500) and keep P1=500, got P0=" +
                     std::to_string(g_pvp_eng.core.sides[0].hp) + " P1=" + std::to_string(g_pvp_eng.core.sides[1].hp));
        }
    }

    // B14: AreaGrab natural panel-revert symmetry when defender vs attacker stands on stolen column
    if (require_tid(tid_areagrab, "AreaGrab")) {
        int32_t revert_jin_r1 = -1, revert_jin_r2 = -1;
        int32_t revert_p0x_r1 = -1, revert_p1x_r2 = -1;
        // Case 1: P0 uses AreaGrab, moves onto stolen column (local pos_x = 3), waits 520 ticks for natural revert
        reset_test_arena(500, 500);
        queue_tips_fifo(0, {static_cast<int16_t>(tid_areagrab)});
        for (int k = 0; k < 520; ++k) {
            int32_t m0 = (k == 0) ? (1 << 20) : ((k >= 35 && k <= 50) ? (1 << 18) : 0);
            step_pvp_combat_tick(rt, cp, m0, 0);
        }
        ensure_board_orient(rt, 0);
        revert_jin_r1 = rt.field_i32(rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 1), 3), 3);
        revert_p0x_r1 = g_pvp_eng.core.sides[0].pos_x;

        // Case 2: P1 uses AreaGrab, moves onto stolen column (local pos_x = 3 -> world x = 2), waits 520 ticks
        reset_test_arena(500, 500);
        queue_tips_fifo(1, {static_cast<int16_t>(tid_areagrab)});
        for (int k = 0; k < 520; ++k) {
            int32_t m1 = (k == 0) ? (1 << 20) : ((k >= 35 && k <= 50) ? (1 << 18) : 0);
            step_pvp_combat_tick(rt, cp, 0, m1);
        }
        ensure_board_orient(rt, 0);
        revert_jin_r2 = 1 - rt.field_i32(rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 1), 2), 3);
        revert_p1x_r2 = g_pvp_eng.core.sides[1].pos_x;

        if (revert_jin_r1 != revert_jin_r2 || revert_p0x_r1 != revert_p1x_r2) {
            fail_msg("SuiteB-AreaGrabRevertSym",
                     "AreaGrab natural revert asymmetry: R1(jin=" + std::to_string(revert_jin_r1) +
                     " x=" + std::to_string(revert_p0x_r1) + ") vs R2(jin=" + std::to_string(revert_jin_r2) +
                     " x=" + std::to_string(revert_p1x_r2) + ")");
        }
    }

    // B14b: AreaGrab panel-return Red/Blue blink transition effect during aria_cnt <= 45
    if (tid_areagrab > 0) {
        reset_test_arena(500, 500);
        const uint32_t* fb = rt.framebuffer();
        auto panel_box_sig = [&](int col) -> uint64_t {
            uint64_t h = 14695981039346656037ULL;
            for (int py = 110; py <= 123; ++py) {
                for (int px = col * 40 + 6; px <= col * 40 + 33; ++px) {
                    uint32_t pix = fb[py * 240 + px];
                    h ^= static_cast<uint64_t>(pix);
                    h *= 1099511628211ULL;
                }
            }
            return h;
        };

        g_pvp_eng.local_idx = 0;
        render_pvp_combat_frame(rt, cp);
        uint64_t sig_blue_c3 = panel_box_sig(3);
        uint64_t sig_red_c2 = panel_box_sig(2);

        queue_tips_fifo(0, {static_cast<int16_t>(tid_areagrab)});
        bool stolen = false;
        for (int k = 0; k < 120; ++k) {
            step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
            ensure_board_orient(rt, 0);
            Ref p03 = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 0), 3);
            if (sf_app_CpCanvas_stop_cnt == 0 && rt.field_i32(p03, 3) == 0 && rt.field_i32(p03, 8) > 100) {
                stolen = true;
                break;
            }
        }
        if (!stolen) {
            fail_msg("SuiteB-AreaGrabBlinkSetup", "AreaGrab did not steal panel[0][3] with aria_cnt > 100");
        } else {
            g_pvp_eng.local_idx = 0;
            render_pvp_combat_frame(rt, cp);
            uint64_t sig_red_c3 = panel_box_sig(3);
            if (sig_red_c3 == sig_blue_c3) {
                fail_msg("SuiteB-AreaGrabBlinkStealPx", "Expected stolen col=3 Red pixels to differ from Blue pixels");
            }

            bool reached_45 = false;
            for (int k = 0; k < 600; ++k) {
                step_pvp_combat_tick(rt, cp, 0, 0);
                ensure_board_orient(rt, 0);
                Ref p03 = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 0), 3);
                if (rt.field_i32(p03, 8) == 45) {
                    reached_45 = true;
                    if (rt.field_i32(p03, 3) != 1) {
                        fail_msg("SuiteB-AreaGrabBlinkJin45",
                                 "Expected panel[0][3].jin == 1 at aria_cnt=45, got " +
                                 std::to_string(rt.field_i32(p03, 3)));
                    }
                    break;
                }
            }
            if (!reached_45) {
                fail_msg("SuiteB-AreaGrabBlinkReach45", "Did not reach aria_cnt == 45");
            } else {
                bool seen_44 = false, seen_43 = false, seen_0 = false;
                for (int k = 0; k < 60; ++k) {
                    step_pvp_combat_tick(rt, cp, 0, 0);
                    ensure_board_orient(rt, 0);
                    Ref p03 = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 0), 3);
                    int32_t ac = rt.field_i32(p03, 8);
                    if (ac == 44) {
                        seen_44 = true;
                        uint16_t h_before = compute_combat_state_hash(rt);
                        g_pvp_eng.local_idx = 0;
                        render_pvp_combat_frame(rt, cp);
                        uint16_t h_after0 = compute_combat_state_hash(rt);
                        uint64_t px0 = panel_box_sig(3);

                        g_pvp_eng.local_idx = 1;
                        render_pvp_combat_frame(rt, cp);
                        uint16_t h_after1 = compute_combat_state_hash(rt);
                        uint64_t px1 = panel_box_sig(2);

                        if (h_after0 != h_before || h_after1 != h_before) {
                            fail_msg("SuiteB-AreaGrabBlinkReadOnly",
                                     "render_pvp_combat_frame mutated combat state hash during recovery blink");
                        }
                        if (px0 != sig_red_c3) {
                            fail_msg("SuiteB-AreaGrabBlinkOnP0",
                                     "Expected col=3 to blink Red at aria_cnt=44 on P0");
                        }
                        if (px1 == sig_red_c2) {
                            fail_msg("SuiteB-AreaGrabBlinkOnP1",
                                     "Expected mirrored col=2 to blink Blue at aria_cnt=44 on P1");
                        }

                        g_pvp_eng.phase = Phase::ROUND_BANNER;
                        g_pvp_eng.local_idx = 0;
                        render_pvp_combat_frame(rt, cp);
                        if (panel_box_sig(3) != sig_blue_c3) {
                            fail_msg("SuiteB-AreaGrabBlinkSuppressedBanner",
                                     "Expected ROUND_BANNER to suppress recovery blink on col=3");
                        }
                        g_pvp_eng.phase = Phase::COMBAT;
                        g_pvp_eng.local_idx = 0;
                    } else if (ac == 43) {
                        seen_43 = true;
                        g_pvp_eng.local_idx = 0;
                        render_pvp_combat_frame(rt, cp);
                        if (panel_box_sig(3) != sig_blue_c3) {
                            fail_msg("SuiteB-AreaGrabBlinkOffP0",
                                     "Expected col=3 to show Blue at aria_cnt=43 on P0");
                        }
                        g_pvp_eng.local_idx = 1;
                        render_pvp_combat_frame(rt, cp);
                        if (panel_box_sig(2) != sig_red_c2) {
                            fail_msg("SuiteB-AreaGrabBlinkOffP1",
                                     "Expected mirrored col=2 to show Red at aria_cnt=43 on P1");
                        }
                        g_pvp_eng.local_idx = 0;
                    } else if (ac == 0) {
                        seen_0 = true;
                        g_pvp_eng.local_idx = 0;
                        render_pvp_combat_frame(rt, cp);
                        if (panel_box_sig(3) != sig_blue_c3) {
                            fail_msg("SuiteB-AreaGrabBlinkEnd",
                                     "Expected col=3 to settle Blue at aria_cnt=0");
                        }
                        break;
                    }
                }
                if (!seen_44 || !seen_43 || !seen_0) {
                    fail_msg("SuiteB-AreaGrabBlinkWindow",
                             "Missed transition milestones (seen_44=" + std::to_string(seen_44) +
                             " seen_43=" + std::to_string(seen_43) + " seen_0=" + std::to_string(seen_0) + ")");
                }
            }
        }
    }

    // B14c: Opponent Non-Dimming BattleChip Right-Side HUD Display + GGPO Rollback + Region/Orientation Determinism
    {
        auto set_both_row2 = [&]() {
            for (int s = 0; s < 2; ++s) {
                load_side(rt, s);
                sf_app_Rock_pos_y = 2;
                sf_app_Rock_tmp_y = 2;
                save_side(rt, s);
            }
        };
        auto scan_hud_pixels = [&](int x0, int x1, int y0, int y1,
                                   int& white_cnt, int& yellow_cnt,
                                   int& min_white_x, int& min_yellow_x) {
            white_cnt = 0;
            yellow_cnt = 0;
            min_white_x = 9999;
            min_yellow_x = 9999;
            const uint32_t* fb = rt.framebuffer();
            for (int py = y0; py <= y1; ++py) {
                for (int px = x0; px < x1; ++px) {
                    uint32_t pix = fb[py * 240 + px];
                    if (pix == 0xFFFFFFFFu) {
                        ++white_cnt;
                        min_white_x = std::min(min_white_x, px);
                    } else if (pix == 0xFFFFFF00u) {
                        ++yellow_cnt;
                        min_yellow_x = std::min(min_yellow_x, px);
                    }
                }
            }
        };

        // 1. Buster / Charge Shot exclusion
        reset_test_arena(500, 500);
        set_both_row2();
        step_pvp_combat_tick(rt, cp, 0, (1 << 11)); // Buster tap
        for (int k = 0; k < 5; ++k) step_pvp_combat_tick(rt, cp, 0, 0);
        if (g_pvp_eng.core.sides[1].nondim_ticks != 0 || g_pvp_eng.core.sides[1].nondim_tip_id != -1) {
            fail_msg("SuiteB-NonDimBusterTap", "Buster tap must not trigger opponent non-dimming HUD");
        }
        for (int k = 0; k < 22; ++k) step_pvp_combat_tick(rt, cp, 0, (1 << 11)); // Charge
        step_pvp_combat_tick(rt, cp, 0, 0); // Release Charge Shot
        if (g_pvp_eng.core.sides[1].nondim_ticks != 0 || g_pvp_eng.core.sides[1].nondim_tip_id != -1) {
            fail_msg("SuiteB-NonDimChargeShot", "Charge Shot must not trigger opponent non-dimming HUD");
        }
        g_pvp_eng.local_idx = 0;
        render_pvp_combat_frame(rt, cp);
        {
            int wc = 0, yc = 0, mwx = 0, myx = 0;
            scan_hud_pixels(120, 240, 66, 80, wc, yc, mwx, myx);
            if (wc != 0 || yc != 0) {
                fail_msg("SuiteB-NonDimBusterPixels",
                         "Expected 0 white/yellow HUD pixels after Buster/Charge, got w=" +
                         std::to_string(wc) + " y=" + std::to_string(yc));
            }
        }

        // 2. Damaging non-dimming chip + Atk+10, P0 vs P1 visibility, read-only render, orientation-invariant hash, and GGPO rollback
        reset_test_arena(500, 500);
        set_both_row2();
        if (tid_atk10 > 0) {
            queue_tips_fifo(1, {2, static_cast<int16_t>(tid_atk10)});
        } else {
            queue_tips_fifo(1, {2});
        }
        step_pvp_combat_tick(rt, cp, 0, (1 << 20));
        {
            const SideContext& s1 = g_pvp_eng.core.sides[1];
            int16_t exp_plus = (tid_atk10 > 0) ? 10 : 0;
            if (s1.nondim_tip_id != 2 || s1.nondim_base_pow != 40 ||
                s1.nondim_plus_pow != exp_plus || s1.nondim_ticks != kOppNonDimChipHudTicks) {
                fail_msg("SuiteB-NonDimCannonState",
                         "Unexpected Cannon state: id=" + std::to_string(s1.nondim_tip_id) +
                         " base=" + std::to_string(s1.nondim_base_pow) +
                         " plus=" + std::to_string(s1.nondim_plus_pow) +
                         " ticks=" + std::to_string(s1.nondim_ticks));
            }

            ensure_board_orient(rt, 0);
            uint16_t h0 = compute_combat_state_hash(rt);
            ensure_board_orient(rt, 1);
            uint16_t h1 = compute_combat_state_hash(rt);
            ensure_board_orient(rt, 0);
            if (h0 != h1) {
                fail_msg("SuiteB-NonDimOrientHash", "Hash mismatch across board_orient 0 vs 1");
            }

            g_pvp_eng.local_idx = 0;
            render_pvp_combat_frame(rt, cp);
            uint16_t h_after_render = compute_combat_state_hash(rt);
            if (h_after_render != h0) {
                fail_msg("SuiteB-NonDimRenderReadOnly", "render_pvp_combat_frame mutated combat state hash");
            }
            int wr = 0, yr = 0, mwxr = 0, myxr = 0;
            int wl = 0, yl = 0, mwxl = 0, myxl = 0;
            scan_hud_pixels(120, 240, 66, 80, wr, yr, mwxr, myxr);
            scan_hud_pixels(0, 120, 66, 80, wl, yl, mwxl, myxl);
            if (wr <= 0 || yr <= 0 || mwxr >= myxr || wl != 0 || yl != 0) {
                fail_msg("SuiteB-NonDimP0Render",
                         "Unexpected P0 HUD pixels: right(w=" + std::to_string(wr) +
                         ",y=" + std::to_string(yr) + ",mwx=" + std::to_string(mwxr) +
                         ",myx=" + std::to_string(myxr) + ") left(w=" + std::to_string(wl) +
                         ",y=" + std::to_string(yl) + ")");
            }

            g_pvp_eng.local_idx = 1;
            render_pvp_combat_frame(rt, cp);
            int wall = 0, yall = 0, mwxall = 0, myxall = 0;
            scan_hud_pixels(0, 240, 66, 80, wall, yall, mwxall, myxall);
            if (wall != 0 || yall != 0) {
                fail_msg("SuiteB-NonDimP1Render",
                         "P1 (chip user) should see 0 opp HUD pixels, got w=" +
                         std::to_string(wall) + " y=" + std::to_string(yall));
            }
            g_pvp_eng.local_idx = 0;

            uint32_t rb_turn = g_pvp_eng.core.combat_turn;
            save_rollback_slot(rt, rb_turn, 0, 0, false);
            for (int k = 1; k <= 15; ++k) {
                step_pvp_combat_tick(rt, cp, 0, 0);
                if (g_pvp_eng.core.sides[1].nondim_ticks != static_cast<uint8_t>(16 - k)) {
                    fail_msg("SuiteB-NonDimCountdown",
                             "Expected nondim_ticks=" + std::to_string(16 - k) +
                             " at step " + std::to_string(k) + ", got " +
                             std::to_string(g_pvp_eng.core.sides[1].nondim_ticks));
                    break;
                }
            }
            step_pvp_combat_tick(rt, cp, 0, 0);
            if (g_pvp_eng.core.sides[1].nondim_ticks != 0 || g_pvp_eng.core.sides[1].nondim_tip_id != -1) {
                fail_msg("SuiteB-NonDimExpire",
                         "Expected clear on tick 16, got ticks=" +
                         std::to_string(g_pvp_eng.core.sides[1].nondim_ticks) +
                         " id=" + std::to_string(g_pvp_eng.core.sides[1].nondim_tip_id));
            }
            if (!restore_rollback_slot(rt, rb_turn)) {
                fail_msg("SuiteB-NonDimRollback", "restore_rollback_slot failed");
            } else if (g_pvp_eng.core.sides[1].nondim_ticks != 16 ||
                       g_pvp_eng.core.sides[1].nondim_tip_id != 2 ||
                       compute_combat_state_hash(rt) != h0) {
                fail_msg("SuiteB-NonDimRollbackState",
                         "Rollback did not restore exact nondim_ticks==16 & hash");
            }
        }

        // 3. Buffered mid-movement chip (tmp_ata)
        reset_test_arena(500, 500);
        queue_tips_fifo(1, {2});
        step_pvp_combat_tick(rt, cp, 0, (1 << 17)); // UP
        step_pvp_combat_tick(rt, cp, 0, (1 << 20)); // SELECT while moving
        if (g_pvp_eng.core.sides[1].tmp_ata < 0 || g_pvp_eng.core.sides[1].nondim_ticks != 0) {
            fail_msg("SuiteB-NonDimBufferedPending",
                     "Expected buffered tmp_ata>=0 and nondim_ticks==0 during movement, got tmp_ata=" +
                     std::to_string(g_pvp_eng.core.sides[1].tmp_ata) +
                     " ticks=" + std::to_string(g_pvp_eng.core.sides[1].nondim_ticks));
        } else {
            bool fired_after_move = false;
            for (int k = 0; k < 20; ++k) {
                step_pvp_combat_tick(rt, cp, 0, 0);
                if (g_pvp_eng.core.sides[1].tmp_ata >= 0) {
                    if (g_pvp_eng.core.sides[1].nondim_ticks != 0) {
                        fail_msg("SuiteB-NonDimBufferedEarly", "nondim_ticks became >0 before tmp_ata cleared");
                        break;
                    }
                } else {
                    fired_after_move = (g_pvp_eng.core.sides[1].nondim_ticks == kOppNonDimChipHudTicks &&
                                        g_pvp_eng.core.sides[1].nondim_tip_id == 2);
                    break;
                }
            }
            if (!fired_after_move) {
                fail_msg("SuiteB-NonDimBufferedFire",
                         "Expected buffered chip to set nondim_ticks==16 when movement completed");
            }
        }

        // 4. Non-dimming support chip (t_type >= 3 && waza.field_11 == 0)
        reset_test_arena(500, 500);
        set_both_row2();
        int tid_nondim_support = -1;
        if (sf_app_CpCanvas_tip > 0 && sf_app_CpCanvas_waza > 0) {
            int32_t tlen = rt.array_length(sf_app_CpCanvas_tip);
            int32_t wlen = rt.array_length(sf_app_CpCanvas_waza);
            for (int tid = 2; tid < tlen; ++tid) {
                Ref to = rt.iaload(sf_app_CpCanvas_tip, tid);
                if (to <= 0) continue;
                int32_t t_kind = rt.field_i32(to, 1);
                int32_t w_idx = rt.field_i32(to, 8);
                if (t_kind == 3 && w_idx >= 0 && w_idx < wlen) {
                    Ref wo = rt.iaload(sf_app_CpCanvas_waza, w_idx);
                    if (wo > 0 && rt.field_i32(wo, 11) == 0 && rt.field_i32(to, 9) > 0) {
                        tid_nondim_support = tid;
                        break;
                    }
                }
            }
        }
        if (tid_nondim_support < 0) {
            fail_msg("SuiteB-NonDimSupportLookup", "Could not locate non-dimming support chip (t_kind==3, waza.field_11==0)");
        } else {
            queue_tips_fifo(1, {static_cast<int16_t>(tid_nondim_support)});
            step_pvp_combat_tick(rt, cp, 0, (1 << 20));
            const SideContext& s1 = g_pvp_eng.core.sides[1];
            if (s1.nondim_tip_id != tid_nondim_support || s1.nondim_base_pow != -1 ||
                s1.nondim_ticks != kOppNonDimChipHudTicks) {
                fail_msg("SuiteB-NonDimSupportState",
                         "Unexpected support state: id=" + std::to_string(s1.nondim_tip_id) +
                         " base=" + std::to_string(s1.nondim_base_pow) +
                         " ticks=" + std::to_string(s1.nondim_ticks));
            }
            g_pvp_eng.local_idx = 0;
            render_pvp_combat_frame(rt, cp);
            int ws = 0, ys = 0, mwxs = 0, myxs = 0;
            scan_hud_pixels(120, 240, 66, 80, ws, ys, mwxs, myxs);
            if (ws <= 0 || ys != 0) {
                fail_msg("SuiteB-NonDimSupportPixels",
                         "Expected >0 white and 0 yellow HUD pixels for support chip, got w=" +
                         std::to_string(ws) + " y=" + std::to_string(ys));
            }
        }

        // 5. Time-Freeze immediate clear & dimming chip exclusion
        reset_test_arena(500, 500);
        int16_t dim_tid = static_cast<int16_t>(tid_areagrab > 0 ? tid_areagrab : 137);
        queue_tips_fifo(1, {2});
        queue_tips_fifo(0, {dim_tid});
        step_pvp_combat_tick(rt, cp, 0, (1 << 20));
        if (g_pvp_eng.core.sides[1].nondim_ticks != kOppNonDimChipHudTicks) {
            fail_msg("SuiteB-NonDimTfSetup", "Expected P1 nondim_ticks==16 before Time-Freeze");
        } else {
            step_pvp_combat_tick(rt, cp, (1 << 20), 0);
            if (sf_app_CpCanvas_stop_cnt == 0 ||
                g_pvp_eng.core.sides[0].nondim_ticks != 0 || g_pvp_eng.core.sides[0].nondim_tip_id != -1 ||
                g_pvp_eng.core.sides[1].nondim_ticks != 0 || g_pvp_eng.core.sides[1].nondim_tip_id != -1) {
                fail_msg("SuiteB-NonDimTfImmediateClear",
                         "Expected Time-Freeze to immediately clear nondim HUD on both sides");
            }
            for (int k = 0; k < 120 && sf_app_CpCanvas_stop_cnt > 0; ++k) {
                step_pvp_combat_tick(rt, cp, 0, 0);
            }
            if (sf_app_CpCanvas_stop_cnt != 0 ||
                g_pvp_eng.core.sides[0].nondim_ticks != 0 || g_pvp_eng.core.sides[1].nondim_ticks != 0) {
                fail_msg("SuiteB-NonDimTfPostClear", "Expected nondim_ticks==0 after Time-Freeze finishes");
            }
        }
        // Standalone dimming chip used by P1 must never populate nondim_ticks
        reset_test_arena(500, 500);
        queue_tips_fifo(1, {dim_tid});
        step_pvp_combat_tick(rt, cp, 0, (1 << 20));
        if (g_pvp_eng.core.sides[1].nondim_ticks != 0 || g_pvp_eng.core.sides[1].nondim_tip_id != -1) {
            fail_msg("SuiteB-NonDimDimExclusion", "Dimming chip must not populate nondim HUD");
        }
    }

    // B15: Mid-battle Round 2+ Custom Screen isolation (asymmetric BattleTip frames + local RNG must not desync combat)
    {
        auto run_client_custom_round = [&](int client_idx, int ui_frames) -> uint16_t {
            reset_test_arena(500, 500);
            g_pvp_eng.local_idx = client_idx;
            queue_tips_fifo(0, {static_cast<int16_t>(tid_rockcube > 0 ? tid_rockcube : 2), 2});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_satelit > 0 ? tid_satelit : 2), 2});
            for (int k = 0; k < 75; ++k) {
                int32_t m0 = (k == 0 || k == 70) ? (1 << 20) : (1 << 11);
                int32_t m1 = (k == 68) ? (1 << 20) : (1 << 11);
                step_pvp_combat_tick(rt, cp, m0, m1);
            }
            enter_custom_screen_phase(rt, cp);
            for (int f = 0; f < ui_frames; ++f) {
                sf_app_CpCanvas_key = (f == 1) ? (1 << 20) : 0;
                rt.set_keypad_override(true, sf_app_CpCanvas_key);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
                rt.set_keypad_override(false, 0);
            }
            pvp::PvpCustomCommit c0{}, c1{};
            c0.turn_no = g_pvp_eng.core.custom_round;
            c0.tip_no = 0;
            c0.max_sel = 5;
            c0.pa_no = -1;
            for (int i = 0; i < 9; ++i) c0.rock_tip_ids[i] = -1;
            c0.rock_tip_ids[0] = 2;
            c1 = c0;

            g_pvp_eng.core = g_pvp_eng.pre_custom_core;
            generated_restore_statics(g_pvp_eng.pre_custom_statics);
            rt.restore_combat_snapshot(g_pvp_eng.pre_custom_snap);
            g_pvp_eng.local_idx = client_idx;
            if (sf_app_CpCanvas_rand > 0) {
                uint64_t combat_seed = (static_cast<uint64_t>(g_pvp_eng.match_seed) +
                                        static_cast<uint64_t>(g_pvp_eng.core.combat_turn) * 0x9E3779B97F4A7C15ULL) &
                                       ((1ULL << 48) - 1ULL);
                rt.get(sf_app_CpCanvas_rand).rng_seed = combat_seed;
            }
            ensure_board_orient(rt, 0);
            apply_custom_commit_to_side(rt, 0, c0);
            apply_custom_commit_to_side(rt, 1, c1);
            for (int k = 0; k < 20; ++k) {
                step_pvp_combat_tick(rt, cp, (k == 2) ? (1 << 20) : 0, (k == 4) ? (1 << 20) : 0);
            }
            return compute_combat_state_hash(rt);
        };
        uint16_t h_c0 = run_client_custom_round(0, 28);
        uint16_t h_c1 = run_client_custom_round(1, 5);
        g_pvp_eng.local_idx = 0;
        if (h_c0 != h_c1) {
            fail_msg("SuiteB-CustomScreenIsolation",
                     "Client 0 vs Client 1 diverged after mid-battle Custom Screen (h0=" +
                     std::to_string(h_c0) + " h1=" + std::to_string(h_c1) + ")");
        }
    }

    // B16: Native virus-battle Custom Screen leftover chip retention vs overwrite
    //      - OK with 0 chips selected (and 30s timeout with 0 chips selected) preserves previous round's chips + Atk+10
    //      - Selecting >= 1 chip + OK overwrites previous round's leftover chips
    if (tid_atk10 > 0) {
        reset_test_arena(500, 500, 0, 0, 1);
        g_pvp_eng.local_idx = 0;
        // Round 1 hand: [Cannon, Cannon, Atk+10] -> fire first Cannon so [Cannon + Atk+10] remains unused!
        queue_tips_fifo(0, {2, 2, static_cast<int16_t>(tid_atk10)});
        for (int k = 0; k < 35; ++k) {
            step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
        }
        if (g_pvp_eng.core.sides[1].hp != 460 ||
            g_pvp_eng.core.sides[0].tip_no != 1 ||
            g_pvp_eng.core.sides[0].next_tip != 2) {
            fail_msg("SuiteB-LeftoverSetup", "Expected [Cannon + Atk+10] remaining after firing first Cannon");
        } else {
            auto finish_local_custom_commit_round = [&](const pvp::PvpCustomCommit& c0) {
                pvp::PvpCustomCommit c1{};
                c1.turn_no = g_pvp_eng.core.custom_round;
                c1.tip_no = -1;
                c1.max_sel = 5;
                c1.pa_no = -1;
                for (int i = 0; i < 9; ++i) c1.rock_tip_ids[i] = -1;

                g_pvp_eng.core = g_pvp_eng.pre_custom_core;
                generated_restore_statics(g_pvp_eng.pre_custom_statics);
                rt.restore_combat_snapshot(g_pvp_eng.pre_custom_snap);
                g_pvp_eng.local_idx = 0;
                ensure_board_orient(rt, 0);
                apply_custom_commit_to_side(rt, 0, c0);
                apply_custom_commit_to_side(rt, 1, c1);
                g_pvp_eng.phase = Phase::COMBAT;
            };

            // (1) Enter Custom Screen, highlight OK (sel_cas_mode=1) with 0 chips selected, press OK!
            enter_custom_screen_phase(rt, cp);
            for (int f = 0; f < 30 && sf_app_CpCanvas_scene != 1; ++f) {
                if (f == 4) {
                    sf_app_CpCanvas_sel_cas_mode = 1; // Cursor on OK with sel_cnt == 0
                    sf_app_CpCanvas_key = (1 << 20);  // Press OK
                } else {
                    sf_app_CpCanvas_key = 0;
                }
                rt.set_keypad_override(true, sf_app_CpCanvas_key);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
                rt.set_keypad_override(false, 0);
            }
            pvp::PvpCustomCommit keep_ok_commit = capture_local_custom_commit(rt);
            finish_local_custom_commit_round(keep_ok_commit);
            int32_t pow0_after_ok = rt.iaload(g_pvp_eng.core.sides[0].rock_pow_ref, 0);
            if (sf_app_CpCanvas_scene == 2 ||
                g_pvp_eng.core.sides[0].tip_no != 1 ||
                g_pvp_eng.core.sides[0].next_tip != 2 ||
                pow0_after_ok != 10) {
                fail_msg("SuiteB-KeepChipsOnEmptyOK",
                         "Expected Cannon+Atk10 preserved on 0-chip OK (tip_no=" +
                         std::to_string(g_pvp_eng.core.sides[0].tip_no) + ", next_tip=" +
                         std::to_string(g_pvp_eng.core.sides[0].next_tip) + ", pow0=" +
                         std::to_string(pow0_after_ok) + ")");
            }

            // (2) Enter Custom Screen again and simulate 30s timeout with valid_sel == 0 -> still preserved!
            enter_custom_screen_phase(rt, cp);
            sf_app_CpCanvas_sel_cnt = 0;
            sf_app_CpCanvas_cas_cnt = 0;
            sf_app_CpCanvas_pa_cnt2 = 0;
            sf_app_CpCanvas_scene = 1; // Exact post-timeout state when valid_sel == 0
            pvp::PvpCustomCommit keep_timeout_commit = capture_local_custom_commit(rt);
            finish_local_custom_commit_round(keep_timeout_commit);
            int32_t pow0_after_timeout = rt.iaload(g_pvp_eng.core.sides[0].rock_pow_ref, 0);
            if (g_pvp_eng.core.sides[0].tip_no != 1 ||
                g_pvp_eng.core.sides[0].next_tip != 2 ||
                pow0_after_timeout != 10) {
                fail_msg("SuiteB-KeepChipsOnEmptyTimeout",
                         "Expected Cannon+Atk10 preserved on 0-chip timeout (tip_no=" +
                         std::to_string(g_pvp_eng.core.sides[0].tip_no) + ", pow0=" +
                         std::to_string(pow0_after_timeout) + ")");
            }

            // (3) Enter Custom Screen again, select slot 0 chip (sel_cnt == 1) and press OK -> overwrites previous chips!
            enter_custom_screen_phase(rt, cp);
            int32_t slot0_fl = rt.iaload(sf_app_CpCanvas_cas_tip, 0);
            int32_t expected_new_tid = (slot0_fl >= 0) ? ((rt.iaload(sf_app_CpCanvas_tip_list, slot0_fl) >> 8) & 0xFF) : -1;
            for (int f = 0; f < 35 && sf_app_CpCanvas_scene != 1; ++f) {
                if (f == 4) {
                    sf_app_CpCanvas_sel_cas_mode = 0;
                    sf_app_CpCanvas_sel_cas_tip = 0;
                    sf_app_CpCanvas_key = (1 << 20); // Select slot 0 chip
                } else if (f == 6) {
                    sf_app_CpCanvas_sel_cas_mode = 1;
                    sf_app_CpCanvas_key = (1 << 20); // Confirm OK
                } else {
                    sf_app_CpCanvas_key = 0;
                }
                rt.set_keypad_override(true, sf_app_CpCanvas_key);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
                rt.set_keypad_override(false, 0);
            }
            pvp::PvpCustomCommit overwrite_commit = capture_local_custom_commit(rt);
            finish_local_custom_commit_round(overwrite_commit);
            Ref p0_rock_tips = rt.field_i32(g_pvp_eng.core.sides[0].rock_ref, 0);
            int32_t new_queued_tid = rt.field_i32(rt.iaload(p0_rock_tips, 0), 0);
            int32_t pow0_after_overwrite = rt.iaload(g_pvp_eng.core.sides[0].rock_pow_ref, 0);
            if (g_pvp_eng.core.sides[0].tip_no != 0 ||
                new_queued_tid != expected_new_tid ||
                pow0_after_overwrite != 0) {
                fail_msg("SuiteB-OverwriteChipsOnSelectOK",
                         "Expected 1-chip OK to overwrite leftover hand (tip_no=" +
                         std::to_string(g_pvp_eng.core.sides[0].tip_no) + ", tid=" +
                         std::to_string(new_queued_tid) + " vs " + std::to_string(expected_new_tid) +
                         ", pow0=" + std::to_string(pow0_after_overwrite) + ")");
            }
        }
    }

    std::printf("[PvP SELF-TEST] Suite B complete: 16 dedicated mechanics checks finished (failures=%d)\n", failures);
    std::fflush(stdout);

    // ------------------------------------------------------------------------
    // Suite C: GGPO Rollback Determinism Torture Test (300 Frames + 1..20f Rollbacks)
    // ------------------------------------------------------------------------
    reset_test_arena(900, 900, 1, 2, 2);
    std::vector<int32_t> hist_m0(320, 0), hist_m1(320, 0);
    std::vector<uint16_t> post_hashes(320, 0);
    const int16_t chaos_hands[5][4] = {
        {static_cast<int16_t>(tid_rockcube > 0 ? tid_rockcube : 2),
         static_cast<int16_t>(tid_mine > 0 ? tid_mine : 2),
         static_cast<int16_t>(tid_guard > 0 ? tid_guard : 2), 137},
        {static_cast<int16_t>(tid_areagrab > 0 ? tid_areagrab : 2),
         static_cast<int16_t>(tid_antidmg > 0 ? tid_antidmg : 2),
         static_cast<int16_t>(tid_wind > 0 ? tid_wind : 2), 131},
        {static_cast<int16_t>(tid_bari > 0 ? tid_bari : 2),
         static_cast<int16_t>(tid_nrthwind > 0 ? tid_nrthwind : 2),
         static_cast<int16_t>(tid_crsshld > 0 ? tid_crsshld : 2), 2},
        {static_cast<int16_t>(tid_satelit > 0 ? tid_satelit : 2),
         static_cast<int16_t>(tid_fan > 0 ? tid_fan : 2),
         static_cast<int16_t>(tid_break > 0 ? tid_break : 2), 2},
        {static_cast<int16_t>(tid_antifire > 0 ? tid_antifire : 2),
         static_cast<int16_t>(tid_fire_atk > 0 ? tid_fire_atk : 2),
         static_cast<int16_t>(tid_antinavi > 0 ? tid_antinavi : 2), 137},
    };

    auto gen_chaos_input = [](uint32_t turn, int side) -> int32_t {
        uint32_t x = (turn * 1103515245u + static_cast<uint32_t>(side) * 12345u + 0x9E3779B9u);
        int32_t m = 0;
        uint32_t dir = (x >> 16) & 7u;
        if (dir < 4u && (turn % 5u) == 0u) m |= (1 << (16 + dir));
        if (((x >> 20) & 7u) == 0u) m |= (1 << 20);
        if (((x >> 24) & 3u) != 0u) m |= (1 << 11);
        return m;
    };

    queue_tips_fifo(0, {chaos_hands[0][0], chaos_hands[0][1], chaos_hands[0][2], chaos_hands[0][3]});
    queue_tips_fifo(1, {chaos_hands[1][0], chaos_hands[1][1], chaos_hands[1][2], chaos_hands[1][3]});

    int rollback_checks = 0;
    for (uint32_t t = 0; t < 300; ++t) {
        if (t > 0 && (t % 60u) == 0u) {
            // Refill hands at round boundaries and reset rollback ring for the new sub-round
            int h0 = static_cast<int>((t / 60u) % 5u);
            int h1 = static_cast<int>((t / 60u + 2u) % 5u);
            queue_tips_fifo(0, {chaos_hands[h0][0], chaos_hands[h0][1], chaos_hands[h0][2], chaos_hands[h0][3]});
            queue_tips_fifo(1, {chaos_hands[h1][0], chaos_hands[h1][1], chaos_hands[h1][2], chaos_hands[h1][3]});
            for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
        }

        int32_t m0 = gen_chaos_input(t, 0);
        int32_t m1 = gen_chaos_input(t, 1);
        hist_m0[t] = m0;
        hist_m1[t] = m1;

        save_rollback_slot(rt, t, m0, m1, false);
        step_pvp_combat_tick(rt, cp, m0, m1);
        if ((t & 3u) == 1u) {
            g_pvp_eng.local_idx = static_cast<int>(t & 1u);
            render_pvp_combat_frame(rt, cp);
            g_pvp_eng.local_idx = 0;
        }
        post_hashes[t] = compute_combat_state_hash(rt);

        uint32_t seg_start = (t / 60u) * 60u;
        if ((t % 6u) == 5u && t > seg_start + 2u) {
            uint32_t depth = std::min<uint32_t>(20u, t - seg_start);
            uint32_t rb_t = t + 1u - depth;
            SideContext expected_s0 = g_pvp_eng.core.sides[0];
            SideContext expected_s1 = g_pvp_eng.core.sides[1];
            uint16_t expected_h = post_hashes[t];

            if (!restore_rollback_slot(rt, rb_t)) {
                fail_msg("SuiteC-RollbackRestore", "Failed to restore slot at turn " + std::to_string(rb_t));
                break;
            }
            for (uint32_t u = rb_t; u <= t; ++u) {
                save_rollback_slot(rt, u, hist_m0[u], hist_m1[u], false);
                step_pvp_combat_tick(rt, cp, hist_m0[u], hist_m1[u]);
                uint16_t resim_h = compute_combat_state_hash(rt);
                if (resim_h != post_hashes[u]) {
                    fail_msg("SuiteC-RollbackHash",
                             "Resimulation diverged at turn " + std::to_string(u) +
                             " after rollback to " + std::to_string(rb_t));
                    break;
                }
            }
            ++rollback_checks;
            if (compute_combat_state_hash(rt) != expected_h ||
                g_pvp_eng.core.sides[0].hp != expected_s0.hp ||
                g_pvp_eng.core.sides[1].hp != expected_s1.hp ||
                g_pvp_eng.core.sides[0].pos_x != expected_s0.pos_x ||
                g_pvp_eng.core.sides[1].pos_x != expected_s1.pos_x) {
                fail_msg("SuiteC-RollbackState", "State mismatch after rollback from turn " + std::to_string(t));
                break;
            }
        }
    }
    std::printf("[PvP SELF-TEST] Suite C complete: %d multi-frame GGPO rollbacks verified (failures=%d)\n",
                rollback_checks, failures);

    // ========================================================================
    // Suite D: Buster Charge (蓄力集气) Host-Timer/Audio Isolation, BGM/SE Fidelity & Trill Time-Sync
    // ========================================================================
    {
        sf_app_CpCanvas_audio_flg = 0; // Unmuted BGM & SE path
        reset_test_arena(500, 500, 0, 1, 5);
        g_pvp_eng.match_seed = 0xA5A55A5Au;
        g_pvp_eng.core.sides[0].cha_lv = 5; // cha_thresh = 42 - 25 = 17 ticks
        g_pvp_eng.core.sides[1].cha_lv = 5;
        for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
        for (auto& em : g_pvp_eng.turn_emitted_se) {
            em.turn = UINT32_MAX;
            em.count = 0;
        }

        // 1. Verify all 30 m_audio presenters (11 BGM + 19 jingle/UI cues) are distinct & non-aliasing
        if (sf_app_CpCanvas_m_audio <= 0 || rt.array_length(sf_app_CpCanvas_m_audio) < 30) {
            fail_msg("SuiteD-MAudioArray", "CpCanvas.m_audio array missing or < 30 entries");
        } else {
            bool distinct_ok = true;
            for (int32_t i = 0; i < 30 && distinct_ok; ++i) {
                Ref pi = rt.iaload(sf_app_CpCanvas_m_audio, i);
                Ref si = rt.audio_presenter_sound_ref(pi);
                if (pi <= 0 || si <= 0) {
                    fail_msg("SuiteD-MAudioBind", "Unbound m_audio presenter at index " + std::to_string(i));
                    distinct_ok = false;
                    break;
                }
                for (int32_t j = 0; j < i; ++j) {
                    Ref pj = rt.iaload(sf_app_CpCanvas_m_audio, j);
                    if (pi == pj) {
                        fail_msg("SuiteD-MAudioAlias",
                                 "m_audio[" + std::to_string(i) + "] aliases m_audio[" + std::to_string(j) + "]");
                        distinct_ok = false;
                        break;
                    }
                    if (i < 11 && si == rt.audio_presenter_sound_ref(pj)) {
                        fail_msg("SuiteD-BgmSoundAlias",
                                 "BGM m_audio[" + std::to_string(i) + "] shares MediaSound with m_audio[" +
                                 std::to_string(j) + "]");
                        distinct_ok = false;
                        break;
                    }
                }
            }
        }

        // 2. Verify PvP arena plays Navi Battle BGM (id=2) and NEVER aliases Title (0) or Ending (10)
        if (sf_app_CpCanvas_now_bgm != 2 || sf_app_CpCanvas_boss_flg != 1) {
            fail_msg("SuiteD-PvpNaviBgm",
                     "Expected now_bgm=2 & boss_flg=1 in PvP arena, got now_bgm=" +
                     std::to_string(sf_app_CpCanvas_now_bgm) + " boss_flg=" +
                     std::to_string(sf_app_CpCanvas_boss_flg));
        }
        int nav_bgm_sid = rt.last_played_port_sound_id(0);
        fn_app_CpCanvas_Audio___II__V(rt, 1, 0);  // Title BGM
        int title_bgm_sid = rt.last_played_port_sound_id(0);
        fn_app_CpCanvas_Audio___II__V(rt, 1, 10); // Ending BGM
        int ending_bgm_sid = rt.last_played_port_sound_id(0);
        fn_app_CpCanvas_Audio___II__V(rt, 1, 2);  // Restore Navi Battle BGM
        if (nav_bgm_sid < 0 || title_bgm_sid < 0 || ending_bgm_sid < 0 ||
            nav_bgm_sid == ending_bgm_sid || title_bgm_sid == ending_bgm_sid || nav_bgm_sid == title_bgm_sid) {
            fail_msg("SuiteD-BgmDistinctPlayback",
                     "BGM sound IDs aliased: title=" + std::to_string(title_bgm_sid) +
                     " navi_battle=" + std::to_string(nav_bgm_sid) +
                     " ending=" + std::to_string(ending_bgm_sid));
        }

        // 3. Verify dynamic getAudioPresenter(1) lives outside heap_ (0 heap growth across calls)
        size_t heap_before = rt.heap_size();
        Slot ap_arg[1];
        ap_arg[0].i = 1;
        Ref ap1_first = rt.invoke_external("com/nttdocomo/ui/AudioPresenter", "getAudioPresenter",
                                           "(I)Lcom/nttdocomo/ui/AudioPresenter;", ap_arg, 1).r;
        Ref ap1_second = rt.invoke_external("com/nttdocomo/ui/AudioPresenter", "getAudioPresenter",
                                            "(I)Lcom/nttdocomo/ui/AudioPresenter;", ap_arg, 1).r;
        if (ap1_first <= 0 || ap1_second <= 0 || rt.heap_size() != heap_before) {
            fail_msg("SuiteD-AudioPresenterHeap",
                     "getAudioPresenter(1) grew heap or returned invalid ref (" +
                     std::to_string(ap1_first) + ", " + std::to_string(ap1_second) + ")");
        }

        // 4. Simulate 40 ticks of Buster Charging (key_cnt -> 4 -> 17 -> Charge Shot release),
        // verifying authentic SE queueing/playback, rollback SE deduplication, and mill isolation!
        int queued_se_ticks = 0;
        for (uint32_t t = 0; t < 40; ++t) {
            int32_t m0 = (t < 24) ? (1 << 10) : 0; // Hold Buster Charge for 24 ticks, then release Charge Shot!
            int32_t m1 = ((t % 9) == 2) ? (1 << 17) : (((t % 9) == 6) ? (1 << 19) : (1 << 10));
            sf_app_CpCanvas_mill = static_cast<int64_t>(100000 + t * 62);
            save_rollback_slot(rt, t, m0, m1, false);
            step_pvp_combat_tick(rt, cp, m0, m1);
            render_pvp_combat_frame(rt, cp);
            if (sf_app_CpCanvas_se_set > 0 && rt.iaload(sf_app_CpCanvas_se_set, 0) > 0) {
                ++queued_se_ticks;
            }
            try {
                fn_app_CpCanvas_sePlay_____V(rt, cp);
            } catch (const JavaException& ex) {
                fail_msg("SuiteD-ChargeSePlay",
                         "CpCanvas.sePlay() threw JavaException during Buster Charge at turn " + std::to_string(t));
                break;
            }
            if ((t == 12 || t == 22 || t == 30) && t >= 8) {
                int64_t live_wall_mill = 987654321LL + static_cast<int64_t>(t);
                sf_app_CpCanvas_mill = live_wall_mill;
                uint16_t expected_post_h = compute_combat_state_hash(rt);
                uint32_t rb_from = t - 8;
                if (!restore_rollback_slot(rt, rb_from)) {
                    fail_msg("SuiteD-ChargeRollback", "restore_rollback_slot failed at turn " + std::to_string(rb_from));
                    break;
                }
                if (sf_app_CpCanvas_mill != live_wall_mill) {
                    fail_msg("SuiteD-MillPreserved",
                             "Rollback overwrote sf_app_CpCanvas_mill (" + std::to_string(sf_app_CpCanvas_mill) +
                             " != " + std::to_string(live_wall_mill) + ")");
                    break;
                }
                for (uint32_t u = rb_from; u <= t; ++u) {
                    int32_t um0 = (u < 24) ? (1 << 10) : 0;
                    int32_t um1 = ((u % 9) == 2) ? (1 << 17) : (((u % 9) == 6) ? (1 << 19) : (1 << 10));
                    save_rollback_slot(rt, u, um0, um1, false);
                    step_pvp_combat_tick(rt, cp, um0, um1);
                }
                render_pvp_combat_frame(rt, cp);
                // Identical rollback resimulation must NOT re-queue already-emitted SEs!
                if (sf_app_CpCanvas_se_set > 0 && rt.iaload(sf_app_CpCanvas_se_set, 0) != 0) {
                    fail_msg("SuiteD-RollbackSeDedup",
                             "Rollback resimulation re-queued duplicate SE " +
                             std::to_string(rt.iaload(sf_app_CpCanvas_se_set, 0)) + " at turn " + std::to_string(t));
                    break;
                }
                fn_app_CpCanvas_sePlay_____V(rt, cp);
                if (sf_app_CpCanvas_mill != live_wall_mill) {
                    fail_msg("SuiteD-MillAfterRender", "render_pvp_combat_frame overwrote sf_app_CpCanvas_mill");
                    break;
                }
                if (compute_combat_state_hash(rt) != expected_post_h) {
                    fail_msg("SuiteD-ChargeRollbackHash", "Hash diverged after Buster Charge rollback");
                    break;
                }
            }
        }
        if (queued_se_ticks < 3 || rt.last_played_port_sound_id(1) < 0) {
            fail_msg("SuiteD-CombatSeMissing",
                     "Expected >= 3 combat SE cues queued & played during Charge+Shot, got ticks=" +
                     std::to_string(queued_se_ticks) + " last_sid=" + std::to_string(rt.last_played_port_sound_id(1)));
        }

        // 5. Verify UI confirm chime Audio(1, 11) -> m_audio[23] is NEVER overwritten by sePlay() on port 1
        Ref ui_confirm_pres = rt.iaload(sf_app_CpCanvas_m_audio, 23);
        Ref ui_confirm_sref = rt.audio_presenter_sound_ref(ui_confirm_pres);
        int32_t expected_ui_sid = (ui_confirm_sref > 0) ? rt.get(ui_confirm_sref).aux_i32 : -1;
        fn_app_CpCanvas_Audio___II__V(rt, 1, 11);
        if (expected_ui_sid < 0 || rt.last_played_port_sound_id(1) != expected_ui_sid) {
            fail_msg("SuiteD-UiChimePreserved",
                     "Audio(1, 11) played sound_id=" + std::to_string(rt.last_played_port_sound_id(1)) +
                     " instead of m_audio[23] sound_id=" + std::to_string(expected_ui_sid));
        }

        // 6. Verify TrillTimeSyncThrottler behavior (high-RTT invariance + asymmetric EMA + zero negative windup)
        TrillTimeSyncThrottler th{};
        for (int i = 0; i < 10; ++i) {
            // Equal clocks under 250ms RTT: local_adv = +3, rem_adv = +3 -> skew = 0, spec_balance = +3
            if (th.update_extra_wait_ms(0, 3) != 0) {
                fail_msg("SuiteD-TrillHighRttZeroSkew", "Expected 0ms slowdown when skew == 0 under high RTT");
                break;
            }
        }
        th.reset();
        // Trailing history (-8 skew, -4 balance) must floor cleanly and never delay subsequent lead engagement
        for (int i = 0; i < 20; ++i) {
            if (th.update_extra_wait_ms(-8, -4) != 0) {
                fail_msg("SuiteD-TrillTrailingZero", "Trailing peer must never receive positive extra_wait_ms");
                break;
            }
        }
        int64_t lead_wait = 0;
        for (int i = 0; i < 6; ++i) {
            lead_wait = th.update_extra_wait_ms(8, 4); // 4 ticks ahead (skew = 2 * 4 = 8)
        }
        if (lead_wait <= 0 || lead_wait > 38) {
            fail_msg("SuiteD-TrillLeadingThrottle",
                     "Expected positive throttle (1..38ms) for +4 tick lead, got " + std::to_string(lead_wait));
        }
    }
    std::printf("[PvP SELF-TEST] Suite D complete: Buster Charge host/audio isolation & Trill time-sync verified (failures=%d)\n",
                failures);

    // ========================================================================
    // Suite E: Native Virus-Battle Time-Freeze Dimming (CpCanvas.Ann) & Announcement (stop_ch / strDraw)
    // ========================================================================
    {
        int dim_tid = (tid_areagrab > 0) ? tid_areagrab : 137; // AreaGrab or Roll
        reset_test_arena(500, 500, 0, 0, 1);
        sf_app_CpCanvas_cus_gage = 64; // Half-filled Custom Gauge to verify bar fill hides during Time-Freeze
        queue_tips_fifo(0, {static_cast<int16_t>(dim_tid)});

        const uint32_t* fb = rt.framebuffer();
        auto pack_rgb = [](int32_t r, int32_t g, int32_t b) -> uint32_t {
            return 0xFF000000u |
                   (static_cast<uint32_t>(r & 0xFF) << 16) |
                   (static_cast<uint32_t>(g & 0xFF) << 8) |
                   static_cast<uint32_t>(b & 0xFF);
        };
        auto count_white_banner_px = [&](int x0, int x1) -> int {
            int cnt = 0;
            for (int y = 66; y <= 80; ++y) {
                for (int x = x0; x < x1; ++x) {
                    if (fb[y * 240 + x] == 0xFFFFFFFFu) ++cnt;
                }
            }
            return cnt;
        };

        // Pre-Time-Freeze baseline frame: Custom Gauge fill (x=60, y=52) must be 0xFFC6D2EA
        render_pvp_combat_frame(rt, cp);
        if (fb[52 * 240 + 60] != 0xFFC6D2EAu) {
            fail_msg("SuiteE-PreGaugeFill", "Expected Custom Gauge fill 0xFFC6D2EA before Time-Freeze");
        }

        // Tick until P0 activates the dimming chip (stop_cnt == 1)
        bool triggered = false;
        for (int k = 0; k < 12; ++k) {
            step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
            if (sf_app_CpCanvas_stop_cnt == 1) {
                triggered = true;
                break;
            }
        }
        if (!triggered) {
            fail_msg("SuiteE-Trigger", "Failed to trigger Time-Freeze chip tid=" + std::to_string(dim_tid));
        } else {
            // Trigger tick (stop_cnt == 1): matches native BattleMain early-return (un-dimmed, no text yet)
            uint16_t h_before = compute_combat_state_hash(rt);
            render_pvp_combat_frame(rt, cp);
            if (compute_combat_state_hash(rt) != h_before) {
                fail_msg("SuiteE-ReadOnlyRender1", "render_pvp_combat_frame mutated combat state hash at stop_cnt=1");
            }
            if (fb[92 * 240 + 120] != pack_rgb(155, 255, 210)) {
                fail_msg("SuiteE-StopCnt1Bright", "Expected full brightness backdrop on trigger tick stop_cnt=1");
            }
            if (count_white_banner_px(0, 240) != 0) {
                fail_msg("SuiteE-StopCnt1NoText", "Expected no announcement text on trigger tick stop_cnt=1");
            }

            // Step across full Time-Freeze duration until stop_cnt returns to 0
            int seen_banner_frames = 0;
            bool checked_cnt15 = false;
            bool checked_fadeout = false;
            for (int step = 0; step < 120 && sf_app_CpCanvas_stop_cnt > 0; ++step) {
                step_pvp_combat_tick(rt, cp, 0, 0);
                int32_t sc = sf_app_CpCanvas_stop_cnt;
                int32_t st = sf_app_CpCanvas_stop_time;

                if (sc >= 2 && sc < 15) {
                    ++seen_banner_frames;
                    int32_t er = 0, eg = 0, eb = 0;
                    compute_ann_backdrop_rgb(sc, st, er, eg, eb);

                    // Render from P0 perspective (stop_owner == local_idx -> left half x < 120)
                    g_pvp_eng.local_idx = 0;
                    uint16_t h0 = compute_combat_state_hash(rt);
                    render_pvp_combat_frame(rt, cp);
                    if (compute_combat_state_hash(rt) != h0) {
                        fail_msg("SuiteE-ReadOnlyRenderP0", "render_pvp_combat_frame mutated hash during banner");
                        break;
                    }
                    if (fb[92 * 240 + 120] != pack_rgb(er, eg, eb)) {
                        fail_msg("SuiteE-DarkenCurve",
                                 "Backdrop RGB mismatch at stop_cnt=" + std::to_string(sc));
                        break;
                    }
                    if (fb[52 * 240 + 60] == 0xFFC6D2EAu) {
                        fail_msg("SuiteE-GaugeFillHidden", "Custom Gauge fill should hide during Time-Freeze");
                        break;
                    }
                    if (count_white_banner_px(0, 120) <= 0 || count_white_banner_px(120, 240) != 0) {
                        fail_msg("SuiteE-LeftHalfBanner",
                                 "Expected white announcement strictly in left half for P0 at stop_cnt=" + std::to_string(sc));
                        break;
                    }

                    // Render from P1 perspective (stop_owner == opp_i -> right half x >= 120)
                    g_pvp_eng.local_idx = 1;
                    render_pvp_combat_frame(rt, cp);
                    g_pvp_eng.local_idx = 0;
                    if (count_white_banner_px(120, 240) <= 0 || count_white_banner_px(0, 120) != 0) {
                        fail_msg("SuiteE-RightHalfBanner",
                                 "Expected white announcement strictly in right half for P1 at stop_cnt=" + std::to_string(sc));
                        break;
                    }
                } else if (sc == 15) {
                    checked_cnt15 = true;
                    g_pvp_eng.local_idx = 0;
                    render_pvp_combat_frame(rt, cp);
                    // Verify chip name text at y=66..80 is cleared once attack animation begins at stop_cnt==15
                    if (count_white_banner_px(0, 240) != 0 && dim_tid == tid_areagrab) {
                        fail_msg("SuiteE-BannerEndsAt15", "Announcement text persisted onto stop_cnt=15");
                    }
                } else if (sc > 0 && st > 0 && sc >= st) {
                    checked_fadeout = true;
                    int32_t er = 0, eg = 0, eb = 0;
                    compute_ann_backdrop_rgb(sc, st, er, eg, eb);
                    render_pvp_combat_frame(rt, cp);
                    if (fb[92 * 240 + 120] != pack_rgb(er, eg, eb)) {
                        fail_msg("SuiteE-BrightenCurve",
                                 "Fade-out backdrop RGB mismatch at stop_cnt=" + std::to_string(sc));
                        break;
                    }
                }
            }

            if (seen_banner_frames != 13 || !checked_cnt15 || !checked_fadeout || sf_app_CpCanvas_stop_cnt != 0) {
                fail_msg("SuiteE-Lifecycle",
                         "Unexpected Time-Freeze lifecycle (banner_frames=" + std::to_string(seen_banner_frames) +
                         ", stop_cnt=" + std::to_string(sf_app_CpCanvas_stop_cnt) + ")");
            }

            // Post-Time-Freeze: backdrop and Custom Gauge fill must return to 100% normal
            render_pvp_combat_frame(rt, cp);
            if (fb[92 * 240 + 120] != pack_rgb(155, 255, 210) || fb[52 * 240 + 60] != 0xFFC6D2EAu) {
                fail_msg("SuiteE-PostRestore", "Backdrop or Custom Gauge fill failed to restore after Time-Freeze");
            }
        }

        // E2: Trap chip (Kawarimi / AntiDmg) asymmetric Time-Freeze announcement ("????" on opponent client,
        //     real chip name on own client), active trap "????" HUD indicators at left (x=5,y=75) & mirrored
        //     right (y=75), and Kawarimi counterattack smoke puff (effect 59, zero "!!" effect 39).
        if (tid_antidmg > 0) {
            reset_test_arena(500, 500, 0, 0, 1);
            queue_tips_fifo(0, {2}); // P0 Cannon (to trigger P1's Kawarimi later)
            queue_tips_fifo(1, {static_cast<int16_t>(tid_antidmg)}); // P1 Kawarimi / AntiDmg

            auto count_white_rect_px = [&](int x0, int x1, int y0, int y1) -> int {
                int cnt = 0;
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        if (fb[y * 240 + x] == 0xFFFFFFFFu) ++cnt;
                    }
                }
                return cnt;
            };

            // Measure exact white-pixel signature of "????" when drawn centered on right half at y=80
            int32_t qmark_w = 24;
            Ref qm_ref = get_trap_mask_str_ref(rt);
            if (sf_app_CpCanvas_f > 0) {
                Slot w_args[2];
                w_args[0].r = sf_app_CpCanvas_f;
                w_args[1].r = qm_ref;
                qmark_w = rt.invoke_external("com/nttdocomo/ui/Font", "stringWidth", "(Ljava/lang/String;)I", w_args, 2).i;
            }
            int32_t expected_rx = std::clamp<int32_t>(240 - 5 - qmark_w, 0, std::max<int32_t>(0, 240 - qmark_w));

            bool trap_stop_started = false;
            for (int k = 0; k < 12; ++k) {
                step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                if (sf_app_CpCanvas_stop_cnt == 1) {
                    trap_stop_started = true;
                    break;
                }
            }
            if (!trap_stop_started) {
                fail_msg("SuiteE-TrapStart", "Failed to start P1 Kawarimi Time-Freeze");
            } else {
                step_pvp_combat_tick(rt, cp, 0, 0); // Advance to stop_cnt == 2
                // P1's own client (local_idx = 1): sees real chip name on left half (x in [0, 120))
                g_pvp_eng.local_idx = 1;
                render_pvp_combat_frame(rt, cp);
                int own_left_px = count_white_rect_px(0, 120, 66, 80);
                // P0's client (local_idx = 0): opponent P1 used trap -> sees "????" on right half!
                g_pvp_eng.local_idx = 0;
                render_pvp_combat_frame(rt, cp);
                int opp_right_px = count_white_rect_px(120, 240, 66, 80);
                // Verify "????" bounding span on right half is strictly inside [(120 - qmark_w)/2 + 120 .. + qmark_w]
                int32_t mask_tx = std::clamp<int32_t>((120 - qmark_w) / 2 + 120, 0, std::max<int32_t>(0, 240 - qmark_w));
                int outside_mask_px =
                    count_white_rect_px(120, mask_tx, 66, 80) +
                    count_white_rect_px(mask_tx + qmark_w + 1, 240, 66, 80);
                if (own_left_px <= 0 || opp_right_px <= 0 || outside_mask_px != 0 || own_left_px == opp_right_px) {
                    fail_msg("SuiteE-TrapMaskBanner",
                             "Expected P1 real trap name (" + std::to_string(own_left_px) +
                             " px) vs P0 masked ???? (" + std::to_string(opp_right_px) +
                             " px, outside=" + std::to_string(outside_mask_px) + ")");
                }

                // Finish P1's Kawarimi Time-Freeze so P1.wana == 5 is active
                for (int step = 0; step < 80 && sf_app_CpCanvas_stop_cnt > 0; ++step) {
                    step_pvp_combat_tick(rt, cp, 0, 0);
                }
                if (g_pvp_eng.core.sides[1].wana != 5) {
                    fail_msg("SuiteE-TrapArmed", "Expected P1.wana == 5 after Kawarimi Time-Freeze");
                } else {
                    // On P0's client (local_idx = 0): P0 has no trap (left x=5..5+qmark_w empty),
                    // P1 has active trap (right x=expected_rx..expected_rx+qmark_w has white "????" at y=63..75)
                    g_pvp_eng.local_idx = 0;
                    render_pvp_combat_frame(rt, cp);
                    int p0_left_hud = count_white_rect_px(5, 5 + qmark_w, 63, 75);
                    int p0_right_hud = count_white_rect_px(expected_rx, expected_rx + qmark_w, 63, 75);
                    // On P1's client (local_idx = 1): P1 has active trap at top-left (x=5..5+qmark_w), right empty!
                    g_pvp_eng.local_idx = 1;
                    render_pvp_combat_frame(rt, cp);
                    g_pvp_eng.local_idx = 0;
                    int p1_left_hud = count_white_rect_px(5, 5 + qmark_w, 63, 75);
                    int p1_right_hud = count_white_rect_px(expected_rx, expected_rx + qmark_w, 63, 75);
                    if (p0_left_hud != 0 || p0_right_hud <= 0 ||
                        p1_left_hud != p0_right_hud || p1_right_hud != 0) {
                        fail_msg("SuiteE-ActiveTrapHud",
                                 "Active trap ???? HUD mismatch: P0(L=" + std::to_string(p0_left_hud) +
                                 ",R=" + std::to_string(p0_right_hud) + ") P1(L=" +
                                 std::to_string(p1_left_hud) + ",R=" + std::to_string(p1_right_hud) + ")");
                    }

                    // Now P0 fires Cannon to trigger P1's Kawarimi: verify effect 59 spawns and effect 39 ("!!") NEVER spawns!
                    bool saw_eff59 = false;
                    bool saw_eff39 = false;
                    for (int k = 0; k < 55; ++k) {
                        step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
                        for (int s = 0; s < 2; ++s) {
                            Ref arr = g_pvp_eng.core.sides[s].ata_arr;
                            for (int ai = 0; ai < 30; ++ai) {
                                Ref a = rt.iaload(arr, ai);
                                if (a > 0 && rt.field_i32(a, 38) != 0) {
                                    int32_t eff_id = rt.field_i32(a, 2);
                                    int32_t mode39 = rt.field_i32(a, 39);
                                    if (mode39 >= 2 && eff_id == 59) saw_eff59 = true;
                                    if (mode39 >= 2 && eff_id == 39) saw_eff39 = true;
                                }
                            }
                        }
                    }
                    if (!saw_eff59 || saw_eff39) {
                        fail_msg("SuiteE-KawarimiEff",
                                 "Expected Kawarimi smoke eff 59 without '!!' eff 39 (saw59=" +
                                 std::to_string(saw_eff59) + ", saw39=" + std::to_string(saw_eff39) + ")");
                    }
                    // After Kawarimi triggers, both left and right "????" HUD indicators must clear
                    render_pvp_combat_frame(rt, cp);
                    if (count_white_rect_px(5, 5 + qmark_w, 63, 75) != 0 ||
                        count_white_rect_px(expected_rx, expected_rx + qmark_w, 63, 75) != 0) {
                        fail_msg("SuiteE-TrapClearedHud", "Expected ???? HUD to clear after Kawarimi triggered");
                    }
                }
            }

            // E3: Kawarimi vs multi-hit Time-Freeze attacks (both Giga multi-hit e.g. MeteorRedSun AND
            //     Navi multi-hit e.g. Roll): must block ALL hits during Time-Freeze and counterattack for 100 dmg!
            int multihit_candidates[2] = {tid_multihit_stop, tid_multihit_navi};
            for (int ci = 0; ci < 2; ++ci) {
                int m_tid = multihit_candidates[ci];
                if (m_tid < 0 || (ci == 1 && m_tid == multihit_candidates[0])) continue;

                reset_test_arena(500, 500, 0, 0, 1);
                queue_tips_fifo(0, {static_cast<int16_t>(m_tid)});
                queue_tips_fifo(1, {static_cast<int16_t>(tid_antidmg)});

                // 1. P1 arms Kawarimi (wana == 5)
                for (int k = 0; k < 90; ++k) {
                    step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                    if (k > 5 && sf_app_CpCanvas_stop_cnt == 0 && g_pvp_eng.core.sides[1].wana == 5) break;
                }
                // 2. P0 unleashes multi-hit Time-Freeze chip -> step until Time-Freeze + post-counterattack settle
                for (int k = 0; k < 240; ++k) {
                    step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
                    if (k > 30 && sf_app_CpCanvas_stop_cnt == 0 && g_pvp_eng.core.sides[0].hp == 400) {
                        // Step a few extra ticks to make sure no trailing projectiles damage P1 either
                        for (int extra = 0; extra < 15; ++extra) step_pvp_combat_tick(rt, cp, 0, 0);
                        break;
                    }
                }
                if (g_pvp_eng.core.sides[1].hp != 500 ||
                    g_pvp_eng.core.sides[1].wana != 0 ||
                    g_pvp_eng.core.sides[0].hp != 400) {
                    fail_msg("SuiteE-KawarimiBlocksAllMultiHitStop",
                             "Expected Kawarimi to block all hits of multi-hit Time-Freeze Tip " +
                             std::to_string(m_tid) + " (" + tip_name_of(m_tid) +
                             "): P1 hp=" + std::to_string(g_pvp_eng.core.sides[1].hp) +
                             " (want 500), P0 hp=" + std::to_string(g_pvp_eng.core.sides[0].hp) +
                             " (want 400), stop_cnt=" + std::to_string(sf_app_CpCanvas_stop_cnt));
                }
            }
        }

        // E4: Invis vs Time-Freeze attacks & Elemental Traps:
        //  (a) Non-piercing Time-Freeze attacks (Roll / MeteorRedSun / SuperNorthWind) must NOT damage
        //      an Invisible opponent, must preserve remaining Invis duration across Time-Freeze, and
        //      must NOT prematurely consume an armed Kawarimi trap while Invisible.
        //  (b) Elemental Traps (AntiFire..AntiWood) DO pierce Invis on stop_cnt == 16 (matching native
        //      CpCanvas.BattleMain L_3483 -> Ene.Hit).
        if (tid_invis < 0 || tid_multihit_navi < 0 || tid_multihit_stop < 0 ||
            tid_antifire < 0 || tid_fire_atk < 0) {
            fail_msg("SuiteE-InvisPrereqs",
                     "Missing Invis test chips: invis=" + std::to_string(tid_invis) +
                     " navi=" + std::to_string(tid_multihit_navi) +
                     " stop=" + std::to_string(tid_multihit_stop));
        } else {
            auto wait_side_ready = [&](int side_idx) {
                for (int k = 0; k < 60; ++k) {
                    const SideContext& sc = g_pvp_eng.core.sides[side_idx];
                    if (sf_app_CpCanvas_stop_cnt == 0 && sc.joutai == 0 &&
                        sc.non_cnt <= 0 && sc.non_cnt2 <= 0 && sc.move_flg == 0 && sc.tmp_ata < 0) {
                        break;
                    }
                    step_pvp_combat_tick(rt, cp, 0, 0);
                }
            };

            int invis_test_atks[3] = {tid_multihit_navi, tid_multihit_stop, tid_nrthwind};
            for (int ci = 0; ci < 3; ++ci) {
                int m_tid = invis_test_atks[ci];
                if (m_tid < 0) continue;
                if (ci == 1 && m_tid == invis_test_atks[0]) continue;

                reset_test_arena(500, 500, 0, 0, 1);
                queue_tips_fifo(0, {static_cast<int16_t>(m_tid)});
                if (tid_antidmg > 0) {
                    queue_tips_fifo(1, {static_cast<int16_t>(tid_antidmg), static_cast<int16_t>(tid_invis)});
                    for (int k = 0; k < 90; ++k) {
                        step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                        if (k > 5 && sf_app_CpCanvas_stop_cnt == 0 && g_pvp_eng.core.sides[1].wana == 5) break;
                    }
                    wait_side_ready(1);
                } else {
                    queue_tips_fifo(1, {static_cast<int16_t>(tid_invis)});
                }

                // P1 casts Invis -> muteki_cnt becomes 90 when Invis Time-Freeze ends
                for (int k = 0; k < 90; ++k) {
                    step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                    if (k > 5 && sf_app_CpCanvas_stop_cnt == 0 && g_pvp_eng.core.sides[1].muteki_cnt > 0) break;
                }
                for (int k = 0; k < 10; ++k) step_pvp_combat_tick(rt, cp, 0, 0);
                int32_t pre_stop_muteki = g_pvp_eng.core.sides[1].muteki_cnt;
                if (pre_stop_muteki < 75 || pre_stop_muteki > 85) {
                    fail_msg("SuiteE-InvisSetup",
                             "Expected P1 muteki_cnt ~80 after Invis + 10 ticks, got " +
                             std::to_string(pre_stop_muteki));
                }

                // P0 unleashes Time-Freeze chip against Invisible P1
                for (int k = 0; k < 240; ++k) {
                    step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
                    if (k > 15 && sf_app_CpCanvas_stop_cnt == 0) break;
                }
                int32_t post_stop_muteki = g_pvp_eng.core.sides[1].muteki_cnt;
                int32_t expected_wana = (tid_antidmg > 0) ? 5 : 0;
                if (g_pvp_eng.core.sides[1].hp != 500 ||
                    post_stop_muteki < pre_stop_muteki - 2 ||
                    g_pvp_eng.core.sides[1].wana != expected_wana) {
                    fail_msg("SuiteE-InvisBlocksTimeFreeze",
                             "Expected Invis to block Tip " + std::to_string(m_tid) +
                             " (" + tip_name_of(m_tid) + ") & preserve Invis/Kawarimi: P1 hp=" +
                             std::to_string(g_pvp_eng.core.sides[1].hp) +
                             " muteki=" + std::to_string(post_stop_muteki) +
                             " (pre=" + std::to_string(pre_stop_muteki) +
                             ") wana=" + std::to_string(g_pvp_eng.core.sides[1].wana));
                }
            }

            // E4b: P0 arms AntiFire, P1 casts Invis and then uses a Fire chip -> AntiFire pierces Invis!
            reset_test_arena(500, 500, 0, 0, 1);
            queue_tips_fifo(0, {static_cast<int16_t>(tid_antifire)});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_invis), static_cast<int16_t>(tid_fire_atk)});
            for (int k = 0; k < 90; ++k) {
                step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
                if (k > 5 && sf_app_CpCanvas_stop_cnt == 0 && g_pvp_eng.core.sides[0].wana == 1) break;
            }
            for (int k = 0; k < 90; ++k) {
                step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                if (k > 5 && sf_app_CpCanvas_stop_cnt == 0 && g_pvp_eng.core.sides[1].muteki_cnt > 0) break;
            }
            wait_side_ready(1);
            if (g_pvp_eng.core.sides[1].muteki_cnt <= 60) {
                fail_msg("SuiteE-AntiElemInvisActive",
                         "Expected P1 still Invisible (>60 ticks) before Fire chip, got " +
                         std::to_string(g_pvp_eng.core.sides[1].muteki_cnt));
            }
            for (int k = 0; k < 90; ++k) {
                step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                if (k > 20 && sf_app_CpCanvas_stop_cnt == 0) break;
            }
            if (g_pvp_eng.core.sides[1].hp != 350 || g_pvp_eng.core.sides[0].wana != 0) {
                fail_msg("SuiteE-AntiElemPiercesInvis",
                         "Expected AntiFire to pierce P1 Invis for 150 dmg (want P1 hp=350), got hp=" +
                         std::to_string(g_pvp_eng.core.sides[1].hp) +
                         " P0.wana=" + std::to_string(g_pvp_eng.core.sides[0].wana));
            }
        }

        // E5: 30-Chip Folder Depletion Across Custom Screen Rounds + Shuffle + Regular Chip + Empty Tray (max_sel == 0)
        {
            reset_test_arena(600, 600, 0, 0, 1);
            // Configure P0 with 30 asterisk-code ('*' = 26) Cannon chips, Regular chip at slot 14, and Shuffle skill (sec_cha = 1)
            pvp::PvpFighterBuild dep_b0 = g_pvp_eng.builds[0];
            for (int i = 0; i < 30; ++i) {
                dep_b0.folder_packed[i] = (2 << 8) | 26;
            }
            dep_b0.regu_flg = 1;
            dep_b0.regu_id = 14;
            dep_b0.skill_kouka[0] = 0;
            dep_b0.skill_kouka[1] = 0;
            dep_b0.skill_kouka[15] = 1; // Shuffle skill enabled once per battle
            g_pvp_eng.builds[0] = dep_b0;
            init_pvp_battle_arena(rt, cp);
            g_pvp_eng.local_idx = 0;

            auto commit_p0_round = [&](const pvp::PvpCustomCommit& c0) {
                pvp::PvpCustomCommit c1{};
                c1.turn_no = g_pvp_eng.core.custom_round;
                c1.tip_no = -1;
                c1.max_sel = 5;
                c1.pa_no = -1;
                for (int i = 0; i < 9; ++i) c1.rock_tip_ids[i] = -1;

                g_pvp_eng.core = g_pvp_eng.pre_custom_core;
                generated_restore_statics(g_pvp_eng.pre_custom_statics);
                rt.restore_combat_snapshot(g_pvp_eng.pre_custom_snap);
                g_pvp_eng.local_idx = 0;
                ensure_board_orient(rt, 0);
                apply_custom_commit_to_side(rt, 0, c0);
                apply_custom_commit_to_side(rt, 1, c1);
                g_pvp_eng.phase = Phase::COMBAT;
            };

            uint32_t consumed_slots_seen = 0;
            bool duplicate_slot_drawn = false;

            auto step_bt_until_ready = [&]() {
                for (int w = 0; w < 8 && sf_app_CpCanvas_cas_cnt < 0; ++w) {
                    sf_app_CpCanvas_key = 0;
                    rt.set_keypad_override(true, 0);
                    fn_app_CpCanvas_BattleTip_____V(rt, cp);
                    rt.set_keypad_override(false, 0);
                    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
                }
            };

            for (int rnd = 1; rnd <= 6; ++rnd) {
                enter_custom_screen_phase(rt, cp);
                step_bt_until_ready();
                if (rnd == 1 && rt.iaload(sf_app_CpCanvas_cas_tip, 0) != 15) {
                    fail_msg("SuiteE-ReguSlotDraw",
                             "Expected Regular chip slot 14 (cas_tip=15) in slot 0 on Round 1, got " +
                             std::to_string(rt.iaload(sf_app_CpCanvas_cas_tip, 0)));
                }
                // Verify zero already-consumed folder slots appear in cas_tip
                for (int i = 0; i < 10; ++i) {
                    int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, i);
                    if (ct >= 1 && ct <= 30 && ((consumed_slots_seen >> (ct - 1)) & 1u) != 0) {
                        duplicate_slot_drawn = true;
                    }
                }

                if (rnd == 2) {
                    // Select slot 0 then cancel (SOFT1 = 1<<21), then exercise Shuffle (sec_cha=1, sel_cas_mode=2)
                    sf_app_CpCanvas_sel_cas_mode = 0;
                    sf_app_CpCanvas_sel_cas_tip = 0;
                    sf_app_CpCanvas_key = (1 << 20);
                    rt.set_keypad_override(true, sf_app_CpCanvas_key);
                    fn_app_CpCanvas_BattleTip_____V(rt, cp);
                    rt.set_keypad_override(false, 0);
                    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);

                    sf_app_CpCanvas_key = (1 << 21); // SOFT1 = Cancel selection back to sel_cnt == 0
                    rt.set_keypad_override(true, sf_app_CpCanvas_key);
                    fn_app_CpCanvas_BattleTip_____V(rt, cp);
                    rt.set_keypad_override(false, 0);
                    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);

                    sf_app_CpCanvas_sec_cha = 1;     // Enable Shuffle on Round 2 to test consumed-slot immunity
                    sf_app_CpCanvas_sel_cas_mode = 2; // Highlight Shuffle
                    sf_app_CpCanvas_key = (1 << 20);
                    rt.set_keypad_override(true, sf_app_CpCanvas_key);
                    fn_app_CpCanvas_BattleTip_____V(rt, cp);
                    rt.set_keypad_override(false, 0);
                    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
                    step_bt_until_ready();

                    // Verify Shuffle did NOT resurrect any Round-1 consumed chips!
                    for (int i = 0; i < 10; ++i) {
                        int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, i);
                        if (ct >= 1 && ct <= 30 && ((consumed_slots_seen >> (ct - 1)) & 1u) != 0) {
                            duplicate_slot_drawn = true;
                        }
                    }
                }

                // Record and select all 5 drawn chips in slots 0..4, then confirm OK
                for (int s = 0; s < 5; ++s) {
                    int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, s);
                    if (ct < 1 || ct > 30) {
                        fail_msg("SuiteE-FolderDrawCount",
                                 "Expected valid cas_tip in slot " + std::to_string(s) + " on round " + std::to_string(rnd));
                    } else {
                        if (((consumed_slots_seen >> (ct - 1)) & 1u) != 0) {
                            duplicate_slot_drawn = true;
                        }
                        consumed_slots_seen |= (1u << (ct - 1));
                    }
                    sf_app_CpCanvas_sel_cas_mode = 0;
                    sf_app_CpCanvas_sel_cas_tip = s;
                    sf_app_CpCanvas_key = (1 << 20);
                    rt.set_keypad_override(true, sf_app_CpCanvas_key);
                    fn_app_CpCanvas_BattleTip_____V(rt, cp);
                    rt.set_keypad_override(false, 0);
                    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
                }

                for (int f = 0; f < 30 && sf_app_CpCanvas_scene != 1; ++f) {
                    if (f == 0) {
                        sf_app_CpCanvas_sel_cas_mode = 1;
                        sf_app_CpCanvas_key = (1 << 20);
                    } else {
                        sf_app_CpCanvas_key = 0;
                    }
                    rt.set_keypad_override(true, sf_app_CpCanvas_key);
                    fn_app_CpCanvas_BattleTip_____V(rt, cp);
                    rt.set_keypad_override(false, 0);
                    sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
                }
                pvp::PvpCustomCommit raw_c0 = capture_local_custom_commit(rt);
                std::vector<uint8_t> wire = raw_c0.serialize();
                pvp::PvpCustomCommit c0{};
                pvp::PvpCustomCommit::deserialize(wire.data(), wire.size(), c0);
                commit_p0_round(c0);
            }

            if (duplicate_slot_drawn || consumed_slots_seen != 0x3FFFFFFFu) {
                fail_msg("SuiteE-FolderDepletion30Chips",
                         "Expected all 30 unique folder slots consumed exactly once (mask=" +
                         std::to_string(consumed_slots_seen) + ", dup=" + std::to_string(duplicate_slot_drawn) + ")");
            }

            // Round 7: All 30 chips depleted -> max_sel must be 0, tray must be empty, D-pad safe, OK exits cleanly
            enter_custom_screen_phase(rt, cp);
            step_bt_until_ready();
            bool any_chip_in_r7 = false;
            for (int i = 0; i < 10; ++i) {
                if (rt.iaload(sf_app_CpCanvas_cas_tip, i) >= 1) any_chip_in_r7 = true;
            }
            if (sf_app_CpCanvas_max_sel != 0 || any_chip_in_r7 || sf_app_CpCanvas_sel_cas_mode != 1) {
                fail_msg("SuiteE-EmptyFolderRound7",
                         "Expected max_sel=0 & empty tray in Round 7, got max_sel=" +
                         std::to_string(sf_app_CpCanvas_max_sel) + " any_chip=" + std::to_string(any_chip_in_r7));
            }
            // Test D-pad navigation with max_sel == 0 (must not underflow sel_cas_tip to -1 or throw)
            const int dpad_keys[4] = {(1 << 16), (1 << 18), (1 << 17), (1 << 19)};
            for (int dk : dpad_keys) {
                int32_t bt_k = dk;
                if (sf_app_CpCanvas_max_sel <= 0) {
                    sf_app_CpCanvas_sel_cas_mode = 1;
                    sf_app_CpCanvas_sel_cas_tip = 0;
                    bt_k &= ~((1 << 16) | (1 << 17) | (1 << 18) | (1 << 19));
                }
                sf_app_CpCanvas_key = bt_k;
                rt.set_keypad_override(true, dk);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
                rt.set_keypad_override(false, 0);
                sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
            }
            for (int f = 0; f < 30 && sf_app_CpCanvas_scene != 1; ++f) {
                sf_app_CpCanvas_key = (f == 0) ? (1 << 20) : 0;
                rt.set_keypad_override(true, sf_app_CpCanvas_key);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
                rt.set_keypad_override(false, 0);
                sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
            }
            pvp::PvpCustomCommit empty_c0 = capture_local_custom_commit(rt);
            commit_p0_round(empty_c0);
            if (empty_c0.max_sel != 0 || g_pvp_eng.core.sides[0].max_sel != 0) {
                fail_msg("SuiteE-EmptyMaxSelCommit",
                         "Expected max_sel=0 preserved across commit, got " +
                         std::to_string(g_pvp_eng.core.sides[0].max_sel));
            }
        }

        // E6: Cross-Round Buster Charge Continuation + Held KEY_SELECT Suppress Guard + Round-Start Rollback
        {
            reset_test_arena(500, 500, 1, 0, 5); // P0 Heat element, Buster lv=5 / Charge lv=5 (cha_thresh = 17)
            g_pvp_eng.local_idx = 0;
            queue_tips_fifo(0, {});
            queue_tips_fifo(1, {});

            // Charge Buster in Round 1 for 10 frames (key_cnt reaches 11 < 17, bas_no >= 0, P1 HP stays 500)
            for (int k = 0; k < 10; ++k) {
                step_pvp_combat_tick(rt, cp, (1 << 10), 0);
            }
            int32_t r1_key_cnt = g_pvp_eng.core.sides[0].key_cnt;
            if (r1_key_cnt < 8 || r1_key_cnt >= 17 || g_pvp_eng.core.sides[1].hp != 500) {
                fail_msg("SuiteE-CrossRoundChargeSetup",
                         "Expected partial Buster charge in Round 1 (key_cnt=" + std::to_string(r1_key_cnt) +
                         ", P1.hp=" + std::to_string(g_pvp_eng.core.sides[1].hp) + ")");
            }

            // Open Custom Screen, commit 1 Cannon chip on P0 while P0 keeps Buster (1<<10) + OK Select (1<<20) held!
            enter_custom_screen_phase(rt, cp);
            pvp::PvpCustomCommit c0{}, c1{};
            c0.turn_no = g_pvp_eng.core.custom_round;
            c0.tip_no = 0;
            c0.max_sel = 5;
            c0.pa_no = -1;
            for (int i = 0; i < 9; ++i) c0.rock_tip_ids[i] = -1;
            c0.rock_tip_ids[0] = 2; // Cannon queued
            c1 = c0;
            c1.tip_no = -1;

            g_pvp_eng.core = g_pvp_eng.pre_custom_core;
            generated_restore_statics(g_pvp_eng.pre_custom_statics);
            rt.restore_combat_snapshot(g_pvp_eng.pre_custom_snap);
            g_pvp_eng.local_idx = 0;
            ensure_board_orient(rt, 0);
            apply_custom_commit_to_side(rt, 0, c0);
            apply_custom_commit_to_side(rt, 1, c1);
            int32_t p1_initial_rem_pred = compute_round_initial_remote_buster_pred(0);
            if (p1_initial_rem_pred != (1 << 10)) {
                fail_msg("SuiteE-CrossRoundRemotePredSeed",
                         "Expected opponent client to seed round_initial_remote_pred=(1<<10) for charging P0, got " +
                         std::to_string(p1_initial_rem_pred));
            }
            g_pvp_eng.core.sides[0].prev_input_mask = 0;
            g_pvp_eng.core.sides[1].prev_input_mask = 0;
            sf_app_CpCanvas_cus_gage = 0;
            sf_app_CpCanvas_scene = 1;

            uint32_t r2_start = g_pvp_eng.core.combat_turn;
            g_pvp_eng.round_start_turn = r2_start;
            g_pvp_eng.first_unconfirmed_turn = r2_start;
            g_pvp_eng.max_scheduled_turn = r2_start;
            g_pvp_eng.round_needs_initial_schedule = true;
            for (auto& em : g_pvp_eng.turn_emitted_se) {
                em.turn = UINT32_MAX;
                em.count = 0;
            }
            int32_t held_at_transition = (1 << 10) | (1 << 20); // Both Buster & Confirm-OK held!
            int32_t suppress_bits = (0xF << 16) | (1 << 21) | (1 << 22) |
                                    (g_pvp_eng.core.sides[0].tip_no >= 0 ? (1 << 20) : 0);
            g_pvp_eng.round_held_edge_suppress_mask = held_at_transition & suppress_bits;

            // Verify Peer 1's speculative frame 0 (using p1_initial_rem_pred before P0's UDP packet arrives)
            // keeps P0 charging silently with ZERO phantom Buster / Charge Shot sound effects!
            save_rollback_slot(rt, r2_start, p1_initial_rem_pred, 0, true);
            step_pvp_combat_tick(rt, cp, p1_initial_rem_pred, 0);
            const auto& r2_em = g_pvp_eng.turn_emitted_se[r2_start % kRollbackWindow];
            if (g_pvp_eng.core.sides[0].key_cnt != r1_key_cnt + 1 || (r2_em.turn == r2_start && r2_em.count != 0)) {
                fail_msg("SuiteE-CrossRoundOpponentNoPhantomShotSE",
                         "Opponent speculative frame 0 fired or emitted phantom SE (key_cnt=" +
                         std::to_string(g_pvp_eng.core.sides[0].key_cnt) + ", se_count=" +
                         std::to_string(r2_em.count) + ")");
            }
            if (!restore_rollback_slot(rt, r2_start)) {
                fail_msg("SuiteE-RoundStartRollback", "Failed to restore rollback slot at round_start_turn");
            }
            for (auto& em : g_pvp_eng.turn_emitted_se) {
                em.turn = UINT32_MAX;
                em.count = 0;
            }

            // Now step Round 2 frames 0..9 with live held_at_transition filtered by round_held_edge_suppress_mask
            for (int f = 0; f < 10; ++f) {
                int32_t raw_m = held_at_transition;
                g_pvp_eng.round_held_edge_suppress_mask &= raw_m;
                int32_t eff_m = raw_m & ~g_pvp_eng.round_held_edge_suppress_mask;
                step_pvp_combat_tick(rt, cp, eff_m, 0);
            }
            if (g_pvp_eng.core.sides[0].tip_no != 0 ||
                g_pvp_eng.core.sides[0].key_cnt <= 17 ||
                g_pvp_eng.core.sides[1].hp != 500) {
                fail_msg("SuiteE-CrossRoundChargeContinue",
                         "Expected Buster charge to continue past threshold (>17) without firing chip or pea-shot (key_cnt=" +
                         std::to_string(g_pvp_eng.core.sides[0].key_cnt) + ", tip_no=" +
                         std::to_string(g_pvp_eng.core.sides[0].tip_no) + ", P1.hp=" +
                         std::to_string(g_pvp_eng.core.sides[1].hp) + ")");
            }

            // Release Buster -> must fire full Heat Charge Shot (> 10 dmg, NOT 5-dmg uncharged buster!)
            for (int f = 0; f < 16; ++f) {
                step_pvp_combat_tick(rt, cp, 0, 0);
            }
            int32_t charge_dmg = 500 - g_pvp_eng.core.sides[1].hp;
            if (charge_dmg < 40) {
                fail_msg("SuiteE-CrossRoundChargeRelease",
                         "Expected full Charge Shot damage (>=40) after releasing cross-round charge, got dmg=" +
                         std::to_string(charge_dmg));
            }
        }

        // E7: Opponent Thrown Bomb (MiniBomb) Mirrored Parabolic Flight Smoothness (Zero Sub-Panel Teleports)
        if (tid_throw_bomb > 0) {
            auto trace_bomb_flight_centroids = [&](int thrower_side) -> std::vector<double> {
                reset_test_arena(500, 500, 0, 0, 1);
                g_pvp_eng.local_idx = 0; // Always render from P0's local perspective!
                queue_tips_fifo(thrower_side, {static_cast<int16_t>(tid_throw_bomb)});
                queue_tips_fifo(1 - thrower_side, {});

                std::vector<double> xs;
                for (int k = 0; k < 40; ++k) {
                    int32_t m0 = (thrower_side == 0 && k == 0) ? (1 << 20) : 0;
                    int32_t m1 = (thrower_side == 1 && k == 0) ? (1 << 20) : 0;
                    step_pvp_combat_tick(rt, cp, m0, m1);

                    // Check if thrown projectile is active in air on thrower_side
                    Ref ata_arr = g_pvp_eng.core.sides[thrower_side].ata_arr;
                    Ref bomb_a = 0;
                    for (int ai = 0; ai < 30; ++ai) {
                        Ref a = rt.iaload(ata_arr, ai);
                        if (a > 0 && rt.field_i32(a, 38) != 0 && rt.field_i32(a, 1) == 13) {
                            bomb_a = a;
                            break;
                        }
                    }
                    if (bomb_a <= 0) continue;

                    // Render with bomb visible vs hidden to isolate exact bomb sprite pixels on screen!
                    render_pvp_combat_frame(rt, cp);
                    std::vector<uint32_t> with_bomb(fb, fb + 240 * 240);

                    int32_t saved_on = rt.field_i32(bomb_a, 38);
                    rt.field_i32(bomb_a, 38) = 0;
                    render_pvp_combat_frame(rt, cp);
                    rt.field_i32(bomb_a, 38) = saved_on;

                    double sum_x = 0.0;
                    int diff_cnt = 0;
                    for (int y = 40; y < 220; ++y) {
                        for (int x = 0; x < 240; ++x) {
                            if (with_bomb[y * 240 + x] != fb[y * 240 + x]) {
                                sum_x += static_cast<double>(x);
                                ++diff_cnt;
                            }
                        }
                    }
                    if (diff_cnt >= 8) {
                        xs.push_back(sum_x / static_cast<double>(diff_cnt));
                    }
                }
                return xs;
            };

            std::vector<double> p0_xs = trace_bomb_flight_centroids(0);
            std::vector<double> p1_xs = trace_bomb_flight_centroids(1);
            if (p0_xs.size() < 8 || p0_xs.size() != p1_xs.size()) {
                fail_msg("SuiteE-BombFlightFrames",
                         "Expected >=8 matching bomb flight frames, got p0=" +
                         std::to_string(p0_xs.size()) + " p1=" + std::to_string(p1_xs.size()));
            } else {
                for (size_t i = 0; i < p1_xs.size(); ++i) {
                    // Opponent's mirrored bomb X + Local's bomb X must sum to screen width (239.0) within 2px
                    double mirror_sum = p0_xs[i] + p1_xs[i];
                    if (std::abs(mirror_sum - 239.0) > 2.5) {
                        fail_msg("SuiteE-BombMirrorSymmetry",
                                 "Opponent bomb X not symmetric with local bomb at frame " + std::to_string(i) +
                                 " (p0_x=" + std::to_string(p0_xs[i]) + ", p1_x=" + std::to_string(p1_xs[i]) + ")");
                        break;
                    }
                    if (i > 0) {
                        double step_dx = p1_xs[i] - p1_xs[i - 1];
                        // Opponent throws right-to-left: step_dx must be smoothly negative (~ -10px/tick), never positive or -70px jump!
                        if (step_dx > -2.0 || step_dx < -22.0) {
                            fail_msg("SuiteE-BombSmoothFlight",
                                     "Opponent bomb teleported/reversed at frame " + std::to_string(i) +
                                     " (step_dx=" + std::to_string(step_dx) + ")");
                            break;
                        }
                    }
                }
            }
        }
    }
    std::printf("[PvP SELF-TEST] Suite E complete: Native virus-battle Time-Freeze dimming & announcement verified (failures=%d)\n",
                failures);

    // ------------------------------------------------------------------------
    // Suite F1: Immediate Custom Retract + "等待对手选牌确认..." + Synced 15f BATTLE START
    // ------------------------------------------------------------------------
    {
        struct LoopbackRelay {
            plat::sock_t tcp_fd = plat::kInvalidSocket;
            plat::sock_t udp_fd = plat::kInvalidSocket;
            uint16_t port = 0;
            std::atomic<bool> stop_flag{false};
            pvp::PvpServerEngine engine;
            std::thread worker;

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

        LoopbackRelay relay;
        if (!relay.start()) {
            fail_msg("SuiteF1-LoopbackStart", "Failed to start local loopback PvP relay");
        } else {
            uint32_t* fb = rt.framebuffer();
            pvp::PvpClientSession peer;
            SelfTestFlagGuard st_guard(rt);
            DefaultSingleFormatGuard default_fmt_guard(rt.pvp_session());

            auto setup_live_match = [&](int32_t r_zoku0, int32_t ata0, int32_t cha0,
                                        const pvp::PvpFighterBuild& peer_build) -> bool {
                rt.restore_full_snapshot(base_heap);
                generated_restore_statics(base_statics);
                g_pvp_eng = PvpEngineState{};
                peer.close_udp();
                rt.pvp_session().close_udp();

                if (!rt.pvp_session().create_room(relay.addr(), "F1_HOST", "F1_P0", "pon") ||
                    !peer.join_room(relay.addr(), rt.pvp_session().room_id(), "F1_GUEST", "F1_P1", "pon")) {
                    return false;
                }

                // Configure local save stats before HANDSHAKE extracts local_build
                fn_app_CpCanvas_init___I__V(rt, cp, 1);
                fn_app_CpCanvas_Load___I__V(rt, cp, 0);
                sf_app_CpCanvas_max_hp = 500;
                sf_app_CpCanvas_ata_lv = ata0;
                sf_app_CpCanvas_cha_lv = cha0;
                sf_app_Edit_r_zoku = r_zoku0;
                // Fill active folder with 30 '*' Cannon chips and Regular at slot 14
                if (sf_app_CpCanvas_tip_list > 0 && sf_app_CpCanvas_fol > 0) {
                    int32_t star_cannon_slot = -1;
                    int32_t tlen = rt.array_length(sf_app_CpCanvas_tip_list);
                    for (int32_t si = 0; si < tlen; ++si) {
                        int32_t pk = rt.iaload(sf_app_CpCanvas_tip_list, si);
                        if (((pk >> 8) & 0xFF) == 2 && (pk & 0xFF) == 26) {
                            star_cannon_slot = si;
                            break;
                        }
                    }
                    if (star_cannon_slot >= 0) {
                        Ref fol_obj = rt.iaload(sf_app_CpCanvas_fol, std::clamp(sf_app_CpCanvas_now_fol, 0, 2));
                        if (fol_obj > 0) {
                            Ref tip_id_arr = rt.field_i32(fol_obj, 0);
                            if (tip_id_arr > 0 && rt.array_length(tip_id_arr) >= 30) {
                                for (int i = 0; i < 30; ++i) rt.iaload(tip_id_arr, i) = star_cannon_slot;
                                rt.field_i32(fol_obj, 7) = 1;  // regu_flg
                                rt.field_i32(fol_obj, 8) = 14; // regu_id
                            }
                        }
                    }
                }

                for (int w = 0; w < 80; ++w) {
                    rt.pvp_session().poll_udp();
                    peer.poll_udp();
                    if (rt.pvp_session().match_started() && peer.match_started()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (!rt.pvp_session().match_started() || !peer.match_started()) return false;

                for (int w = 0; w < 80 && g_pvp_eng.phase != Phase::CUSTOM_SCREEN; ++w) {
                    if (!peer.local_build_acked() && (w % 3) == 0) {
                        peer.send_build_sync(peer_build);
                    }
                    peer.poll_udp();
                    pvp_pon_tick_override_impl(rt, cp);
                    rt.present_screen();
                    if (g_pvp_eng.phase != Phase::CUSTOM_SCREEN) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }
                return g_pvp_eng.phase == Phase::CUSTOM_SCREEN;
            };

            auto step_override = [&]() -> bool {
                bool r = pvp_pon_tick_override_impl(rt, cp);
                rt.present_screen();
                return r;
            };

            auto wait_peer_commit_delivered = [&](uint16_t rnd, const pvp::PvpCustomCommit& rem_c) -> bool {
                for (int w = 0; w < 80; ++w) {
                    peer.send_custom_commit(rem_c);
                    peer.poll_udp();
                    rt.pvp_session().poll_udp();
                    pvp::PvpCustomCommit dummy{};
                    if (rt.pvp_session().has_remote_commit(rnd, dummy) &&
                        rt.pvp_session().is_custom_acked(rnd)) {
                        return true;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                return false;
            };

            auto pump_peer_inputs = [&](uint16_t rnd, uint32_t from_t, uint32_t to_t, int32_t mask) {
                for (uint32_t t = from_t; t <= to_t; ++t) {
                    peer.send_input_bundle(rnd, t, &mask, 1, 0);
                }
                peer.poll_udp();
                rt.pvp_session().poll_udp();
            };

            pvp::PvpFighterBuild peer_b{};
            peer_b.player_name = "F1_P1";
            peer_b.max_hp = 500;
            peer_b.ata_lv = 5;
            peer_b.cha_lv = 5;
            peer_b.r_zoku = 0;
            for (int i = 0; i < 30; ++i) peer_b.folder_packed[i] = (2 << 8) | 26;

            // --- F1.1 & F1.3 & F1.2 & F1.4: Manual OK immediate retract -> waiting -> 15f ROUND_BANNER ->
            //     Buster charge -> 30s timeout -> 15f ROUND_BANNER -> 0 phantom shot SE -> folder consumption ---
            if (!setup_live_match(1, 5, 5, peer_b)) {
                fail_msg("SuiteF1-HandshakeSetup", "Failed to reach Phase::CUSTOM_SCREEN over loopback relay");
            } else {
                // Wait out BattleTip slide-in (cas_cnt < 0 -> 0)
                for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) {
                    step_override();
                }

                // Select slot 0 chip, then highlight OK and press OK
                int32_t r1_picked_ct = rt.iaload(sf_app_CpCanvas_cas_tip, 0);
                uint32_t r1_selected_mask = (r1_picked_ct >= 1 && r1_picked_ct <= 30) ? (1u << (r1_picked_ct - 1)) : 0u;
                sf_app_CpCanvas_sel_cas_mode = 0;
                sf_app_CpCanvas_sel_cas_tip = 0;
                rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                step_override();
                step_override(); // neutral frame so key edge resets
                sf_app_CpCanvas_sel_cas_mode = 1;
                rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                step_override();

                // Verify immediate retract + waiting banner + zero pre-sync BATTLE START
                if (sf_app_CpCanvas_cas_cnt != 0 || sf_app_CpCanvas_pa_cnt2 != 0 ||
                    sf_app_CpCanvas_scene != 1 || !g_pvp_eng.local_custom_done ||
                    g_pvp_eng.phase != Phase::CUSTOM_SCREEN || g_pvp_eng.core.combat_turn != 0) {
                    fail_msg("SuiteF1-ManualOKImmediateRetract",
                             "Expected immediate retract & waiting state on OK tick (cas_cnt=" +
                             std::to_string(sf_app_CpCanvas_cas_cnt) + ", scene=" +
                             std::to_string(sf_app_CpCanvas_scene) + ", done=" +
                             std::to_string(g_pvp_eng.local_custom_done) + ", phase=" +
                             std::to_string(static_cast<int>(g_pvp_eng.phase)) + ")");
                }
                if (fb[102 * 240 + 28] != 0xFF68A8FFu) {
                    fail_msg("SuiteF1-WaitingBannerRender",
                             "Expected waiting banner border while awaiting peer commit");
                }
                // Hold in waiting state for 3 more ticks while peer hasn't committed
                for (int w = 0; w < 3; ++w) {
                    step_override();
                    if (g_pvp_eng.phase != Phase::CUSTOM_SCREEN || sf_app_CpCanvas_cas_cnt != 0 ||
                        g_pvp_eng.core.combat_turn != 0 || fb[102 * 240 + 28] != 0xFF68A8FFu) {
                        fail_msg("SuiteF1-WaitingHold",
                                 "Waiting state mutated prematurely on tick " + std::to_string(w));
                        break;
                    }
                }

                // Now deliver peer Round-1 commit & ACK
                pvp::PvpCustomCommit r1_rem{};
                r1_rem.turn_no = 1;
                r1_rem.tip_no = -1;
                r1_rem.max_sel = 5;
                r1_rem.pa_no = -1;
                for (int i = 0; i < 9; ++i) r1_rem.rock_tip_ids[i] = -1;
                if (!wait_peer_commit_delivered(1, r1_rem)) {
                    fail_msg("SuiteF1-PeerCommitR1", "Timed out waiting for peer Round 1 commit/ACK");
                } else {
                    // Tick 0 of ROUND_BANNER (transitions from CUSTOM_SCREEN -> ROUND_BANNER and renders frame 0)
                    step_override();
                    if (g_pvp_eng.phase != Phase::ROUND_BANNER || g_pvp_eng.banner_ticks != 1 ||
                        g_pvp_eng.core.combat_turn != 0) {
                        fail_msg("SuiteF1-RoundBannerEnter",
                                 "Expected ROUND_BANNER frame 0 immediately after sync (phase=" +
                                 std::to_string(static_cast<int>(g_pvp_eng.phase)) + ", banner_ticks=" +
                                 std::to_string(g_pvp_eng.banner_ticks) + ")");
                    }
                    // Verify BATTLE START sprite pixels actually drew onto the center region
                    std::vector<uint32_t> with_banner(fb, fb + 240 * 240);
                    render_pvp_combat_frame(rt, cp);
                    int banner_diff = 0;
                    for (int y = 104; y < 122; ++y) {
                        for (int x = 52; x < 188; ++x) {
                            if (with_banner[y * 240 + x] != fb[y * 240 + x]) ++banner_diff;
                        }
                    }
                    if (banner_diff == 0) {
                        fail_msg("SuiteF1-BannerSpritePixels",
                                 "Expected draw_native_battle_start_sprite to render pixels in center banner");
                    }

                    // Remaining 14 ticks of ROUND_BANNER (total 15 frames)
                    for (int b = 1; b < 15; ++b) {
                        step_override();
                        if (b < 14) {
                            if (g_pvp_eng.phase != Phase::ROUND_BANNER || g_pvp_eng.banner_ticks != b + 1 ||
                                g_pvp_eng.core.combat_turn != 0) {
                                fail_msg("SuiteF1-BannerDurationMid",
                                         "Expected ROUND_BANNER at frame " + std::to_string(b) +
                                         " with combat_turn==0");
                                break;
                            }
                        } else {
                            if (g_pvp_eng.phase != Phase::COMBAT || g_pvp_eng.banner_ticks != 15 ||
                                g_pvp_eng.core.combat_turn != 0) {
                                fail_msg("SuiteF1-BannerToCombat",
                                         "Expected transition to COMBAT after 15 ROUND_BANNER frames (phase=" +
                                         std::to_string(static_cast<int>(g_pvp_eng.phase)) + ", banner_ticks=" +
                                         std::to_string(g_pvp_eng.banner_ticks) + ", turn=" +
                                         std::to_string(g_pvp_eng.core.combat_turn) + ")");
                            }
                        }
                    }

                    // Round 1 COMBAT: Charge Buster for 10 frames (key_cnt reaches ~10 < 17)
                    pump_peer_inputs(1, 0, 14, 0);
                    int f_start = rt.frame_count() + 1;
                    rt.inject_key_span(f_start, f_start + 9, 10);
                    for (int k = 0; k < 10; ++k) {
                        step_override();
                    }
                    int32_t r1_key_cnt = g_pvp_eng.core.sides[0].key_cnt;
                    if (r1_key_cnt < 8 || r1_key_cnt >= 17 || g_pvp_eng.core.sides[1].hp != 500) {
                        fail_msg("SuiteF1-R1ChargeSetup",
                                 "Expected partial Buster charge in R1 (key_cnt=" + std::to_string(r1_key_cnt) +
                                 ", P1.hp=" + std::to_string(g_pvp_eng.core.sides[1].hp) + ")");
                    }

                    // Open Round 2 Custom Screen cleanly
                    g_pvp_eng.core.open_custom_req = true;
                    step_override();
                    if (g_pvp_eng.phase != Phase::CUSTOM_SCREEN || g_pvp_eng.core.custom_round != 2) {
                        fail_msg("SuiteF1-EnterR2Custom",
                                 "Expected Round 2 CUSTOM_SCREEN (phase=" +
                                 std::to_string(static_cast<int>(g_pvp_eng.phase)) + ", round=" +
                                 std::to_string(g_pvp_eng.core.custom_round) + ")");
                    } else {
                        for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) {
                            step_override();
                        }

                        // Verify 30-chip folder permanent consumption: no R1 consumed slot appears in R2 tray
                        bool r2_dup_slot = false;
                        for (int i = 0; i < 10; ++i) {
                            int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, i);
                            if (ct >= 1 && ct <= 30 && ((r1_selected_mask >> (ct - 1)) & 1u) != 0) {
                                r2_dup_slot = true;
                            }
                        }
                        if (r2_dup_slot || (g_pvp_eng.local_permanently_consumed_mask & r1_selected_mask) != r1_selected_mask) {
                            fail_msg("SuiteF1-FolderPermanentConsumption",
                                     "Round 1 consumed folder slot reappeared or lost from permanent mask");
                        }

                        // Select slot 0 in Round 2, then trigger 30s timeout (custom_timer_ticks == 1 -> 0)
                        sf_app_CpCanvas_sel_cas_mode = 0;
                        sf_app_CpCanvas_sel_cas_tip = 0;
                        rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                        step_override();

                        g_pvp_eng.custom_timer_ticks = 1;
                        int charge_f0 = rt.frame_count() + 1;
                        // Keep Buster (10) held across timeout, waiting, 15f ROUND_BANNER, and first 10f of R2 COMBAT
                        rt.inject_key_span(charge_f0, charge_f0 + 40, 10);
                        step_override();

                        if (sf_app_CpCanvas_cas_cnt != 0 || sf_app_CpCanvas_scene != 1 ||
                            !g_pvp_eng.local_custom_done || g_pvp_eng.phase != Phase::CUSTOM_SCREEN) {
                            fail_msg("SuiteF1-TimeoutImmediateRetract",
                                     "Expected 30s timeout to immediately retract & wait (cas_cnt=" +
                                     std::to_string(sf_app_CpCanvas_cas_cnt) + ", scene=" +
                                     std::to_string(sf_app_CpCanvas_scene) + ")");
                        }

                        pvp::PvpCustomCommit r2_rem{};
                        r2_rem.turn_no = 2;
                        r2_rem.tip_no = -1;
                        r2_rem.max_sel = 5;
                        r2_rem.pa_no = -1;
                        for (int i = 0; i < 9; ++i) r2_rem.rock_tip_ids[i] = -1;
                        if (!wait_peer_commit_delivered(2, r2_rem)) {
                            fail_msg("SuiteF1-PeerCommitR2", "Timed out waiting for peer Round 2 commit/ACK");
                        } else {
                            uint32_t r2_start_t = g_pvp_eng.core.combat_turn;
                            // Step 15 frames of ROUND_BANNER, also holding SELECT (20) as ROUND_BANNER finishes
                            for (int b = 0; b < 15; ++b) {
                                if (b == 14) {
                                    rt.inject_key_span(rt.frame_count() + 1, rt.frame_count() + 12, 20);
                                }
                                step_override();
                                if (b < 14 && (g_pvp_eng.phase != Phase::ROUND_BANNER ||
                                               g_pvp_eng.core.combat_turn != r2_start_t)) {
                                    fail_msg("SuiteF1-TimeoutBannerMid",
                                             "Timeout ROUND_BANNER exited early at frame " + std::to_string(b));
                                    break;
                                }
                            }
                            if (g_pvp_eng.phase != Phase::COMBAT || g_pvp_eng.core.combat_turn != r2_start_t) {
                                fail_msg("SuiteF1-TimeoutBannerToCombat",
                                         "Expected COMBAT after 15f timeout ROUND_BANNER");
                            }

                            // Verify opponent speculative frame 0 keeps charge with 0 phantom shot SE
                            {
                                int32_t rem_pred0 = compute_round_initial_remote_buster_pred(0);
                                if (rem_pred0 != (1 << 10)) {
                                    fail_msg("SuiteF1-CrossRoundPred",
                                             "Expected compute_round_initial_remote_buster_pred(0) == (1<<10) after ROUND_BANNER");
                                }
                                save_rollback_slot(rt, r2_start_t, rem_pred0, 0, true);
                                step_pvp_combat_tick(rt, cp, rem_pred0, 0);
                                const auto& em0 = g_pvp_eng.turn_emitted_se[r2_start_t % kRollbackWindow];
                                if (g_pvp_eng.core.sides[0].key_cnt != r1_key_cnt + 1 ||
                                    (em0.turn == r2_start_t && em0.count != 0)) {
                                    fail_msg("SuiteF1-ZeroPhantomShotSE",
                                             "Speculative frame 0 emitted phantom SE or lost charge (key_cnt=" +
                                             std::to_string(g_pvp_eng.core.sides[0].key_cnt) + ", se_count=" +
                                             std::to_string(em0.count) + ")");
                                }
                                restore_rollback_slot(rt, r2_start_t);
                                for (auto& em : g_pvp_eng.turn_emitted_se) {
                                    em.turn = UINT32_MAX;
                                    em.count = 0;
                                }
                            }

                            // Step 10 frames of R2 COMBAT while (1<<10)|(1<<20) are held:
                            // charge continues past 17 without prematurely firing queued chip!
                            pump_peer_inputs(2, r2_start_t, r2_start_t + 30, 0);
                            for (int k = 0; k < 10; ++k) {
                                step_override();
                            }
                            if (g_pvp_eng.core.sides[0].tip_no < 0 ||
                                g_pvp_eng.core.sides[0].key_cnt <= 17 ||
                                g_pvp_eng.core.sides[1].hp != 500) {
                                fail_msg("SuiteF1-CrossRoundChargeHold",
                                         "Expected Buster charge to continue past 17 without firing chip (key_cnt=" +
                                         std::to_string(g_pvp_eng.core.sides[0].key_cnt) + ", tip_no=" +
                                         std::to_string(g_pvp_eng.core.sides[0].tip_no) + ", P1.hp=" +
                                         std::to_string(g_pvp_eng.core.sides[1].hp) + ")");
                            }

                            // Release Buster -> fires full Heat Charge Shot (>= 40 dmg)
                            // Note: ensure frame_count_ is past charge_f0 + 40
                            while (rt.frame_count() < charge_f0 + 42) {
                                step_override();
                            }
                            for (int k = 0; k < 16; ++k) {
                                step_override();
                            }
                            int32_t r2_dmg = 500 - g_pvp_eng.core.sides[1].hp;
                            if (r2_dmg < 40) {
                                fail_msg("SuiteF1-CrossRoundChargeRelease",
                                         "Expected Heat Charge Shot >= 40 dmg after release, got " +
                                         std::to_string(r2_dmg));
                            }
                        }
                    }
                }
            }

            // --- F1.4b: 0-chip OK leftover hand retention across CUSTOM_SCREEN -> waiting -> ROUND_BANNER -> COMBAT ---
            if (tid_atk10 > 0) {
                if (!setup_live_match(0, 1, 1, peer_b)) {
                    fail_msg("SuiteF1-LeftoverMatchSetup", "Failed to setup loopback match for leftover hand test");
                } else {
                    // Complete Round 1 Custom Screen quickly so we are in live Round 1 COMBAT
                    for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) step_override();
                    sf_app_CpCanvas_sel_cas_mode = 1;
                    rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                    step_override();
                    pvp::PvpCustomCommit r1_rem{};
                    r1_rem.turn_no = 1; r1_rem.tip_no = -1; r1_rem.max_sel = 5; r1_rem.pa_no = -1;
                    for (int i = 0; i < 9; ++i) r1_rem.rock_tip_ids[i] = -1;
                    if (wait_peer_commit_delivered(1, r1_rem)) {
                        for (int b = 0; b < 15; ++b) step_override();
                        // Queue [Cannon, Cannon, Atk+10] and fire the first Cannon!
                        queue_tips_fifo(0, {2, 2, static_cast<int16_t>(tid_atk10)});
                        pump_peer_inputs(1, 0, 40, 0);
                        rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                        for (int k = 0; k < 35; ++k) step_override();

                        if (g_pvp_eng.core.sides[1].hp != 460 ||
                            g_pvp_eng.core.sides[0].tip_no != 1 ||
                            g_pvp_eng.core.sides[0].next_tip != 2) {
                            fail_msg("SuiteF1-LeftoverPreCustom",
                                     "Expected [Cannon + Atk+10] remaining before Round 2 Custom Screen");
                        } else {
                            g_pvp_eng.core.open_custom_req = true;
                            step_override();
                            for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) step_override();

                            // Press OK with 0 chips selected!
                            sf_app_CpCanvas_sel_cas_mode = 1;
                            rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                            step_override();

                            if (sf_app_CpCanvas_cas_cnt != 0 || sf_app_CpCanvas_scene != 1 ||
                                !g_pvp_eng.local_custom_done || g_pvp_eng.phase != Phase::CUSTOM_SCREEN) {
                                fail_msg("SuiteF1-LeftoverEmptyOKRetract",
                                         "Expected immediate retract on 0-chip OK");
                            }
                            pvp::PvpCustomCommit r2_rem = r1_rem;
                            r2_rem.turn_no = 2;
                            if (wait_peer_commit_delivered(2, r2_rem)) {
                                for (int b = 0; b < 15; ++b) step_override();
                                int32_t pow0_keep = rt.iaload(g_pvp_eng.core.sides[0].rock_pow_ref, 0);
                                if (g_pvp_eng.phase != Phase::COMBAT ||
                                    g_pvp_eng.core.sides[0].tip_no != 1 ||
                                    g_pvp_eng.core.sides[0].next_tip != 2 ||
                                    pow0_keep != 10) {
                                    fail_msg("SuiteF1-LeftoverRetained",
                                             "Expected Cannon+Atk10 retained across 0-chip OK -> ROUND_BANNER -> COMBAT (tip_no=" +
                                             std::to_string(g_pvp_eng.core.sides[0].tip_no) + ", next_tip=" +
                                             std::to_string(g_pvp_eng.core.sides[0].next_tip) + ", pow0=" +
                                             std::to_string(pow0_keep) + ")");
                                }
                            } else {
                                fail_msg("SuiteF1-LeftoverPeerCommitR2", "Timed out waiting for peer Round 2 commit");
                            }
                        }
                    } else {
                        fail_msg("SuiteF1-LeftoverPeerCommitR1", "Timed out waiting for peer Round 1 commit");
                    }
                }
            }

            // --- F1.5: SuiteF1-DroppedCommitImmediateChipRecovery ---
            {
                relay.engine.set_legacy_wire_compat_for_test(true);
                if (!setup_live_match(0, 5, 5, peer_b)) {
                    fail_msg("SuiteF1-DroppedCommitImmediateChipRecovery-Setup",
                             "Failed to setup loopback match for dropped commit recovery");
                } else {
                    // Part A: Relay drops first 2 commit deliveries to slot 1 (Host).
                    relay.engine.set_drop_commit_to_slot_for_test(1, 2);

                    pvp::PvpCustomCommit r1_rem{};
                    r1_rem.turn_no = 1;
                    r1_rem.tip_no = 0;
                    r1_rem.max_sel = 5;
                    r1_rem.pa_no = -1;
                    r1_rem.rock_tip_ids[0] = 2;
                    for (int i = 1; i < 9; ++i) r1_rem.rock_tip_ids[i] = -1;

                    peer.send_custom_commit(r1_rem);
                    for (int w = 0; w < 40 && !peer.is_custom_acked(1); ++w) {
                        peer.poll_udp();
                        if (!peer.is_custom_acked(1)) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                    }

                    for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) step_override();
                    sf_app_CpCanvas_sel_cas_mode = 0;
                    sf_app_CpCanvas_sel_cas_tip = 0;
                    rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                    step_override();
                    step_override();
                    sf_app_CpCanvas_sel_cas_mode = 1;
                    rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                    step_override();

                    bool saw_acked_without_rem = false;
                    bool recovered_r1 = (g_pvp_eng.phase == Phase::ROUND_BANNER);
                    for (int w = 0; w < 40 && !recovered_r1; ++w) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        rt.pvp_session().poll_udp();
                        pvp::PvpCustomCommit probe{};
                        if (rt.pvp_session().is_custom_acked(1) &&
                            !rt.pvp_session().has_remote_commit(1, probe)) {
                            saw_acked_without_rem = true;
                        }
                        step_override();
                        if (g_pvp_eng.phase == Phase::ROUND_BANNER) {
                            recovered_r1 = true;
                            break;
                        }
                    }

                    if (!saw_acked_without_rem || !recovered_r1) {
                        fail_msg("SuiteF1-DroppedCommitImmediateChipRecovery-PartA",
                                 "Expected Host to observe ACK without remote commit and recover R1 automatically (saw_acked=" +
                                 std::to_string(saw_acked_without_rem) + ", recovered=" + std::to_string(recovered_r1) + ")");
                    } else {
                        for (int b = g_pvp_eng.banner_ticks; b < 15 && g_pvp_eng.phase == Phase::ROUND_BANNER; ++b) {
                            step_override();
                        }
                        if (g_pvp_eng.phase != Phase::COMBAT) {
                            fail_msg("SuiteF1-DroppedCommitImmediateChipRecovery-PartACombat",
                                     "Expected Phase::COMBAT after 15f ROUND_BANNER");
                        } else {
                            uint32_t r1_start = g_pvp_eng.round_start_turn;
                            pump_peer_inputs(1, r1_start, r1_start + 35, 0);
                            std::this_thread::sleep_for(std::chrono::milliseconds(4));
                            rt.pvp_session().poll_udp();
                            rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                            for (int k = 0; k < 25; ++k) {
                                step_override();
                            }
                            if (g_pvp_eng.phase != Phase::COMBAT ||
                                g_pvp_eng.stall_frames != 0 ||
                                g_pvp_eng.desync_warnings != 0) {
                                fail_msg("SuiteF1-DroppedCommitImmediateChipRecovery-PartAClean",
                                         "Expected clean R1 COMBAT after commit recovery (phase=" +
                                         std::to_string(static_cast<int>(g_pvp_eng.phase)) + ", stall=" +
                                         std::to_string(g_pvp_eng.stall_frames) + ", desync=" +
                                         std::to_string(g_pvp_eng.desync_warnings) + ")");
                            } else {
                                // Part B: Relay drops first 2 commit deliveries to slot 2 (peer) in Round 2.
                                g_pvp_eng.core.open_custom_req = true;
                                step_override();
                                if (g_pvp_eng.phase != Phase::CUSTOM_SCREEN || g_pvp_eng.core.custom_round != 2) {
                                    fail_msg("SuiteF1-DroppedCommitImmediateChipRecovery-PartBR2Enter",
                                             "Expected Round 2 CUSTOM_SCREEN");
                                } else {
                                    relay.engine.set_drop_commit_to_slot_for_test(2, 2);

                                    pvp::PvpCustomCommit r2_rem = r1_rem;
                                    r2_rem.turn_no = 2;
                                    peer.send_custom_commit(r2_rem);
                                    for (int w = 0; w < 40 && !peer.is_custom_acked(2); ++w) {
                                        peer.poll_udp();
                                        if (!peer.is_custom_acked(2)) {
                                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                        }
                                    }

                                    for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) step_override();
                                    sf_app_CpCanvas_sel_cas_mode = 0;
                                    sf_app_CpCanvas_sel_cas_tip = 0;
                                    rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                                    step_override();
                                    step_override();
                                    sf_app_CpCanvas_sel_cas_mode = 1;
                                    rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                                    step_override();

                                    for (int w = 0; w < 30 && g_pvp_eng.phase == Phase::CUSTOM_SCREEN; ++w) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                        step_override();
                                    }
                                    for (int b = g_pvp_eng.banner_ticks; b < 15 && g_pvp_eng.phase == Phase::ROUND_BANNER; ++b) {
                                        step_override();
                                    }
                                    if (g_pvp_eng.phase != Phase::COMBAT) {
                                        fail_msg("SuiteF1-DroppedCommitImmediateChipRecovery-PartBR2Combat",
                                                 "Expected Round 2 COMBAT (phase=" +
                                                 std::to_string(static_cast<int>(g_pvp_eng.phase)) + ")");
                                    } else {
                                        uint32_t r2_start = g_pvp_eng.round_start_turn;
                                        rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                                        step_override();

                                        bool peer_got_r2 = false;
                                        for (int w = 0; w < 60; ++w) {
                                            peer.poll_udp();
                                            pvp::PvpCustomCommit peer_probe{};
                                            if (peer.has_remote_commit(2, peer_probe)) {
                                                peer_got_r2 = true;
                                                break;
                                            }
                                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                            step_override();
                                        }
                                        if (!peer_got_r2) {
                                            fail_msg("SuiteF1-DroppedCommitImmediateChipRecovery-PartB",
                                                     "Expected peer to receive Host R2 commit via ROUND_BANNER/COMBAT re-delivery");
                                        } else {
                                            uint32_t cur_r2_t = g_pvp_eng.core.combat_turn;
                                            pump_peer_inputs(2, r2_start, cur_r2_t + 40, 0);
                                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                            rt.pvp_session().poll_udp();
                                            for (int k = 0; k < 25; ++k) {
                                                step_override();
                                            }
                                            if (g_pvp_eng.stall_frames != 0 || g_pvp_eng.desync_warnings != 0) {
                                                fail_msg("SuiteF1-DroppedCommitImmediateChipRecovery-PartBClean",
                                                         "Expected zero stall & zero desync after R2 peer recovery (stall=" +
                                                         std::to_string(g_pvp_eng.stall_frames) + ", desync=" +
                                                         std::to_string(g_pvp_eng.desync_warnings) + ")");
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                relay.engine.set_drop_commit_to_slot_for_test(0, 0);
                relay.engine.set_legacy_wire_compat_for_test(false);
            }

            // --- F1.6: SuiteF1-ImmediateChipRollbackDeterminism ---
            {
                int16_t freeze_tid = static_cast<int16_t>(tid_multihit_stop >= 0 ? tid_multihit_stop : 2);
                auto setup_imm_rb = [&]() {
                    reset_test_arena(500, 500);
                    queue_tips_fifo(0, {freeze_tid});
                    queue_tips_fifo(1, {2});
                    for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
                };

                setup_imm_rb();
                for (int k = 0; k < 25; ++k) {
                    int32_t m0 = (k == 0) ? (1 << 20) : 0;
                    int32_t m1 = (k == 0) ? ((1 << 17) | (1 << 20)) : 0;
                    step_pvp_combat_tick(rt, cp, m0, m1);
                }
                uint16_t canon_hash = compute_combat_state_hash(rt);
                int32_t canon_hp0 = g_pvp_eng.core.sides[0].hp;
                int32_t canon_hp1 = g_pvp_eng.core.sides[1].hp;

                setup_imm_rb();
                for (int k = 0; k < 12; ++k) {
                    int32_t m0 = (k == 0) ? (1 << 20) : 0;
                    int32_t m1 = (k == 0) ? (1 << 10) : 0;
                    save_rollback_slot(rt, static_cast<uint32_t>(k), m0, m1, true);
                    step_pvp_combat_tick(rt, cp, m0, m1);
                }
                if (!restore_rollback_slot(rt, 0)) {
                    fail_msg("SuiteF1-ImmediateChipRollbackRestore", "Failed to restore tick 0 rollback slot");
                } else {
                    for (int k = 0; k < 25; ++k) {
                        int32_t m0 = (k == 0) ? (1 << 20) : 0;
                        int32_t m1 = (k == 0) ? ((1 << 17) | (1 << 20)) : 0;
                        save_rollback_slot(rt, static_cast<uint32_t>(k), m0, m1, false);
                        step_pvp_combat_tick(rt, cp, m0, m1);
                    }
                    if (compute_combat_state_hash(rt) != canon_hash ||
                        g_pvp_eng.core.sides[0].hp != canon_hp0 ||
                        g_pvp_eng.core.sides[1].hp != canon_hp1) {
                        fail_msg("SuiteF1-ImmediateChipRollbackDeterminism",
                                 "Immediate chip rollback diverged from canonical state (hp0=" +
                                 std::to_string(g_pvp_eng.core.sides[0].hp) + "/" + std::to_string(canon_hp0) +
                                 ", hp1=" + std::to_string(g_pvp_eng.core.sides[1].hp) + "/" + std::to_string(canon_hp1) + ")");
                    }
                }
            }

            peer.close_udp();
            cleanup_and_restore_singleplayer(rt, true);
        std::printf("[PvP SELF-TEST] Suite F1 complete: Immediate Custom Retract, Waiting Banner & 15f Synced BATTLE START verified (failures=%d)\n",
                    failures);

    // =========================================================================
    // Suite F2: Turn X / FINAL TURN Banners, Turn 15 10s Countdown & Hit Damage Judge
    // =========================================================================
    {
        // Locate a Recov chip dynamically (works identically on JP and EN ROMs)
        int tid_recov = -1;
        for (int tid = 2; tid < tip_len && tid_recov < 0; ++tid) {
            Ref t_obj = rt.iaload(sf_app_CpCanvas_tip, tid);
            if (t_obj <= 0) continue;
            int32_t t_type = rt.field_i32(t_obj, 1);
            if (t_type != 2 && t_type != 3) continue;
            reset_test_arena(400, 400);
            load_side(rt, 0);
            sf_app_Rock_hp = 200;
            save_side(rt, 0);
            queue_tips_fifo(0, {static_cast<int16_t>(tid)});
            for (int k = 0; k < 12; ++k) {
                step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
            }
            if (g_pvp_eng.core.sides[0].hp > 200) {
                tid_recov = tid;
            }
        }
        if (tid_recov < 0) {
            fail_msg("SuiteF2-FindRecov", "Failed to locate a working Recov chip in Tip table");
        }

        // --- F2.1: Turn 1 / Turn 2..14 / Turn 15 ROUND_BANNER & Turn 15 Gauge Countdown Visuals ---
        {
            reset_test_arena(500, 500, 0, 0, 1);
            rt.set_suppress_av(false);
            const uint32_t* fb = rt.framebuffer();

            uint16_t h0 = compute_combat_state_hash(rt);
            render_pvp_combat_frame(rt, cp);
            std::vector<uint32_t> base_fb(fb, fb + 240 * 240);
            render_round_banner_overlay(rt, 5, 1);
            if (compute_combat_state_hash(rt) != h0) {
                fail_msg("SuiteF2-RenderReadOnlyT1", "Turn 1 banner mutated combat state hash");
            }
            int sprite_diff_t1 = 0;
            for (int y = 112; y < 128; ++y) {
                for (int x = 52; x < 188; ++x) {
                    if (fb[y * 240 + x] != base_fb[y * 240 + x]) ++sprite_diff_t1;
                }
            }
            if (fb[82 * 240 + 52] != 0xFFFFD84Cu || sprite_diff_t1 == 0) {
                fail_msg("SuiteF2-Turn1BannerVisual",
                         "Expected TURN 1 START gold badge & shifted BATTLE START sprite");
            }

            render_pvp_combat_frame(rt, cp);
            render_round_banner_overlay(rt, 5, 7);
            if (fb[98 * 240 + 34] != 0xFF58B8FFu) {
                fail_msg("SuiteF2-Turn7BannerVisual", "Expected TURN 7 START blue border banner");
            }

            render_pvp_combat_frame(rt, cp);
            render_round_banner_overlay(rt, 5, 15);
            if (fb[92 * 240 + 28] != 0xFFFF5868u) {
                fail_msg("SuiteF2-Turn15BannerVisual", "Expected FINAL TURN! crimson border banner");
            }

            g_pvp_eng.core.custom_round = 15;
            g_pvp_eng.core.final_turn_ticks_left = kFinalTurnDurationTicks;
            sf_app_CpCanvas_cus_gage = 128;
            render_pvp_combat_frame(rt, cp);
            if (fb[37 * 240 + 74] != 0xFFFFC84Cu) {
                fail_msg("SuiteF2-Turn15CountdownGold", "Expected Turn 15 gold countdown border at 10.0s");
            }
            g_pvp_eng.core.final_turn_ticks_left = 48;
            sf_app_CpCanvas_cus_gage = (48 * 128) / kFinalTurnDurationTicks;
            render_pvp_combat_frame(rt, cp);
            if (fb[37 * 240 + 74] != 0xFFFF4858u) {
                fail_msg("SuiteF2-Turn15CountdownRed", "Expected Turn 15 red countdown border at <=3.0s");
            }
        }

        // --- F2.2: Turn 15 Rules (16th Custom blocked, proportional drain ignores FastGauge, Time-Freeze pause, exact tick 160 Judge) ---
        {
            reset_test_arena(500, 500, 0, 0, 1);
            g_pvp_eng.core.custom_round = 15;
            g_pvp_eng.core.final_turn_ticks_left = kFinalTurnDurationTicks;
            g_pvp_eng.core.shared_cus_sp = 4; // FastGauge max speed must be ignored
            sf_app_CpCanvas_cus_gage = 128;
            queue_tips_fifo(0, {});
            queue_tips_fifo(1, {});

            if ((compute_auto_bot_combat_input(rt) & (1 << 21)) != 0) {
                fail_msg("SuiteF2-BotNoCustomOnT15", "Auto-bot must not press SOFT1 on Turn 15");
            }

            for (int k = 0; k < 10; ++k) {
                int32_t m = ((k & 1) == 0) ? ((1 << 21) | (1 << 22)) : 0; // Hammer SOFT1 | SOFT2
                step_pvp_combat_tick(rt, cp, m, m);
                if (g_pvp_eng.core.open_custom_req) {
                    fail_msg("SuiteF2-Block16thCustom", "Turn 15 allowed opening 16th Custom Screen");
                    break;
                }
            }
            int32_t exp_gage_150 = (150 * 128) / kFinalTurnDurationTicks;
            if (g_pvp_eng.core.final_turn_ticks_left != 150 || sf_app_CpCanvas_cus_gage != exp_gage_150 ||
                rt.iaload(sf_app_CpCanvas_soft_id, 0) != 0) {
                fail_msg("SuiteF2-Turn15GaugeDrain",
                         "Expected 150 ticks left & proportional gauge (ticks=" +
                         std::to_string(g_pvp_eng.core.final_turn_ticks_left) + ", gage=" +
                         std::to_string(sf_app_CpCanvas_cus_gage) + ")");
            }

            // Pause during Time-Freeze (stop_cnt > 0)
            if (tid_multihit_stop >= 0) {
                queue_tips_fifo(0, {static_cast<int16_t>(tid_multihit_stop)});
                step_pvp_combat_tick(rt, cp, (1 << 20), 0); // Triggers Time-Freeze (stop_cnt = 1)
                uint16_t pre_tf_ticks = g_pvp_eng.core.final_turn_ticks_left;
                int32_t pre_tf_gage = sf_app_CpCanvas_cus_gage;
                int tf_frames = 0;
                for (int k = 0; k < 150 && sf_app_CpCanvas_stop_cnt > 0; ++k) {
                    ++tf_frames;
                    step_pvp_combat_tick(rt, cp, 0, 0);
                    if (sf_app_CpCanvas_stop_cnt > 0 &&
                        (g_pvp_eng.core.final_turn_ticks_left != pre_tf_ticks ||
                         sf_app_CpCanvas_cus_gage != pre_tf_gage)) {
                        fail_msg("SuiteF2-Turn15TimeFreezePause",
                                 "Turn 15 countdown must pause while stop_cnt > 0");
                        break;
                    }
                }
                if (tf_frames == 0) {
                    fail_msg("SuiteF2-Turn15TimeFreezeSetup", "Time-Freeze chip did not enter stop_cnt > 0");
                }
            }

            // Step down to exact tick 160 boundary
            reset_test_arena(500, 500, 0, 0, 1);
            g_pvp_eng.core.custom_round = 15;
            g_pvp_eng.core.final_turn_ticks_left = kFinalTurnDurationTicks;
            g_pvp_eng.core.shared_cus_sp = 4;
            sf_app_CpCanvas_cus_gage = 128;
            for (int k = 1; k <= 159; ++k) {
                step_pvp_combat_tick(rt, cp, 0, 0);
                if (g_pvp_eng.core.ended_by_judge || g_pvp_eng.core.winner != -2) {
                    fail_msg("SuiteF2-Turn15PrematureJudge",
                             "Turn 15 ended prematurely at tick " + std::to_string(k));
                    break;
                }
            }
            step_pvp_combat_tick(rt, cp, 0, 0); // Exact 160th tick
            if (!g_pvp_eng.core.ended_by_judge || g_pvp_eng.core.winner != -1 ||
                g_pvp_eng.core.final_turn_ticks_left != 0 || sf_app_CpCanvas_cus_gage != 0) {
                fail_msg("SuiteF2-Turn15Exact160Judge",
                         "Expected ended_by_judge=true, winner=-1 on exact 160th tick");
            }
        }

        // --- F2.3: Hit Damage Judge Exact Accounting ---
        {
            // Direct attack + overkill clamp (60 HP hit by two 40-dmg Cannons = 60, not 80!)
            reset_test_arena(500, 60, 0, 0, 1);
            queue_tips_fifo(0, {2, 2});
            for (int k = 0; k < 45; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
            if (g_pvp_eng.core.damage_dealt[0] != 40 || g_pvp_eng.core.sides[1].hp != 20) {
                fail_msg("SuiteF2-DirectHitDmg",
                         "Expected 40 damage_dealt after Cannon #1, got " + std::to_string(g_pvp_eng.core.damage_dealt[0]));
            }
            for (int k = 0; k < 25; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
            if (g_pvp_eng.core.damage_dealt[0] != 60 || g_pvp_eng.core.sides[1].hp != 0) {
                fail_msg("SuiteF2-OverkillClamp",
                         "Expected overkill clamped to 60 damage_dealt, got " + std::to_string(g_pvp_eng.core.damage_dealt[0]));
            }

            // Barrier absorbs -> 0, Recov heals -> does NOT subtract opponent's damage_dealt
            reset_test_arena(500, 500, 0, 0, 1);
            if (tid_bari >= 0) {
                queue_tips_fifo(0, {2, 2});
                queue_tips_fifo(1, {static_cast<int16_t>(tid_bari)});
                for (int k = 0; k < 8; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                for (int k = 0; k < 20; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
                if (g_pvp_eng.core.damage_dealt[0] != 0) {
                    fail_msg("SuiteF2-BarrierZeroDmg",
                             "Expected 0 damage_dealt against Barrier, got " + std::to_string(g_pvp_eng.core.damage_dealt[0]));
                }
            }
            if (tid_recov >= 0) {
                reset_test_arena(500, 500, 0, 0, 1);
                queue_tips_fifo(0, {2});
                queue_tips_fifo(1, {static_cast<int16_t>(tid_recov)});
                for (int k = 0; k < 45; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
                int32_t pre_heal_d0 = g_pvp_eng.core.damage_dealt[0];
                int32_t pre_heal_hp1 = g_pvp_eng.core.sides[1].hp;
                for (int k = 0; k < 20; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                if (g_pvp_eng.core.sides[1].hp <= pre_heal_hp1 || g_pvp_eng.core.damage_dealt[0] != pre_heal_d0) {
                    fail_msg("SuiteF2-RecovNoSubtract",
                             "Recov must heal without subtracting attacker damage_dealt (d0=" +
                             std::to_string(g_pvp_eng.core.damage_dealt[0]) + ", want " + std::to_string(pre_heal_d0) + ")");
                }
            }

            // Shield Reflect credits defender
            if (tid_guard >= 0) {
                int cannon_hit_k = measure_hit_tick(2);
                int guard_start_k = measure_han_start(static_cast<int16_t>(tid_guard));
                int g_press = std::max(0, cannon_hit_k - guard_start_k - 2);
                int a_press = std::max(0, guard_start_k + 2 - cannon_hit_k);
                reset_test_arena(500, 500, 0, 0, 1);
                queue_tips_fifo(0, {2});
                queue_tips_fifo(1, {static_cast<int16_t>(tid_guard)});
                for (int k = 0; k < 50; ++k) {
                    int32_t m0 = (k == a_press) ? (1 << 20) : 0;
                    int32_t m1 = (k == g_press) ? (1 << 20) : 0;
                    step_pvp_combat_tick(rt, cp, m0, m1);
                }
                if (g_pvp_eng.core.damage_dealt[1] <= 0 ||
                    g_pvp_eng.core.damage_dealt[1] != (500 - g_pvp_eng.core.sides[0].hp) ||
                    g_pvp_eng.core.damage_dealt[0] != 0) {
                    fail_msg("SuiteF2-ShieldReflectDmg",
                             "Expected Shield reflect damage credited to P1 (d1=" +
                             std::to_string(g_pvp_eng.core.damage_dealt[1]) + ")");
                }
            }

            // AntiDmg (Kawarimi) credits defender
            if (tid_antidmg >= 0) {
                reset_test_arena(500, 500, 0, 0, 1);
                queue_tips_fifo(0, {2});
                queue_tips_fifo(1, {static_cast<int16_t>(tid_antidmg)});
                for (int k = 0; k < 60; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                for (int k = 0; k < 55; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
                if (g_pvp_eng.core.damage_dealt[1] <= 0 ||
                    g_pvp_eng.core.damage_dealt[1] != (500 - g_pvp_eng.core.sides[0].hp) ||
                    g_pvp_eng.core.damage_dealt[0] != 0) {
                    fail_msg("SuiteF2-KawarimiDmg",
                             "Expected Kawarimi counterattack credited to P1 (d1=" +
                             std::to_string(g_pvp_eng.core.damage_dealt[1]) + ")");
                }
            }

            // Mine (oki syu == 0) credits Mine owner
            if (tid_mine >= 0) {
                reset_test_arena(500, 500, 0, 0, 1);
                queue_tips_fifo(1, {static_cast<int16_t>(tid_mine)});
                for (int k = 0; k < 35; ++k) step_pvp_combat_tick(rt, cp, 0, (k == 0) ? (1 << 20) : 0);
                int mine_x = -1, mine_y = -1;
                for (int i = 0; i < 6; ++i) {
                    Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                    if (ok > 0 && rt.field_i32(ok, 1) != 0 && rt.field_i32(ok, 2) == 0) {
                        mine_x = rt.field_i32(ok, 9);
                        mine_y = rt.field_i32(ok, 10);
                        break;
                    }
                }
                if (mine_x >= 0 && mine_x <= 2 && mine_y >= 0 && mine_y <= 2) {
                    for (int step = 0; step < 30 && g_pvp_eng.core.sides[0].hp == 500; ++step) {
                        int32_t m0 = 0;
                        if (g_pvp_eng.core.sides[0].move_flg == 0 && (step & 1) == 0) {
                            if (g_pvp_eng.core.sides[0].pos_x < mine_x) m0 = (1 << 18);
                            else if (g_pvp_eng.core.sides[0].pos_x > mine_x) m0 = (1 << 16);
                            else if (g_pvp_eng.core.sides[0].pos_y < mine_y) m0 = (1 << 19);
                            else if (g_pvp_eng.core.sides[0].pos_y > mine_y) m0 = (1 << 17);
                        }
                        step_pvp_combat_tick(rt, cp, m0, 0);
                    }
                    int32_t p0_lost = 500 - g_pvp_eng.core.sides[0].hp;
                    if (p0_lost <= 0 || g_pvp_eng.core.damage_dealt[1] != p0_lost) {
                        fail_msg("SuiteF2-MineOwnerCredit",
                                 "Expected Mine damage credited to P1 (p0_lost=" + std::to_string(p0_lost) +
                                 ", d1=" + std::to_string(g_pvp_eng.core.damage_dealt[1]) + ")");
                    }
                }
            }

            // Poison (pane_id == 6) and Lava (pane_id == 3) terrain damage must add 0
            {
                reset_test_arena(500, 500, 0, 0, 1);
                ensure_board_orient(rt, 0);
                Ref pan_poison = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 1), 1);
                fn_app_BtPanel_Henka___II__V(rt, pan_poison, 6, 0);
                for (int k = 0; k < 48; ++k) step_pvp_combat_tick(rt, cp, 0, 0);
                if (g_pvp_eng.core.sides[0].hp >= 500 ||
                    g_pvp_eng.core.damage_dealt[0] != 0 || g_pvp_eng.core.damage_dealt[1] != 0) {
                    fail_msg("SuiteF2-PoisonZeroDmg",
                             "Poison terrain ticks must NOT add to damage_dealt (hp0=" +
                             std::to_string(g_pvp_eng.core.sides[0].hp) + ", d1=" +
                             std::to_string(g_pvp_eng.core.damage_dealt[1]) + ")");
                }

                reset_test_arena(500, 500, 0, 0, 1);
                ensure_board_orient(rt, 0);
                Ref pan_lava = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 1), 2);
                fn_app_BtPanel_Henka___II__V(rt, pan_lava, 3, 0);
                for (int k = 0; k < 20; ++k) step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 18) : 0, 0);
                if (g_pvp_eng.core.sides[0].hp >= 500 ||
                    g_pvp_eng.core.damage_dealt[0] != 0 || g_pvp_eng.core.damage_dealt[1] != 0) {
                    fail_msg("SuiteF2-LavaZeroDmg",
                             "Lava panel step must NOT add to damage_dealt (hp0=" +
                             std::to_string(g_pvp_eng.core.sides[0].hp) + ", d1=" +
                             std::to_string(g_pvp_eng.core.damage_dealt[1]) + ")");
                }
            }

            // Tie-breaker checks: d0 > d1, d1 > d0, and d0 == d1 -> -1 even when remaining HP differs
            {
                auto run_judge_tiebreak = [&](int32_t d0, int32_t d1, int32_t hp0, int32_t hp1, int want_win, const char* tag) {
                    reset_test_arena(500, 500, 0, 0, 1);
                    load_side(rt, 0); sf_app_Rock_hp = hp0; save_side(rt, 0);
                    load_side(rt, 1); sf_app_Rock_hp = hp1; save_side(rt, 1);
                    g_pvp_eng.core.custom_round = 15;
                    g_pvp_eng.core.final_turn_ticks_left = 1;
                    g_pvp_eng.core.damage_dealt[0] = d0;
                    g_pvp_eng.core.damage_dealt[1] = d1;
                    step_pvp_combat_tick(rt, cp, 0, 0);
                    if (!g_pvp_eng.core.ended_by_judge || g_pvp_eng.core.winner != want_win) {
                        fail_msg(tag, "Expected ended_by_judge=true, winner=" + std::to_string(want_win) +
                                      ", got " + std::to_string(g_pvp_eng.core.winner));
                    }
                };
                run_judge_tiebreak(120, 80, 200, 420, 0, "SuiteF2-JudgeWinP0");
                run_judge_tiebreak(80, 120, 420, 200, 1, "SuiteF2-JudgeWinP1");
                run_judge_tiebreak(100, 100, 320, 480, -1, "SuiteF2-JudgeTieDrawEvenDiffHP");
            }
        }

        // --- F2.4: GGPO Rollback Across Turn 15 Judge Boundary + Loopback Phase::JUDGE (48f) ---
        {
            auto setup_t15_rb = [&]() {
                reset_test_arena(500, 500, 0, 0, 1);
                g_pvp_eng.core.custom_round = 15;
                g_pvp_eng.core.final_turn_ticks_left = 8; // tick 152 -> 8 ticks to Judge boundary
                sf_app_CpCanvas_cus_gage = (8 * 128) / kFinalTurnDurationTicks;
                queue_tips_fifo(0, {2}); // Cannon (40 dmg, hits in <= 6 ticks)
                queue_tips_fifo(1, {});
                for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
            };

            // Canonical non-rollback run with Cannon fired on tick 152 (k == 0)
            setup_t15_rb();
            for (int k = 0; k < 8; ++k) {
                step_pvp_combat_tick(rt, cp, (k == 0) ? (1 << 20) : 0, 0);
            }
            int32_t canon_d0 = g_pvp_eng.core.damage_dealt[0];
            int32_t canon_d1 = g_pvp_eng.core.damage_dealt[1];
            int canon_win = g_pvp_eng.core.winner;
            bool canon_judge = g_pvp_eng.core.ended_by_judge;
            uint16_t canon_hash = compute_combat_state_hash(rt);

            // Rollback run: step 8 ticks speculatively with wrong input (p0_mask=0) crossing Judge expiry,
            // then roll back to turn 0 (tick 152) with corrected input (1<<20 on k==0) and re-step!
            setup_t15_rb();
            for (int k = 0; k < 8; ++k) {
                save_rollback_slot(rt, static_cast<uint32_t>(k), 0, 0, true);
                step_pvp_combat_tick(rt, cp, 0, 0);
            }
            if (!g_pvp_eng.core.ended_by_judge || g_pvp_eng.core.winner != -1 || g_pvp_eng.core.damage_dealt[0] != 0) {
                fail_msg("SuiteF2-RollbackPreJudge", "Expected speculative run to cross Judge boundary as Draw");
            }
            if (!restore_rollback_slot(rt, 0)) {
                fail_msg("SuiteF2-RollbackRestore", "Failed to restore tick-152 rollback slot");
            } else {
                for (int k = 0; k < 8; ++k) {
                    int32_t m0 = (k == 0) ? (1 << 20) : 0;
                    save_rollback_slot(rt, static_cast<uint32_t>(k), m0, 0, false);
                    step_pvp_combat_tick(rt, cp, m0, 0);
                }
                if (canon_d0 != 40 || !canon_judge || canon_win != 0 ||
                    g_pvp_eng.core.damage_dealt[0] != canon_d0 ||
                    g_pvp_eng.core.damage_dealt[1] != canon_d1 ||
                    g_pvp_eng.core.winner != canon_win ||
                    g_pvp_eng.core.ended_by_judge != canon_judge ||
                    compute_combat_state_hash(rt) != canon_hash) {
                    fail_msg("SuiteF2-RollbackAcrossJudgeMismatch",
                             "Rollback across Turn 15 Judge boundary diverged from canonical state (d0=" +
                             std::to_string(g_pvp_eng.core.damage_dealt[0]) + "/" + std::to_string(canon_d0) +
                             ", win=" + std::to_string(g_pvp_eng.core.winner) + ")");
                }
            }

            // Live loopback test verifying Phase::JUDGE 48-tick presentation -> Phase::RESULT
            if (!setup_live_match(0, 5, 5, peer_b)) {
                fail_msg("SuiteF2-LoopbackMatchSetup", "Failed to enter live CUSTOM_SCREEN");
            } else {
                for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) step_override();
                sf_app_CpCanvas_sel_cas_mode = 1;
                rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                step_override();
                pvp::PvpCustomCommit r1_rem{};
                r1_rem.turn_no = 1; r1_rem.tip_no = -1; r1_rem.max_sel = 5; r1_rem.pa_no = -1;
                for (int i = 0; i < 9; ++i) r1_rem.rock_tip_ids[i] = -1;
                if (!wait_peer_commit_delivered(1, r1_rem)) {
                    fail_msg("SuiteF2-LoopbackR1Commit", "Timed out waiting for Round 1 commit");
                } else {
                    for (int b = 0; b < 15; ++b) step_override();
                    // Enter Turn 15 CUSTOM_SCREEN and verify FINAL TURN! card + waiting badge
                    g_pvp_eng.core.custom_round = 14;
                    g_pvp_eng.core.open_custom_req = true;
                    step_override();
                    step_override();
                    for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) step_override();
                    if (g_pvp_eng.phase != Phase::CUSTOM_SCREEN || g_pvp_eng.core.custom_round != 15 ||
                        (fb[4 * 240 + 4] != 0xFFFFD84Cu && fb[4 * 240 + 4] != 0xFFFF4858u)) {
                        fail_msg("SuiteF2-Turn15CustomCard",
                                 "Expected Turn 15 CUSTOM_SCREEN with FINAL TURN! card (phase=" +
                                 std::to_string(static_cast<int>(g_pvp_eng.phase)) + ", round=" +
                                 std::to_string(g_pvp_eng.core.custom_round) + ")");
                    }
                    sf_app_CpCanvas_sel_cas_mode = 1;
                    rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                    step_override();
                    if (fb[78 * 240 + 44] != 0xFFFF5868u || fb[102 * 240 + 28] != 0xFF68A8FFu) {
                        fail_msg("SuiteF2-Turn15WaitingBadge",
                                 "Expected FINAL TURN badge above waiting banner in Turn 15 CUSTOM_SCREEN");
                    }
                    pvp::PvpCustomCommit r15_rem{};
                    r15_rem.turn_no = 15; r15_rem.tip_no = -1; r15_rem.max_sel = 5; r15_rem.pa_no = -1;
                    for (int i = 0; i < 9; ++i) r15_rem.rock_tip_ids[i] = -1;
                    if (!wait_peer_commit_delivered(15, r15_rem)) {
                        fail_msg("SuiteF2-LoopbackR15Commit", "Timed out waiting for Round 15 commit");
                    } else {
                        for (int b = 0; b < 15; ++b) step_override();
                        if (g_pvp_eng.phase != Phase::COMBAT ||
                            g_pvp_eng.core.final_turn_ticks_left != kFinalTurnDurationTicks) {
                            fail_msg("SuiteF2-Turn15CombatStart",
                                     "Expected Turn 15 COMBAT with final_turn_ticks_left==" +
                                     std::to_string(kFinalTurnDurationTicks) + ", got phase=" +
                                     std::to_string(static_cast<int>(g_pvp_eng.phase)) + ", ticks=" +
                                     std::to_string(g_pvp_eng.core.final_turn_ticks_left));
                        }
                        // Jump to Turn 15 with 1 tick left and confirmed P0 damage lead
                        g_pvp_eng.core.custom_round = 15;
                        g_pvp_eng.core.final_turn_ticks_left = 1;
                        g_pvp_eng.core.damage_dealt[0] = 90;
                        g_pvp_eng.core.damage_dealt[1] = 30;
                        uint32_t t0 = g_pvp_eng.core.combat_turn;
                        for (int q = 0; q < 40; ++q) {
                            pump_peer_inputs(15, t0, t0 + 4, 0);
                            int32_t dummy = 0;
                            if (rt.pvp_session().get_remote_turn_input(15, t0, dummy)) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        }
                        step_override(); // Steps tick t0: expires Turn 15 -> flush_round_tail -> Phase::JUDGE (judge_ticks == 0)
                        if (g_pvp_eng.phase != Phase::JUDGE || g_pvp_eng.judge_ticks != 0) {
                            fail_msg("SuiteF2-EnterPhaseJudge",
                                     "Expected Phase::JUDGE (judge_ticks==0) after Turn 15 expiry (phase=" +
                                     std::to_string(static_cast<int>(g_pvp_eng.phase)) + ", jt=" +
                                     std::to_string(g_pvp_eng.judge_ticks) + ")");
                        } else {
                            for (int jt = 0; jt < 48; ++jt) {
                                step_override();
                                if (jt < 47) {
                                    if (g_pvp_eng.phase != Phase::JUDGE || g_pvp_eng.judge_ticks != jt + 1 ||
                                        fb[68 * 240 + 16] != 0xFFFFD84Cu) {
                                        fail_msg("SuiteF2-JudgePresentation48f",
                                                 "Phase::JUDGE failed during 48-tick presentation at jt=" + std::to_string(jt));
                                        break;
                                    }
                                } else {
                                    if (g_pvp_eng.phase != Phase::RESULT || g_pvp_eng.core.winner != 0) {
                                        fail_msg("SuiteF2-JudgeToResult",
                                                 "Expected Phase::RESULT with winner=0 after 48 JUDGE ticks (phase=" +
                                                 std::to_string(static_cast<int>(g_pvp_eng.phase)) + ")");
                                    }
                                }
                            }
                        }
                    }
                }
                peer.close_udp();
                cleanup_and_restore_singleplayer(rt, true);
            }
        }
    }
    std::printf("[PvP SELF-TEST] Suite F2 complete: Turn Banners, Turn 15 10s Countdown & Hit Damage Judge verified (failures=%d)\n",
                failures);

    // =========================================================================
    // Suite F3: Stage 3B — Triple Battle (BO3) Engine State Machine, Full Reset & Series Outcomes
    // =========================================================================
    {
        struct PreferredFormatGuard {
            pvp::PvpClientSession& sess;
            uint8_t prev;
            explicit PreferredFormatGuard(pvp::PvpClientSession& s)
                : sess(s), prev(s.preferred_format()) {
                sess.set_preferred_format(pvp::PVP_FORMAT_TRIPLE);
            }
            ~PreferredFormatGuard() {
                sess.set_preferred_format(prev);
            }
        } fmt_guard(rt.pvp_session());

        auto f3_check = [&](bool cond, const char* tag, const std::string& msg) {
            if (!cond) {
                fail_msg(tag, msg);
            }
        };

        auto confirm_round_into_combat = [&](uint16_t wire_rnd) -> bool {
            for (int w = 0; w < 10 && sf_app_CpCanvas_cas_cnt < 0; ++w) {
                step_override();
            }
            sf_app_CpCanvas_sel_cas_mode = 1;
            rt.inject_key_at_frame(rt.frame_count() + 1, 20);
            step_override();

            pvp::PvpCustomCommit rem_c{};
            rem_c.turn_no = wire_rnd;
            rem_c.tip_no = -1;
            rem_c.max_sel = 5;
            rem_c.pa_no = -1;
            for (int i = 0; i < 9; ++i) {
                rem_c.rock_tip_ids[i] = -1;
            }
            if (!wait_peer_commit_delivered(wire_rnd, rem_c)) {
                return false;
            }
            // Advance CUSTOM_SCREEN -> ROUND_BANNER -> 15 banner frames -> COMBAT
            for (int step = 0; step < 30 && g_pvp_eng.phase != Phase::COMBAT; ++step) {
                peer.poll_udp();
                step_override();
            }
            return g_pvp_eng.phase == Phase::COMBAT;
        };

        auto finish_game_ko = [&](int winner_idx) -> bool {
            int loser_idx = 1 - winner_idx;
            load_side(rt, loser_idx);
            sf_app_Rock_hp = 0;
            save_side(rt, loser_idx);
            uint16_t wire_rnd = current_wire_round_key();
            uint32_t t0 = g_pvp_eng.core.combat_turn;
            for (int q = 0; q < 40; ++q) {
                pump_peer_inputs(wire_rnd, t0, t0 + 8, 0);
                int32_t dummy = 0;
                if (rt.pvp_session().get_remote_turn_input(wire_rnd, t0, dummy)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            for (int step = 0; step < 12; ++step) {
                step_override();
                peer.poll_udp();
                if (g_pvp_eng.phase == Phase::ROUND_RESULT || g_pvp_eng.phase == Phase::RESULT) {
                    return true;
                }
            }
            return false;
        };

        auto finish_game_turn15_draw = [&]() -> bool {
            uint16_t wire_rnd = current_wire_round_key();
            if (g_pvp_eng.phase == Phase::CUSTOM_SCREEN && !confirm_round_into_combat(wire_rnd)) {
                return false;
            }
            // Jump cleanly to Turn 15 with 1 tick remaining and tied damage_dealt
            g_pvp_eng.core.custom_round = 15;
            g_pvp_eng.core.final_turn_ticks_left = 1;
            g_pvp_eng.core.damage_dealt[0] = 200;
            g_pvp_eng.core.damage_dealt[1] = 200;
            uint16_t cur_wire = current_wire_round_key();
            uint32_t t0 = g_pvp_eng.core.combat_turn;
            for (int q = 0; q < 40; ++q) {
                pump_peer_inputs(cur_wire, t0, t0 + 8, 0);
                int32_t dummy = 0;
                if (rt.pvp_session().get_remote_turn_input(cur_wire, t0, dummy)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            step_override(); // Expires Turn 15 -> flush_round_tail -> Phase::JUDGE
            if (g_pvp_eng.phase != Phase::JUDGE) {
                return false;
            }
            for (int t = 0; t < 48; ++t) {
                step_override();
                peer.poll_udp();
            }
            return g_pvp_eng.phase == Phase::ROUND_RESULT || g_pvp_eng.phase == Phase::RESULT;
        };

        // 1) Cross-Game Full State Reset + Wire Key Isolation + PKT_MATCH_END guard
        if (!setup_live_match(1, 5, 5, peer_b)) {
            fail_msg("SuiteF3-1-Setup", "setup_live_match failed");
        } else {
            // Set Regular preset chip to slot 0 (reg_index = 0) for Game 2 reset verification
            g_pvp_eng.builds[0].regu_flg = 1;
            g_pvp_eng.builds[0].regu_id = 0;

            f3_check(g_pvp_eng.is_triple_battle == true, "F3.1a", "BO3 format negotiated (is_triple_battle == true)");
            f3_check(g_pvp_eng.bo3_game_index == 0, "F3.1b", "Game 1 starts at bo3_game_index == 0");
            f3_check(current_wire_round_key() == 1, "F3.1c", "Game 1 Round 1 wire key == 1");
            f3_check(confirm_round_into_combat(1), "F3.1d", "Game 1 Round 1 enters COMBAT");

            int32_t orig_jou = 1;
            if (sf_app_CpCanvas_panel > 0) {
                Ref row0 = rt.iaload(sf_app_CpCanvas_panel, 0);
                if (row0 > 0) {
                    Ref p00 = rt.iaload(row0, 0);
                    if (p00 > 0) {
                        orig_jou = rt.field_i32(p00, 4);
                    }
                }
            }

            // Mutate persistent battle state, panels, oki, Muramasa Tip[131], Folder, regular chip, and damage_dealt
            g_pvp_eng.core.sides[0].hp = 123;
            g_pvp_eng.core.sides[0].joutai = 2;
            g_pvp_eng.core.sides[0].wana = 5;
            g_pvp_eng.core.sides[0].bari = 100;
            g_pvp_eng.core.sides[0].han = 1;
            g_pvp_eng.core.sides[0].key_cnt = 25;
            g_pvp_eng.core.sides[0].fol_used_mask = 0x1F;
            g_pvp_eng.local_permanently_consumed_mask = 0x1F;
            sync_folder_used_from_mask(rt, g_pvp_eng.core.sides[0].fol_ref, 0x1F);
            if (g_pvp_eng.core.sides[0].fol_ref > 0) {
                rt.field_i32(g_pvp_eng.core.sides[0].fol_ref, 7) = 0; // regu_flg cleared
            }
            if (sf_app_CpCanvas_oki > 0) {
                Ref ok0 = rt.iaload(sf_app_CpCanvas_oki, 0);
                if (ok0 > 0) {
                    rt.field_i32(ok0, 1) = 1;
                }
            }
            if (sf_app_CpCanvas_panel > 0) {
                Ref p00 = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 0), 0);
                Ref p25 = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 2), 5);
                if (p00 > 0) {
                    rt.field_i32(p00, 4) = 6; // jou mutated
                    rt.field_i32(p00, 3) = 1; // jin flipped to Blue
                }
                if (p25 > 0) {
                    rt.field_i32(p25, 4) = 6; // jou mutated
                    rt.field_i32(p25, 3) = 0; // jin flipped to Red
                }
            }
            if (sf_app_CpCanvas_tip > 0 && rt.array_length(sf_app_CpCanvas_tip) > 131) {
                Ref mura = rt.iaload(sf_app_CpCanvas_tip, 131);
                if (mura > 0) {
                    rt.field_i32(mura, 3) = 999;
                }
            }
            g_pvp_eng.core.damage_dealt[0] = 350;
            g_pvp_eng.core.damage_dealt[1] = 120;

            f3_check(finish_game_ko(0), "F3.1e", "Game 1 KO enters ROUND_RESULT");
            f3_check(g_pvp_eng.phase == Phase::ROUND_RESULT, "F3.1f", "Mid-BO3 transitions to ROUND_RESULT (not RESULT)");
            f3_check(g_pvp_eng.bo3_wins[0] == 1 && g_pvp_eng.bo3_wins[1] == 0, "F3.1g", "BO3 score is 1-0 after Game 1");
            for (int p = 0; p < 5; ++p) { peer.poll_udp(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            f3_check(!peer.remote_match_ended(), "F3.1h", "PKT_MATCH_END NOT sent during mid-series ROUND_RESULT");

            for (int t = 0; t < 48; ++t) {
                step_override();
                peer.poll_udp();
            }

            f3_check(g_pvp_eng.phase == Phase::CUSTOM_SCREEN, "F3.1i", "After 48 ROUND_RESULT ticks enters Game 2 CUSTOM_SCREEN");
            f3_check(g_pvp_eng.bo3_game_index == 1, "F3.1j", "bo3_game_index incremented to 1");
            f3_check(g_pvp_eng.core.custom_round == 1, "F3.1k", "Game 2 resets custom_round == 1");
            f3_check(current_wire_round_key() == 101, "F3.1l", "Game 2 Round 1 wire key == 101");
            f3_check(g_pvp_eng.match_seed != g_pvp_eng.base_match_seed, "F3.1m", "Game 2 match_seed derived != base_match_seed");
            f3_check(g_pvp_eng.core.damage_dealt[0] == 0 && g_pvp_eng.core.damage_dealt[1] == 0,
                     "F3.1n", "damage_dealt reset to 0 for both sides");
            f3_check(g_pvp_eng.local_permanently_consumed_mask == 0,
                     "F3.1n2", "local_permanently_consumed_mask reset to 0");

            for (int si = 0; si < 2; ++si) {
                const SideContext& s = g_pvp_eng.core.sides[si];
                f3_check(s.hp == g_pvp_eng.builds[si].max_hp, "F3.1o", "Side HP reset to max_hp");
                f3_check(s.joutai == 0 && s.bari == 0 && s.han == 0 && s.wana == 0 && s.key_cnt == 0,
                         "F3.1p", "Side status/bari/han/wana/key_cnt reset to 0");
                f3_check(s.fol_used_mask == 0, "F3.1q", "Side fol_used_mask reset to 0");
            }
            bool panels_clean = true;
            if (sf_app_CpCanvas_panel > 0) {
                for (int y = 0; y < 3; ++y) {
                    Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
                    for (int x = 0; x < 6; ++x) {
                        Ref p = rt.iaload(row, x);
                        if (rt.field_i32(p, 4) != orig_jou || rt.field_i32(p, 3) != (x >= 3 ? 1 : 0)) {
                            panels_clean = false;
                        }
                    }
                }
            }
            f3_check(panels_clean, "F3.1r", "All 18 panels restored (jou == orig_jou, jin == (x>=3))");

            bool oki_clean = true;
            if (sf_app_CpCanvas_oki > 0) {
                for (int i = 0; i < 6; ++i) {
                    Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                    if (ok > 0 && rt.field_i32(ok, 1) != 0) {
                        oki_clean = false;
                    }
                }
            }
            f3_check(oki_clean, "F3.1s", "All oki[0..5] reset inactive");

            int32_t mura_bonus = 0;
            if (sf_app_CpCanvas_tip > 0 && rt.array_length(sf_app_CpCanvas_tip) > 131) {
                Ref mura = rt.iaload(sf_app_CpCanvas_tip, 131);
                if (mura > 0) {
                    mura_bonus = rt.field_i32(mura, 3);
                }
            }
            f3_check(mura_bonus == 0, "F3.1t", "Muramasa Tip[131] bonus field reset to 0");

            Ref loc_fol = g_pvp_eng.core.sides[0].fol_ref;
            Ref rem_fol = g_pvp_eng.core.sides[1].fol_ref;
            uint32_t loc_used_after_draw = read_folder_used_mask(rt, loc_fol);
            int loc_drawn_cnt = 0;
            for (int b = 0; b < 30; ++b) {
                if ((loc_used_after_draw >> b) & 1u) ++loc_drawn_cnt;
            }
            bool fol_clean = (loc_fol > 0 && rem_fol > 0 &&
                              rt.field_i32(loc_fol, 2) == 30 - sf_app_CpCanvas_max_sel &&
                              loc_drawn_cnt == sf_app_CpCanvas_max_sel &&
                              (loc_used_after_draw & 1u) != 0 &&
                              rt.field_i32(loc_fol, 7) == 1 &&
                              rt.field_i32(loc_fol, 8) == 0 &&
                              rt.field_i32(rem_fol, 2) == 30 &&
                              read_folder_used_mask(rt, rem_fol) == 0);
            f3_check(fol_clean, "F3.1u", "Local & Remote Folders reset (30 chips fresh, Regular slot 0 drawn)");
            int32_t slot0_cas = (sf_app_CpCanvas_cas_tip > 0) ? rt.iaload(sf_app_CpCanvas_cas_tip, 0) : -1;
            f3_check(slot0_cas == 1, "F3.1v", "Game 2 Round 1 custom slot 0 holds Regular chip 1");

            peer.close_udp();
            cleanup_and_restore_singleplayer(rt, true);
        }

        // 2a) BO3 2-0 sweep
        if (!setup_live_match(1, 5, 5, peer_b)) {
            fail_msg("SuiteF3-2a-Setup", "setup_live_match failed");
        } else {
            f3_check(confirm_round_into_combat(1) && finish_game_ko(0), "F3.2a-1", "Game 1 P0 wins -> ROUND_RESULT");
            for (int t = 0; t < 48; ++t) step_override();
            f3_check(confirm_round_into_combat(101) && finish_game_ko(0), "F3.2a-2", "Game 2 P0 wins -> sweep");
            f3_check(g_pvp_eng.phase == Phase::RESULT && g_pvp_eng.match_winner == 0 &&
                     g_pvp_eng.bo3_wins[0] == 2 && g_pvp_eng.bo3_wins[1] == 0,
                     "F3.2a-3", "2-0 sweep ends immediately in Phase::RESULT with match_winner == 0");
            for (int p = 0; p < 10; ++p) { peer.poll_udp(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            f3_check(peer.remote_match_ended(), "F3.2a-4", "PKT_MATCH_END transmitted only on final Phase::RESULT");
            peer.close_udp();
            cleanup_and_restore_singleplayer(rt, true);
        }

        // 2b) BO3 1-1 -> 2-1 comeback (3 full games)
        if (!setup_live_match(1, 5, 5, peer_b)) {
            fail_msg("SuiteF3-2b-Setup", "setup_live_match failed");
        } else {
            f3_check(confirm_round_into_combat(1) && finish_game_ko(0), "F3.2b-1", "Game 1 P0 wins");
            for (int t = 0; t < 48; ++t) step_override();
            f3_check(confirm_round_into_combat(101) && finish_game_ko(1), "F3.2b-2", "Game 2 P1 wins (1-1)");
            f3_check(g_pvp_eng.phase == Phase::ROUND_RESULT && g_pvp_eng.bo3_wins[0] == 1 && g_pvp_eng.bo3_wins[1] == 1,
                     "F3.2b-3", "Tied 1-1 continues to Game 3 via ROUND_RESULT");
            for (int t = 0; t < 48; ++t) step_override();
            f3_check(g_pvp_eng.bo3_game_index == 2 && current_wire_round_key() == 201,
                     "F3.2b-4", "Game 3 Round 1 wire key == 201");
            f3_check(confirm_round_into_combat(201) && finish_game_ko(1), "F3.2b-5", "Game 3 P1 wins -> 1-2");
            f3_check(g_pvp_eng.phase == Phase::RESULT && g_pvp_eng.match_winner == 1 &&
                     g_pvp_eng.bo3_wins[0] == 1 && g_pvp_eng.bo3_wins[1] == 2,
                     "F3.2b-6", "2-1 series ends in Phase::RESULT with match_winner == 1");
            peer.close_udp();
            cleanup_and_restore_singleplayer(rt, true);
        }

        // 2c) BO3 1-1-1 Draw
        if (!setup_live_match(1, 5, 5, peer_b)) {
            fail_msg("SuiteF3-2c-Setup", "setup_live_match failed");
        } else {
            f3_check(confirm_round_into_combat(1) && finish_game_ko(0), "F3.2c-1", "Game 1 P0 wins");
            for (int t = 0; t < 48; ++t) step_override();
            f3_check(confirm_round_into_combat(101) && finish_game_ko(1), "F3.2c-2", "Game 2 P1 wins");
            for (int t = 0; t < 48; ++t) step_override();
            f3_check(finish_game_turn15_draw(), "F3.2c-3", "Game 3 Turn 15 Judge tied -> Draw");
            f3_check(g_pvp_eng.phase == Phase::RESULT && g_pvp_eng.match_winner == -1 &&
                     g_pvp_eng.bo3_wins[0] == 1 && g_pvp_eng.bo3_wins[1] == 1 &&
                     g_pvp_eng.bo3_game_results[2] == -1,
                     "F3.2c-4", "1-1-1 ends in Phase::RESULT with match_winner == -1 (Draw)");
            peer.close_udp();
            cleanup_and_restore_singleplayer(rt, true);
        }

        // 2d) BO3 1-0-2 (P0 win, Draw, Draw)
        if (!setup_live_match(1, 5, 5, peer_b)) {
            fail_msg("SuiteF3-2d-Setup", "setup_live_match failed");
        } else {
            f3_check(confirm_round_into_combat(1) && finish_game_ko(0), "F3.2d-1", "Game 1 P0 wins");
            for (int t = 0; t < 48; ++t) step_override();
            f3_check(finish_game_turn15_draw(), "F3.2d-2", "Game 2 Draw");
            f3_check(g_pvp_eng.phase == Phase::ROUND_RESULT && g_pvp_eng.bo3_wins[0] == 1 && g_pvp_eng.bo3_wins[1] == 0,
                     "F3.2d-3", "1-0-1 proceeds to Game 3 (not concluded early)");
            for (int t = 0; t < 48; ++t) step_override();
            f3_check(finish_game_turn15_draw(), "F3.2d-4", "Game 3 Draw");
            f3_check(g_pvp_eng.phase == Phase::RESULT && g_pvp_eng.match_winner == 0 &&
                     g_pvp_eng.bo3_wins[0] == 1 && g_pvp_eng.bo3_wins[1] == 0 &&
                     g_pvp_eng.bo3_game_results[0] == 0 && g_pvp_eng.bo3_game_results[1] == -1 &&
                     g_pvp_eng.bo3_game_results[2] == -1,
                     "F3.2d-5", "1-0-2 ends in Phase::RESULT with match_winner == 0");
            peer.close_udp();
            cleanup_and_restore_singleplayer(rt, true);
        }

        // 2e) Legacy Relay Server BO3 Negotiation
        relay.engine.set_legacy_wire_compat_for_test(true);
        peer.set_preferred_format(pvp::PVP_FORMAT_SINGLE);
        if (!setup_live_match(1, 5, 5, peer_b)) {
            fail_msg("SuiteF3-2e-Setup", "setup_live_match failed");
        } else {
            for (int w = 0; w < 30 && !peer.is_triple_battle(); ++w) {
                peer.poll_udp();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            f3_check(rt.pvp_session().is_triple_battle() == true, "F3.2e-1",
                     "Host negotiates BO3 over legacy relay");
            f3_check(peer.is_triple_battle() == true, "F3.2e-2",
                     "Guest negotiates BO3 via PKT_BUILD_SYNC over legacy relay");
            f3_check(g_pvp_eng.is_triple_battle == true, "F3.2e-3",
                     "Engine enables BO3 over legacy relay");
            f3_check(confirm_round_into_combat(1) && finish_game_ko(0), "F3.2e-4",
                     "Game 1 P0 wins -> ROUND_RESULT");
            f3_check(g_pvp_eng.phase == Phase::ROUND_RESULT, "F3.2e-5",
                     "Game 1 enters Phase::ROUND_RESULT");
            for (int t = 0; t < 48; ++t) step_override();
            f3_check(confirm_round_into_combat(101) && finish_game_ko(0), "F3.2e-6",
                     "Game 2 (wire_rnd=101) P0 wins -> 2-0 sweep");
            f3_check(g_pvp_eng.phase == Phase::RESULT && g_pvp_eng.match_winner == 0 &&
                     g_pvp_eng.bo3_wins[0] == 2 && g_pvp_eng.bo3_wins[1] == 0,
                     "F3.2e-7", "2e sweep ends in Phase::RESULT with match_winner == 0 (2-0)");
            peer.close_udp();
            cleanup_and_restore_singleplayer(rt, true);
        }

        // 2f) Legacy BO1-Only Opponent Fallback
        peer.set_send_legacy_build_for_test(true);
        if (!setup_live_match(1, 5, 5, peer_b)) {
            fail_msg("SuiteF3-2f-Setup", "setup_live_match failed");
        } else {
            f3_check(g_pvp_eng.is_triple_battle == false, "F3.2f-1",
                     "Host falls back to BO1 against legacy opponent build");
            f3_check(confirm_round_into_combat(1) && finish_game_ko(0), "F3.2f-2",
                     "Game 1 KO in BO1 fallback");
            f3_check(g_pvp_eng.phase == Phase::RESULT && g_pvp_eng.match_winner == 0,
                     "F3.2f-3", "Fallback BO1 transitions directly from Game 1 KO to Phase::RESULT");
            peer.close_udp();
            cleanup_and_restore_singleplayer(rt, true);
        }
        peer.set_send_legacy_build_for_test(false);
        relay.engine.set_legacy_wire_compat_for_test(false);
    }
    std::printf("[PvP SELF-TEST] Suite F3 complete: Triple Battle (BO3) Reset & Series Outcomes verified (failures=%d)\n",
                failures);
        }
    }

    rt.restore_full_snapshot(base_heap);
    generated_restore_statics(base_statics);
    g_pvp_eng = PvpEngineState{};

    if (failures == 0) {
        std::printf("[PvP SELF-TEST PASS] ALL SUITES PASSED (0 failures)!\n");
        std::fflush(stdout);
        return 0;
    }
    std::fprintf(stderr, "[PvP SELF-TEST FAILED] Total failures: %d\n", failures);
    std::fflush(stderr);
    return 1;
}

} // namespace pvp_pon

// ============================================================================
// Per-frame main-loop hook called inside CpCanvas.exe() right after g.lock()
// Returns true when PvP owns the frame (skipping single-player scene dispatch)
// ============================================================================
static bool pvp_pon_tick_override_impl(DojaRuntime& rt, Ref cp);

static bool pvp_pon_tick_override(DojaRuntime& rt, Ref cp) {
    try {
        return pvp_pon_tick_override_impl(rt, cp);
    } catch (const JavaException& ex) {
        std::fprintf(stderr, "[PvP Exception] caught JavaException(ref=%d, where=%s, a=%d, b=%d) in phase=%d turn=%u\n",
                     ex.exc_ref, ex.where ? ex.where : "?", ex.detail_a, ex.detail_b,
                     static_cast<int>(pvp_pon::g_pvp_eng.phase), pvp_pon::g_pvp_eng.core.combat_turn);
        pvp_pon::cleanup_and_restore_singleplayer(rt, true);
        rt.show_ui_toast("对战发生异常，已安全返回单机模式", 150);
        return false;
    }
}

static bool pvp_pon_tick_override_impl(DojaRuntime& rt, Ref cp) {
    using namespace pvp_pon;

    // Note: CpCanvas.exe() calls SetUp() completely before entering the L_265 frame loop,
    // and PoN's main Title screen ("Title2") runs at scene == -2.
    // Guard only that core Tip/Panel tables from SetUp() exist.
    if ((sf_app_CpCanvas_tip <= 0 || sf_app_CpCanvas_panel <= 0) && g_pvp_eng.phase == Phase::IDLE) {
        return false;
    }

    if (rt.pvp_self_test()) {
        int rc = run_pvp_headless_self_test(rt, cp);
        rt.shutdown();
        std::fflush(stdout);
        std::fflush(stderr);
        std::exit(rc);
    }

    // Check CLI --pvp-create / --pvp-join auto-start once booted onto Title screen
    if (g_pvp_eng.phase == Phase::IDLE && !rt.pvp_session().active()) {
        if (rt.pvp_auto_bot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        uint32_t cli_rm = 0;
        rt.take_cli_pvp_action(cli_rm);
    }

    pvp::PvpClientSession& net = rt.pvp_session();

    // If user closed room via F1 menu while in PvP, restore single-player cleanly
    if (!net.active()) {
        if (g_pvp_eng.phase != Phase::IDLE) {
            cleanup_and_restore_singleplayer(rt, false);
        }
        return false;
    }

    // If user left an active PvP match and created/joined a brand-new room inside F1,
    // restore single-player state first without closing the newly opened room session!
    if (g_pvp_eng.phase >= Phase::HANDSHAKE &&
        g_pvp_eng.active_room_id != 0 &&
        net.room_id() != g_pvp_eng.active_room_id) {
        cleanup_and_restore_singleplayer(rt, false);
    }

    net.poll_udp();

    // Phase 1: Waiting for Challenger in Lobby (non-blocking banner while user plays)
    if (!net.match_started() && g_pvp_eng.phase <= Phase::LOBBY_WAIT) {
        g_pvp_eng.phase = Phase::LOBBY_WAIT;
        if (rt.pvp_auto_bot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false; // Let single-player run normally; softkey bar & toast show room ID
    }

    // Transition into active PvP match! Take full VM snapshot once so we can restore cleanly later
    if (g_pvp_eng.phase == Phase::IDLE || g_pvp_eng.phase == Phase::LOBBY_WAIT) {
        rt.set_pvp_match_active(true);
        g_pvp_eng.active_room_id = net.room_id();
        g_pvp_eng.local_idx = net.is_host() ? 0 : 1;
        g_pvp_eng.match_seed = net.rng_seed() ? net.rng_seed() : 0x2468ACE1u;

        if (!g_pvp_eng.has_pre_match_snap) {
            rt.save_full_snapshot(g_pvp_eng.pre_match_heap);
            generated_save_statics(g_pvp_eng.pre_match_statics);
            g_pvp_eng.has_pre_match_snap = true;
        }

        g_pvp_eng.local_build = extract_local_build_from_sav(rt, cp);
        g_pvp_eng.build_prepared = true;
        g_pvp_eng.last_sync_send_ms = 0;
        g_pvp_eng.last_build_keepalive_ms = 0;
        g_pvp_eng.phase = Phase::HANDSHAKE;
        rt.show_ui_toast("", 0);
    }

    // Check unexpected peer disconnect during active match
    if (net.peer_disconnected() && g_pvp_eng.phase != Phase::RESULT) {
        g_pvp_eng.match_winner = g_pvp_eng.local_idx;
        g_pvp_eng.core.winner = g_pvp_eng.local_idx;
        g_pvp_eng.phase = Phase::RESULT;
        g_pvp_eng.result_ticks = 0;
        play_pvp_result_bgm(rt);
        rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
    }

    // ------------------------------------------------------------------------
    // Phase::HANDSHAKE — Exchange .sav Fighter Builds (HP, Buster, Folder, Skills)
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::HANDSHAKE) {
        int64_t now = steady_ms();
        if (!(net.has_remote_build() && net.local_build_acked()) &&
            (now - g_pvp_eng.last_sync_send_ms >= 120)) {
            net.send_build_sync(g_pvp_eng.local_build);
            g_pvp_eng.last_sync_send_ms = now;
        }
        net.poll_udp();

        // Draw cyber-grid connecting screen
        rt.fb_fill_rect_alpha(0, 0, 240, 240, 0xFF0A1428u);
        rt.fb_fill_rect_alpha(16, 68, 208, 104, 0xF0142448u);
        rt.fb_draw_border(16, 68, 208, 104, 0xFF5898FFu);
        std::string t1 = "NETBATTLE 联机对战准备中";
        if (net.is_triple_battle()) {
            t1 += " [三回战]";
        }
        std::string t2 = "房间 #" + std::to_string(net.room_id()) + "  VS  " + net.opponent_name();
        std::string t3 = "正在同步双方存档牌组与领航员改造...";
        rt.fb_draw_text(t1, 120 - rt.fb_measure_text(t1) / 2, 94, 0xFFFFE066u);
        rt.fb_draw_text(t2, 120 - rt.fb_measure_text(t2) / 2, 122, 0xFFFFFFFFu);
        rt.fb_draw_text(t3, 120 - rt.fb_measure_text(t3) / 2, 148, 0xFF88CCFFu);

        if (net.has_remote_build() && net.local_build_acked()) {
            int loc_i = g_pvp_eng.local_idx;
            g_pvp_eng.builds[loc_i] = g_pvp_eng.local_build;
            g_pvp_eng.builds[1 - loc_i] = net.remote_build();

            g_pvp_eng.base_match_seed = net.rng_seed() ? net.rng_seed() : 0x2468ACE1u;
            g_pvp_eng.is_triple_battle = net.is_triple_battle();
            if (net.is_host() && net.preferred_format() == pvp::PVP_FORMAT_TRIPLE && !net.peer_supports_bo3()) {
                rt.show_ui_toast("对手版本较旧，已自动切换为一回战", 150);
            }
            g_pvp_eng.bo3_game_index = 0;
            g_pvp_eng.bo3_wins[0] = 0;
            g_pvp_eng.bo3_wins[1] = 0;
            g_pvp_eng.bo3_game_results[0] = -2;
            g_pvp_eng.bo3_game_results[1] = -2;
            g_pvp_eng.bo3_game_results[2] = -2;
            g_pvp_eng.match_winner = -2;
            g_pvp_eng.round_result_ticks = 0;
            g_pvp_eng.desync_warnings = 0;
            g_pvp_eng.match_seed = derive_bo3_game_seed(g_pvp_eng.base_match_seed, 0);

            init_pvp_battle_arena(rt, cp);
            enter_custom_screen_phase(rt, cp);
        } else if (rt.pvp_auto_bot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::CUSTOM_SCREEN — Native PoN Chip Selection + Simultaneous Commit
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::CUSTOM_SCREEN) {
        int32_t local_mask = rt.poll_local_pvp_keypad();
        if (!net.active() || (g_pvp_eng.active_room_id != 0 && net.room_id() != g_pvp_eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        if (net.peer_disconnected()) {
            g_pvp_eng.match_winner = g_pvp_eng.local_idx;
            g_pvp_eng.core.winner = g_pvp_eng.local_idx;
            g_pvp_eng.phase = Phase::RESULT;
            g_pvp_eng.result_ticks = 0;
            play_pvp_result_bgm(rt);
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
        }
        if (g_pvp_eng.bo3_game_index == 0 && g_pvp_eng.core.custom_round == 1) {
            pvp::PvpCustomCommit dummy_cc{};
            int64_t now = steady_ms();
            if (!net.has_remote_commit(current_wire_round_key(), dummy_cc) &&
                (now - g_pvp_eng.last_build_keepalive_ms >= 250)) {
                net.send_build_sync(g_pvp_eng.local_build);
                g_pvp_eng.last_build_keepalive_ms = now;
            }
        }
        if (rt.pvp_auto_bot() && !g_pvp_eng.local_custom_done) {
            if (g_pvp_eng.custom_timer_ticks == 30 * 16 - 2) {
                local_mask = (1 << 20); // Select slot 0 BattleChip
            } else if (g_pvp_eng.custom_timer_ticks <= 30 * 16 - 5) {
                local_mask = 0;
                g_pvp_eng.custom_timer_ticks = 1; // Trigger clean TipIn + commit!
            } else {
                local_mask = 0;
            }
        }
        local_mask &= ~(1 << 22); // Mask out SOFT2 (Escape) during PvP Custom Screen

        if (!g_pvp_eng.local_custom_done) {
            int loc_i = g_pvp_eng.local_idx;
            SideContext& ls = g_pvp_eng.core.sides[loc_i];
            int32_t newly_pressed = local_mask & ~ls.prev_input_mask;
            ls.prev_input_mask = local_mask;

            // Convert newly pressed key bit to CpCanvas.key for BattleTip cursor navigation
            int32_t bt_key = 0;
            const int check_bits[] = {20, 10, 21, 16, 18, 17, 19, 11};
            for (int b : check_bits) {
                if (newly_pressed & (1 << b)) {
                    bt_key = (1 << b);
                    break;
                }
            }

            sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
            if (sf_app_CpCanvas_max_sel <= 0) {
                sf_app_CpCanvas_sel_cas_mode = 1;
                sf_app_CpCanvas_sel_cas_tip = 0;
                bt_key &= ~((1 << 16) | (1 << 17) | (1 << 18) | (1 << 19));
            }

            // Auto-confirm when 30-second Custom timer expires
            if (--g_pvp_eng.custom_timer_ticks <= 0) {
                if (sf_app_CpCanvas_cas_cnt > 0 || sf_app_CpCanvas_pa_cnt2 > 0) {
                    // TipIn() already ran and compacted cas_tip; just finish the close animation
                    sf_app_CpCanvas_cas_cnt = 0;
                    sf_app_CpCanvas_pa_cnt2 = 0;
                    sf_app_CpCanvas_sel_cnt = 0;
                    sf_app_CpCanvas_scene = 1;
                } else {
                    int32_t valid_sel = 0;
                    if (sf_app_CpCanvas_sel_cnt > 0 && sf_app_CpCanvas_set_tip > 0 && sf_app_CpCanvas_cas_tip > 0) {
                        int32_t lim = std::min<int32_t>(sf_app_CpCanvas_sel_cnt, rt.array_length(sf_app_CpCanvas_set_tip));
                        int32_t clim = rt.array_length(sf_app_CpCanvas_cas_tip);
                        for (int32_t i = 0; i < lim; ++i) {
                            int32_t st = rt.iaload(sf_app_CpCanvas_set_tip, i);
                            if (st >= 0 && st < clim && rt.iaload(sf_app_CpCanvas_cas_tip, st) >= 1) {
                                rt.iaload(sf_app_CpCanvas_set_tip, valid_sel++) = st;
                            }
                        }
                    }
                    sf_app_CpCanvas_sel_cnt = valid_sel;
                    if (valid_sel > 0) {
                        fn_app_CpCanvas_TipIn_____V(rt, cp);
                        for (int32_t i = 0; i < 10; ++i) {
                            if (rt.iaload(sf_app_CpCanvas_cas_tip, i) < 1) {
                                for (int32_t j = i + 1; j < 10; ++j) {
                                    int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, j);
                                    if (ct >= 1) {
                                        rt.iaload(sf_app_CpCanvas_cas_tip, i) = ct;
                                        rt.iaload(sf_app_CpCanvas_cas_ok, i) = 1;
                                        rt.iaload(sf_app_CpCanvas_cas_tip, j) = -1;
                                        rt.iaload(sf_app_CpCanvas_cas_ok, j) = 0;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    // If valid_sel == 0, keep sf_app_Rock_tip_no & rock_tips untouched so leftover chips carry over!
                    sf_app_CpCanvas_sel_cnt = 0;
                    sf_app_CpCanvas_cas_cnt = 0;
                    sf_app_CpCanvas_pa_cnt2 = 0;
                    sf_app_CpCanvas_sec_cha = 0;
                    sf_app_CpCanvas_key = 0;
                    sf_app_CpCanvas_scene = 1;
                }
                sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
            } else {
                sf_app_CpCanvas_key = bt_key;
                rt.set_keypad_override(true, local_mask);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
                rt.set_keypad_override(false, 0);
                if (sf_app_CpCanvas_cas_cnt > 0) {
                    // Intercept post-OK / post-ADD immediately after TipIn() + tray compaction:
                    // retract Custom window at once without playing local BATTLE START or PA flash first.
                    sf_app_CpCanvas_cas_cnt = 0;
                    sf_app_CpCanvas_pa_cnt2 = 0;
                    sf_app_CpCanvas_sel_cnt = 0;
                    sf_app_CpCanvas_key = 0;
                    sf_app_CpCanvas_scene = 1;
                }
                sanitize_custom_tray_and_folder(rt, g_pvp_eng.local_permanently_consumed_mask);
                if (sf_app_CpCanvas_max_sel <= 0) {
                    if (sf_app_CpCanvas_sel_cas_mode == 0) {
                        sf_app_CpCanvas_sel_cas_mode = 1;
                    }
                    sf_app_CpCanvas_sel_cas_tip = 0;
                }
                // Hide the "[S] 逃げる" softkey label in PvP Custom Screen
                if (sf_app_CpCanvas_soft_id > 0 && rt.array_length(sf_app_CpCanvas_soft_id) > 1) {
                    rt.iaload(sf_app_CpCanvas_soft_id, 1) = 0;
                }
            }

            if (sf_app_CpCanvas_scene != 1) {
                // Draw Custom Screen timer / FINAL TURN card on top-left & Opponent Name/HP card on top-right
                int secs_left = std::max(0, (g_pvp_eng.custom_timer_ticks + 15) / 16);
                int opp_i = 1 - loc_i;
                if (g_pvp_eng.core.custom_round >= 15) {
                    uint32_t ft_bcol = (((g_pvp_eng.custom_timer_ticks / 4) & 1) == 0) ? 0xFFFFD84Cu : 0xFFFF4858u;
                    rt.fb_fill_rect_alpha(4, 4, 114, 32, 0xEE2C0C18u);
                    rt.fb_draw_border(4, 4, 114, 32, ft_bcol);
                    std::string ft_hdr = "FINAL TURN!";
                    rt.fb_draw_text(ft_hdr, 61 - rt.fb_measure_text(ft_hdr) / 2, 16, 0xFFFFE866u);
                    std::string t_str = "选牌 " + std::to_string(secs_left) + "s (10s)";
                    rt.fb_draw_text(t_str, 61 - rt.fb_measure_text(t_str) / 2, 31,
                                    (secs_left <= 5) ? 0xFFFF6868u : 0xFFFFB8C8u);
                } else {
                    std::string t_str = "选牌 " + std::to_string(secs_left) + "s";
                    rt.fb_fill_rect_alpha(4, 10, 62, 20, 0xCC101830u);
                    rt.fb_draw_border(4, 10, 62, 20, 0xFF5898FFu);
                    rt.fb_draw_text(t_str, 10, 24, (secs_left <= 5) ? 0xFFFF6868u : 0xFFFFE066u);
                    if (g_pvp_eng.is_triple_battle) {
                        std::string bo3_str = "G" + std::to_string(g_pvp_eng.bo3_game_index + 1) + " " +
                                              std::to_string(g_pvp_eng.bo3_wins[loc_i]) + "-" +
                                              std::to_string(g_pvp_eng.bo3_wins[opp_i]);
                        rt.fb_fill_rect_alpha(70, 10, 48, 20, 0xCC182444u);
                        rt.fb_draw_border(70, 10, 48, 20, 0xFFFFD84Cu);
                        rt.fb_draw_text(bo3_str, 94 - rt.fb_measure_text(bo3_str) / 2, 24, 0xFFFFE866u);
                    }
                }

                const SideContext& opp_sc = g_pvp_eng.core.sides[opp_i];
                if (opp_sc.wana != 0) {
                    draw_active_trap_hud_indicators(rt, false, true);
                }
                std::string opp_hdr = "VS " + g_pvp_eng.builds[opp_i].player_name;
                std::string opp_hp_str = "HP " + std::to_string(std::max(0, opp_sc.hp)) + "/" + std::to_string(opp_sc.max_hp);
                rt.fb_fill_rect_alpha(122, 4, 114, 32, 0xE0381420u);
                rt.fb_draw_border(122, 4, 114, 32, 0xFFFF5868u);
                rt.fb_draw_text(opp_hdr, 126, 16, 0xFFFF98A8u);
                rt.fb_draw_text(opp_hp_str, 126, 31, (opp_sc.hp * 4 <= opp_sc.max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);
            }

            if (sf_app_CpCanvas_scene == 1) {
                // Local player confirmed chips ("OK!") -> Capture commit & save local leftover tray!
                g_pvp_eng.local_commit = capture_local_custom_commit(rt);
                g_pvp_eng.local_custom_done = true;
                g_pvp_eng.last_sync_send_ms = 0;
            }
        }

        if (g_pvp_eng.local_custom_done) {
            int64_t now = steady_ms();
            uint16_t rnd = current_wire_round_key();
            pvp::PvpCustomCommit probe_rem{};
            bool has_rem_pre = net.has_remote_commit(rnd, probe_rem);
            bool acked_pre = net.is_custom_acked(rnd);
            int64_t commit_retry_ms = acked_pre ? 100 : 45;
            if (!(has_rem_pre && acked_pre) && (now - g_pvp_eng.last_sync_send_ms >= commit_retry_ms)) {
                net.send_custom_commit(g_pvp_eng.local_commit);
                g_pvp_eng.last_sync_send_ms = now;
            }
            net.poll_udp();

            pvp::PvpCustomCommit rem_commit{};
            if (net.has_remote_commit(rnd, rem_commit) && net.is_custom_acked(rnd)) {
                // Both players committed & ACKed!
                // First restore exact canonical pre-custom combat state so zero CharaDraw(0) / local RNG
                // mutations from BattleTip() on Client 0 vs Client 1 can ever leak into combat!
                int loc_i = g_pvp_eng.local_idx;
                const pvp::PvpCustomCommit& c0 = (loc_i == 0) ? g_pvp_eng.local_commit : rem_commit;
                const pvp::PvpCustomCommit& c1 = (loc_i == 1) ? g_pvp_eng.local_commit : rem_commit;

                HostRuntimeState host_st = capture_host_runtime_state(rt);
                g_pvp_eng.core = g_pvp_eng.pre_custom_core;
                generated_restore_statics(g_pvp_eng.pre_custom_statics);
                rt.restore_combat_snapshot(g_pvp_eng.pre_custom_snap);
                restore_host_runtime_state(rt, host_st, true);
                g_pvp_eng.local_idx = loc_i;

                if (sf_app_CpCanvas_rand > 0) {
                    uint64_t combat_seed = (static_cast<uint64_t>(g_pvp_eng.match_seed) +
                                            static_cast<uint64_t>(g_pvp_eng.core.combat_turn) * 0x9E3779B97F4A7C15ULL) &
                                           ((1ULL << 48) - 1ULL);
                    rt.get(sf_app_CpCanvas_rand).rng_seed = combat_seed;
                }

                ensure_board_orient(rt, 0);
                apply_custom_commit_to_side(rt, 0, c0);
                apply_custom_commit_to_side(rt, 1, c1);

                // If the remote fighter ended the previous round holding a Buster charge (key_cnt > 0),
                // predict that they keep Buster held on round_start_turn until their Round-N+1 UDP input
                // arrives — preventing a speculative frame-0 release from playing a phantom Buster / Charge
                // Shot sound effect on the local speakers!
                g_pvp_eng.round_initial_remote_pred =
                    compute_round_initial_remote_buster_pred(1 - loc_i);

                g_pvp_eng.core.sides[0].prev_input_mask = 0;
                g_pvp_eng.core.sides[1].prev_input_mask = 0;
                if (g_pvp_eng.core.custom_round >= 15) {
                    g_pvp_eng.core.final_turn_ticks_left = kFinalTurnDurationTicks;
                    sf_app_CpCanvas_cus_gage = 128;
                } else {
                    g_pvp_eng.core.final_turn_ticks_left = 0;
                    sf_app_CpCanvas_cus_gage = 0;
                }
                sf_app_CpCanvas_scene = 1;
                sf_app_CpCanvas_stop_cnt = 0;
                sf_app_CpCanvas_stop_time = 0;

                // Reset rollback ring & per-round tracking at the start of each combat round.
                // Leave cur_t unconfirmed/unscheduled so frame 0 of the round captures live held Buster input
                // (allowing cross-round Buster charge continuation just like native PoN virus battles), while
                // suppressing edge-triggered D-pad/Softkey/Chip-Select keys still held from confirming "OK".
                for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
                for (auto& em : g_pvp_eng.turn_emitted_se) {
                    em.turn = UINT32_MAX;
                    em.count = 0;
                }
                uint32_t cur_t = g_pvp_eng.core.combat_turn;
                const size_t hist_n = g_pvp_eng.local_inputs.size();
                g_pvp_eng.round_start_turn = cur_t;
                g_pvp_eng.first_unconfirmed_turn = cur_t;
                g_pvp_eng.max_scheduled_turn = cur_t;
                g_pvp_eng.round_needs_initial_schedule = true;
                int32_t held_now = rt.poll_local_pvp_keypad();
                int32_t suppress_bits = (0xF << 16) | (1 << 21) | (1 << 22);
                if (g_pvp_eng.core.sides[loc_i].tip_no >= 0) {
                    suppress_bits |= (1 << 20);
                }
                g_pvp_eng.round_held_edge_suppress_mask = held_now & suppress_bits;
                g_pvp_eng.stall_frames = 0;
                g_pvp_eng.throttler.reset();
                for (uint32_t d = 0; d <= kInputDelayFrames + 8; ++d) {
                    size_t slot = (cur_t + d) % hist_n;
                    g_pvp_eng.local_inputs[slot] = 0;
                    g_pvp_eng.predicted_remote[slot] = g_pvp_eng.round_initial_remote_pred;
                    g_pvp_eng.remote_confirmed[slot] = false;
                }

                g_pvp_eng.phase = Phase::ROUND_BANNER;
                g_pvp_eng.banner_ticks = 0;
                g_pvp_eng.last_sync_send_ms = now;
                // Fall through immediately into Phase::ROUND_BANNER so frame 0 renders right away!
            } else {
                // Render arena behind "Waiting for opponent..." banner
                render_pvp_combat_frame(rt, cp);
                if (g_pvp_eng.core.custom_round >= 15) {
                    rt.fb_fill_rect_alpha(44, 78, 152, 20, 0xEE2C0C18u);
                    rt.fb_draw_border(44, 78, 152, 20, 0xFFFF5868u);
                    std::string ft_wait = "FINAL TURN · 最终回合";
                    rt.fb_draw_text(ft_wait, 120 - rt.fb_measure_text(ft_wait) / 2, 92, 0xFFFFE866u);
                }
                rt.fb_fill_rect_alpha(28, 102, 184, 36, 0xE6101C38u);
                rt.fb_draw_border(28, 102, 184, 36, 0xFF68A8FFu);
                std::string wait_msg = "等待对手选牌确认...";
                rt.fb_draw_text(wait_msg, 120 - rt.fb_measure_text(wait_msg) / 2, 125, 0xFFFFE878u);
                if (rt.pvp_auto_bot()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            }
        }
        if (g_pvp_eng.phase == Phase::CUSTOM_SCREEN) {
            return true;
        }
    }

    // ------------------------------------------------------------------------
    // Phase::ROUND_BANNER — Synchronized 15-Frame Native BATTLE START Banner
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::ROUND_BANNER) {
        if (!net.active() || (g_pvp_eng.active_room_id != 0 && net.room_id() != g_pvp_eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        {
            uint16_t rnd = current_wire_round_key();
            uint32_t rem_top = 0;
            int64_t now = steady_ms();
            if (!net.get_highest_remote_turn_frame(rnd, rem_top) &&
                (now - g_pvp_eng.last_sync_send_ms >= 120)) {
                net.send_custom_commit(g_pvp_eng.local_commit);
                g_pvp_eng.last_sync_send_ms = now;
            }
        }
        net.poll_udp();
        if (net.peer_disconnected()) {
            g_pvp_eng.match_winner = g_pvp_eng.local_idx;
            g_pvp_eng.core.winner = g_pvp_eng.local_idx;
            g_pvp_eng.phase = Phase::RESULT;
            g_pvp_eng.result_ticks = 0;
            play_pvp_result_bgm(rt);
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
        }
        (void)rt.poll_local_pvp_keypad();
        if (rt.pvp_auto_bot() && !rt.fast_forward() && (g_pvp_eng.banner_ticks & 3) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        render_pvp_combat_frame(rt, cp);
        render_round_banner_overlay(rt, g_pvp_eng.banner_ticks, g_pvp_eng.core.custom_round);
        g_pvp_eng.banner_ticks++;

        if (g_pvp_eng.banner_ticks >= 15) {
            int loc_i = g_pvp_eng.local_idx;
            int32_t held_now = rt.poll_local_pvp_keypad();
            int32_t suppress_bits = (0xF << 16) | (1 << 21) | (1 << 22);
            if (g_pvp_eng.core.sides[loc_i].tip_no >= 0) {
                suppress_bits |= (1 << 20);
            }
            g_pvp_eng.round_held_edge_suppress_mask = held_now & suppress_bits;
            g_pvp_eng.stall_frames = 0;
            g_pvp_eng.throttler.reset();
            g_pvp_eng.phase = Phase::COMBAT;
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::COMBAT — GGPO Rollback + Symmetric Dual-Perspective Combat Engine
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::COMBAT) {
        uint32_t cur_t = g_pvp_eng.core.combat_turn;
        int32_t raw_local = rt.poll_local_pvp_keypad();
        if (!net.active() || (g_pvp_eng.active_room_id != 0 && net.room_id() != g_pvp_eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        if (net.peer_disconnected()) {
            g_pvp_eng.match_winner = g_pvp_eng.local_idx;
            g_pvp_eng.core.winner = g_pvp_eng.local_idx;
            g_pvp_eng.phase = Phase::RESULT;
            g_pvp_eng.result_ticks = 0;
            play_pvp_result_bgm(rt);
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
        }
        if (rt.pvp_auto_bot()) {
            g_pvp_eng.round_held_edge_suppress_mask = 0;
            raw_local = compute_auto_bot_combat_input(rt);
            if ((cur_t & 3u) == 0u) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            g_pvp_eng.round_held_edge_suppress_mask &= raw_local;
            raw_local &= ~g_pvp_eng.round_held_edge_suppress_mask;
        }

        const size_t hist_n = g_pvp_eng.local_inputs.size();
        uint16_t rnd = current_wire_round_key();
        bool pending_transition = (g_pvp_eng.core.open_custom_req || g_pvp_eng.core.winner != -2);

        // Schedule new local inputs immutably: never overwrite an already-scheduled turn during a stall!
        if (!pending_transition && g_pvp_eng.round_needs_initial_schedule) {
            size_t s0 = cur_t % hist_n;
            g_pvp_eng.local_inputs[s0] = raw_local;
            g_pvp_eng.remote_confirmed[s0] = false;
            g_pvp_eng.max_scheduled_turn = cur_t;
            g_pvp_eng.round_needs_initial_schedule = false;
        }
        uint32_t target_sched = pending_transition
            ? (cur_t > g_pvp_eng.round_start_turn ? cur_t - 1 : g_pvp_eng.round_start_turn)
            : (cur_t + kInputDelayFrames);
        while (g_pvp_eng.max_scheduled_turn < target_sched) {
            uint32_t next_s = g_pvp_eng.max_scheduled_turn + 1;
            size_t s_idx = next_s % hist_n;
            g_pvp_eng.local_inputs[s_idx] = raw_local;
            g_pvp_eng.remote_confirmed[s_idx] = false;
            g_pvp_eng.max_scheduled_turn = next_s;
        }

        // Build up to 8-frame redundant input bundle scoped strictly to the current round
        uint32_t send_top = g_pvp_eng.max_scheduled_turn;
        uint8_t bundle_cnt = static_cast<uint8_t>(
            std::min<uint32_t>(8u, send_top - g_pvp_eng.round_start_turn + 1u));
        int32_t hist_newest_first[8] = {0};
        for (uint8_t i = 0; i < bundle_cnt; ++i) {
            hist_newest_first[i] = g_pvp_eng.local_inputs[(send_top - i) % hist_n];
        }
        uint16_t cur_hash = (!pending_transition &&
                             send_top == cur_t + kInputDelayFrames &&
                             g_pvp_eng.first_unconfirmed_turn >= cur_t)
            ? compute_combat_state_hash(rt)
            : 0;
        uint32_t pre_rem_top = 0;
        bool has_rem_input = net.get_highest_remote_turn_frame(rnd, pre_rem_top);
        if (!has_rem_input) {
            int64_t now = steady_ms();
            if (now - g_pvp_eng.last_sync_send_ms >= 120) {
                net.send_custom_commit(g_pvp_eng.local_commit);
                g_pvp_eng.last_sync_send_ms = now;
            }
        }
        int32_t pre_rem_frontier = has_rem_input
            ? static_cast<int32_t>(pre_rem_top)
            : static_cast<int32_t>(g_pvp_eng.round_start_turn);
        int16_t local_tick_adv = static_cast<int16_t>(
            std::clamp<int32_t>(static_cast<int32_t>(send_top) - pre_rem_frontier, -16000, 16000));
        net.send_input_bundle(rnd, send_top, hist_newest_first, bundle_cnt, cur_hash, local_tick_adv);
        net.poll_udp();

        // Check if any older turn within this round's rollback window received a mispredicted remote input
        uint32_t oldest_rb = (cur_t >= kRollbackWindow - 2) ? (cur_t - (kRollbackWindow - 2)) : 0;
        oldest_rb = std::max(oldest_rb, g_pvp_eng.round_start_turn);
        uint32_t rollback_from = UINT32_MAX;
        for (uint32_t t = oldest_rb; t < cur_t; ++t) {
            int32_t net_rem = 0;
            if (net.get_remote_turn_input(rnd, t, net_rem)) {
                size_t idx = t % hist_n;
                if (!g_pvp_eng.remote_confirmed[idx]) {
                    if (g_pvp_eng.predicted_remote[idx] != net_rem && rollback_from == UINT32_MAX) {
                        rollback_from = t;
                    }
                    g_pvp_eng.predicted_remote[idx] = net_rem;
                    g_pvp_eng.remote_confirmed[idx] = true;
                }
            }
        }

        // Execute GGPO Rollback & fast-forward resimulation if remote input differed from prediction!
        if (rollback_from != UINT32_MAX && restore_rollback_slot(rt, rollback_from)) {
            uint32_t prev_cur_t = cur_t;
            for (uint32_t t = rollback_from; t < prev_cur_t; ++t) {
                size_t idx = t % hist_n;
                int32_t loc_m = g_pvp_eng.local_inputs[idx];
                int32_t rem_m = 0;
                bool is_pred = false;
                if (g_pvp_eng.remote_confirmed[idx]) {
                    rem_m = g_pvp_eng.predicted_remote[idx];
                } else if (net.get_remote_turn_input(rnd, t, rem_m)) {
                    g_pvp_eng.predicted_remote[idx] = rem_m;
                    g_pvp_eng.remote_confirmed[idx] = true;
                } else {
                    rem_m = (t > g_pvp_eng.round_start_turn)
                        ? g_pvp_eng.predicted_remote[(t - 1) % hist_n]
                        : g_pvp_eng.round_initial_remote_pred;
                    g_pvp_eng.predicted_remote[idx] = rem_m;
                    is_pred = true;
                }
                int32_t p0_m = (g_pvp_eng.local_idx == 0) ? loc_m : rem_m;
                int32_t p1_m = (g_pvp_eng.local_idx == 1) ? loc_m : rem_m;

                save_rollback_slot(rt, t, p0_m, p1_m, is_pred);
                step_pvp_combat_tick(rt, cp, p0_m, p1_m);
                if (g_pvp_eng.core.open_custom_req || g_pvp_eng.core.winner != -2) {
                    break;
                }
            }
            cur_t = g_pvp_eng.core.combat_turn;
            for (uint32_t u = cur_t; u < prev_cur_t; ++u) {
                if (u >= g_pvp_eng.round_start_turn) {
                    g_pvp_eng.remote_confirmed[u % hist_n] = false;
                }
            }
        }

        // Advance monotonic confirmed frontier & verify canonical state hashes on confirmed turns
        while (g_pvp_eng.first_unconfirmed_turn < cur_t) {
            uint32_t ft = g_pvp_eng.first_unconfirmed_turn;
            size_t idx = ft % hist_n;
            if (!g_pvp_eng.remote_confirmed[idx]) {
                int32_t late_m = 0;
                if (net.get_remote_turn_input(rnd, ft, late_m)) {
                    g_pvp_eng.predicted_remote[idx] = late_m;
                    g_pvp_eng.remote_confirmed[idx] = true;
                } else {
                    break;
                }
            }
            int32_t dummy_m = 0;
            uint16_t peer_h = 0;
            if (ft > g_pvp_eng.last_hash_checked_turn &&
                net.get_remote_turn_input(rnd, ft + kInputDelayFrames, dummy_m, &peer_h) &&
                peer_h != 0) {
                const RollbackSlot& rbs = g_pvp_eng.rb_ring[ft % kRollbackWindow];
                if (rbs.turn == ft && rbs.pre_hash != 0 && rbs.pre_hash != peer_h) {
                    if (++g_pvp_eng.desync_warnings <= 8) {
                        std::fprintf(stderr,
                                     "[PvP DESYNC] round=%u turn=%u local_hash=0x%04x remote_hash=0x%04x\n",
                                     static_cast<unsigned>(rnd), ft,
                                     static_cast<unsigned>(rbs.pre_hash),
                                     static_cast<unsigned>(peer_h));
                    }
                }
                g_pvp_eng.last_hash_checked_turn = ft;
            }
            g_pvp_eng.first_unconfirmed_turn++;
        }

        auto flush_round_tail_and_transition = [&]() -> bool {
            if (cur_t > g_pvp_eng.round_start_turn) {
                uint32_t last_t = cur_t - 1;
                uint8_t fcnt = static_cast<uint8_t>(
                    std::min<uint32_t>(8u, last_t - g_pvp_eng.round_start_turn + 1u));
                int32_t fhist[8] = {0};
                for (uint8_t i = 0; i < fcnt; ++i) {
                    fhist[i] = g_pvp_eng.local_inputs[(last_t - i) % hist_n];
                }
                net.send_input_bundle(rnd, last_t, fhist, fcnt, 0);
            }
            g_pvp_eng.stall_frames = 0;
            if (g_pvp_eng.core.winner != -2) {
                if (g_pvp_eng.core.ended_by_judge) {
                    g_pvp_eng.phase = Phase::JUDGE;
                    g_pvp_eng.judge_ticks = 0;
                } else {
                    conclude_current_game(rt, cp);
                }
                render_pvp_combat_frame(rt, cp);
                return true;
            }
            enter_custom_screen_phase(rt, cp);
            return true;
        };

        // Never commit Custom Screen or KO transition until all turns < cur_t are 100% confirmed!
        if (g_pvp_eng.core.winner != -2 || g_pvp_eng.core.open_custom_req) {
            if (g_pvp_eng.first_unconfirmed_turn < cur_t) {
                ++g_pvp_eng.stall_frames;
                net.request_missing_input(rnd, g_pvp_eng.first_unconfirmed_turn);
                render_pvp_combat_frame(rt, cp);
                if (g_pvp_eng.stall_frames >= 8) {
                    std::string sync_txt = "网络同步中...";
                    rt.fb_fill_rect_alpha(72, 108, 96, 22, 0xD8101830u);
                    rt.fb_draw_border(72, 108, 96, 22, 0xFFFFB84Cu);
                    rt.fb_draw_text(sync_txt, 120 - rt.fb_measure_text(sync_txt) / 2, 123, 0xFFFFE088u);
                }
                if (rt.pvp_auto_bot()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            }
            return flush_round_tail_and_transition();
        }

        // Resolve remote input for cur_t (confirmed vs predicted) and proactively NACK any gap
        size_t cur_slot = cur_t % hist_n;
        int32_t cur_rem_mask = 0;
        bool cur_rem_confirmed = g_pvp_eng.remote_confirmed[cur_slot];
        if (cur_rem_confirmed) {
            cur_rem_mask = g_pvp_eng.predicted_remote[cur_slot];
        } else if (net.get_remote_turn_input(rnd, cur_t, cur_rem_mask)) {
            cur_rem_confirmed = true;
        }

        uint32_t lag_turns = (cur_t > g_pvp_eng.first_unconfirmed_turn)
            ? (cur_t - g_pvp_eng.first_unconfirmed_turn)
            : (cur_rem_confirmed ? 0u : 1u);
        if (g_pvp_eng.first_unconfirmed_turn <= cur_t && !cur_rem_confirmed) {
            uint32_t miss_t = (g_pvp_eng.first_unconfirmed_turn < cur_t)
                ? g_pvp_eng.first_unconfirmed_turn
                : cur_t;
            if (lag_turns >= 4 || net.has_any_remote_turn_input_after(rnd, miss_t)) {
                net.request_missing_input(rnd, miss_t);
            }
        }

        if (!cur_rem_confirmed && lag_turns >= kMaxPredictionFrames) {
            ++g_pvp_eng.stall_frames;
            net.request_missing_input(rnd, g_pvp_eng.first_unconfirmed_turn);
            render_pvp_combat_frame(rt, cp);
            if (g_pvp_eng.stall_frames >= 3) {
                std::string sync_txt = "网络同步中...";
                rt.fb_fill_rect_alpha(72, 108, 96, 22, 0xD8101830u);
                rt.fb_draw_border(72, 108, 96, 22, 0xFFFFB84Cu);
                rt.fb_draw_text(sync_txt, 120 - rt.fb_measure_text(sync_txt) / 2, 123, 0xFFFFE088u);
            }
            if (rt.pvp_auto_bot()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return true;
        }
        g_pvp_eng.stall_frames = 0;

        // HCTOrganization/trill (getgud + throttler.rs) rollback clock-drift synchronization:
        //  1. Leading peer smoothly stretches sf_app_CpCanvas_mill by extra_wait_ms (0..38ms) using
        //     latency-invariant skew = local_adv - remote_adv gated by speculation_balance.
        //  2. Trailing peer with >= 2 confirmed future remote turns already buffered trims mill to catch up fast.
        {
            uint32_t max_rem_f = 0;
            bool has_rem_f = net.get_highest_remote_turn_frame(rnd, max_rem_f);
            int32_t fresh_rem_frontier = has_rem_f
                ? static_cast<int32_t>(max_rem_f)
                : static_cast<int32_t>(g_pvp_eng.round_start_turn);
            int32_t fresh_local_adv = static_cast<int32_t>(send_top) - fresh_rem_frontier;
            int16_t rem_adv = 0;
            int32_t skew = 0;
            if (net.get_remote_tick_advantage(rnd, rem_adv)) {
                skew = fresh_local_adv - static_cast<int32_t>(rem_adv);
            } else {
                int32_t one_way_ticks = (net.rtt_ms() > 0) ? ((net.rtt_ms() + 62) / 124) : 0;
                skew = 2 * (fresh_local_adv - one_way_ticks);
            }
            int64_t extra_wait_ms = g_pvp_eng.throttler.update_extra_wait_ms(skew, fresh_local_adv);
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

        if (!cur_rem_confirmed) {
            cur_rem_mask = (cur_t > g_pvp_eng.round_start_turn)
                ? g_pvp_eng.predicted_remote[(cur_t - 1) % hist_n]
                : g_pvp_eng.round_initial_remote_pred;
        }

        g_pvp_eng.predicted_remote[cur_slot] = cur_rem_mask;
        g_pvp_eng.remote_confirmed[cur_slot] = cur_rem_confirmed;
        if (cur_rem_confirmed && g_pvp_eng.first_unconfirmed_turn == cur_t) {
            g_pvp_eng.first_unconfirmed_turn = cur_t + 1;
        }

        int32_t cur_loc_mask = g_pvp_eng.local_inputs[cur_slot];
        int32_t p0_mask = (g_pvp_eng.local_idx == 0) ? cur_loc_mask : cur_rem_mask;
        int32_t p1_mask = (g_pvp_eng.local_idx == 1) ? cur_loc_mask : cur_rem_mask;

        // Save snapshot before stepping cur_t, then advance combat!
        save_rollback_slot(rt, cur_t, p0_mask, p1_mask, !cur_rem_confirmed);
        step_pvp_combat_tick(rt, cp, p0_mask, p1_mask);
        cur_t = g_pvp_eng.core.combat_turn;

        if ((g_pvp_eng.core.winner != -2 || g_pvp_eng.core.open_custom_req) &&
            g_pvp_eng.first_unconfirmed_turn >= cur_t) {
            return flush_round_tail_and_transition();
        }

        render_pvp_combat_frame(rt, cp);
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::JUDGE — Deterministic Hit Damage Judge Presentation (48 Ticks = 3s)
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::JUDGE) {
        if (!net.active() || (g_pvp_eng.active_room_id != 0 && net.room_id() != g_pvp_eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        net.poll_udp();
        if (net.peer_disconnected()) {
            g_pvp_eng.match_winner = g_pvp_eng.local_idx;
            g_pvp_eng.core.winner = g_pvp_eng.local_idx;
            g_pvp_eng.phase = Phase::RESULT;
            g_pvp_eng.result_ticks = 0;
            play_pvp_result_bgm(rt);
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
        }
        (void)rt.poll_local_pvp_keypad();
        render_pvp_combat_frame(rt, cp);

        int loc_i = g_pvp_eng.local_idx;
        int opp_i = 1 - loc_i;
        int32_t loc_dmg = g_pvp_eng.core.damage_dealt[loc_i];
        int32_t opp_dmg = g_pvp_eng.core.damage_dealt[opp_i];
        bool is_draw = (g_pvp_eng.core.winner == -1);
        bool is_win = (g_pvp_eng.core.winner == loc_i);

        rt.fb_fill_rect_alpha(16, 68, 208, 92, 0xF00E1830u);
        rt.fb_draw_border(16, 68, 208, 92, 0xFFFFD84Cu);

        std::string title = "TIME UP! 伤害判定 (JUDGE)";
        rt.fb_draw_text(title, 120 - rt.fb_measure_text(title) / 2, 88, 0xFFFFE866u);

        std::string loc_line = g_pvp_eng.builds[loc_i].player_name + " 有效伤害: " + std::to_string(loc_dmg);
        std::string opp_line = g_pvp_eng.builds[opp_i].player_name + " 有效伤害: " + std::to_string(opp_dmg);
        rt.fb_draw_text(loc_line, 26, 110, 0xFF88CCFFu);
        rt.fb_draw_text(opp_line, 26, 128, 0xFFFF98A8u);

        if (g_pvp_eng.judge_ticks >= 12) {
            uint32_t vcol = is_draw ? 0xFFFFD866u : (is_win ? 0xFF58FF98u : 0xFFFF6070u);
            std::string verdict = is_draw
                ? "判定结果: 平局 (DRAW)"
                : (is_win ? "判定结果: 己方胜利!" : "判定结果: 对手胜利...");
            rt.fb_draw_text(verdict, 120 - rt.fb_measure_text(verdict) / 2, 150, vcol);
        }

        if (rt.pvp_auto_bot() && !rt.fast_forward() && (g_pvp_eng.judge_ticks & 3) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        g_pvp_eng.judge_ticks++;
        if (g_pvp_eng.judge_ticks >= 48) {
            conclude_current_game(rt, cp);
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::ROUND_RESULT — Between-Game BO3 Scoreboard (48 Ticks = 3s)
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::ROUND_RESULT) {
        if (!net.active() || (g_pvp_eng.active_room_id != 0 && net.room_id() != g_pvp_eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        net.poll_udp();
        if (net.peer_disconnected()) {
            g_pvp_eng.match_winner = g_pvp_eng.local_idx;
            g_pvp_eng.core.winner = g_pvp_eng.local_idx;
            g_pvp_eng.phase = Phase::RESULT;
            g_pvp_eng.result_ticks = 0;
            play_pvp_result_bgm(rt);
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
        }
        (void)rt.poll_local_pvp_keypad();
        render_pvp_combat_frame(rt, cp);

        int loc_i = g_pvp_eng.local_idx;
        int g_win = (g_pvp_eng.bo3_game_index < 3)
            ? g_pvp_eng.bo3_game_results[g_pvp_eng.bo3_game_index]
            : g_pvp_eng.core.winner;
        uint32_t hdr_col = (g_win == -1) ? 0xFFFFD866u : ((g_win == loc_i) ? 0xFF58FF98u : 0xFFFF6070u);
        std::string line1 = "GAME " + std::to_string(g_pvp_eng.bo3_game_index + 1) +
            (g_win == -1 ? " - DRAW!" : (g_win == loc_i ? " - WIN!" : " - LOSE"));
        std::string line2 = "当前比分: " + std::to_string(g_pvp_eng.bo3_wins[loc_i]) + " - " +
            std::to_string(g_pvp_eng.bo3_wins[1 - loc_i]);
        std::string line3 = "即将进入第 " + std::to_string(g_pvp_eng.bo3_game_index + 2) + " 局...";

        rt.fb_fill_rect_alpha(18, 74, 204, 76, 0xEE0E1830u);
        rt.fb_draw_border(18, 74, 204, 76, 0xFF58B8FFu);
        rt.fb_draw_text(line1, 120 - rt.fb_measure_text(line1) / 2, 96, hdr_col);
        rt.fb_draw_text(line2, 120 - rt.fb_measure_text(line2) / 2, 118, 0xFFFFFFFFu);
        rt.fb_draw_text(line3, 120 - rt.fb_measure_text(line3) / 2, 140, 0xFF88CCFFu);

        if (rt.pvp_auto_bot() && !rt.fast_forward()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        g_pvp_eng.round_result_ticks++;
        if (g_pvp_eng.round_result_ticks >= 48) {
            start_next_bo3_game(rt, cp);
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::RESULT — Victory / Defeat / Draw Banner & Clean Single-Player Restore
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::RESULT) {
        render_pvp_combat_frame(rt, cp);

        int loc_i = g_pvp_eng.local_idx;
        int eff_win = (g_pvp_eng.match_winner != -2) ? g_pvp_eng.match_winner : g_pvp_eng.core.winner;
        bool is_draw = (eff_win == -1);
        bool is_win = (eff_win == loc_i);
        uint32_t border_col = is_draw ? 0xFFFFD866u : (is_win ? 0xFF58FF98u : 0xFFFF6070u);
        std::string headline = is_draw ? "DRAW GAME - 平局!" : (is_win ? "YOU WIN!! 对战胜利!" : "YOU LOSE... 对战落败");
        std::string subline = "按 决定键 返回游戏 (" + std::to_string(std::max(0, (96 - g_pvp_eng.result_ticks) / 16)) + "s)";

        if (g_pvp_eng.is_triple_battle) {
            std::string series_line = "三回战总比分 " + std::to_string(g_pvp_eng.bo3_wins[loc_i]) + " - " +
                std::to_string(g_pvp_eng.bo3_wins[1 - loc_i]);
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

        int32_t k = rt.poll_local_pvp_keypad();
        if (!net.active() || (g_pvp_eng.active_room_id != 0 && net.room_id() != g_pvp_eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        ++g_pvp_eng.result_ticks;
        if (rt.pvp_auto_bot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            if (g_pvp_eng.result_ticks >= 32) {
                int win = eff_win;
                uint32_t turns = g_pvp_eng.core.combat_turn;
                unsigned rnds = static_cast<unsigned>(g_pvp_eng.core.custom_round);
                int32_t hp0 = g_pvp_eng.core.sides[0].hp;
                int32_t hp1 = g_pvp_eng.core.sides[1].hp;
                int desyncs = g_pvp_eng.desync_warnings;
                unsigned games = static_cast<unsigned>(g_pvp_eng.bo3_game_index + 1);
                std::printf("[PvP BOT RESULT] local_idx=%d winner=%d turns=%u rounds=%u hp0=%d hp1=%d desyncs=%d bo3=%d games=%u w0=%u w1=%u\n",
                            loc_i, win, turns, rnds, hp0, hp1, desyncs,
                            g_pvp_eng.is_triple_battle ? 1 : 0, games,
                            static_cast<unsigned>(g_pvp_eng.bo3_wins[0]),
                            static_cast<unsigned>(g_pvp_eng.bo3_wins[1]));
                cleanup_and_restore_singleplayer(rt);
                rt.shutdown();
                std::fflush(stdout);
                std::fflush(stderr);
                std::exit(desyncs == 0 ? 0 : 2);
            }
            return true;
        }
        if (g_pvp_eng.result_ticks >= 96 || (g_pvp_eng.result_ticks >= 24 && (k & ((1 << 20) | (1 << 10))))) {
            cleanup_and_restore_singleplayer(rt);
        }
        return true;
    }

    return false;
}
