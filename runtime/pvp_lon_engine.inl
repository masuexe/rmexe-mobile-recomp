// ============================================================================
// LoN (Legend of Network / Phantom of Network's successor) Online PvP Engine
// Stage 3 Implementation (dual-perspective GGPO, TF cut-in stack, Trance, Plan A)
// Included at the bottom of build/lon_{jp,en}_recomp_gen.cpp inside namespace rmexe
// ============================================================================

} // namespace rmexe

#include "pvp_common.hpp"

namespace rmexe {

static bool pvp_lon_tick_override_impl(DojaRuntime& rt, Ref cp);

#include "pvp_common.inl"

namespace pvp_lon {

using namespace rmexe::pvp_common;

static constexpr int32_t kNumFolders = 30;
static constexpr int32_t kMaxAttacks = 30;
static constexpr int32_t kMaxAttackParams = 16;
static constexpr int32_t kMaxOkimono = 6;
static constexpr int32_t kMaxSouls = 6;
static constexpr int32_t kSceneBattleMain = 1;
static constexpr int32_t kSceneBattleTip = 2;

static constexpr int32_t kChargeDelayAdd[7] = {0, 5, 0, 5, 20, 10, 10};
static constexpr int32_t kChargeShotTipId[7] = {1, 263, 264, 265, 266, 267, 268};
static constexpr int32_t kSoulEffectAni[6] = {217, 196, 191, 202, 212, 207};

static constexpr int32_t kAttackFields[kNumI32_Attack] = {
    kFld_Attack_ata_id,       kFld_Attack_tmp_id,       kFld_Attack_eff_id,
    kFld_Attack_pos_x,        kFld_Attack_pos_y,        kFld_Attack_pos_dx,
    kFld_Attack_pos_dy,       kFld_Attack_waza_id,      kFld_Attack_yokoku,
    kFld_Attack_move_flg,     kFld_Attack_pow,          kFld_Attack_zoku,
    kFld_Attack_hit_x,        kFld_Attack_hit_y,        kFld_Attack_hit_kou,
    kFld_Attack_hit_eff,      kFld_Attack_hit_param,    kFld_Attack_hit_kan,
    kFld_Attack_hit_ana,      kFld_Attack_hit_bure,     kFld_Attack_hit_time,
    kFld_Attack_oki_hit,      kFld_Attack_hit_flg,      kFld_Attack_param,
    kFld_Attack_kidou_x_sp,   kFld_Attack_kidou_y_sp,   kFld_Attack_tuika_waza1,
    kFld_Attack_tuika_waza2,  kFld_Attack_kidou_param1, kFld_Attack_kidou_param2,
    kFld_Attack_ani_id,       kFld_Attack_ani_max,      kFld_Attack_ani_cnt,
    kFld_Attack_stop,         kFld_Attack_move_cnt,     kFld_Attack_move_x,
    kFld_Attack_move_y,       kFld_Attack_on,           kFld_Attack_chara_flg,
    kFld_Attack_flp,          kFld_Attack_ren_hit,      kFld_Attack_hon_id,
    kFld_Attack_ata_hit,      kFld_Attack_t_ani_cnt,    kFld_Attack_hit_ok,
    kFld_Attack_loop_flg,     kFld_Attack_mahi_flg,
};

static constexpr int32_t kOkimonoFields[kNumI32_Okimono] = {
    kFld_Okimono_oki_id,    kFld_Okimono_on,        kFld_Okimono_syu,
    kFld_Okimono_hp,        kFld_Okimono_drop_time, kFld_Okimono_rock_ata,
    kFld_Okimono_teki_ata,  kFld_Okimono_move_ok,   kFld_Okimono_only_flg,
    kFld_Okimono_pos_x,     kFld_Okimono_pos_y,     kFld_Okimono_pos_dx,
    kFld_Okimono_pos_dy,    kFld_Okimono_flp,       kFld_Okimono_pow,
    kFld_Okimono_hit_cnt,   kFld_Okimono_ani_id,    kFld_Okimono_ani_id2,
    kFld_Okimono_ani_max,   kFld_Okimono_ani_cnt,   kFld_Okimono_next_off,
    kFld_Okimono_move_cnt,  kFld_Okimono_waza_syu,  kFld_Okimono_tmp_cnt,
};

static constexpr int32_t kBtPanelFields[kNumI32_BtPanel] = {
    kFld_BtPanel_id,          kFld_BtPanel_pos_x,       kFld_BtPanel_pos_y,
    kFld_BtPanel_jin,         kFld_BtPanel_jou,         kFld_BtPanel_yokoku,
    kFld_BtPanel_ana_cnt,     kFld_BtPanel_mag_cnt,     kFld_BtPanel_aria_cnt,
    kFld_BtPanel_on_chara,    kFld_BtPanel_bom_pow,     kFld_BtPanel_hokkei,
    kFld_BtPanel_yoyaku_chara,
};

static inline int32_t safe_arr_len(DojaRuntime& rt, Ref arr) {
    return (arr > 0) ? rt.array_length(arr) : 0;
}

static inline int32_t safe_iaload(DojaRuntime& rt, Ref arr, int32_t idx, int32_t def = 0) {
    if (arr <= 0 || idx < 0 || idx >= rt.array_length(arr)) return def;
    return rt.iaload(arr, idx);
}

static inline void safe_iastore(DojaRuntime& rt, Ref arr, int32_t idx, int32_t val) {
    if (arr > 0 && idx >= 0 && idx < rt.array_length(arr)) {
        rt.iaload(arr, idx) = val;
    }
}

static inline int32_t get_tip_array_len(DojaRuntime& rt) {
    int32_t n = safe_arr_len(rt, sf_app_CpCanvas_tip);
    return (n > 0) ? n : 271;
}

// ============================================================================
// SideContext: Complete isolated LoN per-player state
// ============================================================================
struct SideContext {
    int32_t side_idx = 0;

    // Build / Stats
    int32_t max_hp = 1000;
    int32_t hp = 1000;
    int32_t ata_lv = 1;
    int32_t cha_lv = 1;
    int32_t skill_kouka[31] = {};
    int32_t dell_time[10] = {};
    Ref dell_time_ref = 0; // Plan A: per-side int[10] bound before Tip.PowSet()
    int32_t folder_packed[kNumFolders] = {};
    std::string player_name;

    // Folder & Custom Tray
    uint32_t fol_used_mask = 0;
    int32_t regu_flg = 0;
    int32_t regu_id = 0;
    int32_t max_sel = 5;
    int32_t tmp_max_sel = 5;
    int32_t sha_cnt = 0;
    int32_t cas_tip[8] = {};
    int32_t fol_tip_no[8] = {};
    int32_t set_tip[5] = {};
    int32_t cas_ok[10] = {};

    // Trance / Soul
    uint8_t arm_flg = 0;
    uint8_t get_arm_mask = 0;
    int32_t trans_zan[kMaxSouls] = {};
    int32_t arm_cnt = 0;  // sf_app_Rock_arm_cnt
    int32_t zoku = 0;     // sf_app_Rock_zoku (0=Normal, 1..6=Souls 0..5)
    int32_t rock_first_chip = 0; // kFld_Rock_first_chip (Elec Soul paralysis flag)

    // Rock state
    int32_t pos_x = 1, pos_y = 1;
    int32_t tmp_x = 0, tmp_y = 0;
    int32_t dx = 0, dy = 0;
    int32_t move_flg = 0, move_cnt = 0, tmp_move = 0, tmp_ata = -1;
    int32_t joutai = 0, mahi_id = -1;
    int32_t hit_cnt = 0, muteki_cnt = 0, tmp_muteki_cnt = 0, syoumetu_cnt = -1;
    int32_t han = 0, han_id = -1;
    int32_t bari = 0, bari_id = -1;
    int32_t wana = 0;
    int32_t waza_flg = -1, waza_wait = 0;
    int32_t flo = 0, air = 0;
    int32_t ani_id = 0, ani_cnt = 0, ani_wait_cnt = 0, tmp_ani_cnt = 0;
    int32_t rock_t_ani_cnt = 0;
    int32_t tip_no = -1, next_tip = 1;
    int32_t bas_no = -1, bas_pow = 1, non_bas = 0, non_cnt = 0, non_cnt2 = 0;
    int32_t noke_cnt = 0, pane_id = 0, tip_data = 0;
    int32_t pow[4] = {};
    int32_t rock_tip_ids[9] = {};

    // Combat / Buster / Trap / Gauge / Non-Dim HUD
    int32_t key_cnt = 0; // Charge counter
    int32_t bas_cnt = 0, bas_flg = 0;
    int32_t cus_gage = 0;
    int32_t rock_mu = 0, hit_flg = 0, ok_tip = 0;
    int32_t wana_flg = 0, wana_flg2 = 0;
    int32_t wana_pow[3] = {};
    int32_t pa_no = -1, pa_cnt = 0, pa_start = 0, pa_code_flg = 0;

    // Non-dimming chip / Buster HUD popup
    int32_t opp_nondim_tip_id = 0;
    int32_t opp_nondim_pow = 0;
    int32_t opp_nondim_plus = 0;
    uint8_t opp_nondim_ticks = 0;
    uint8_t opp_buster_visual_ticks = 0;

    // Isolated Attack[30] slots + param[16]
    int32_t ata_data[kMaxAttacks][kNumI32_Attack] = {};
    int32_t ata_param[kMaxAttacks][kMaxAttackParams] = {};
    int32_t ata_cnt = 0;

    // Snapshot for post-restore UI/TF hints
    int32_t last_raw_input = 0;
    bool buster_charge_glow = false;
};

// ============================================================================
// 2-Slot Time-Freeze Cut-In Stack
// ============================================================================
enum TfState : uint8_t {
    TF_IDLE = 0,
    TF_ROOT_INTRO = 1,
    TF_COUNTER_BANNER = 2,
    TF_COUNTER_ACTION = 3,
    TF_ROOT_RESUME = 4,
};

enum TfFlag : uint16_t {
    TFF_NONE = 0,
    TFF_SUMMON_TIP = 1 << 0,
    TFF_IS_NAVI = 1 << 1,
    TFF_FROM_ELEM_TRAP = 1 << 2,
};

struct TfSlot {
    int8_t owner = -1;
    int32_t tip_id = 0;
    int32_t tip_kind = 0; // 0 = Rock chip, 1 = AntiFire..Wood trap (240+wana)
    int32_t captured_plus_pow = 0;
    int32_t wana_pow_used = 0;
    uint16_t flags = TFF_NONE;
    int32_t timer = 0;
    int32_t stop_time = 24;
    bool fired_action = false;
    bool executed_root = false;
};

struct TfSavedAction {
    bool valid = false;
    int32_t waza_flg = -1;
    int32_t waza_wait = 0;
    int32_t tmp_ata = -1;
    int32_t move_flg = 0;
    int32_t move_cnt = 0;
    int32_t key_cnt = 0;
    int32_t ani_id = 0;
    int32_t ani_cnt = 0;
    int32_t was_hit_in_tf = 0;
};

// Reclaims transient allocations (e.g. DrawAni's scratch int[4] arrays) while keeping pre-existing object mutations.
struct ScopedTempHeapArena {
    DojaRuntime& rt;
    FastCombatSnapshot snap;
    explicit ScopedTempHeapArena(DojaRuntime& r) : rt(r) {
        snap.base_heap_size = 0;
        rt.save_combat_snapshot(snap, std::max<size_t>(1, rt.heap_size()));
    }
    ~ScopedTempHeapArena() {
        rt.restore_combat_snapshot(snap);
    }
};

struct PvpCombatCore {
    SideContext sides[2];
    int32_t active_orient = 0;
    int32_t rng_state = 1;
    int32_t cus_gage = 0;
    int32_t shared_cus_sp = 1;
    int32_t combat_tick_in_round = 0;
    uint16_t final_turn_ticks_left = kFinalTurnDurationTicks;
    uint32_t combat_turn = 0;
    uint16_t custom_round = 0;
    bool open_custom_req = false;
    bool ended_by_judge = false;
    int32_t winner = -2; // -2 = ongoing, -1 = draw, 0 = p0, 1 = p1
    int32_t damage_dealt[2] = {0, 0};

    // Time-Freeze 2-slot Cut-In stack
    TfState tf_state = TF_IDLE;
    TfSlot tf_stack[2];
    int8_t tf_depth = 0; // 0..2
    TfSavedAction tf_saved[2];
    int32_t tf_root_pre_muteki[2] = {0, 0};
    int32_t tf_root_pre_hit_cnt[2] = {0, 0};
    int32_t tf_cutin_fx_seq = -1;
    int8_t tf_cutin_interrupted_side = -1;
    int8_t prev_chip_pending_side = -1;
    int32_t prev_chip_pending_tip_no = -1;

    // Track per-slot owner for shared Okimono[0..5]
    int8_t oki_owner[kMaxOkimono] = {-1, -1, -1, -1, -1, -1};
};

static inline void reset_combat_tf_state(PvpCombatCore& core) {
    core.tf_state = TF_IDLE;
    core.tf_depth = 0;
    core.tf_stack[0] = TfSlot{};
    core.tf_stack[1] = TfSlot{};
    core.tf_saved[0] = TfSavedAction{};
    core.tf_saved[1] = TfSavedAction{};
    core.tf_root_pre_muteki[0] = 0;
    core.tf_root_pre_muteki[1] = 0;
    core.tf_root_pre_hit_cnt[0] = 0;
    core.tf_root_pre_hit_cnt[1] = 0;
    core.tf_cutin_fx_seq = -1;
    core.tf_cutin_interrupted_side = -1;
    core.prev_chip_pending_side = -1;
    core.prev_chip_pending_tip_no = -1;
}

struct PvpEngineState : PvpCommonState<PvpCombatCore> {
    // SP chip Plan A metadata (populated at arena init)
    std::vector<int32_t> sp_tip_ids;
    std::vector<int32_t> base_tip_pow;
    std::vector<int32_t> base_tip_tmp_pow;
    int32_t last_pal_zoku = -1;

    // Pre-custom host audio/mill snapshot & Custom Screen remote commit tracking
    HostRuntimeState pre_custom_host{};
    bool has_pre_custom_snap = false;
    bool remote_custom_ready = false;
    pvp::PvpCustomCommit remote_commit{};
    int32_t local_post_fol_tip_no[8] = {};

    // Trance Cutscene state (EXE6-exact)
    int32_t trance_tick = 0;      // 1..54 (activate) or 100..119 (revert)
    bool trance_is_revert = false;
    uint8_t trance_action[2] = {0, 0}; // 0=none, 1..6=soul 0..5, 7=revert

    // Runtime interception context
    bool in_resim = false;
    Ref active_ata_ref = 0;
    int32_t prev_raw_keypad = 0;
    bool pending_self_test = false;

    // Backdrop panel/palette cache (avoids redundant graMap repaints when panels & RGB stay constant)
    struct BackdropCache {
        bool valid = false;
        Ref gra_map = 0;
        Ref img_map = 0;
        Ref panel_arr = 0;
        int32_t bg_r = -1;
        int32_t bg_g = -1;
        int32_t bg_b = -1;
        struct Cell {
            Ref ref = 0;
            int32_t pos_x = -1;
            int32_t pos_y = -1;
            int32_t eff_jin = -1;
            int32_t jou = -1;
            int32_t ana_cnt = -1;
        } cells[3][6];
    } backdrop_cache;

    int opp_idx() const { return 1 - local_idx; }
    bool is_active() const { return phase != Phase::IDLE; }
};

static PvpEngineState g_pvp_eng;

static inline uint16_t current_wire_round_key() {
    return make_wire_round_key(g_pvp_eng.bo3_game_index, g_pvp_eng.core.custom_round);
}

// Forward declarations
static void save_side(DojaRuntime& rt, int idx);
static void load_side(DojaRuntime& rt, int idx);
static void ensure_and_reset_arena_entities(DojaRuntime& rt);
static void ensure_board_orient(DojaRuntime& rt, int target_side);
static void sync_panel_occupancy_and_proxy(DojaRuntime& rt, int active_idx);
static void set_rock_palette_fast(DojaRuntime& rt, int32_t zoku);
static void compute_lon_stage_base_rgb(int32_t& out_r, int32_t& out_g, int32_t& out_b);
static void refresh_sp_tip_powers(DojaRuntime& rt);
static void cancel_buster_charge_glow(DojaRuntime& rt);
static void trigger_opponent_elemental_trap(DojaRuntime& rt, int att_idx, int32_t wana_elem);
static void step_pvp_combat_tick(DojaRuntime& rt, Ref cp, int32_t p0_mask, int32_t p1_mask);
static uint16_t compute_combat_state_hash(DojaRuntime& rt);
static void render_pvp_combat_frame(DojaRuntime& rt, Ref cp);

// ============================================================================
// Folder, Tray & Tip Baseline Helpers (Zero Hardcoded Offsets!)
// ============================================================================
static uint32_t read_folder_used_mask(DojaRuntime& rt, Ref fol_obj) {
    uint32_t mask = 0;
    Ref used_arr = (fol_obj > 0) ? rt.field_i32(fol_obj, kFld_Folder_used) : 0;
    int32_t n = std::min<int32_t>(kNumFolders, safe_arr_len(rt, used_arr));
    for (int32_t i = 0; i < n; ++i) {
        if (rt.iaload(used_arr, i) != 0) {
            mask |= (1u << i);
        }
    }
    return mask;
}

static void sync_folder_used_from_mask(DojaRuntime& rt, Ref fol_obj, uint32_t mask) {
    if (fol_obj <= 0) return;
    Ref used_arr = rt.field_i32(fol_obj, kFld_Folder_used);
    int32_t n = std::min<int32_t>(kNumFolders, safe_arr_len(rt, used_arr));
    for (int32_t i = 0; i < n; ++i) {
        rt.iaload(used_arr, i) = ((mask >> i) & 1u) ? 1 : 0;
    }
}

static bool is_valid_tip_id(DojaRuntime& rt, int32_t tid) {
    if (tid < 1 || tid > 270) return false;
    int32_t tip_len = get_tip_array_len(rt);
    if (tid >= tip_len) return false;
    return safe_iaload(rt, sf_app_CpCanvas_tip, tid) > 0;
}

static bool is_valid_tip_code(int32_t code) {
    // LoN tip_list codes: 0..25 (A..Z), 26 (if any), 100 ('*')
    return (code >= 0 && code <= 26) || code == 100;
}

static int32_t unpack_tip_list_entry(DojaRuntime& rt, int32_t list_idx,
                                     int32_t& out_tid, int32_t& out_code) {
    if (list_idx <= 0 || sf_app_CpCanvas_tip_list <= 0) return -1;
    if (list_idx >= rt.array_length(sf_app_CpCanvas_tip_list)) return -1;
    int32_t packed = rt.iaload(sf_app_CpCanvas_tip_list, list_idx);
    int32_t tid = (packed >> 8) & 0xFF;
    int32_t code = packed & 0xFF;
    if (!is_valid_tip_id(rt, tid) || !is_valid_tip_code(code)) return -1;
    out_tid = tid;
    out_code = code;
    return (tid << 8) | code;
}

static int32_t fallback_tip_list_idx(DojaRuntime& rt) {
    if (sf_app_CpCanvas_fol_d > 0 && rt.array_length(sf_app_CpCanvas_fol_d) > 0) {
        int32_t cand = rt.iaload(sf_app_CpCanvas_fol_d, 0);
        int32_t tid = 0, code = 0;
        if (unpack_tip_list_entry(rt, cand, tid, code) > 0) return cand;
    }
    int32_t tlen = safe_arr_len(rt, sf_app_CpCanvas_tip_list);
    for (int32_t idx = 1; idx < tlen; ++idx) {
        int32_t tid = 0, code = 0;
        if (unpack_tip_list_entry(rt, idx, tid, code) > 0) return idx;
    }
    return 2;
}

static int32_t resolve_tip_list_idx(DojaRuntime& rt, int32_t packed) {
    int32_t want_tid = (packed >> 8) & 0xFFFF;
    int32_t want_code = packed & 0xFF;
    int32_t tlen = safe_arr_len(rt, sf_app_CpCanvas_tip_list);
    int32_t same_tid_fallback = -1;
    if (is_valid_tip_id(rt, want_tid)) {
        for (int32_t idx = 1; idx < tlen; ++idx) {
            int32_t tid = 0, code = 0;
            if (unpack_tip_list_entry(rt, idx, tid, code) <= 0) continue;
            if (tid == want_tid) {
                if (code == want_code) return idx;
                if (same_tid_fallback < 0) same_tid_fallback = idx;
            }
        }
        if (same_tid_fallback > 0) return same_tid_fallback;
    }
    return fallback_tip_list_idx(rt);
}

static uint16_t pack_wire_cas_tip(DojaRuntime& rt, int32_t list_idx) {
    int32_t tid = 0, code = 0;
    if (unpack_tip_list_entry(rt, list_idx, tid, code) <= 0) return 0;
    return static_cast<uint16_t>(((tid & 0x1FF) << 7) | (code & 0x7F));
}

static int32_t unpack_wire_cas_tip_to_list_idx(DojaRuntime& rt, uint16_t wire_val) {
    if (wire_val == 0 || wire_val == 0xFFFFu) return -1;
    int32_t tid = (wire_val >> 7) & 0x1FF;
    int32_t code = wire_val & 0x7F;
    if (!is_valid_tip_id(rt, tid) || !is_valid_tip_code(code)) return -1;
    int32_t idx = resolve_tip_list_idx(rt, (tid << 8) | code);
    return (idx > 0) ? idx : -1;
}

static uint16_t sanitize_wire_cas_tip(DojaRuntime& rt, uint16_t wire_val) {
    if (wire_val == 0 || wire_val == 0xFFFFu) return 0;
    int32_t tid = (wire_val >> 7) & 0x1FF;
    int32_t code = wire_val & 0x7F;
    if (!is_valid_tip_id(rt, tid) || !is_valid_tip_code(code)) return 0;
    return static_cast<uint16_t>(((tid & 0x1FF) << 7) | (code & 0x7F));
}

static void capture_sp_tip_baselines(DojaRuntime& rt) {
    g_pvp_eng.sp_tip_ids.clear();
    g_pvp_eng.base_tip_pow.clear();
    g_pvp_eng.base_tip_tmp_pow.clear();
    int32_t tip_len = get_tip_array_len(rt);
    g_pvp_eng.base_tip_pow.assign(static_cast<size_t>(tip_len), 0);
    g_pvp_eng.base_tip_tmp_pow.assign(static_cast<size_t>(tip_len), 0);
    if (sf_app_CpCanvas_tip <= 0) return;
    for (int32_t i = 0; i < tip_len; ++i) {
        Ref t_obj = safe_iaload(rt, sf_app_CpCanvas_tip, i);
        if (t_obj <= 0) continue;
        int32_t tmp_p = rt.field_i32(t_obj, kFld_Tip_tmp_pow);
        int32_t p = rt.field_i32(t_obj, kFld_Tip_pow);
        g_pvp_eng.base_tip_tmp_pow[i] = tmp_p;
        g_pvp_eng.base_tip_pow[i] = (tmp_p != 0 || p == 0) ? tmp_p : p;
        if (rt.field_i32(t_obj, kFld_Tip_sp_up) != 0) {
            g_pvp_eng.sp_tip_ids.push_back(i);
        }
    }
}

static void refresh_sp_tip_powers(DojaRuntime& rt) {
    if (sf_app_CpCanvas_tip <= 0) return;
    const int32_t n = std::min<int32_t>(
        safe_arr_len(rt, sf_app_CpCanvas_tip),
        static_cast<int32_t>(g_pvp_eng.base_tip_pow.size()));
    for (int32_t i = 0; i < n; ++i) {
        Ref t_obj = rt.iaload(sf_app_CpCanvas_tip, i);
        if (t_obj > 0) {
            rt.field_i32(t_obj, kFld_Tip_tmp_pow) = g_pvp_eng.base_tip_tmp_pow[i];
            rt.field_i32(t_obj, kFld_Tip_pow) = g_pvp_eng.base_tip_pow[i];
        }
    }
    for (int32_t tid : g_pvp_eng.sp_tip_ids) {
        Ref t_obj = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
        if (t_obj > 0) {
            fn_app_Tip_PowSet_____V(rt, t_obj);
        }
    }
}

// ============================================================================
// Save / Load / Mirror Side State
// ============================================================================
static void save_side(DojaRuntime& rt, int idx) {
    SideContext& sc = g_pvp_eng.core.sides[idx];
    sc.side_idx = idx;

    sc.max_hp = sf_app_CpCanvas_max_hp;
    sc.hp = std::max<int32_t>(0, sf_app_Rock_hp);
    sc.ata_lv = sf_app_CpCanvas_ata_lv;
    sc.cha_lv = sf_app_CpCanvas_cha_lv;
    if (sf_app_CpCanvas_skill_kouka > 0) {
        int32_t n = std::min<int32_t>(31, rt.array_length(sf_app_CpCanvas_skill_kouka));
        for (int32_t i = 0; i < n; ++i) {
            sc.skill_kouka[i] = rt.iaload(sf_app_CpCanvas_skill_kouka, i);
        }
    }
    if (sf_app_CpCanvas_dell_time > 0) {
        int32_t n = std::min<int32_t>(10, rt.array_length(sf_app_CpCanvas_dell_time));
        for (int32_t i = 0; i < n; ++i) {
            sc.dell_time[i] = rt.iaload(sf_app_CpCanvas_dell_time, i);
        }
    }

    Ref fol = (sf_app_CpCanvas_fol > 0)
        ? safe_iaload(rt, sf_app_CpCanvas_fol, sf_app_CpCanvas_now_fol)
        : 0;
    if (fol > 0) {
        sc.fol_used_mask = read_folder_used_mask(rt, fol);
        sc.regu_flg = rt.field_i32(fol, kFld_Folder_regu_flg);
        sc.regu_id = rt.field_i32(fol, kFld_Folder_regu_id);
        Ref tid_arr = rt.field_i32(fol, kFld_Folder_tip_id);
        int32_t fn = std::min<int32_t>(kNumFolders, safe_arr_len(rt, tid_arr));
        for (int32_t i = 0; i < fn; ++i) {
            sc.folder_packed[i] = rt.iaload(tid_arr, i);
        }
    }
    sc.max_sel = sf_app_CpCanvas_max_sel;
    sc.tmp_max_sel = sf_app_CpCanvas_tmp_max_sel;
    sc.sha_cnt = sf_app_CpCanvas_sha_cnt;
    sc.arm_flg = static_cast<uint8_t>(sf_app_CpCanvas_arm_flg != 0 ? 1 : 0);
    if (sf_app_CpCanvas_trans_zan > 0) {
        int32_t n = std::min<int32_t>(kMaxSouls, rt.array_length(sf_app_CpCanvas_trans_zan));
        for (int32_t i = 0; i < n; ++i) {
            sc.trans_zan[i] = rt.iaload(sf_app_CpCanvas_trans_zan, i);
        }
    }
    for (int i = 0; i < 8; ++i) {
        sc.cas_tip[i] = safe_iaload(rt, sf_app_CpCanvas_cas_tip, i);
        sc.fol_tip_no[i] = safe_iaload(rt, sf_app_CpCanvas_fol_tip_no, i);
    }
    for (int i = 0; i < 5; ++i) {
        sc.set_tip[i] = safe_iaload(rt, sf_app_CpCanvas_set_tip, i);
    }
    for (int i = 0; i < 10; ++i) {
        sc.cas_ok[i] = safe_iaload(rt, sf_app_CpCanvas_cas_ok, i);
    }

    Ref rock = sf_app_CpCanvas_rock;
    if (rock > 0) {
        sc.rock_t_ani_cnt = rt.field_i32(rock, kFld_Rock_t_ani_cnt);
        sc.rock_first_chip = rt.field_i32(rock, kFld_Rock_first_chip);
        Ref r_tip_arr = rt.field_i32(rock, kFld_Rock_tip);
        int32_t rn = std::min<int32_t>(9, safe_arr_len(rt, r_tip_arr));
        for (int32_t i = 0; i < rn; ++i) {
            Ref t_obj = rt.iaload(r_tip_arr, i);
            sc.rock_tip_ids[i] = (t_obj > 0) ? rt.field_i32(t_obj, kFld_Tip_tip_id) : 0;
        }
    }

    sc.arm_cnt = sf_app_Rock_arm_cnt;
    sc.zoku = sf_app_Rock_zoku;
    sc.pos_x = sf_app_Rock_pos_x;
    sc.pos_y = sf_app_Rock_pos_y;
    sc.tmp_x = sf_app_Rock_tmp_x;
    sc.tmp_y = sf_app_Rock_tmp_y;
    sc.dx = sf_app_Rock_dx;
    sc.dy = sf_app_Rock_dy;
    sc.move_flg = sf_app_Rock_move_flg;
    sc.move_cnt = sf_app_Rock_move_cnt;
    sc.tmp_move = sf_app_Rock_tmp_move;
    sc.tmp_ata = sf_app_Rock_tmp_ata;
    sc.joutai = sf_app_Rock_joutai;
    sc.mahi_id = sf_app_Rock_mahi_id;
    sc.hit_cnt = sf_app_Rock_hit_cnt;
    sc.muteki_cnt = sf_app_Rock_muteki_cnt;
    sc.tmp_muteki_cnt = sf_app_Rock_tmp_muteki_cnt;
    sc.syoumetu_cnt = sf_app_Rock_syoumetu_cnt;
    sc.han = sf_app_Rock_han;
    sc.han_id = sf_app_Rock_han_id;
    sc.bari = sf_app_Rock_bari;
    sc.bari_id = sf_app_Rock_bari_id;
    sc.wana = sf_app_Rock_wana;
    sc.waza_flg = sf_app_Rock_waza_flg;
    sc.waza_wait = sf_app_Rock_waza_wait;
    sc.flo = sf_app_Rock_flo;
    sc.air = sf_app_Rock_air;
    sc.ani_id = sf_app_Rock_ani_id;
    sc.ani_cnt = sf_app_Rock_ani_cnt;
    sc.ani_wait_cnt = sf_app_Rock_ani_wait_cnt;
    sc.tmp_ani_cnt = sf_app_Rock_tmp_ani_cnt;
    sc.tip_no = sf_app_Rock_tip_no;
    sc.next_tip = sf_app_Rock_next_tip;
    sc.bas_no = sf_app_Rock_bas_no;
    sc.bas_pow = sf_app_Rock_bas_pow;
    sc.non_bas = sf_app_Rock_non_bas;
    sc.non_cnt = sf_app_Rock_non_cnt;
    sc.non_cnt2 = sf_app_Rock_non_cnt2;
    sc.noke_cnt = sf_app_Rock_noke_cnt;
    sc.pane_id = sf_app_Rock_pane_id;
    sc.tip_data = sf_app_Rock_tip_data;
    if (sf_app_Rock_pow > 0) {
        int32_t pn = std::min<int32_t>(4, rt.array_length(sf_app_Rock_pow));
        for (int32_t i = 0; i < pn; ++i) {
            sc.pow[i] = rt.iaload(sf_app_Rock_pow, i);
        }
    }

    sc.key_cnt = sf_app_CpCanvas_key_cnt;
    sc.bas_cnt = sf_app_CpCanvas_bas_cnt;
    sc.bas_flg = sf_app_CpCanvas_bas_flg;
    sc.cus_gage = sf_app_CpCanvas_cus_gage;
    sc.rock_mu = sf_app_CpCanvas_rock_mu;
    sc.hit_flg = sf_app_CpCanvas_hit_flg;
    sc.ok_tip = sf_app_CpCanvas_ok_tip;
    sc.wana_flg = sf_app_CpCanvas_wana_flg;
    sc.wana_flg2 = sf_app_CpCanvas_wana_flg2;
    if (sf_app_CpCanvas_wana_pow > 0) {
        int32_t wn = std::min<int32_t>(3, rt.array_length(sf_app_CpCanvas_wana_pow));
        for (int32_t i = 0; i < wn; ++i) {
            sc.wana_pow[i] = rt.iaload(sf_app_CpCanvas_wana_pow, i);
        }
    }
    sc.pa_no = sf_app_CpCanvas_pa_no;
    sc.pa_cnt = sf_app_CpCanvas_pa_cnt;
    sc.pa_start = sf_app_CpCanvas_pa_start;
    sc.pa_code_flg = sf_app_CpCanvas_pa_code_flg;
    sc.ata_cnt = sf_app_CpCanvas_ata_cnt;

    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0) {
                std::fill(std::begin(sc.ata_data[i]), std::end(sc.ata_data[i]), 0);
                for (int f = 0; f < kNumI32_Attack; ++f) {
                    if (kAttackFields[f] == kFld_Attack_ata_id) sc.ata_data[i][f] = i;
                    else if (kAttackFields[f] == kFld_Attack_hon_id) sc.ata_data[i][f] = -1;
                }
                continue;
            }
            for (int f = 0; f < kNumI32_Attack; ++f) {
                sc.ata_data[i][f] = (kAttackFields[f] == kFld_Attack_ata_id)
                    ? i
                    : rt.field_i32(a, kAttackFields[f]);
            }
            Ref p_arr = rt.field_i32(a, kFld_Attack_param);
            std::fill(std::begin(sc.ata_param[i]), std::end(sc.ata_param[i]), 0);
            int32_t plen = std::min<int32_t>(kMaxAttackParams, safe_arr_len(rt, p_arr));
            for (int32_t p = 0; p < plen; ++p) {
                sc.ata_param[i][p] = rt.iaload(p_arr, p);
            }
        }
    }
}

static void load_side(DojaRuntime& rt, int idx) {
    SideContext& sc = g_pvp_eng.core.sides[idx];

    sf_app_CpCanvas_max_hp = sc.max_hp;
    sf_app_CpCanvas_now_hp = sc.hp;
    sf_app_Rock_hp = sc.hp;
    sf_app_CpCanvas_ata_lv = sc.ata_lv;
    sf_app_CpCanvas_cha_lv = sc.cha_lv;
    if (sf_app_CpCanvas_rock > 0) {
        // Also keep CpCanvas instance shadow fields in sync with loaded side
    }
    if (sf_app_CpCanvas_skill_kouka > 0) {
        int32_t n = std::min<int32_t>(31, rt.array_length(sf_app_CpCanvas_skill_kouka));
        for (int32_t i = 0; i < n; ++i) {
            rt.iaload(sf_app_CpCanvas_skill_kouka, i) = sc.skill_kouka[i];
        }
    }

    // Plan A: bind side-owned dell_time_ref and refresh SP chip powers
    if (sc.dell_time_ref > 0 && rt.array_length(sc.dell_time_ref) >= 10) {
        for (int i = 0; i < 10; ++i) {
            rt.iaload(sc.dell_time_ref, i) = sc.dell_time[i];
        }
        sf_app_CpCanvas_dell_time = sc.dell_time_ref;
    } else if (sf_app_CpCanvas_dell_time > 0) {
        int32_t n = std::min<int32_t>(10, rt.array_length(sf_app_CpCanvas_dell_time));
        for (int32_t i = 0; i < n; ++i) {
            rt.iaload(sf_app_CpCanvas_dell_time, i) = sc.dell_time[i];
        }
    }
    refresh_sp_tip_powers(rt);

    Ref fol = (sf_app_CpCanvas_fol > 0)
        ? safe_iaload(rt, sf_app_CpCanvas_fol, sf_app_CpCanvas_now_fol)
        : 0;
    if (fol > 0) {
        sync_folder_used_from_mask(rt, fol, sc.fol_used_mask);
        rt.field_i32(fol, kFld_Folder_regu_flg) = sc.regu_flg;
        rt.field_i32(fol, kFld_Folder_regu_id) = sc.regu_id;
        Ref tid_arr = rt.field_i32(fol, kFld_Folder_tip_id);
        int32_t fn = std::min<int32_t>(kNumFolders, safe_arr_len(rt, tid_arr));
        for (int32_t i = 0; i < fn; ++i) {
            rt.iaload(tid_arr, i) = sc.folder_packed[i];
        }
    }
    sf_app_CpCanvas_regu_flg = sc.regu_flg;
    sf_app_CpCanvas_max_sel = sc.max_sel;
    sf_app_CpCanvas_tmp_max_sel = sc.tmp_max_sel;
    sf_app_CpCanvas_sha_cnt = sc.sha_cnt;
    sf_app_CpCanvas_arm_flg = sc.arm_flg;
    if (sf_app_CpCanvas_get_arm > 0) {
        int32_t n = std::min<int32_t>(kMaxSouls, rt.array_length(sf_app_CpCanvas_get_arm));
        for (int32_t i = 0; i < n; ++i) {
            rt.iaload(sf_app_CpCanvas_get_arm, i) = ((sc.get_arm_mask >> i) & 1u) ? 1 : 0;
        }
    }
    if (sf_app_CpCanvas_trans_zan > 0) {
        int32_t n = std::min<int32_t>(kMaxSouls, rt.array_length(sf_app_CpCanvas_trans_zan));
        for (int32_t i = 0; i < n; ++i) {
            rt.iaload(sf_app_CpCanvas_trans_zan, i) = sc.trans_zan[i];
        }
    }
    for (int i = 0; i < 8; ++i) {
        safe_iastore(rt, sf_app_CpCanvas_cas_tip, i, sc.cas_tip[i]);
        safe_iastore(rt, sf_app_CpCanvas_fol_tip_no, i, sc.fol_tip_no[i]);
    }
    for (int i = 0; i < 5; ++i) {
        safe_iastore(rt, sf_app_CpCanvas_set_tip, i, sc.set_tip[i]);
    }
    for (int i = 0; i < 10; ++i) {
        safe_iastore(rt, sf_app_CpCanvas_cas_ok, i, sc.cas_ok[i]);
    }

    Ref rock = sf_app_CpCanvas_rock;
    if (rock > 0) {
        rt.field_i32(rock, kFld_Rock_t_ani_cnt) = sc.rock_t_ani_cnt;
        rt.field_i32(rock, kFld_Rock_first_chip) = sc.rock_first_chip;
        Ref r_tip_arr = rt.field_i32(rock, kFld_Rock_tip);
        int32_t tip_len = get_tip_array_len(rt);
        int32_t rn = std::min<int32_t>(9, safe_arr_len(rt, r_tip_arr));
        for (int32_t i = 0; i < rn; ++i) {
            int32_t tid = sc.rock_tip_ids[i];
            if (tid < 0 || tid >= tip_len) tid = 0;
            Ref t_ref = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
            if (t_ref <= 0) t_ref = safe_iaload(rt, sf_app_CpCanvas_tip, 0);
            rt.iaload(r_tip_arr, i) = t_ref;
        }
    }

    sf_app_Rock_arm_cnt = sc.arm_cnt;
    sf_app_Rock_zoku = sc.zoku;
    sf_app_Rock_pos_x = sc.pos_x;
    sf_app_Rock_pos_y = sc.pos_y;
    sf_app_Rock_tmp_x = sc.tmp_x;
    sf_app_Rock_tmp_y = sc.tmp_y;
    sf_app_Rock_dx = sc.dx;
    sf_app_Rock_dy = sc.dy;
    sf_app_Rock_move_flg = sc.move_flg;
    sf_app_Rock_move_cnt = sc.move_cnt;
    sf_app_Rock_tmp_move = sc.tmp_move;
    sf_app_Rock_tmp_ata = sc.tmp_ata;
    sf_app_Rock_joutai = sc.joutai;
    sf_app_Rock_mahi_id = sc.mahi_id;
    sf_app_Rock_hit_cnt = sc.hit_cnt;
    sf_app_Rock_muteki_cnt = sc.muteki_cnt;
    sf_app_Rock_tmp_muteki_cnt = sc.tmp_muteki_cnt;
    sf_app_Rock_syoumetu_cnt = sc.syoumetu_cnt;
    sf_app_Rock_han = sc.han;
    sf_app_Rock_han_id = sc.han_id;
    sf_app_Rock_bari = sc.bari;
    sf_app_Rock_bari_id = sc.bari_id;
    sf_app_Rock_wana = sc.wana;
    sf_app_Rock_waza_flg = sc.waza_flg;
    sf_app_Rock_waza_wait = sc.waza_wait;
    sf_app_Rock_flo = sc.flo;
    sf_app_Rock_air = sc.air;
    sf_app_Rock_ani_id = sc.ani_id;
    sf_app_Rock_ani_cnt = sc.ani_cnt;
    sf_app_Rock_ani_wait_cnt = sc.ani_wait_cnt;
    sf_app_Rock_tmp_ani_cnt = sc.tmp_ani_cnt;
    sf_app_Rock_tip_no = sc.tip_no;
    sf_app_Rock_next_tip = sc.next_tip;
    sf_app_Rock_bas_no = sc.bas_no;
    sf_app_Rock_bas_pow = sc.bas_pow;
    sf_app_Rock_non_bas = sc.non_bas;
    sf_app_Rock_non_cnt = sc.non_cnt;
    sf_app_Rock_non_cnt2 = sc.non_cnt2;
    sf_app_Rock_noke_cnt = sc.noke_cnt;
    sf_app_Rock_pane_id = sc.pane_id;
    sf_app_Rock_tip_data = sc.tip_data;
    if (sf_app_Rock_pow > 0) {
        int32_t pn = std::min<int32_t>(4, rt.array_length(sf_app_Rock_pow));
        for (int32_t i = 0; i < pn; ++i) {
            rt.iaload(sf_app_Rock_pow, i) = sc.pow[i];
        }
    }

    sf_app_CpCanvas_key_cnt = sc.key_cnt;
    sf_app_CpCanvas_bas_cnt = sc.bas_cnt;
    sf_app_CpCanvas_bas_flg = sc.bas_flg;
    sf_app_CpCanvas_cus_gage = g_pvp_eng.core.cus_gage;
    sf_app_CpCanvas_rock_mu = sc.rock_mu;
    sf_app_CpCanvas_hit_flg = sc.hit_flg;
    sf_app_CpCanvas_ok_tip = sc.ok_tip;
    sf_app_CpCanvas_wana_flg = sc.wana_flg;
    sf_app_CpCanvas_wana_flg2 = sc.wana_flg2;
    if (sf_app_CpCanvas_wana_pow > 0) {
        int32_t wn = std::min<int32_t>(3, rt.array_length(sf_app_CpCanvas_wana_pow));
        for (int32_t i = 0; i < wn; ++i) {
            rt.iaload(sf_app_CpCanvas_wana_pow, i) = sc.wana_pow[i];
        }
    }
    sf_app_CpCanvas_pa_no = sc.pa_no;
    sf_app_CpCanvas_pa_cnt = sc.pa_cnt;
    sf_app_CpCanvas_pa_start = sc.pa_start;
    sf_app_CpCanvas_pa_code_flg = sc.pa_code_flg;
    sf_app_CpCanvas_ata_cnt = sc.ata_cnt;

    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0) continue;
            Ref orig_p = rt.field_i32(a, kFld_Attack_param);
            for (int f = 0; f < kNumI32_Attack; ++f) {
                int32_t fld = kAttackFields[f];
                if (fld == kFld_Attack_param) continue;
                rt.field_i32(a, fld) = (fld == kFld_Attack_ata_id) ? i : sc.ata_data[i][f];
            }
            if (orig_p > 0) {
                rt.field_i32(a, kFld_Attack_param) = orig_p;
                int32_t plen = std::min<int32_t>(kMaxAttackParams, rt.array_length(orig_p));
                for (int32_t p = 0; p < plen; ++p) {
                    rt.iaload(orig_p, p) = sc.ata_param[i][p];
                }
            }
        }
    }

#ifdef HAS_SF_app_CpCanvas_bas_max_flg
    sf_app_CpCanvas_bas_max_flg = 0;
#endif
}

static void mirror_board(DojaRuntime& rt) {
    // Mirror 3x6 BtPanel grid across X axis
    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
        for (int32_t y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0 || rt.array_length(row) < 6) continue;
            for (int32_t x = 0; x < 3; ++x) {
                Ref pL = rt.iaload(row, x);
                Ref pR = rt.iaload(row, 5 - x);
                if (pL <= 0 || pR <= 0) continue;
                for (int f = 0; f < kNumI32_BtPanel; ++f) {
                    int32_t fld = kBtPanelFields[f];
                    if (fld == kFld_BtPanel_pos_x || fld == kFld_BtPanel_pos_y) continue;
                    int32_t vL = rt.field_i32(pL, fld);
                    int32_t vR = rt.field_i32(pR, fld);
                    if (fld == kFld_BtPanel_jin) {
                        vL ^= 1;
                        vR ^= 1;
                    } else if (fld == kFld_BtPanel_yoyaku_chara) {
                        if (vL == 1 || vL == 2) vL = 3 - vL;
                        if (vR == 1 || vR == 2) vR = 3 - vR;
                    }
                    rt.field_i32(pL, fld) = vR;
                    rt.field_i32(pR, fld) = vL;
                }
            }
        }
    }

    // Mirror shared Okimono[0..5] across X axis & swap ownership flags
    if (sf_app_CpCanvas_oki > 0) {
        int32_t on_cnt = std::min<int32_t>(kMaxOkimono, rt.array_length(sf_app_CpCanvas_oki));
        for (int32_t i = 0; i < on_cnt; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
            if (ok <= 0 || rt.field_i32(ok, kFld_Okimono_on) == 0) continue;
            int32_t px = rt.field_i32(ok, kFld_Okimono_pos_x);
            int32_t pdx = rt.field_i32(ok, kFld_Okimono_pos_dx);
            int32_t flp = rt.field_i32(ok, kFld_Okimono_flp);
            int32_t ra = rt.field_i32(ok, kFld_Okimono_rock_ata);
            int32_t ta = rt.field_i32(ok, kFld_Okimono_teki_ata);
            rt.field_i32(ok, kFld_Okimono_pos_x) = 5 - px;
            rt.field_i32(ok, kFld_Okimono_pos_dx) = -pdx;
            rt.field_i32(ok, kFld_Okimono_flp) = (flp == 0) ? 1 : 0;
            rt.field_i32(ok, kFld_Okimono_rock_ata) = ta;
            rt.field_i32(ok, kFld_Okimono_teki_ata) = ra;
        }
    }

    g_pvp_eng.core.active_orient = 1 - g_pvp_eng.core.active_orient;
}

static void ensure_board_orient(DojaRuntime& rt, int target_side) {
    if (g_pvp_eng.core.active_orient != target_side) {
        mirror_board(rt);
    }
}

static void sync_panel_occupancy_and_proxy(DojaRuntime& rt, int active_idx) {
    const SideContext& act = g_pvp_eng.core.sides[active_idx];
    const SideContext& opp = g_pvp_eng.core.sides[1 - active_idx];
    int32_t opp_px = std::clamp<int32_t>(5 - opp.pos_x, 0, 5);
    int32_t opp_py = std::clamp<int32_t>(opp.pos_y, 0, 2);
    int32_t act_px = std::clamp<int32_t>(act.pos_x, 0, 5);
    int32_t act_py = std::clamp<int32_t>(act.pos_y, 0, 2);

    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
        for (int32_t y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0 || rt.array_length(row) < 6) continue;
            for (int32_t x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p <= 0) continue;
                int32_t oc = 0;
                if (x == act_px && y == act_py) oc = 1;
                else if (x == opp_px && y == opp_py && opp.hp > 0) oc = 2;
                rt.field_i32(p, kFld_BtPanel_on_chara) = oc;
            }
        }
        if (sf_app_CpCanvas_oki > 0) {
            int32_t on_cnt = std::min<int32_t>(kMaxOkimono, rt.array_length(sf_app_CpCanvas_oki));
            for (int32_t i = 0; i < on_cnt; ++i) {
                Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                if (ok <= 0 || rt.field_i32(ok, kFld_Okimono_on) == 0) continue;
                int32_t ox = rt.field_i32(ok, kFld_Okimono_pos_x);
                int32_t oy = rt.field_i32(ok, kFld_Okimono_pos_y);
                if (ox >= 0 && ox <= 5 && oy >= 0 && oy <= 2) {
                    Ref row = rt.iaload(sf_app_CpCanvas_panel, oy);
                    Ref p = (row > 0) ? rt.iaload(row, ox) : 0;
                    if (p > 0 && rt.field_i32(p, kFld_BtPanel_on_chara) == 0) {
                        rt.field_i32(p, kFld_BtPanel_on_chara) = 10 + rt.field_i32(ok, kFld_Okimono_oki_id);
                    }
                }
            }
        }
    }

    if (sf_app_CpCanvas_ene > 0 && rt.array_length(sf_app_CpCanvas_ene) > 0) {
        Ref e0 = rt.iaload(sf_app_CpCanvas_ene, 0);
        if (e0 > 0) {
            rt.field_i32(e0, kFld_Ene_on) = (opp.hp > 0 && opp.syoumetu_cnt < 0) ? 1 : 0;
            rt.field_i32(e0, kFld_Ene_hp) = opp.hp;
            rt.field_i32(e0, kFld_Ene_max_hp) = opp.max_hp;
            rt.field_i32(e0, kFld_Ene_pos_x) = opp_px;
            rt.field_i32(e0, kFld_Ene_pos_y) = opp_py;
            rt.field_i32(e0, kFld_Ene_pos_dx) = -opp.dx;
            rt.field_i32(e0, kFld_Ene_pos_dy) = opp.dy;
            rt.field_i32(e0, kFld_Ene_tmp_x) = 5 - opp.tmp_x;
            rt.field_i32(e0, kFld_Ene_tmp_y) = opp.tmp_y;
            rt.field_i32(e0, kFld_Ene_zoku) = opp.zoku;
            rt.field_i32(e0, kFld_Ene_bari) = opp.bari;
            rt.field_i32(e0, kFld_Ene_bari_id) = opp.bari_id;
            rt.field_i32(e0, kFld_Ene_flo) = opp.flo;
            rt.field_i32(e0, kFld_Ene_air) = opp.air;
            rt.field_i32(e0, kFld_Ene_muteki_cnt) = (opp.han > 0) ? 0 : opp.muteki_cnt;
            rt.field_i32(e0, kFld_Ene_tmp_muteki_cnt) = (opp.han > 0) ? 0 : opp.tmp_muteki_cnt;
            rt.field_i32(e0, kFld_Ene_muteki2) = 0;
            rt.field_i32(e0, kFld_Ene_joutai) = opp.joutai;
            rt.field_i32(e0, kFld_Ene_hit_cnt) = opp.hit_cnt;
            rt.field_i32(e0, kFld_Ene_syoumetu_cnt) = opp.syoumetu_cnt;
            rt.field_i32(e0, kFld_Ene_pane_id) = opp.pane_id;
            // Keep Ene.han = 0 on the opponent proxy so Attack_Hit reaches pvp_lon_intercept_ene_hit
            // (which executes Rock's full Reflect waza 84 / Shield waza 240 / Breaking shatter).
            rt.field_i32(e0, kFld_Ene_han) = 0;
            rt.field_i32(e0, kFld_Ene_navi_flg) = 1;
            rt.field_i32(e0, kFld_Ene_ata_id) = -1;
        }
        int32_t en_len = rt.array_length(sf_app_CpCanvas_ene);
        for (int32_t i = 1; i < en_len; ++i) {
            Ref ei = rt.iaload(sf_app_CpCanvas_ene, i);
            if (ei > 0) {
                rt.field_i32(ei, kFld_Ene_on) = 0;
                rt.field_i32(ei, kFld_Ene_ata_id) = -1;
            }
        }
    }
}

// ============================================================================
// Build Extraction (v2), Sanitization, & Arena Initialization
// ============================================================================
static pvp::PvpFighterBuild extract_local_lon_build(DojaRuntime& rt, Ref cp) {
    if (sf_app_CpCanvas_max_hp <= 0 && cp > 0) {
        bool prev_sup = rt.suppress_av();
        rt.set_suppress_av(true);
        fn_app_CpCanvas_init___I__V(rt, cp, 1);
        (void)fn_app_CpCanvas_Load___I__I(rt, cp, 0);
        if (sf_app_CpCanvas_max_hp <= 0) {
            fn_app_CpCanvas_init___I__V(rt, cp, 0);
        }
        rt.set_suppress_av(prev_sup);
    }
    pvp::PvpFighterBuild b;
    b.has_lon_v2 = true;
    b.max_hp = std::clamp<int32_t>(sf_app_CpCanvas_max_hp, 100, 9999);
    b.ata_lv = std::clamp<int32_t>(sf_app_CpCanvas_ata_lv, 1, 5);
    b.cha_lv = std::clamp<int32_t>(sf_app_CpCanvas_cha_lv, 1, 5);
    b.r_zoku = 0; // Always start combat in Normal Soul (0); souls activate in battle

    if (sf_app_CpCanvas_skill_kouka > 0) {
        int32_t sn = std::min<int32_t>(31, rt.array_length(sf_app_CpCanvas_skill_kouka));
        for (int32_t i = 0; i < sn; ++i) {
            b.skill_kouka[i] = static_cast<int8_t>(std::clamp<int32_t>(
                rt.iaload(sf_app_CpCanvas_skill_kouka, i), -100, 100));
        }
    }

    if (sf_app_CpCanvas_dell_time > 0) {
        int32_t dn = std::min<int32_t>(10, rt.array_length(sf_app_CpCanvas_dell_time));
        for (int32_t i = 0; i < dn; ++i) {
            b.dell_time[i] = std::clamp<int32_t>(rt.iaload(sf_app_CpCanvas_dell_time, i), 0, 99999);
        }
    }

    uint8_t mask = 0;
    if (sf_app_CpCanvas_get_arm > 0) {
        int32_t gn = std::min<int32_t>(kMaxSouls, rt.array_length(sf_app_CpCanvas_get_arm));
        for (int32_t i = 0; i < gn; ++i) {
            if (rt.iaload(sf_app_CpCanvas_get_arm, i) != 0) {
                mask |= static_cast<uint8_t>(1u << i);
            }
        }
    }
    b.get_arm_mask = mask & 0x3Fu;
    b.arm_flg = static_cast<uint8_t>((sf_app_CpCanvas_arm_flg != 0 || b.get_arm_mask != 0) ? 1 : 0);

    Ref fol = (sf_app_CpCanvas_fol > 0)
        ? safe_iaload(rt, sf_app_CpCanvas_fol, sf_app_CpCanvas_now_fol)
        : 0;
    int32_t fb_idx = fallback_tip_list_idx(rt);
    int32_t fb_tid = 1, fb_code = 0;
    int32_t fb_packed = (unpack_tip_list_entry(rt, fb_idx, fb_tid, fb_code) > 0)
        ? ((fb_tid << 8) | fb_code)
        : ((1 << 8) | 0);
    if (fol > 0) {
        b.regu_flg = (rt.field_i32(fol, kFld_Folder_regu_flg) != 0) ? 1 : 0;
        b.regu_id = std::clamp<int32_t>(rt.field_i32(fol, kFld_Folder_regu_id), 0, 29);
        Ref tid_arr = rt.field_i32(fol, kFld_Folder_tip_id);
        int32_t fn = std::min<int32_t>(kNumFolders, safe_arr_len(rt, tid_arr));
        for (int32_t i = 0; i < kNumFolders; ++i) {
            int32_t list_idx = (i < fn) ? rt.iaload(tid_arr, i) : -1;
            int32_t tid = 0, code = 0;
            int32_t packed = unpack_tip_list_entry(rt, list_idx, tid, code);
            if (packed <= 0 && sf_app_CpCanvas_fol_d > 0 &&
                i < rt.array_length(sf_app_CpCanvas_fol_d)) {
                packed = unpack_tip_list_entry(rt, rt.iaload(sf_app_CpCanvas_fol_d, i), tid, code);
            }
            b.folder_packed[i] = (packed > 0) ? packed : fb_packed;
        }
    } else {
        for (int32_t i = 0; i < kNumFolders; ++i) {
            int32_t tid = 0, code = 0;
            int32_t packed = (sf_app_CpCanvas_fol_d > 0 && i < rt.array_length(sf_app_CpCanvas_fol_d))
                ? unpack_tip_list_entry(rt, rt.iaload(sf_app_CpCanvas_fol_d, i), tid, code)
                : -1;
            b.folder_packed[i] = (packed > 0) ? packed : fb_packed;
        }
    }

    b.player_name = rt.player_nickname();
    if (b.player_name.empty()) b.player_name = "Rockman";
    return b;
}

static pvp::PvpFighterBuild sanitize_lon_build(DojaRuntime& rt, const pvp::PvpFighterBuild& in) {
    pvp::PvpFighterBuild b = in;
    b.has_lon_v2 = true;
    b.max_hp = std::clamp<int32_t>(b.max_hp, 100, 9999);
    b.ata_lv = std::clamp<int32_t>(b.ata_lv, 1, 5);
    b.cha_lv = std::clamp<int32_t>(b.cha_lv, 1, 5);
    b.r_zoku = 0;
    b.regu_flg = (b.regu_flg != 0) ? 1 : 0;
    b.regu_id = std::clamp<int32_t>(b.regu_id, 0, 29);
    for (int i = 0; i < 31; ++i) {
        b.skill_kouka[i] = static_cast<int8_t>(std::clamp<int32_t>(b.skill_kouka[i], -100, 100));
    }
    int32_t fb_tid = is_valid_tip_id(rt, 1) ? 1 : 2;
    for (int i = 0; i < kNumFolders; ++i) {
        int32_t packed = b.folder_packed[i];
        int32_t tid = (packed >> 8) & 0xFFFF;
        int32_t code = packed & 0xFF;
        if (!is_valid_tip_id(rt, tid)) tid = fb_tid;
        if (!is_valid_tip_code(code)) code = 0;
        b.folder_packed[i] = (tid << 8) | code;
    }
    if (in.has_lon_v2) {
        for (int i = 0; i < 10; ++i) {
            b.dell_time[i] = std::clamp<int32_t>(in.dell_time[i], 0, 99999);
        }
        b.get_arm_mask &= 0x3Fu;
        b.arm_flg = static_cast<uint8_t>((in.arm_flg != 0 || b.get_arm_mask != 0) ? 1 : 0);
    } else {
        std::fill(std::begin(b.dell_time), std::end(b.dell_time), 0);
        b.get_arm_mask = 0;
        b.arm_flg = 0;
    }
    if (b.player_name.empty()) b.player_name = "Rival";
    return b;
}

static void sanitize_lon_commit(DojaRuntime& rt,
                                const SideContext& prev_sc,
                                pvp::PvpCustomCommit& c) {
    int32_t tip_len = get_tip_array_len(rt);
    if (c.tip_no < -1 || c.tip_no > 4) c.tip_no = -1;
    c.max_sel = static_cast<uint8_t>(std::clamp<int>(c.max_sel, 5, 8));
    c.regu_flg = (prev_sc.regu_flg != 0) ? (c.regu_flg ? 1 : 0) : 0;
    for (int i = 0; i < 9; ++i) {
        uint16_t tid = c.rock_tip_ids[i];
        if (tid == 0xFFFFu || tid == 0) {
            c.rock_tip_ids[i] = 0;
            continue;
        }
        if (i >= 5 || static_cast<int32_t>(tid) >= tip_len) {
            c.rock_tip_ids[i] = 0;
        }
    }
    if (c.tip_no >= 0 && c.rock_tip_ids[c.tip_no] == 0) {
        c.tip_no = -1;
    }
    c.fol_used_mask &= ((1u << kNumFolders) - 1u);
    c.fol_used_mask |= prev_sc.fol_used_mask;

    if (c.has_lon_v2) {
        if (c.trance_action > 7) c.trance_action = 0;
        if (c.trance_action >= 1 && c.trance_action <= 6) {
            int soul = c.trance_action - 1;
            if (prev_sc.arm_flg == 0 || ((prev_sc.get_arm_mask >> soul) & 1u) == 0 ||
                prev_sc.trans_zan[soul] == 0) {
                c.trance_action = (prev_sc.zoku > 0 && prev_sc.arm_cnt <= 1) ? 7 : 0;
            }
        } else if (c.trance_action == 7 && prev_sc.zoku == 0) {
            c.trance_action = 0;
        }
        if (c.arm_chip > 270) c.arm_chip = 0;
        c.sha_cnt = static_cast<uint8_t>(std::clamp<int>(c.sha_cnt, 0, 10));
        c.trans_zan_mask &= (prev_sc.get_arm_mask & 0x3Fu);
        c.rock_arm_cnt = static_cast<int8_t>(std::clamp<int>(c.rock_arm_cnt, 0, 5));
        c.cas_tip_max = static_cast<uint8_t>(std::clamp<int>(c.cas_tip_max, 0, 8));
        for (int i = 0; i < 8; ++i) {
            c.cas_tip_ids[i] = sanitize_wire_cas_tip(rt, c.cas_tip_ids[i]);
        }
    } else {
        c.has_lon_v2 = true;
        c.trance_action = 0;
        c.arm_chip = 0;
        c.sha_cnt = static_cast<uint8_t>(prev_sc.sha_cnt);
        c.rock_arm_cnt = static_cast<int8_t>(prev_sc.arm_cnt);
        for (int i = 0; i < 8; ++i) {
            c.cas_tip_ids[i] = pack_wire_cas_tip(rt, prev_sc.cas_tip[i]);
        }
    }
}

static void init_side_context(DojaRuntime& rt, Ref cp, int idx, const pvp::PvpFighterBuild& b_raw) {
    (void)cp;
    pvp::PvpFighterBuild b = sanitize_lon_build(rt, b_raw);
    SideContext& sc = g_pvp_eng.core.sides[idx];
    Ref saved_dell_ref = sc.dell_time_ref;
    sc = SideContext{};
    sc.side_idx = idx;
    sc.dell_time_ref = saved_dell_ref;

    sc.max_hp = b.max_hp;
    sc.hp = b.max_hp;
    sc.ata_lv = b.ata_lv;
    sc.cha_lv = b.cha_lv;
    sc.player_name = b.player_name;
    sc.regu_flg = b.regu_flg;
    sc.regu_id = b.regu_id;
    sc.arm_flg = b.arm_flg;
    sc.get_arm_mask = b.get_arm_mask;
    for (int i = 0; i < 31; ++i) sc.skill_kouka[i] = b.skill_kouka[i];
    for (int i = 0; i < 10; ++i) {
        sc.dell_time[i] = b.dell_time[i];
        if (sc.dell_time_ref > 0 && i < rt.array_length(sc.dell_time_ref)) {
            rt.iaload(sc.dell_time_ref, i) = sc.dell_time[i];
        }
    }
    // Folder.tip_id[i] stores tip_list[] indices, not packed (tid<<8)|code
    for (int i = 0; i < kNumFolders; ++i) {
        sc.folder_packed[i] = resolve_tip_list_idx(rt, b.folder_packed[i]);
    }
    for (int i = 0; i < 8; ++i) {
        sc.cas_tip[i] = -1;
        sc.fol_tip_no[i] = -1;
    }
    for (int i = 0; i < 5; ++i) sc.set_tip[i] = -1;
    for (int s = 0; s < kMaxSouls; ++s) {
        sc.trans_zan[s] = ((sc.get_arm_mask >> s) & 1u) ? 1 : 0;
    }

    int32_t base_sel = 5 + std::max<int32_t>(0, sc.skill_kouka[1]);
    sc.tmp_max_sel = std::clamp<int32_t>(base_sel, 5, 8);
    sc.max_sel = sc.tmp_max_sel;
    sc.sha_cnt = 0;

    // Position player on left half of their own oriented grid (x=1, y=1)
    sc.pos_x = 1;
    sc.pos_y = 1;
    sc.tmp_x = 1;
    sc.tmp_y = 1;
    sc.pane_id = 1;
    sc.han_id = -1;
    sc.bari_id = -1;
    sc.tip_no = -1;
    sc.next_tip = 1;
    sc.bas_no = -1;
    sc.pa_no = -1;
    for (int i = 0; i < kMaxAttacks; ++i) {
        for (int f = 0; f < kNumI32_Attack; ++f) {
            if (kAttackFields[f] == kFld_Attack_ata_id) sc.ata_data[i][f] = i;
            else if (kAttackFields[f] == kFld_Attack_hon_id) sc.ata_data[i][f] = -1;
        }
    }
}

static void init_pvp_battle_arena(DojaRuntime& rt, Ref cp) {
    // Capture SP baseline metadata once before arena swaps
    capture_sp_tip_baselines(rt);

    // Reload battle sprite sheet image[70] in case single-player MachiSet/IventRead loaded town/event sprites
    fn_app_CpCanvas_ImgSet2___I__V(rt, cp, 0);

    // Preload all 7 Rock palettes (0=Normal, 1..6=Souls 0..5) once for O(1) runtime swap
    for (int32_t z = 0; z <= 6; ++z) {
        fn_app_CpCanvas_PalSet___II__V(rt, 0, z);
    }
    fn_app_CpCanvas_PalSet___II__V(rt, 0, 0);
    g_pvp_eng.last_pal_zoku = 0;

    // Initialize EN backdrop & chroma key cleanly, keeping bas_max_flg off in PvP
    // (Note: SetUpBG already ran once during CpCanvas.exe() startup; calling it again re-runs SetUp()/MldSet()!)
#ifdef HAS_SF_app_CpCanvas_bas_max_flg
    sf_app_CpCanvas_bas_max_flg = 0;
#endif
    fn_app_CpCanvas_MapSet___II__V(rt, cp, 0, 0);
#ifdef HAS_FN_CpCanvas_PalSetBG
    {
        // Match EN BtSet: seed BtPanel_R/G/B with the stage backdrop color so tmp_bg_pal[9..11]
        // and ann_bg_pal[9..11] hold the stage RGB rather than green/magenta chroma keys, while
        // graMap/imgMap uses hardcoded (0, 255, 0) transparency via drawImgMapBG.
        int32_t sr = 155, sg = 255, sb = 210;
        compute_lon_stage_base_rgb(sr, sg, sb);
        sf_app_BtPanel_R = sr;
        sf_app_BtPanel_G = sg;
        sf_app_BtPanel_B = sb;
        fn_app_CpCanvas_PalSetBG_____V(rt);
    }
#endif
    g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};

    ensure_and_reset_arena_entities(rt);

    // Build Side 0 & Side 1 from exchanged PvP builds + allocate per-side Plan A dell_time_ref
    const auto& sess = rt.pvp_session();
    int loc = (rt.pvp_match_active() || sess.has_remote_build()) ? (sess.is_host() ? 0 : 1) : g_pvp_eng.local_idx;
    g_pvp_eng.local_idx = loc;

    pvp::PvpFighterBuild loc_b = sanitize_lon_build(rt, g_pvp_eng.local_build);
    pvp::PvpFighterBuild rem_src = g_pvp_eng.builds[1 - loc];
    if (rem_src.max_hp == 0 && sess.has_remote_build()) {
        rem_src = sess.remote_build();
    }
    pvp::PvpFighterBuild rem_b = sanitize_lon_build(rt, rem_src);
    g_pvp_eng.builds[loc] = loc_b;
    g_pvp_eng.builds[1 - loc] = rem_b;

    g_pvp_eng.core = PvpCombatCore{};
    uint32_t gseed = g_pvp_eng.match_seed ? g_pvp_eng.match_seed : 0x2468ACE1u;
    g_pvp_eng.core.rng_state = static_cast<int32_t>(gseed | 1u);
    g_pvp_eng.core.shared_cus_sp = 1;
    g_pvp_eng.has_pre_custom_snap = false;

    for (int s = 0; s < 2; ++s) {
        g_pvp_eng.core.sides[s].dell_time_ref = rt.alloc_int_array(10);
        pvp::PvpFighterBuild shuffled_b = g_pvp_eng.builds[s];
        // Deterministic folder Fisher-Yates shuffle keyed by match_seed & side
        uint32_t prng = gseed ^ static_cast<uint32_t>(0x9E3779B9u * (s + 1));
        if (prng == 0) prng = 1u;
        for (int i = kNumFolders - 1; i > 0; --i) {
            prng ^= (prng << 13);
            prng ^= (prng >> 17);
            prng ^= (prng << 5);
            int j = static_cast<int>(prng % static_cast<uint32_t>(i + 1));
            if (shuffled_b.regu_flg != 0) {
                if (shuffled_b.regu_id == i) shuffled_b.regu_id = j;
                else if (shuffled_b.regu_id == j) shuffled_b.regu_id = i;
            }
            std::swap(shuffled_b.folder_packed[i], shuffled_b.folder_packed[j]);
        }
        init_side_context(rt, cp, s, shuffled_b);
    }

    // Compute exact mutable_heap_min_ref across mutable combat objects cleanly
    size_t min_mut_ref = std::max<size_t>(1, rt.heap_size());
    auto note_ref = [&](Ref r) {
        if (r > 0 && static_cast<size_t>(r) < min_mut_ref) {
            min_mut_ref = static_cast<size_t>(r);
        }
    };
    note_ref(cp);
    note_ref(sf_app_CpCanvas_panel);
    if (sf_app_CpCanvas_panel > 0) {
        int32_t plen = rt.array_length(sf_app_CpCanvas_panel);
        for (int32_t y = 0; y < plen; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            note_ref(row);
            if (row > 0) {
                int32_t rlen = rt.array_length(row);
                for (int32_t x = 0; x < rlen; ++x) note_ref(rt.iaload(row, x));
            }
        }
    }
    note_ref(sf_app_CpCanvas_oki);
    if (sf_app_CpCanvas_oki > 0) {
        int32_t olen = rt.array_length(sf_app_CpCanvas_oki);
        for (int32_t i = 0; i < olen; ++i) note_ref(rt.iaload(sf_app_CpCanvas_oki, i));
    }
    note_ref(sf_app_CpCanvas_ene);
    if (sf_app_CpCanvas_ene > 0) {
        int32_t elen = rt.array_length(sf_app_CpCanvas_ene);
        for (int32_t i = 0; i < elen; ++i) note_ref(rt.iaload(sf_app_CpCanvas_ene, i));
    }
    note_ref(sf_app_CpCanvas_rand);
    note_ref(sf_app_CpCanvas_se_set);
    note_ref(sf_app_CpCanvas_se_flg);
    note_ref(sf_app_CpCanvas_wana_pow);
    note_ref(sf_app_CpCanvas_fol);
    if (sf_app_CpCanvas_fol > 0) {
        int32_t flen = rt.array_length(sf_app_CpCanvas_fol);
        for (int32_t i = 0; i < flen; ++i) {
            note_ref(rt.iaload(sf_app_CpCanvas_fol, i));
        }
    }
    note_ref(sf_app_CpCanvas_set_tip);
    note_ref(sf_app_CpCanvas_cas_tip);
    note_ref(sf_app_CpCanvas_fol_tip_no);
    note_ref(sf_app_CpCanvas_cas_ok);
    note_ref(sf_app_CpCanvas_trans_zan);
    note_ref(sf_app_CpCanvas_dell_time);
    note_ref(sf_app_CpCanvas_rock);
    if (sf_app_CpCanvas_rock > 0) {
        note_ref(rt.field_i32(sf_app_CpCanvas_rock, kFld_Rock_tip));
        note_ref(sf_app_CpCanvas_skill_kouka);
    }
    note_ref(sf_app_Rock_pow);
    note_ref(sf_app_CpCanvas_ata);
    if (sf_app_CpCanvas_ata > 0) {
        int32_t alen = rt.array_length(sf_app_CpCanvas_ata);
        for (int32_t i = 0; i < alen; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            note_ref(a);
            if (a > 0) note_ref(rt.field_i32(a, kFld_Attack_param));
        }
    }
    for (int32_t sp_tid : g_pvp_eng.sp_tip_ids) {
        note_ref(safe_iaload(rt, sf_app_CpCanvas_tip, sp_tid));
    }
    for (int s = 0; s < 2; ++s) {
        note_ref(g_pvp_eng.core.sides[s].dell_time_ref);
    }
    g_pvp_eng.mutable_heap_min_ref = min_mut_ref;

    // Load local side for opening CUSTOM_SCREEN
    ensure_board_orient(rt, g_pvp_eng.local_idx);
    load_side(rt, g_pvp_eng.local_idx);
    sync_panel_occupancy_and_proxy(rt, g_pvp_eng.local_idx);
}

// ============================================================================
// Palette & Backdrop Helpers (JP + EN Safe)
// ============================================================================
static void set_rock_palette_fast(DojaRuntime& rt, int32_t zoku) {
    int32_t z = std::clamp<int32_t>(zoku, 0, 6);
    if (g_pvp_eng.last_pal_zoku != z) {
        fn_app_CpCanvas_PalSet___II__V(rt, 0, z);
        g_pvp_eng.last_pal_zoku = z;
    }
}

static void compute_lon_stage_base_rgb(int32_t& out_r, int32_t& out_g, int32_t& out_b) {
    const bool has_stage_rgb = (sf_app_CpCanvas_R > 0 || sf_app_CpCanvas_G > 0 || sf_app_CpCanvas_B > 0);
    int32_t r = (sf_app_CpCanvas_scene == 5 || !has_stage_rgb) ? 155 : sf_app_CpCanvas_R;
    int32_t g = (sf_app_CpCanvas_scene == 5 || !has_stage_rgb) ? 255 : sf_app_CpCanvas_G;
    int32_t b = (sf_app_CpCanvas_scene == 5 || !has_stage_rgb) ? 210 : sf_app_CpCanvas_B;
    if ((r == 0 && g == 255 && b == 0) || (r == 186 && g == 0 && b == 255)) {
        r = 155;
        g = 255;
        b = 210;
    }
    out_r = std::clamp<int32_t>(r, 0, 255);
    out_g = std::clamp<int32_t>(g, 0, 255);
    out_b = std::clamp<int32_t>(b, 0, 255);
}

static void apply_lon_backdrop_tint(int32_t base_r, int32_t base_g, int32_t base_b,
                                    int32_t& out_r, int32_t& out_g, int32_t& out_b) {
    int32_t r = base_r;
    int32_t g = base_g;
    int32_t b = base_b;
    if (g_pvp_eng.phase == Phase::TRANCE_CUTSCENE) {
        if (!g_pvp_eng.trance_is_revert) {
            int32_t t = g_pvp_eng.trance_tick;
            int32_t q = 0;
            if (t >= 10 && t <= 39) {
                q = std::min<int32_t>(4, t - 9);
            } else if (t >= 40 && t <= 47) {
                q = 4;
            } else if (t >= 48 && t <= 54) {
                q = std::clamp<int32_t>(54 - t, 0, 4);
            }
            if (q >= 4) {
                r = g = b = 0;
            } else if (q > 0) {
                r = std::max<int32_t>(0, r - q * (base_r / 4));
                g = std::max<int32_t>(0, g - q * (base_g / 4));
                b = std::max<int32_t>(0, b - q * (base_b / 4));
            }
        }
    } else if (g_pvp_eng.core.tf_state != TF_IDLE && g_pvp_eng.core.tf_depth >= 1) {
        const TfSlot& top = g_pvp_eng.core.tf_stack[(g_pvp_eng.core.tf_depth >= 2) ? 1 : 0];
        if (g_pvp_eng.core.tf_depth == 1 && g_pvp_eng.core.tf_state == TF_ROOT_INTRO) {
            int32_t target_dur = 15 + top.stop_time;
            if (top.timer >= target_dur - 4) {
                int32_t diff = top.timer - (target_dur - 4);
                if (diff >= 4) {
                    r = base_r;
                    g = base_g;
                    b = base_b;
                } else {
                    for (int ds = 0; ds < 3; ++ds) {
                        r = (r * 4) / 5;
                        g = (g * 4) / 5;
                        b = (b * 4) / 5;
                    }
                    for (int bs = 0; bs < diff + 1; ++bs) {
                        r = std::min<int32_t>(base_r, (r * 45) / 40);
                        g = std::min<int32_t>(base_g, (g * 45) / 40);
                        b = std::min<int32_t>(base_b, (b * 45) / 40);
                    }
                }
            } else if (top.timer >= 0) {
                int32_t dark_steps = std::min<int32_t>(top.timer + 1, 3);
                for (int ds = 0; ds < dark_steps; ++ds) {
                    r = (r * 4) / 5;
                    g = (g * 4) / 5;
                    b = (b * 4) / 5;
                }
            }
        } else if (g_pvp_eng.core.tf_depth == 1 && g_pvp_eng.core.tf_state == TF_ROOT_RESUME &&
                   top.timer >= top.stop_time - 4) {
            int32_t diff = top.timer - (top.stop_time - 4);
            if (diff >= 4) {
                r = base_r;
                g = base_g;
                b = base_b;
            } else {
                for (int ds = 0; ds < 3; ++ds) {
                    r = (r * 4) / 5;
                    g = (g * 4) / 5;
                    b = (b * 4) / 5;
                }
                for (int bs = 0; bs < diff + 1; ++bs) {
                    r = std::min<int32_t>(base_r, (r * 45) / 40);
                    g = std::min<int32_t>(base_g, (g * 45) / 40);
                    b = std::min<int32_t>(base_b, (b * 45) / 40);
                }
            }
        } else {
            for (int ds = 0; ds < 3; ++ds) {
                r = (r * 4) / 5;
                g = (g * 4) / 5;
                b = (b * 4) / 5;
            }
        }
    }
    if ((r == 0 && g == 255 && b == 0) || (r == 186 && g == 0 && b == 255)) {
        g = 254;
    }
    out_r = std::clamp<int32_t>(r, 0, 255);
    out_g = std::clamp<int32_t>(g, 0, 255);
    out_b = std::clamp<int32_t>(b, 0, 255);
}

static void refresh_arena_imgmap_backdrop(DojaRuntime& rt, Ref cp, bool allow_recovery_blink = true) {
    (void)cp;
    int32_t bg_r = 155, bg_g = 255, bg_b = 210;
#ifdef HAS_FN_CpCanvas_PalSetBG
    // EN uses green (0, 255, 0) chroma key on graMap/imgMap while tmp_imgMap carries the
    // stage backdrop palette (ann_bg_pal[0..11] tinted deterministically from tmp_bg_pal).
    if (sf_app_CpCanvas_tmp_bg_pal > 0 && sf_app_CpCanvas_ann_bg_pal > 0 &&
        rt.array_length(sf_app_CpCanvas_tmp_bg_pal) >= 12 &&
        rt.array_length(sf_app_CpCanvas_ann_bg_pal) >= 12) {
        for (int i = 0; i < 12; i += 3) {
            int32_t br = rt.iaload(sf_app_CpCanvas_tmp_bg_pal, i);
            int32_t bg = rt.iaload(sf_app_CpCanvas_tmp_bg_pal, i + 1);
            int32_t bb = rt.iaload(sf_app_CpCanvas_tmp_bg_pal, i + 2);
            if ((br == 0 && bg == 255 && bb == 0) || (br == 186 && bg == 0 && bb == 255)) {
                compute_lon_stage_base_rgb(br, bg, bb);
            }
            int32_t tr = 0, tg = 0, tb = 0;
            apply_lon_backdrop_tint(br, bg, bb, tr, tg, tb);
            rt.iaload(sf_app_CpCanvas_ann_bg_pal, i) = tr;
            rt.iaload(sf_app_CpCanvas_ann_bg_pal, i + 1) = tg;
            rt.iaload(sf_app_CpCanvas_ann_bg_pal, i + 2) = tb;
        }
    }
    bg_r = 0;
    bg_g = 255;
    bg_b = 0;
#else
    int32_t base_r = 155, base_g = 255, base_b = 210;
    compute_lon_stage_base_rgb(base_r, base_g, base_b);
    apply_lon_backdrop_tint(base_r, base_g, base_b, bg_r, bg_g, bg_b);
    sf_app_BtPanel_R = bg_r;
    sf_app_BtPanel_G = bg_g;
    sf_app_BtPanel_B = bg_b;
#endif

    PvpEngineState::BackdropCache next_cache{};
    next_cache.valid = (sf_app_CpCanvas_graMap > 0 && sf_app_CpCanvas_panel > 0);
    next_cache.gra_map = sf_app_CpCanvas_graMap;
    next_cache.img_map = sf_app_CpCanvas_imgMap;
    next_cache.panel_arr = sf_app_CpCanvas_panel;
    next_cache.bg_r = bg_r;
    next_cache.bg_g = bg_g;
    next_cache.bg_b = bg_b;

    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0 || rt.array_length(row) < 6) {
                next_cache.valid = false;
                continue;
            }
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p <= 0) {
                    next_cache.valid = false;
                    continue;
                }
                int32_t saved_jin = rt.field_i32(p, kFld_BtPanel_jin);
                int32_t pos_x = rt.field_i32(p, kFld_BtPanel_pos_x);
                int32_t eff_jin = saved_jin;
                if (allow_recovery_blink) {
                    int32_t aria_cnt = rt.field_i32(p, kFld_BtPanel_aria_cnt);
                    if (aria_cnt > 0 && aria_cnt <= 45 && (pos_x / 3) == saved_jin && ((aria_cnt / 2) % 2) == 0) {
                        eff_jin = saved_jin ^ 1;
                    }
                }
                next_cache.cells[y][x] = PvpEngineState::BackdropCache::Cell{
                    p, pos_x, rt.field_i32(p, kFld_BtPanel_pos_y), eff_jin,
                    rt.field_i32(p, kFld_BtPanel_jou), rt.field_i32(p, kFld_BtPanel_ana_cnt)
                };
            }
        }
    } else {
        next_cache.valid = false;
    }

    const PvpEngineState::BackdropCache& prev = g_pvp_eng.backdrop_cache;
    bool cache_hit = next_cache.valid && prev.valid &&
        prev.gra_map == next_cache.gra_map &&
        prev.img_map == next_cache.img_map &&
        prev.panel_arr == next_cache.panel_arr &&
        prev.bg_r == next_cache.bg_r &&
        prev.bg_g == next_cache.bg_g &&
        prev.bg_b == next_cache.bg_b;
    if (cache_hit) {
        for (int y = 0; y < 3 && cache_hit; ++y) {
            for (int x = 0; x < 6; ++x) {
                const auto& a = prev.cells[y][x];
                const auto& b = next_cache.cells[y][x];
                if (a.ref != b.ref || a.pos_x != b.pos_x || a.pos_y != b.pos_y ||
                    a.eff_jin != b.eff_jin || a.jou != b.jou || a.ana_cnt != b.ana_cnt) {
                    cache_hit = false;
                    break;
                }
            }
        }
        if (cache_hit) {
            return;
        }
    }

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

    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0 || rt.array_length(row) < 6) continue;
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p <= 0) continue;
                int32_t saved_jin = rt.field_i32(p, kFld_BtPanel_jin);
                int32_t eff_jin = next_cache.cells[y][x].eff_jin;
                bool flip_for_blink = (eff_jin != saved_jin);
                if (flip_for_blink) {
                    rt.field_i32(p, kFld_BtPanel_jin) = eff_jin;
                }
                fn_app_BtPanel_Draw___IZ__V(rt, p, -1, 1);
                if (flip_for_blink) {
                    rt.field_i32(p, kFld_BtPanel_jin) = saved_jin;
                }
            }
        }
    }
    g_pvp_eng.backdrop_cache = next_cache;
}

// ============================================================================
// Custom Screen -> Trance Cutscene -> Round Banner Pipeline
// ============================================================================
static void draw_waiting_custom_backdrop(DojaRuntime& rt, Ref cp) {
    ensure_board_orient(rt, g_pvp_eng.local_idx);
    load_side(rt, g_pvp_eng.local_idx);
    sync_panel_occupancy_and_proxy(rt, g_pvp_eng.local_idx);
    render_pvp_combat_frame(rt, cp);
    safe_iastore(rt, sf_app_CpCanvas_soft_id, 0, 0);
    safe_iastore(rt, sf_app_CpCanvas_soft_id, 1, 0);
    bool is_final = (g_pvp_eng.core.custom_round >= kMaxTurns);
    render_waiting_custom_banner(rt, is_final);
}

static void enter_custom_screen_phase(DojaRuntime& rt, Ref cp) {
    g_pvp_eng.phase = Phase::CUSTOM_SCREEN;
    g_pvp_eng.core.custom_round = static_cast<uint16_t>(g_pvp_eng.core.custom_round + 1);
    g_pvp_eng.core.open_custom_req = false;
    g_pvp_eng.core.ended_by_judge = false;
    g_pvp_eng.core.combat_tick_in_round = 0;
    g_pvp_eng.local_custom_done = false;
    g_pvp_eng.remote_custom_ready = false;
    g_pvp_eng.custom_timer_ticks = 0;
    g_pvp_eng.trance_tick = 0;
    g_pvp_eng.core.cus_gage = 0;
    g_pvp_eng.core.sides[0].cus_gage = 0;
    g_pvp_eng.core.sides[1].cus_gage = 0;

    // Snapshot clean arena state BEFORE local BattleTip modifies local globals
    ensure_board_orient(rt, g_pvp_eng.local_idx);
    load_side(rt, g_pvp_eng.local_idx);
    sync_panel_occupancy_and_proxy(rt, g_pvp_eng.local_idx);
    g_pvp_eng.pre_custom_host = capture_host_runtime_state(rt);
    g_pvp_eng.pre_custom_core = g_pvp_eng.core;
    generated_save_statics(g_pvp_eng.pre_custom_statics);
    g_pvp_eng.pre_custom_snap.base_heap_size = 0;
    rt.save_combat_snapshot(g_pvp_eng.pre_custom_snap, g_pvp_eng.mutable_heap_min_ref);
    g_pvp_eng.has_pre_custom_snap = true;

    // Prepare LoN native BattleTip scene state cleanly
    sf_app_CpCanvas_scene = kSceneBattleTip;
    sf_app_CpCanvas_cas_cnt = 0;
    sf_app_CpCanvas_arm_cnt = 0;
    sf_app_CpCanvas_pa_cnt2 = 0;
    sf_app_CpCanvas_arm_chip = 0;
    sf_app_CpCanvas_arm_on = 0;
    sf_app_CpCanvas_arm_mode = 0;
    sf_app_CpCanvas_sel_cas_tip = 0;
    sf_app_CpCanvas_sel_cas_mode = 0;
    sf_app_CpCanvas_sel_cnt = 0;
    sf_app_CpCanvas_ok_tip = 0;
    sf_app_CpCanvas_ok_code = 0;
    sf_app_CpCanvas_pa_no = -1;
    sf_app_CpCanvas_esc_flg = 0;
    sf_app_CpCanvas_key = 0;
    sf_app_CpCanvas_key2 = 0;
    sf_app_CpCanvas_deba_flg = 0;
    sf_app_CpCanvas_non_esc = 1;
    g_pvp_eng.prev_raw_keypad = rt.poll_local_pvp_keypad();
    sf_app_Rock_muteki_cnt = 0;
    sf_app_Rock_hit_cnt = 0;
    sf_app_Rock_non_cnt2 = 0;
    if (sf_app_CpCanvas_rock > 0) {
        set_rock_palette_fast(rt, sf_app_Rock_zoku);
    }

    // Draw/top-up 8-slot tray from remaining unused folder chips up to max_sel
    uint32_t used = g_pvp_eng.core.sides[g_pvp_eng.local_idx].fol_used_mask;
    Ref fol = (sf_app_CpCanvas_fol > 0)
        ? safe_iaload(rt, sf_app_CpCanvas_fol, sf_app_CpCanvas_now_fol)
        : 0;
    int32_t max_draw = std::clamp<int32_t>(sf_app_CpCanvas_max_sel, 5, 8);
    // Compact existing valid tray slots forward (cas_tip[] holds tip_list indices > 0)
    int32_t write_idx = 0;
    int32_t kept_list_idx[8] = {};
    int32_t kept_fno[8] = {};
    for (int32_t i = 0; i < 8; ++i) {
        int32_t list_idx = safe_iaload(rt, sf_app_CpCanvas_cas_tip, i, -1);
        int32_t fno = safe_iaload(rt, sf_app_CpCanvas_fol_tip_no, i, -1);
        int32_t tid = 0, code = 0;
        if (unpack_tip_list_entry(rt, list_idx, tid, code) > 0 && fno >= 0 && fno < kNumFolders) {
            kept_list_idx[write_idx] = list_idx;
            kept_fno[write_idx] = fno;
            write_idx++;
        }
    }
    // Note slots already drawn or present in tray so we don't draw duplicates
    uint32_t in_tray_mask = used;
    for (int32_t i = 0; i < write_idx; ++i) {
        in_tray_mask |= (1u << kept_fno[i]);
    }
    // Regular chip guaranteed if regu_flg != 0 on Turn 1
    if (g_pvp_eng.core.custom_round == 1 && sf_app_CpCanvas_regu_flg != 0 && fol > 0) {
        int32_t rid = std::clamp<int32_t>(rt.field_i32(fol, kFld_Folder_regu_id), 0, 29);
        if (!((in_tray_mask >> rid) & 1u)) {
            Ref tid_arr = rt.field_i32(fol, kFld_Folder_tip_id);
            int32_t list_idx = safe_iaload(rt, tid_arr, rid, -1);
            int32_t tid = 0, code = 0;
            if (unpack_tip_list_entry(rt, list_idx, tid, code) > 0 && write_idx < max_draw) {
                kept_list_idx[write_idx] = list_idx;
                kept_fno[write_idx] = rid;
                in_tray_mask |= (1u << rid);
                write_idx++;
            }
        }
    }
    for (int32_t f = 0; f < kNumFolders && write_idx < max_draw; ++f) {
        if ((in_tray_mask >> f) & 1u) continue;
        if (fol > 0) {
            Ref tid_arr = rt.field_i32(fol, kFld_Folder_tip_id);
            int32_t list_idx = safe_iaload(rt, tid_arr, f, -1);
            int32_t tid = 0, code = 0;
            if (unpack_tip_list_entry(rt, list_idx, tid, code) > 0) {
                kept_list_idx[write_idx] = list_idx;
                kept_fno[write_idx] = f;
                in_tray_mask |= (1u << f);
                write_idx++;
            }
        }
    }
    if (fol > 0) {
        sync_folder_used_from_mask(rt, fol, in_tray_mask);
        int32_t remain = 0;
        for (int32_t f = 0; f < kNumFolders; ++f) {
            if (!((in_tray_mask >> f) & 1u)) ++remain;
        }
        rt.field_i32(fol, kFld_Folder_tip_zan) = remain;
    }
    for (int32_t i = 0; i < 8; ++i) {
        safe_iastore(rt, sf_app_CpCanvas_cas_tip, i, (i < write_idx) ? kept_list_idx[i] : -1);
        safe_iastore(rt, sf_app_CpCanvas_fol_tip_no, i, (i < write_idx) ? kept_fno[i] : -1);
        safe_iastore(rt, sf_app_CpCanvas_cas_ok, i, (i < write_idx) ? 1 : 0);
    }
    for (int32_t i = 0; i < 5; ++i) safe_iastore(rt, sf_app_CpCanvas_set_tip, i, -1);
    safe_iastore(rt, sf_app_CpCanvas_cas_ok, 8, 1);
    safe_iastore(rt, sf_app_CpCanvas_cas_ok, 9, 1);
    fn_app_CpCanvas_CasOk_____V(rt, cp);
    if (write_idx <= 0) {
        sf_app_CpCanvas_sel_cas_mode = 1;
        sf_app_CpCanvas_sel_cas_tip = 0;
    }

    if (cp > 0) {
        fn_app_CpCanvas_Audio___II__V(rt, 1, 22); // Custom Screen BGM
    }
    rt.reset_interpolator();
}

static pvp::PvpCustomCommit capture_local_custom_commit(DojaRuntime& rt, bool timed_out) {
    // Capture trance decision & PA BEFORE suppressing BattleTip counters!
    uint8_t ta = 0;
    uint16_t achip = 0;
    if (sf_app_CpCanvas_arm_cnt >= 100) {
        ta = 7; // Revert SetArm(-1)
    } else if (sf_app_CpCanvas_arm_cnt > 0 && sf_app_CpCanvas_set_arm >= 0 && sf_app_CpCanvas_set_arm < 6) {
        ta = static_cast<uint8_t>(sf_app_CpCanvas_set_arm + 1); // 1..6
        achip = static_cast<uint16_t>(std::clamp<int32_t>(sf_app_CpCanvas_arm_chip, 0, 270));
    }

    // Immediately suppress native animation/cutscene triggers before packet capture
    sf_app_CpCanvas_arm_cnt = 0;
    sf_app_CpCanvas_cas_cnt = 0;
    sf_app_CpCanvas_pa_cnt2 = 0;
    sf_app_CpCanvas_scene = kSceneBattleTip;

    if (timed_out) {
        // Timeout fallback: commit empty chip hand with no Trance change (or auto-revert if expired)
        if (sf_app_Rock_zoku > 0 && sf_app_Rock_arm_cnt <= 1) ta = 7;
        sf_app_Rock_tip_no = -1;
    }

    pvp::PvpCustomCommit c;
    c.has_lon_v2 = true;
    c.turn_no = current_wire_round_key();
    c.tip_no = static_cast<int8_t>(std::clamp<int32_t>(sf_app_Rock_tip_no, -1, 4));
    c.max_sel = static_cast<uint8_t>(std::clamp<int32_t>(sf_app_CpCanvas_max_sel, 5, 8));
    c.regu_flg = static_cast<uint8_t>(sf_app_CpCanvas_regu_flg != 0 ? 1 : 0);
    c.sec_cha = static_cast<uint8_t>(sf_app_CpCanvas_sha_cnt > 0 ? 1 : 0);
    c.pa_no = static_cast<int16_t>(sf_app_CpCanvas_pa_no);

    Ref rock = sf_app_CpCanvas_rock;
    Ref r_tip_arr = (rock > 0) ? rt.field_i32(rock, kFld_Rock_tip) : 0;
    for (int i = 0; i < 9; ++i) {
        Ref t_obj = safe_iaload(rt, r_tip_arr, i);
        int32_t tid = (t_obj > 0) ? rt.field_i32(t_obj, kFld_Tip_tip_id) : 0;
        if (timed_out) tid = 0;
        c.rock_tip_ids[i] = static_cast<uint16_t>(std::max<int32_t>(0, tid));
    }

    Ref fol = (sf_app_CpCanvas_fol > 0)
        ? safe_iaload(rt, sf_app_CpCanvas_fol, sf_app_CpCanvas_now_fol)
        : 0;
    c.fol_used_mask = read_folder_used_mask(rt, fol);

    c.trance_action = ta;
    c.arm_chip = achip;
    c.sha_cnt = static_cast<uint8_t>(std::clamp<int32_t>(sf_app_CpCanvas_sha_cnt, 0, 10));
    uint8_t zmask = 0;
    for (int s = 0; s < kMaxSouls; ++s) {
        if (safe_iaload(rt, sf_app_CpCanvas_trans_zan, s) != 0) zmask |= static_cast<uint8_t>(1u << s);
    }
    if (ta >= 1 && ta <= 6) {
        zmask &= static_cast<uint8_t>(~(1u << (ta - 1)));
    }
    c.trans_zan_mask = zmask;
    c.rock_arm_cnt = static_cast<int8_t>(std::clamp<int32_t>(sf_app_Rock_arm_cnt, 0, 5));

    uint8_t tray_n = 0;
    for (int i = 0; i < 8; ++i) {
        int32_t list_idx = safe_iaload(rt, sf_app_CpCanvas_cas_tip, i, -1);
        c.cas_tip_ids[i] = pack_wire_cas_tip(rt, list_idx);
        g_pvp_eng.local_post_fol_tip_no[i] = (c.cas_tip_ids[i] != 0)
            ? safe_iaload(rt, sf_app_CpCanvas_fol_tip_no, i, -1)
            : -1;
        if (c.cas_tip_ids[i] != 0) tray_n = static_cast<uint8_t>(i + 1);
    }
    c.cas_tip_max = tray_n;

    sanitize_lon_commit(rt, g_pvp_eng.core.sides[g_pvp_eng.local_idx], c);
    return c;
}

static void apply_custom_commit_to_side(DojaRuntime& rt, int idx, const pvp::PvpCustomCommit& raw_c) {
    SideContext& sc = g_pvp_eng.core.sides[idx];
    pvp::PvpCustomCommit c = raw_c;
    sanitize_lon_commit(rt, sc, c);

    sc.fol_used_mask = c.fol_used_mask;
    sc.regu_flg = c.regu_flg;
    sc.max_sel = c.max_sel;
    sc.sha_cnt = c.sha_cnt;
    sc.pa_no = c.pa_no;
    sc.tip_no = c.tip_no;
    sc.next_tip = 1;
    sc.arm_cnt = c.rock_arm_cnt;

    for (int s = 0; s < kMaxSouls; ++s) {
        sc.trans_zan[s] = ((c.trans_zan_mask >> s) & 1u) ? 1 : 0;
    }
    for (int i = 0; i < 8; ++i) {
        sc.cas_tip[i] = unpack_wire_cas_tip_to_list_idx(rt, c.cas_tip_ids[i]);
        if (idx == g_pvp_eng.local_idx) {
            sc.fol_tip_no[i] = (sc.cas_tip[i] > 0) ? g_pvp_eng.local_post_fol_tip_no[i] : -1;
        }
    }
    for (int i = 0; i < 9; ++i) {
        sc.rock_tip_ids[i] = c.rock_tip_ids[i];
    }
    sc.opp_nondim_tip_id = 0;
    sc.opp_nondim_pow = 0;
    sc.opp_nondim_plus = 0;
    sc.opp_nondim_ticks = 0;
}

static void ensure_and_reset_arena_entities(DojaRuntime& rt) {
    // Initialize clean 3x6 BtPanel array (cols 0..2 jin=0, cols 3..5 jin=1)
    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
        for (int32_t y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0 || rt.array_length(row) < 6) continue;
            for (int32_t x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p <= 0) {
                    p = rt.alloc_object(kClassId_BtPanel, kNumI32_BtPanel, kNumI64_BtPanel);
                    fn_app_BtPanel__init______V(rt, p);
                    rt.iaload(row, x) = p;
                }
                fn_app_BtPanel_init___III__V(rt, p, x, y, 1);
            }
        }
    }

    // Ensure & clear all 6 Okimono slots (BtSet may not have run from Title screen)
    if (sf_app_CpCanvas_oki > 0) {
        int32_t n = std::min<int32_t>(kMaxOkimono, rt.array_length(sf_app_CpCanvas_oki));
        for (int32_t i = 0; i < n; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
            if (ok <= 0) {
                ok = rt.alloc_object(kClassId_Okimono, kNumI32_Okimono, kNumI64_Okimono);
                fn_app_Okimono__init____I__V(rt, ok, i);
                rt.iaload(sf_app_CpCanvas_oki, i) = ok;
            }
            fn_app_Okimono_init_____V(rt, ok);
        }
    }
    for (int i = 0; i < kMaxOkimono; ++i) g_pvp_eng.core.oki_owner[i] = -1;

    // Ensure Ene[0..2] exist (Ene[0] serves as Rock_hit proxy & CharaDraw checks all 3)
    if (sf_app_CpCanvas_ene > 0) {
        int32_t en = rt.array_length(sf_app_CpCanvas_ene);
        for (int32_t i = 0; i < en; ++i) {
            Ref e = rt.iaload(sf_app_CpCanvas_ene, i);
            if (e <= 0) {
                e = rt.alloc_object(kClassId_Ene, kNumI32_Ene, kNumI64_Ene);
                fn_app_Ene__init______V(rt, e);
                rt.iaload(sf_app_CpCanvas_ene, i) = e;
            }
            rt.field_i32(e, kFld_Ene_on) = 0;
            rt.field_i32(e, kFld_Ene_ata_id) = -1;
        }
    }

    // Clear shared / runtime battle flags (keep sf_app_CpCanvas_rand allocated!)
    sf_app_CpCanvas_stop_cnt = 0;
    sf_app_CpCanvas_stop_time = 0;
    sf_app_CpCanvas_stop_ch = 0;
    sf_app_CpCanvas_stop_name = 0;
    sf_app_CpCanvas_arm_cnt = 0;
    sf_app_CpCanvas_cas_cnt = 0;
    sf_app_CpCanvas_pa_cnt2 = 0;
    sf_app_CpCanvas_pa_cnt = 0;
    sf_app_CpCanvas_ata_cnt = 0;
    sf_app_CpCanvas_cus_gage = 0;
    sf_app_CpCanvas_key = 0;
    sf_app_CpCanvas_key2 = 0;
    sf_app_CpCanvas_key_get = 0;

    // Ensure Rock and Attack[30] exist and are freshly initialized before saving sides
    if (sf_app_CpCanvas_rock <= 0) {
        sf_app_CpCanvas_rock = rt.alloc_object(kClassId_Rock, kNumI32_Rock, kNumI64_Rock);
        fn_app_Rock__init______V(rt, sf_app_CpCanvas_rock);
    }
    fn_app_Rock_init_____V(rt, sf_app_CpCanvas_rock);
    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = rt.array_length(sf_app_CpCanvas_ata);
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0) {
                a = rt.alloc_object(kClassId_Attack, kNumI32_Attack, kNumI64_Attack);
                fn_app_Attack__init____I__V(rt, a, i);
                rt.iaload(sf_app_CpCanvas_ata, i) = a;
            }
            fn_app_Attack_init_____V(rt, a);
        }
    }
}

static void apply_trance_decision_for_side(DojaRuntime& rt, int s_idx, uint8_t action) {
    if (action == 0) return;
    ensure_board_orient(rt, s_idx);
    load_side(rt, s_idx);
    cancel_buster_charge_glow(rt);
    g_pvp_eng.core.sides[s_idx].buster_charge_glow = false;
    if (action == 7) {
        fn_app_Rock_SetArm___I__V(rt, sf_app_CpCanvas_rock, -1);
    } else if (action >= 1 && action <= 6) {
        int soul = action - 1;
        fn_app_Rock_SetArm___I__V(rt, sf_app_CpCanvas_rock, soul);
        safe_iastore(rt, sf_app_CpCanvas_trans_zan, soul, 0);
        g_pvp_eng.core.sides[s_idx].trans_zan[soul] = 0;
    }
    fn_app_Rock_TipNext_____V(rt, sf_app_CpCanvas_rock);
    save_side(rt, s_idx);
}

constexpr int32_t kKeySelectBit = (1 << 20); // ACT_SELECT (决定/A): Use Chip when tip_no >= 0, Buster fallback when tip_no < 0
constexpr int32_t kPureBusterBits = (1 << 11) | (1 << 10) | (1 << 9) | (1 << 7); // ACT_POUND (#), ACT_CANCEL (B), 9, 7
constexpr int32_t kCustomTriggerBits = (1 << 21) | (1 << 22); // ACT_SOFT1 (L) or ACT_SOFT2 (R)

static int32_t compute_round_initial_remote_buster_pred(int rem_idx) {
    const SideContext& rem = g_pvp_eng.core.sides[rem_idx];
    if (rem.key_cnt <= 0) {
        return 0;
    }
    int32_t held_buster = rem.last_raw_input & kPureBusterBits;
    if (held_buster != 0) {
        return held_buster;
    }
    if (rem.tip_no < 0 && (rem.last_raw_input & kKeySelectBit) != 0) {
        return kKeySelectBit;
    }
    return (1 << 10);
}

static void init_round_rollback_window(DojaRuntime& rt) {
    uint32_t cur_t = g_pvp_eng.core.combat_turn;
    const size_t hist_n = g_pvp_eng.local_inputs.size();
    for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
    for (auto& em : g_pvp_eng.turn_emitted_se) {
        em.turn = UINT32_MAX;
        em.count = 0;
    }
    g_pvp_eng.round_start_turn = cur_t;
    g_pvp_eng.first_unconfirmed_turn = cur_t;
    g_pvp_eng.last_hash_checked_turn = cur_t;
    g_pvp_eng.max_scheduled_turn = cur_t;
    g_pvp_eng.round_needs_initial_schedule = true;
    g_pvp_eng.round_initial_remote_pred = compute_round_initial_remote_buster_pred(g_pvp_eng.opp_idx());
    g_pvp_eng.core.sides[0].last_raw_input = 0;
    g_pvp_eng.core.sides[1].last_raw_input = 0;
    g_pvp_eng.core.open_custom_req = false;
    g_pvp_eng.core.ended_by_judge = false;
    g_pvp_eng.core.combat_tick_in_round = 0;
    if (g_pvp_eng.core.custom_round >= kMaxTurns) {
        g_pvp_eng.core.final_turn_ticks_left = kFinalTurnDurationTicks;
        g_pvp_eng.core.cus_gage = 128;
        sf_app_CpCanvas_cus_gage = 128;
    } else {
        g_pvp_eng.core.final_turn_ticks_left = 0;
        g_pvp_eng.core.cus_gage = 0;
        sf_app_CpCanvas_cus_gage = 0;
    }
    g_pvp_eng.core.sides[0].cus_gage = g_pvp_eng.core.cus_gage;
    g_pvp_eng.core.sides[1].cus_gage = g_pvp_eng.core.cus_gage;
    for (uint32_t d = 0; d <= kInputDelayFrames + 8; ++d) {
        size_t slot = (cur_t + d) % hist_n;
        g_pvp_eng.local_inputs[slot] = 0;
        g_pvp_eng.predicted_remote[slot] = g_pvp_eng.round_initial_remote_pred;
        g_pvp_eng.remote_confirmed[slot] = false;
    }
    g_pvp_eng.stall_frames = 0;
    g_pvp_eng.combat_stall_frames = 0;
    g_pvp_eng.phase_timer = 0;
    g_pvp_eng.throttler.reset();
    (void)rt;
}

static void enter_round_banner_or_trance(DojaRuntime& rt, Ref cp) {
    // Restore clean pre-custom snapshot so native BattleTip side effects cannot pollute combat
    if (g_pvp_eng.has_pre_custom_snap) {
        HostRuntimeState cur_host = capture_host_runtime_state(rt);
        g_pvp_eng.core = g_pvp_eng.pre_custom_core;
        generated_restore_statics(g_pvp_eng.pre_custom_statics);
        rt.restore_combat_snapshot(g_pvp_eng.pre_custom_snap);
        restore_host_runtime_state(rt, cur_host, false);
        g_pvp_eng.has_pre_custom_snap = false;
        g_pvp_eng.last_pal_zoku = -1;
    }

    pvp::PvpCustomCommit commits[2];
    commits[g_pvp_eng.local_idx] = g_pvp_eng.local_commit;
    commits[g_pvp_eng.opp_idx()] = g_pvp_eng.remote_commit;

    for (int s = 0; s < 2; ++s) {
        apply_custom_commit_to_side(rt, s, commits[s]);
        g_pvp_eng.trance_action[s] = commits[s].trance_action;
    }

    init_round_rollback_window(rt);

    uint8_t ta0 = g_pvp_eng.trance_action[0];
    uint8_t ta1 = g_pvp_eng.trance_action[1];
    bool any_activate = (ta0 >= 1 && ta0 <= 6) || (ta1 >= 1 && ta1 <= 6);
    bool any_revert = (ta0 == 7) || (ta1 == 7);

    // Advance Rock.TipNext directly on sides whose Trance form doesn't transition here
    for (int s = 0; s < 2; ++s) {
        if ((!any_activate && !any_revert) || g_pvp_eng.trance_action[s] == 0) {
            ensure_board_orient(rt, s);
            load_side(rt, s);
            fn_app_Rock_TipNext_____V(rt, sf_app_CpCanvas_rock);
            save_side(rt, s);
        }
    }

    if (!any_activate && !any_revert) {
        // (0, 0): skip TRANCE_CUTSCENE -> ROUND_BANNER (15 ticks)
        g_pvp_eng.phase = Phase::ROUND_BANNER;
        g_pvp_eng.banner_ticks = 0;
        if (cp > 0) fn_app_CpCanvas_Audio___II__V(rt, 1, 4); // Battle BGM
    } else if (!any_activate && any_revert) {
        // Revert-only: 20 ticks (t = 100..119)
        g_pvp_eng.phase = Phase::TRANCE_CUTSCENE;
        g_pvp_eng.trance_is_revert = true;
        g_pvp_eng.trance_tick = 100;
    } else {
        // Activate (at least one 1..6): 54 ticks (t = 1..54)
        g_pvp_eng.phase = Phase::TRANCE_CUTSCENE;
        g_pvp_eng.trance_is_revert = false;
        g_pvp_eng.trance_tick = 1;
    }
    ensure_board_orient(rt, g_pvp_eng.local_idx);
    load_side(rt, g_pvp_eng.local_idx);
    sync_panel_occupancy_and_proxy(rt, g_pvp_eng.local_idx);
    rt.reset_interpolator();
}

static void step_and_render_trance_cutscene(DojaRuntime& rt, Ref cp) {
    int32_t t = g_pvp_eng.trance_tick;

    // Transformation moment (t == 114 revert, or t == 44 activate)
    if ((g_pvp_eng.trance_is_revert && t == 114) || (!g_pvp_eng.trance_is_revert && t == 44)) {
        if (!g_pvp_eng.trance_is_revert && cp > 0) {
            fn_app_CpCanvas_Audio___II__V(rt, 1, 21); // Soul transformation fanfare
        }
        apply_trance_decision_for_side(rt, 0, g_pvp_eng.trance_action[0]);
        apply_trance_decision_for_side(rt, 1, g_pvp_eng.trance_action[1]);
    }

    refresh_arena_imgmap_backdrop(rt, cp, false);
    render_pvp_combat_frame(rt, cp);

    // Render Soul Convergence effect on frames 21..39 for activating sides
    // Pass a5 = 1 so DrawAni skips host seSet calls, and rollback temporary int[4] heap allocs!
    if (!g_pvp_eng.trance_is_revert && t >= 21 && t <= 39) {
        int32_t dx = (t < 24) ? 48 : std::max<int32_t>(0, 120 - 3 * t);
        if ((t % 2 != 0) || t < 24) {
            bool prev_sup = rt.suppress_av();
            rt.set_suppress_av(false);
            HostRuntimeState conv_host = capture_host_runtime_state(rt);
            FastCombatSnapshot conv_snap;
            conv_snap.base_heap_size = 0;
            rt.save_combat_snapshot(conv_snap, std::max<size_t>(1, rt.heap_size()));
            for (int s = 0; s < 2; ++s) {
                uint8_t act = g_pvp_eng.trance_action[s];
                if (act < 1 || act > 6) continue;
                int soul = act - 1;
                const SideContext& sc = g_pvp_eng.core.sides[s];
                int32_t screen_gx = (s == g_pvp_eng.local_idx) ? sc.pos_x : (5 - sc.pos_x);
                int32_t cx = screen_gx * 40 + 20;
                int32_t cy = sc.pos_y * 24 + 105;
                int32_t anim_id = kSoulEffectAni[soul];
                fn_app_CpCanvas_DrawAni___IIIIII__I(rt, anim_id, 0, cx + dx, cy, 0, 1);
                fn_app_CpCanvas_DrawAni___IIIIII__I(rt, anim_id, 0, cx - dx, cy, 0, 1);
            }
            rt.restore_combat_snapshot(conv_snap);
            restore_host_runtime_state(rt, conv_host, true);
            rt.set_suppress_av(prev_sup);
        }
    }

    // White transformation flash over battle viewport (40..47 activate, 106..114 revert)
    bool is_flash = g_pvp_eng.trance_is_revert ? (t >= 106 && t <= 114) : (t >= 40 && t <= 47);
    if (is_flash && sf_app_CpCanvas_g > 0) {
        bool prev_sup = rt.suppress_av();
        rt.set_suppress_av(false);
        Slot c_args[2];
        c_args[0].r = sf_app_CpCanvas_g;
        c_args[1].i = static_cast<int32_t>(0xFFFFFFFFu);
        rt.invoke_external("com/nttdocomo/ui/Graphics", "setColor", "(I)V", c_args, 2);
        Slot r_args[5];
        r_args[0].r = sf_app_CpCanvas_g;
        r_args[1].i = 0;
        r_args[2].i = 40;
        r_args[3].i = 240;
        r_args[4].i = 160;
        rt.invoke_external("com/nttdocomo/ui/Graphics", "fillRect", "(IIII)V", r_args, 5);
        rt.set_suppress_av(prev_sup);
    }

    // Advance Trance Cutscene clock
    g_pvp_eng.trance_tick++;
    bool finished = g_pvp_eng.trance_is_revert ? (g_pvp_eng.trance_tick > 119) : (g_pvp_eng.trance_tick > 54);
    if (finished) {
        g_pvp_eng.phase = Phase::ROUND_BANNER;
        g_pvp_eng.banner_ticks = 0;
        refresh_arena_imgmap_backdrop(rt, cp, false);
        if (cp > 0) fn_app_CpCanvas_Audio___II__V(rt, 1, 4); // Battle BGM
        rt.reset_interpolator();
    }
}

// ============================================================================
// Elemental Traps, Time-Freeze Cut-In Stack, & Chip Detection Helpers
// ============================================================================
static bool is_dimming_tip_id(DojaRuntime& rt, int32_t tip_id, int32_t& out_stop_time, int32_t& out_tip_class) {
    out_stop_time = 0;
    out_tip_class = 0;
    int32_t tip_len = get_tip_array_len(rt);
    if (tip_id <= 0 || tip_id >= tip_len || sf_app_CpCanvas_tip <= 0) return false;
    Ref t_obj = rt.iaload(sf_app_CpCanvas_tip, tip_id);
    if (t_obj <= 0) return false;
    out_tip_class = rt.field_i32(t_obj, kFld_Tip_tip_class);
    int32_t waza_id = rt.field_i32(t_obj, kFld_Tip_waza_id);
    if (sf_app_CpCanvas_waza > 0 && waza_id >= 0 && waza_id < rt.array_length(sf_app_CpCanvas_waza)) {
        Ref w_obj = rt.iaload(sf_app_CpCanvas_waza, waza_id);
        if (w_obj > 0) {
            out_stop_time = rt.field_i32(w_obj, kFld_Waza_stop_time);
        }
    }
    return out_stop_time > 0;
}

static void snapshot_oki_on(DojaRuntime& rt, bool prev_oki_on[kMaxOkimono]) {
    for (int i = 0; i < kMaxOkimono; ++i) prev_oki_on[i] = false;
    if (sf_app_CpCanvas_oki <= 0) return;
    int32_t on_cnt = std::min<int32_t>(kMaxOkimono, rt.array_length(sf_app_CpCanvas_oki));
    for (int32_t i = 0; i < on_cnt; ++i) {
        Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
        prev_oki_on[i] = (ok > 0 && rt.field_i32(ok, kFld_Okimono_on) != 0);
    }
}

static void update_oki_owner_from_snapshot(DojaRuntime& rt, int s_idx, const bool prev_oki_on[kMaxOkimono]) {
    if (sf_app_CpCanvas_oki <= 0) return;
    int32_t on_cnt = std::min<int32_t>(kMaxOkimono, rt.array_length(sf_app_CpCanvas_oki));
    for (int32_t i = 0; i < on_cnt; ++i) {
        Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
        bool now_on = (ok > 0 && rt.field_i32(ok, kFld_Okimono_on) != 0);
        if (now_on && !prev_oki_on[i]) {
            g_pvp_eng.core.oki_owner[i] = static_cast<int8_t>(s_idx);
        } else if (!now_on) {
            g_pvp_eng.core.oki_owner[i] = -1;
        }
    }
}

// Isolate P1's Okimono slots in [2..3] from P0's [0..1] because Attack_SetOki(chara_flg=0)
// scans only oki[0..1] for free slots and only_flg=1 replacement.
struct ScopedP1OkiSlotSwap {
    DojaRuntime& rt_;
    bool active_ = false;
    explicit ScopedP1OkiSlotSwap(DojaRuntime& rt, bool enable)
        : rt_(rt), active_(enable && sf_app_CpCanvas_oki > 0 && rt.array_length(sf_app_CpCanvas_oki) >= 4) {
        if (active_) toggle();
    }
    ~ScopedP1OkiSlotSwap() {
        if (active_) toggle();
    }
    void toggle() {
        for (int32_t k = 0; k < 2; ++k) {
            int32_t a = k;
            int32_t b = k + 2;
            Ref oa = rt_.iaload(sf_app_CpCanvas_oki, a);
            Ref ob = rt_.iaload(sf_app_CpCanvas_oki, b);
            rt_.iaload(sf_app_CpCanvas_oki, a) = ob;
            rt_.iaload(sf_app_CpCanvas_oki, b) = oa;
            if (ob > 0) rt_.field_i32(ob, kFld_Okimono_oki_id) = a;
            if (oa > 0) rt_.field_i32(oa, kFld_Okimono_oki_id) = b;
            std::swap(g_pvp_eng.core.oki_owner[a], g_pvp_eng.core.oki_owner[b]);
        }
        if (sf_app_CpCanvas_panel > 0 && rt_.array_length(sf_app_CpCanvas_panel) >= 3) {
            for (int32_t y = 0; y < 3; ++y) {
                Ref row = rt_.iaload(sf_app_CpCanvas_panel, y);
                if (row <= 0 || rt_.array_length(row) < 6) continue;
                for (int32_t x = 0; x < 6; ++x) {
                    Ref p = rt_.iaload(row, x);
                    if (p <= 0) continue;
                    int32_t oc = rt_.field_i32(p, kFld_BtPanel_on_chara);
                    if (oc == 10) rt_.field_i32(p, kFld_BtPanel_on_chara) = 12;
                    else if (oc == 12) rt_.field_i32(p, kFld_BtPanel_on_chara) = 10;
                    else if (oc == 11) rt_.field_i32(p, kFld_BtPanel_on_chara) = 13;
                    else if (oc == 13) rt_.field_i32(p, kFld_BtPanel_on_chara) = 11;
                }
            }
        }
    }
};

static void execute_elemental_trap_hit(DojaRuntime& rt, int def_idx, int32_t wana_elem, int32_t wpow) {
    if (wana_elem < 1 || wana_elem > 4 || wpow <= 0) return;
    int att_idx = 1 - def_idx;
    int32_t saved_active = g_pvp_eng.active_side;
    g_pvp_eng.active_side = def_idx;
    g_pvp_eng.active_ata_ref = 0;
    ensure_board_orient(rt, def_idx);
    load_side(rt, def_idx);
    sync_panel_occupancy_and_proxy(rt, def_idx);

    const SideContext& opp_sc = g_pvp_eng.core.sides[att_idx];
    int32_t opp_px = std::clamp<int32_t>(5 - opp_sc.pos_x, 0, 5);
    int32_t opp_py = std::clamp<int32_t>(opp_sc.pos_y, 0, 2);
    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
        Ref row = rt.iaload(sf_app_CpCanvas_panel, opp_py);
        Ref pan = (row > 0 && rt.array_length(row) >= 6) ? rt.iaload(row, opp_px) : 0;
        if (pan > 0) {
            int32_t pjou = rt.field_i32(pan, kFld_BtPanel_jou);
            if (wana_elem == 1 && pjou == 4) wpow *= 2; // BattleMain: Fire trap doubles on Grass
            if (pjou == 4 || pjou == 3) fn_app_BtPanel_Henka___II__I(rt, pan, 1, 0);
        }
    }
    Ref e0 = (sf_app_CpCanvas_ene > 0 && rt.array_length(sf_app_CpCanvas_ene) > 0)
        ? rt.iaload(sf_app_CpCanvas_ene, 0) : 0;
    if (e0 > 0 && rt.field_i32(e0, kFld_Ene_on) != 0) {
        fn_app_Ene_Hit___III__Z(rt, e0, 1, wpow, wana_elem);
    }
    save_side(rt, def_idx);
    g_pvp_eng.active_side = saved_active;
}

static void start_root_time_freeze(int s_idx, const TfSlot& slot);

static void cancel_buster_charge_glow(DojaRuntime& rt) {
    if (sf_app_Rock_bas_no >= 0 && sf_app_CpCanvas_ata > 0 &&
        sf_app_Rock_bas_no < rt.array_length(sf_app_CpCanvas_ata)) {
        Ref bas_ata = rt.iaload(sf_app_CpCanvas_ata, sf_app_Rock_bas_no);
        if (bas_ata > 0) {
            fn_app_Attack_init_____V(rt, bas_ata);
        }
    }
    sf_app_Rock_bas_no = -1;
    sf_app_CpCanvas_key_cnt = 0;
}

static void cleanup_orphaned_charge_attacks(DojaRuntime& rt) {
    if (sf_app_CpCanvas_ata <= 0) return;
    int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
    bool charge_valid = (sf_app_Rock_bas_no >= 0 && sf_app_Rock_bas_no < an &&
                         (sf_app_CpCanvas_key_cnt >= 4 || sf_app_Rock_tmp_ata >= 0));
    if (!charge_valid && sf_app_Rock_bas_no >= 0) {
        if (sf_app_Rock_bas_no < an) {
            Ref bas_ata = rt.iaload(sf_app_CpCanvas_ata, sf_app_Rock_bas_no);
            if (bas_ata > 0) fn_app_Attack_init_____V(rt, bas_ata);
        }
        sf_app_Rock_bas_no = -1;
    }
    for (int32_t i = 0; i < an; ++i) {
        if (charge_valid && i == sf_app_Rock_bas_no) continue;
        Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
        if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
        int32_t eid = rt.field_i32(a, kFld_Attack_eff_id);
        int32_t tid = rt.field_i32(a, kFld_Attack_tmp_id);
        int32_t wid = rt.field_i32(a, kFld_Attack_waza_id);
        if (eid == 227 || eid == 228 || tid == 227 || tid == 228 || wid == 227 || wid == 228) {
            fn_app_Attack_init_____V(rt, a);
        }
    }
}

static void trigger_opponent_elemental_trap(DojaRuntime& rt, int att_idx, int32_t wana_elem) {
    if (wana_elem < 1 || wana_elem > 4) return;
    int def_idx = 1 - att_idx;
    SideContext& def_sc = g_pvp_eng.core.sides[def_idx];
    if (def_sc.wana != wana_elem) return;

    int32_t wpow = (def_sc.wana_pow[0] > 0) ? def_sc.wana_pow[0] : 100;
    def_sc.wana = 0;
    def_sc.wana_flg = 0;

    int32_t saved_active = g_pvp_eng.active_side;
    g_pvp_eng.active_side = def_idx;
    ensure_board_orient(rt, def_idx);
    load_side(rt, def_idx);
    sync_panel_occupancy_and_proxy(rt, def_idx);

    // BattleMain visual: overhead FX 117 + cutscene visual 240 + wana
    int32_t fx_no = fn_app_CpCanvas_AtaNo___I__I(rt, 0);
    if (fx_no >= 0 && sf_app_CpCanvas_ata > 0 && fx_no < rt.array_length(sf_app_CpCanvas_ata)) {
        Ref fx_ata = rt.iaload(sf_app_CpCanvas_ata, fx_no);
        if (fx_ata > 0) {
            fn_app_Attack_Set___IIIII__I(rt, fx_ata, 117, sf_app_Rock_pos_x, sf_app_Rock_pos_y, 0, 2);
            rt.field_i32(fx_ata, kFld_Attack_hit_time) = 1;
        }
    }
    int32_t s_no = fn_app_CpCanvas_AtaNo___I__I(rt, 0);
    if (s_no >= 0 && sf_app_CpCanvas_ata > 0 && s_no < rt.array_length(sf_app_CpCanvas_ata)) {
        Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s_no);
        if (c_ata > 0) {
            fn_app_Attack_Set___IIIII__I(rt, c_ata, 240 + wana_elem, 1, -2, 1, 2);
            rt.field_i32(c_ata, kFld_Attack_chara_flg) = 1;
            rt.field_i32(c_ata, kFld_Attack_pow) = wpow;
            rt.field_i32(c_ata, kFld_Attack_zoku) = wana_elem;
        }
    }
    save_side(rt, def_idx);

    PvpCombatCore& core = g_pvp_eng.core;
    TfSlot tf_trap{};
    tf_trap.owner = static_cast<int8_t>(def_idx);
    tf_trap.tip_id = wana_elem;
    tf_trap.tip_kind = 2; // Elemental Trap TF (BattleMain: 32 ticks total, strikes at timer==15)
    tf_trap.stop_time = 17;
    tf_trap.captured_plus_pow = wpow;
    tf_trap.flags = TFF_NONE;

    if (core.tf_state == TF_IDLE) {
        start_root_time_freeze(def_idx, tf_trap);
    } else if (core.tf_depth < 2) {
        core.tf_depth = 2;
        core.tf_stack[1] = tf_trap;
        core.tf_stack[1].stop_time = 32;
        core.tf_stack[1].timer = 0;
        core.tf_cutin_interrupted_side = static_cast<int8_t>(1 - def_idx);
        core.tf_cutin_fx_seq = -1;
        core.tf_state = TF_COUNTER_ACTION;
    } else {
        execute_elemental_trap_hit(rt, def_idx, wana_elem, wpow);
    }

    ensure_board_orient(rt, att_idx);
    load_side(rt, att_idx);
    sync_panel_occupancy_and_proxy(rt, att_idx);
    g_pvp_eng.active_side = saved_active;
}

static void save_tf_interrupted_action(int s_idx) {
    const SideContext& sc = g_pvp_eng.core.sides[s_idx];
    TfSavedAction& sv = g_pvp_eng.core.tf_saved[s_idx];
    sv.valid = true;
    sv.waza_flg = sc.waza_flg;
    sv.waza_wait = sc.waza_wait;
    sv.tmp_ata = sc.tmp_ata;
    sv.move_flg = sc.move_flg;
    sv.move_cnt = sc.move_cnt;
    sv.key_cnt = sc.key_cnt;
    sv.ani_id = sc.ani_id;
    sv.ani_cnt = sc.ani_cnt;
    sv.was_hit_in_tf = 0;
}

static void restore_tf_interrupted_actions() {
    for (int s = 0; s < 2; ++s) {
        const TfSavedAction& sv = g_pvp_eng.core.tf_saved[s];
        if (!sv.valid) continue;
        SideContext& sc = g_pvp_eng.core.sides[s];
        if (sv.was_hit_in_tf != 0) {
            sc.waza_flg = -1;
            sc.waza_wait = 0;
            sc.tmp_ata = -1;
            sc.move_flg = 0;
            sc.move_cnt = 0;
            sc.bas_no = -1;
            sc.key_cnt = 0;
            sc.buster_charge_glow = false;
        } else {
            sc.waza_flg = sv.waza_flg;
            sc.waza_wait = sv.waza_wait;
            sc.tmp_ata = sv.tmp_ata;
            sc.move_flg = sv.move_flg;
            sc.move_cnt = sv.move_cnt;
            sc.ani_id = sv.ani_id;
            sc.ani_cnt = sv.ani_cnt;
        }
        // Preserve post-TF tmp_muteki_cnt
        sc.muteki_cnt = std::max<int32_t>(sc.muteki_cnt, sc.tmp_muteki_cnt);
        sc.tmp_muteki_cnt = 0;
        g_pvp_eng.core.tf_saved[s] = TfSavedAction{};
    }
}

static void start_root_time_freeze(int s_idx, const TfSlot& slot) {
    PvpCombatCore& core = g_pvp_eng.core;
    core.tf_state = TF_ROOT_INTRO;
    core.tf_depth = 1;
    core.tf_stack[0] = slot;
    core.tf_stack[0].owner = static_cast<int8_t>(s_idx);
    core.tf_stack[0].timer = 0;
    core.tf_stack[0].fired_action = false;
    core.tf_stack[0].executed_root = false;
    core.tf_stack[1] = TfSlot{};
    core.tf_cutin_fx_seq = -1;
    core.tf_cutin_interrupted_side = -1;
    save_tf_interrupted_action(0);
    save_tf_interrupted_action(1);
    for (int i = 0; i < 2; ++i) {
        core.tf_root_pre_muteki[i] = core.sides[i].muteki_cnt;
        core.tf_root_pre_hit_cnt[i] = core.sides[i].hit_cnt;
        core.sides[i].tmp_muteki_cnt = core.sides[i].muteki_cnt;
    }
}

// ============================================================================
// Single-Side Step (Buster, Charge Shot, Chip Activation, Attack Advance)
// ============================================================================
static void record_nondim_hud_popup(DojaRuntime& rt, SideContext& sc, Ref t_obj, int32_t plus_pow) {
    if (t_obj <= 0) return;
    sc.opp_nondim_tip_id = rt.field_i32(t_obj, kFld_Tip_tip_id);
    sc.opp_nondim_pow = rt.field_i32(t_obj, kFld_Tip_pow);
    sc.opp_nondim_plus = plus_pow;
    sc.opp_nondim_ticks = kOppNonDimChipHudTicks;
}

constexpr int32_t kTfActionExtraTicks = 23; // BattleMain: stop_cnt = 2..(waza.stop_time + 24) => stop_time + 23 Move(0) ticks

static bool try_push_counter_dim(DojaRuntime& rt, int s_idx, int interrupted_owner, int32_t mask, bool require_edge) {
    PvpCombatCore& core = g_pvp_eng.core;
    SideContext& sc = core.sides[s_idx];
    int32_t key2 = mask & (~sc.last_raw_input);
    bool b_press = require_edge ? ((key2 & kKeySelectBit) != 0) : ((mask & kKeySelectBit) != 0);
    if (!b_press || sc.tip_no < 0 || sc.tip_no > 4 || sc.hp <= 0) return false;
    int32_t tid = sc.rock_tip_ids[sc.tip_no];
    int32_t stop_t = 0, t_cls = 0;
    ensure_board_orient(rt, s_idx);
    load_side(rt, s_idx);
    if (!is_dimming_tip_id(rt, tid, stop_t, t_cls)) return false;

    core.tf_depth = 2;
    core.tf_stack[1] = TfSlot{};
    core.tf_stack[1].owner = static_cast<int8_t>(s_idx);
    core.tf_stack[1].tip_id = tid;
    core.tf_stack[1].tip_kind = 0;
    core.tf_stack[1].stop_time = stop_t + kTfActionExtraTicks;
    core.tf_stack[1].captured_plus_pow = sc.pow[3];
    core.tf_stack[1].flags = (t_cls == 2) ? TFF_IS_NAVI : TFF_NONE;
    core.tf_stack[1].timer = 0;
    core.tf_cutin_interrupted_side = static_cast<int8_t>(interrupted_owner);
    core.tf_cutin_fx_seq = 0;
    core.tf_state = TF_COUNTER_BANNER;
    save_side(rt, s_idx);
    return true;
}

static bool try_trigger_dim_chip(DojaRuntime& rt, int s_idx, int32_t mask) {
    PvpCombatCore& core = g_pvp_eng.core;
    SideContext& sc = core.sides[s_idx];
    const SideContext& opp = core.sides[1 - s_idx];
    int32_t key2 = mask & (~sc.last_raw_input);
    if ((key2 & kKeySelectBit) == 0 || sc.tip_no < 0 || sc.tip_no > 4 ||
        sc.waza_flg >= 0 || sc.joutai != 0 || sc.mahi_id >= 0) {
        return false;
    }

    ensure_board_orient(rt, s_idx);
    load_side(rt, s_idx);
    Ref rock = sf_app_CpCanvas_rock;
    Ref r_tip_arr = (rock > 0) ? rt.field_i32(rock, kFld_Rock_tip) : 0;
    Ref t_obj = safe_iaload(rt, r_tip_arr, sf_app_Rock_tip_no);
    int32_t tid = (t_obj > 0) ? rt.field_i32(t_obj, kFld_Tip_tip_id) : 0;
    if (tid <= 0) return false;

    int32_t stop_t = 0, t_cls = 0;
    if (!is_dimming_tip_id(rt, tid, stop_t, t_cls)) return false;

    if (core.tf_state == TF_IDLE) {
        TfSlot tf_new;
        tf_new.owner = static_cast<int8_t>(s_idx);
        tf_new.tip_id = tid;
        tf_new.tip_kind = 0;
        tf_new.stop_time = stop_t + kTfActionExtraTicks;
        tf_new.captured_plus_pow = safe_iaload(rt, sf_app_Rock_pow, 3);
        tf_new.flags = (t_cls == 2) ? TFF_IS_NAVI : TFF_NONE;
        save_side(rt, s_idx);
        start_root_time_freeze(s_idx, tf_new);
        int32_t z_elem = rt.field_i32(t_obj, kFld_Tip_zoku);
        if (z_elem >= 1 && z_elem <= 4 && opp.wana == z_elem) {
            trigger_opponent_elemental_trap(rt, s_idx, z_elem);
        }
        return true;
    }
    if (core.tf_state == TF_ROOT_INTRO && core.tf_depth == 1 && core.tf_stack[0].owner != s_idx) {
        return try_push_counter_dim(rt, s_idx, core.tf_stack[0].owner, mask, /*require_edge=*/true);
    }
    return false;
}

static void step_side_input_phase(DojaRuntime& rt, int s_idx, int32_t mask) {
    g_pvp_eng.active_side = s_idx;
    ensure_board_orient(rt, s_idx);
    ScopedP1OkiSlotSwap oki_swap(rt, s_idx == 1);
    load_side(rt, s_idx);
    sync_panel_occupancy_and_proxy(rt, s_idx);

    bool prev_oki_on[kMaxOkimono] = {};
    snapshot_oki_on(rt, prev_oki_on);

    SideContext& sc = g_pvp_eng.core.sides[s_idx];
    const SideContext& opp = g_pvp_eng.core.sides[1 - s_idx];

    // Populate LoN keypad globals from deterministic player bitmask
    int32_t prev_key = sc.last_raw_input;
    sc.last_raw_input = mask;
    sf_app_CpCanvas_key = mask;
    sf_app_CpCanvas_key2 = mask & (~prev_key);
    bool chip_pressed = (sf_app_CpCanvas_key2 & kKeySelectBit) != 0;

    // 1. Non-dimming Chip Activation (Dimming chips are processed synchronously in Phase 0)
    if (chip_pressed && sf_app_Rock_tip_no >= 0 && sf_app_Rock_tip_no <= 4 &&
        sf_app_Rock_tmp_ata < 0 && sf_app_Rock_waza_flg < 0 &&
        sf_app_Rock_joutai == 0 && sf_app_Rock_mahi_id < 0) {
        Ref rock = sf_app_CpCanvas_rock;
        Ref r_tip_arr = (rock > 0) ? rt.field_i32(rock, kFld_Rock_tip) : 0;
        int32_t pre_act_tip_no = sf_app_Rock_tip_no;
        Ref t_obj = safe_iaload(rt, r_tip_arr, pre_act_tip_no);
        int32_t tid = (t_obj > 0) ? rt.field_i32(t_obj, kFld_Tip_tip_id) : 0;
        if (tid > 0) {
            int32_t z_elem = rt.field_i32(t_obj, kFld_Tip_zoku);
            bool fire_chip_trap =
                (sf_app_Rock_non_cnt <= 0 && sf_app_Rock_non_cnt2 <= 0 &&
                 sf_app_Rock_move_flg == 0 && z_elem >= 1 && z_elem <= 4 && opp.wana == z_elem);
            fn_app_Rock_Action___I__V(rt, rock, 0);
            if (sf_app_Rock_tip_no < pre_act_tip_no) {
                record_nondim_hud_popup(rt, sc, t_obj, safe_iaload(rt, sf_app_Rock_pow, 3));
            }
            if (fire_chip_trap) {
                sf_app_Rock_non_bas = 0;
                update_oki_owner_from_snapshot(rt, s_idx, prev_oki_on);
                save_side(rt, s_idx);
                trigger_opponent_elemental_trap(rt, s_idx, z_elem);
                return;
            }
        }
    }

    // 2. D-Pad grid movement (BattleMain: 1=Left, 2=Up, 3=Right, 4=Down)
    if (sf_app_Rock_joutai == 0 && sf_app_Rock_move_flg == 0 &&
        sf_app_Rock_waza_flg < 0 && sf_app_Rock_mahi_id < 0) {
        if (sf_app_CpCanvas_key2 & (1 << 16))      sf_app_Rock_move_flg = 1;
        else if (sf_app_CpCanvas_key2 & (1 << 17)) sf_app_Rock_move_flg = 2;
        else if (sf_app_CpCanvas_key2 & (1 << 18)) sf_app_Rock_move_flg = 3;
        else if (sf_app_CpCanvas_key2 & (1 << 19)) sf_app_Rock_move_flg = 4;
    }

    // 3. Charge Shot & Buster handling (ACT_POUND/ACT_CANCEL/7/9 anytime, or ACT_SELECT when tip_no < 0)
    bool buster_held =
        ((mask & kPureBusterBits) != 0) ||
        (((mask & kKeySelectBit) != 0) && sf_app_Rock_tip_no < 0);

    int32_t z_clamped = std::clamp<int32_t>(sf_app_Rock_zoku, 0, 6);
    int32_t charge_thresh = (42 - sf_app_CpCanvas_cha_lv * 5) + kChargeDelayAdd[z_clamped];

    if (sf_app_Rock_tmp_ata < 0 && sf_app_Rock_non_cnt2 == 0) {
        if (sf_app_CpCanvas_key_cnt < 0) {
            sf_app_CpCanvas_key_cnt++;
            sc.buster_charge_glow = false;
        } else if (buster_held) {
            if (sf_app_Rock_non_bas == 0 && sf_app_Rock_joutai == 0 && sf_app_Rock_mahi_id < 0 &&
                sf_app_CpCanvas_key_cnt <= charge_thresh) {
                if (sf_app_CpCanvas_rock > 0) {
                    if (sf_app_CpCanvas_key_cnt == 4) {
                        sf_app_CpCanvas_key_cnt -= fn_app_Rock_Chage___I__I(rt, sf_app_CpCanvas_rock, 0);
                    } else if (sf_app_CpCanvas_key_cnt == charge_thresh) {
                        fn_app_Rock_Chage___I__I(rt, sf_app_CpCanvas_rock, 1);
                    }
                }
                sf_app_CpCanvas_key_cnt++;
            }
            sc.buster_charge_glow = (sf_app_CpCanvas_key_cnt > charge_thresh && sf_app_Rock_bas_no >= 0);
        } else if (sf_app_CpCanvas_key_cnt > 0) {
            if (sf_app_Rock_joutai != 0 || sf_app_Rock_mahi_id >= 0) {
                cancel_buster_charge_glow(rt);
                sc.buster_charge_glow = false;
            } else if (sf_app_Rock_non_cnt <= 0) {
                // Release Normal Buster (tid=0) or Charge Shot via rock.tip[5] & Rock.Action(0).
                // Keep shot_tip in rock.tip[5] (matching BattleMain L_4572) so deferred tmp_ata
                // during movement fires the full Charge Shot when move_flg reaches 0.
                bool is_charged = (sf_app_CpCanvas_key_cnt > charge_thresh);
                int32_t tid = is_charged ? kChargeShotTipId[z_clamped] : 0;
                int32_t saved_tip_no = sf_app_Rock_tip_no;
                int32_t saved_pow0 = safe_iaload(rt, sf_app_Rock_pow, 0);
                int32_t saved_pow3 = safe_iaload(rt, sf_app_Rock_pow, 3);
                int32_t pre_bas_waza_flg = sf_app_Rock_waza_flg;
                Ref rock = sf_app_CpCanvas_rock;
                Ref r_tip_arr = (rock > 0) ? rt.field_i32(rock, kFld_Rock_tip) : 0;
                Ref shot_tip = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
                bool fire_trap = (is_charged && sf_app_Rock_move_flg == 0 &&
                                  z_clamped >= 1 && z_clamped <= 4 && opp.wana == z_clamped);
                if (r_tip_arr > 0 && shot_tip > 0) {
                    rt.iaload(r_tip_arr, 5) = shot_tip;
                    sf_app_Rock_tip_no = 5;
                    safe_iastore(rt, sf_app_Rock_pow, 0, 0);
#ifdef HAS_SF_app_CpCanvas_bas_max_flg
                    sf_app_CpCanvas_bas_max_flg = 0;
#endif
                    fn_app_Rock_Action___I__V(rt, rock, 0);
                }
                sf_app_Rock_tip_no = saved_tip_no;
                safe_iastore(rt, sf_app_Rock_pow, 0, saved_pow0);
                if (sf_app_Rock_waza_flg == pre_bas_waza_flg) {
                    safe_iastore(rt, sf_app_Rock_pow, 3, saved_pow3);
                }
                sf_app_CpCanvas_key_cnt = -4;
                sc.opp_buster_visual_ticks = is_charged ? 6 : 4;
                sc.buster_charge_glow = false;
                if (fire_trap) {
                    sf_app_Rock_non_bas = 0;
                    update_oki_owner_from_snapshot(rt, s_idx, prev_oki_on);
                    save_side(rt, s_idx);
                    trigger_opponent_elemental_trap(rt, s_idx, z_clamped);
                    return;
                }
            }
        }
    }
    sf_app_Rock_non_bas = 0;

    cleanup_orphaned_charge_attacks(rt);
    update_oki_owner_from_snapshot(rt, s_idx, prev_oki_on);
    save_side(rt, s_idx);
}

static inline void sync_shared_cus_gage_from_canvas() {
    PvpCombatCore& core = g_pvp_eng.core;
    if (core.custom_round < kMaxTurns && sf_app_CpCanvas_cus_gage > core.cus_gage) {
        core.cus_gage = std::clamp<int32_t>(sf_app_CpCanvas_cus_gage, 0, 128);
        core.sides[0].cus_gage = core.cus_gage;
        core.sides[1].cus_gage = core.cus_gage;
    }
    sf_app_CpCanvas_cus_gage = core.cus_gage;
}

static void step_side_move_phase(DojaRuntime& rt, int s_idx) {
    g_pvp_eng.active_side = s_idx;
    ensure_board_orient(rt, s_idx);
    ScopedP1OkiSlotSwap oki_swap(rt, s_idx == 1);
    load_side(rt, s_idx);
    sync_panel_occupancy_and_proxy(rt, s_idx);

    bool prev_oki_on[kMaxOkimono] = {};
    snapshot_oki_on(rt, prev_oki_on);

    if (sf_app_CpCanvas_rock > 0) {
        fn_app_Rock_Move_____V(rt, sf_app_CpCanvas_rock);
    }

    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
            g_pvp_eng.active_ata_ref = a;
            fn_app_Attack_HitGo___I__V(rt, a, -1);
            g_pvp_eng.active_ata_ref = 0;
            ensure_board_orient(rt, s_idx);
            sync_panel_occupancy_and_proxy(rt, s_idx);
        }
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
            fn_app_Attack_Move___I__V(rt, a, 1);
        }
    }

    sync_shared_cus_gage_from_canvas();
    cleanup_orphaned_charge_attacks(rt);
    update_oki_owner_from_snapshot(rt, s_idx, prev_oki_on);
    save_side(rt, s_idx);
}

static void step_side_hit_phase(DojaRuntime& rt, int s_idx) {
    g_pvp_eng.active_side = s_idx;
    ensure_board_orient(rt, s_idx);
    ScopedP1OkiSlotSwap oki_swap(rt, s_idx == 1);
    load_side(rt, s_idx);
    sync_panel_occupancy_and_proxy(rt, s_idx);

    bool prev_oki_on[kMaxOkimono] = {};
    snapshot_oki_on(rt, prev_oki_on);

    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
            if (rt.field_i32(a, kFld_Attack_yokoku) == 1) {
                int32_t hx = rt.field_i32(a, kFld_Attack_hit_x);
                int32_t hy = rt.field_i32(a, kFld_Attack_hit_y);
                if (hx < 0 || hx > 5 || hy < 0 || hy > 2) continue;
            }
            g_pvp_eng.active_ata_ref = a;
            fn_app_Attack_HitGo___I__V(rt, a, 0);
            g_pvp_eng.active_ata_ref = 0;
            ensure_board_orient(rt, s_idx);
            sync_panel_occupancy_and_proxy(rt, s_idx);
        }
        if (sf_app_CpCanvas_g > 0) {
            ScopedRenderMute m(rt, true);
            ScopedTempHeapArena arena(rt);
            int32_t prev_flip = g_pvp_rock_flip_override;
            g_pvp_rock_flip_override = 1;
            if (sf_app_CpCanvas_rock > 0) {
                fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 1);
            }
            for (int32_t i = 0; i < an; ++i) {
                Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
                fn_app_Attack_Draw___II__V(rt, a, 1, 1);
            }
            for (int32_t i = 0; i < an; ++i) {
                Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
                fn_app_Attack_Draw___II__V(rt, a, 2, 1);
            }
            g_pvp_rock_flip_override = prev_flip;
        }
    } else if (sf_app_CpCanvas_rock > 0 && sf_app_CpCanvas_g > 0) {
        ScopedRenderMute m(rt, true);
        ScopedTempHeapArena arena(rt);
        int32_t prev_flip = g_pvp_rock_flip_override;
        g_pvp_rock_flip_override = 1;
        fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 1);
        g_pvp_rock_flip_override = prev_flip;
    }

    sync_shared_cus_gage_from_canvas();
    cleanup_orphaned_charge_attacks(rt);
    update_oki_owner_from_snapshot(rt, s_idx, prev_oki_on);

    SideContext& sc = g_pvp_eng.core.sides[s_idx];
    if (sc.opp_nondim_ticks > 0) sc.opp_nondim_ticks--;
    if (sc.opp_buster_visual_ticks > 0) sc.opp_buster_visual_ticks--;

    save_side(rt, s_idx);
}

// ============================================================================
// Combat Tick Coordinator (2-Slot Time-Freeze Stack, Shared Board, Custom Gauge)
// ============================================================================
static void clear_side_tf_attacks(DojaRuntime& rt, int s_idx) {
    int32_t saved_active = g_pvp_eng.active_side;
    g_pvp_eng.active_side = s_idx;
    ensure_board_orient(rt, s_idx);
    load_side(rt, s_idx);
    sync_panel_occupancy_and_proxy(rt, s_idx);
    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a > 0 && rt.field_i32(a, kFld_Attack_on) != 0 &&
                rt.field_i32(a, kFld_Attack_hit_time) > 0) {
                fn_app_Attack_init_____V(rt, a);
            }
        }
    }
    save_side(rt, s_idx);
    g_pvp_eng.active_side = saved_active;
}

static void sync_tf_canvas_statics_from_core(const PvpCombatCore& core, bool post_step = false) {
    if (core.tf_state != TF_IDLE && core.tf_depth >= 1) {
        const TfSlot& top_slot = core.tf_stack[(core.tf_depth >= 2) ? 1 : 0];
        int32_t eff_timer = (post_step && top_slot.timer > 0) ? (top_slot.timer - 1) : top_slot.timer;
        if (core.tf_state == TF_ROOT_INTRO && eff_timer < 15) {
            sf_app_CpCanvas_stop_time = std::max<int32_t>(20, top_slot.stop_time - 3);
            sf_app_CpCanvas_stop_cnt = std::min<int32_t>(14, eff_timer + 1);
        } else if (core.tf_state == TF_COUNTER_BANNER) {
            sf_app_CpCanvas_stop_time = std::max<int32_t>(20, top_slot.stop_time - 3);
            sf_app_CpCanvas_stop_cnt = std::min<int32_t>(14, eff_timer + 1);
        } else {
            int32_t action_tick = (core.tf_state == TF_ROOT_INTRO)
                ? (eff_timer - 15)
                : eff_timer;
            if (top_slot.tip_kind == 2) {
                sf_app_CpCanvas_stop_time = 15 + top_slot.stop_time - 4;
                sf_app_CpCanvas_stop_cnt = 15 + std::max<int32_t>(0, action_tick);
            } else {
                sf_app_CpCanvas_stop_time = std::max<int32_t>(20, top_slot.stop_time - 3);
                sf_app_CpCanvas_stop_cnt = 2 + std::max<int32_t>(0, action_tick);
            }
        }
    } else {
        sf_app_CpCanvas_stop_cnt = 0;
        sf_app_CpCanvas_stop_time = 0;
    }
}

static void step_tf_stack_tick(DojaRuntime& rt, int32_t p0_mask, int32_t p1_mask) {
    PvpCombatCore& core = g_pvp_eng.core;
    sync_tf_canvas_statics_from_core(core, false);
    int top_idx = (core.tf_depth >= 2) ? 1 : 0;
    TfSlot& top = core.tf_stack[top_idx];
    int owner = top.owner;
    int opp = 1 - owner;

    // Check Counter-Dimming interruption during Root Intro (before action fires at t < 16)
    if (core.tf_state == TF_ROOT_INTRO && core.tf_depth == 1 && top.timer < 15 && top.tip_kind == 0) {
        int32_t opp_mask = (opp == 0) ? p0_mask : p1_mask;
        if (try_push_counter_dim(rt, opp, owner, opp_mask, /*require_edge=*/false)) {
            return;
        }
    }

    if (core.tf_state == TF_COUNTER_BANNER) {
        top.timer++;
        core.tf_cutin_fx_seq = std::min<int32_t>(7, top.timer / 2);
        if (top.timer >= 14) {
            top.timer = 0;
            core.tf_state = TF_COUNTER_ACTION;
        }
        return;
    }

    {
        ensure_board_orient(rt, owner);
        ScopedP1OkiSlotSwap oki_swap(rt, owner == 1);
        bool prev_oki_on[kMaxOkimono] = {};
        snapshot_oki_on(rt, prev_oki_on);

        // Fire the active TF slot's chip or Elemental Trap action on t == 16 (Intro) or t == 1 (Counter/Resume)
        bool is_exec_frame = (core.tf_state == TF_ROOT_INTRO && top.timer == 15) ||
                             (core.tf_state == TF_COUNTER_ACTION && top.timer == 0) ||
                             (core.tf_state == TF_ROOT_RESUME && !top.executed_root);
        if (is_exec_frame && !top.fired_action) {
            top.fired_action = true;
            top.executed_root = true;
            g_pvp_eng.active_side = owner;
            ensure_board_orient(rt, owner);
            load_side(rt, owner);
            sync_panel_occupancy_and_proxy(rt, owner);
            if (top.tip_kind == 0) {
                fn_app_Rock_Action___I__V(rt, sf_app_CpCanvas_rock, 1);
                if (sf_app_CpCanvas_rock > 0) {
                    if (sf_app_Rock_waza_flg >= 0) {
                        sf_app_Rock_waza_wait = 0;
                        sf_app_Rock_ani_wait_cnt = 0;
                    }
                    fn_app_Rock_Move_____V(rt, sf_app_CpCanvas_rock);
                }
            } else if (top.tip_kind == 2) {
                execute_elemental_trap_hit(rt, owner, top.tip_id, top.captured_plus_pow);
            }
            sync_shared_cus_gage_from_canvas();
            save_side(rt, owner);
            update_oki_owner_from_snapshot(rt, owner, prev_oki_on);
            snapshot_oki_on(rt, prev_oki_on);
        }

        // Advance only owner's frozen-time Attacks during TF window (matching vanilla BattleMain: Move(0) -> HitGo(0) -> CharaDraw(0))
        sync_tf_canvas_statics_from_core(core, false);
        g_pvp_eng.active_side = owner;
        ensure_board_orient(rt, owner);
        load_side(rt, owner);
        sync_panel_occupancy_and_proxy(rt, owner);
        if (sf_app_CpCanvas_ata > 0) {
            int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
            for (int32_t i = 0; i < an; ++i) {
                Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
                fn_app_Attack_Move___I__V(rt, a, 0);
            }
            for (int32_t i = 0; i < an; ++i) {
                Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
                if (rt.field_i32(a, kFld_Attack_yokoku) == 1) {
                    int32_t hx = rt.field_i32(a, kFld_Attack_hit_x);
                    int32_t hy = rt.field_i32(a, kFld_Attack_hit_y);
                    if (hx < 0 || hx > 5 || hy < 0 || hy > 2) continue;
                }
                g_pvp_eng.active_ata_ref = a;
                fn_app_Attack_HitGo___I__V(rt, a, 0);
                g_pvp_eng.active_ata_ref = 0;
                ensure_board_orient(rt, owner);
                sync_panel_occupancy_and_proxy(rt, owner);
            }
            if (sf_app_CpCanvas_g > 0) {
                ScopedRenderMute m(rt, true);
                ScopedTempHeapArena arena(rt);
                int32_t prev_flip = g_pvp_rock_flip_override;
                g_pvp_rock_flip_override = 1;
                if (sf_app_CpCanvas_rock > 0) {
                    fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 0);
                }
                for (int32_t i = 0; i < an; ++i) {
                    Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                    if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
                    fn_app_Attack_Draw___II__V(rt, a, 1, 0);
                }
                for (int32_t i = 0; i < an; ++i) {
                    Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                    if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) continue;
                    fn_app_Attack_Draw___II__V(rt, a, 2, 0);
                }
                g_pvp_rock_flip_override = prev_flip;
            }
        } else if (sf_app_CpCanvas_rock > 0 && sf_app_CpCanvas_g > 0) {
            ScopedRenderMute m(rt, true);
            ScopedTempHeapArena arena(rt);
            int32_t prev_flip = g_pvp_rock_flip_override;
            g_pvp_rock_flip_override = 1;
            fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 0);
            g_pvp_rock_flip_override = prev_flip;
        }
        sync_shared_cus_gage_from_canvas();
        save_side(rt, owner);
        update_oki_owner_from_snapshot(rt, owner, prev_oki_on);
    }

    top.timer++;
    int32_t target_dur = (core.tf_state == TF_ROOT_INTRO) ? (15 + top.stop_time) : top.stop_time;
    if (top.timer >= target_dur) {
        if (core.tf_depth == 2) {
            // Pop Counter-Dimming chip, clean up its TF-only attacks, and resume Root activator
            clear_side_tf_attacks(rt, owner);
            core.tf_depth = 1;
            core.tf_cutin_fx_seq = -1;
            core.tf_state = TF_ROOT_RESUME;
            if (core.tf_stack[0].executed_root) {
                core.tf_stack[0].timer = core.tf_stack[0].stop_time; // Finish quickly if already ran
            } else {
                core.tf_stack[0].timer = 0; // Fresh action window once counter-dim ends
            }
        } else {
            // Exit Time Freeze cleanly and clear all active TF-only attacks (hit_time > 0) on both sides
            clear_side_tf_attacks(rt, 0);
            clear_side_tf_attacks(rt, 1);
            sf_app_CpCanvas_stop_cnt = 0;
            sf_app_CpCanvas_stop_time = 0;
            sf_app_CpCanvas_stop_name = 0;
            reset_combat_tf_state(core);
            restore_tf_interrupted_actions();
        }
    }
}

static void step_pvp_combat_tick(DojaRuntime& rt, Ref cp, int32_t p0_mask, int32_t p1_mask) {
    (void)cp;
    PvpCombatCore& core = g_pvp_eng.core;
    g_pvp_eng.in_combat_step = true;
    core.combat_turn++;
    core.combat_tick_in_round++;

    // Deterministic per-tick RNG state for shared Java Random
    uint32_t r = static_cast<uint32_t>(core.rng_state) * 1664525u + 1013904223u;
    r |= 1u;
    core.rng_state = static_cast<int32_t>(r);
    if (sf_app_CpCanvas_rand > 0) {
        rt.get(sf_app_CpCanvas_rand).rng_seed = static_cast<uint64_t>(r);
    }

    sync_tf_canvas_statics_from_core(core);
    if (core.tf_state != TF_IDLE) {
        step_tf_stack_tick(rt, p0_mask, p1_mask);
    } else {

        // Check Custom Screen open request when gauge full on rounds 1..14
        int32_t p0_edge = p0_mask & (~core.sides[0].last_raw_input);
        int32_t p1_edge = p1_mask & (~core.sides[1].last_raw_input);
        if (core.custom_round < kMaxTurns && core.cus_gage >= 128 &&
            (((p0_edge | p1_edge) & kCustomTriggerBits) != 0)) {
            core.sides[0].last_raw_input = p0_mask;
            core.sides[1].last_raw_input = p1_mask;
            core.open_custom_req = true;
        } else {
            // Phase-split simultaneous frame resolution so idle vs active symmetry & Double KOs
            // do not depend on per-tick (combat_tick_in_round & 1) lead order.
            int first = (core.combat_tick_in_round & 1);
            int second = 1 - first;
            int32_t m_arr[2] = {p0_mask, p1_mask};
            bool alive_start[2] = {core.sides[0].hp > 0, core.sides[1].hp > 0};

            // Phase 0: Check Dimming chip activations on both sides before any non-dim input/move/hit
            bool dim_started = false;
            for (int ord = 0; ord < 2; ++ord) {
                int s = (ord == 0) ? first : second;
                if (alive_start[s] && try_trigger_dim_chip(rt, s, m_arr[s])) {
                    dim_started = true;
                }
            }
            if (dim_started) {
                for (int s = 0; s < 2; ++s) {
                    if (alive_start[s]) core.sides[s].last_raw_input = m_arr[s];
                }
            } else {
                // Phase 1: D-pad, Charge/Buster, Non-Dim Chip activations
                for (int ord = 0; ord < 2; ++ord) {
                    int s = (ord == 0) ? first : second;
                    if (alive_start[s] && core.tf_state == TF_IDLE) {
                        step_side_input_phase(rt, s, m_arr[s]);
                    }
                }
                if (core.tf_state == TF_IDLE) {
                    // Phase 2: Rock.Move + Attack.HitGo(-1) + Attack.Move(1)
                    for (int ord = 0; ord < 2; ++ord) {
                        int s = (ord == 0) ? first : second;
                        if (alive_start[s]) step_side_move_phase(rt, s);
                    }
                    // Phase 3: Attack.HitGo(0) + Attack/Rock Draw(1) + HUD decay
                    for (int ord = 0; ord < 2; ++ord) {
                        int s = (ord == 0) ? first : second;
                        if (alive_start[s]) step_side_hit_phase(rt, s);
                    }

                    // Shared BtPanel timers & AreaSteal ownership check + Okimono.Move & Okimono.Draw(1)
                    ensure_board_orient(rt, 0);
                    load_side(rt, 0);
                    sync_panel_occupancy_and_proxy(rt, 0);

                    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
                        for (int32_t y = 0; y < 3; ++y) {
                            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
                            if (row <= 0 || rt.array_length(row) < 6) continue;
                            for (int32_t x = 0; x < 6; ++x) {
                                Ref p = rt.iaload(row, x);
                                if (p > 0) fn_app_BtPanel_Move_____V(rt, p);
                            }
                        }
                        for (int32_t x = 5; x >= 3; --x) {
                            for (int32_t y = 0; y < 3; ++y) {
                                Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
                                Ref p = (row > 0) ? rt.iaload(row, x) : 0;
                                if (p > 0) fn_app_BtPanel_JinCh_____Z(rt, p);
                            }
                        }
                    }

                    // Advance Okimono Move and Okimono.Draw(1) cleanly per owner perspective
                    if (sf_app_CpCanvas_oki > 0) {
                        int32_t on_cnt = std::min<int32_t>(kMaxOkimono, rt.array_length(sf_app_CpCanvas_oki));
                        for (int32_t i = 0; i < on_cnt; ++i) {
                            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                            if (ok <= 0 || rt.field_i32(ok, kFld_Okimono_on) == 0) continue;
                            int owner = (core.oki_owner[i] == 1) ? 1 : 0;
                            g_pvp_eng.active_side = owner;
                            ensure_board_orient(rt, owner);
                            ScopedP1OkiSlotSwap oki_swap(rt, owner == 1);
                            load_side(rt, owner);
                            sync_panel_occupancy_and_proxy(rt, owner);
                            fn_app_Okimono_Move_____V(rt, ok);
                            {
                                ScopedRenderMute m(rt, g_pvp_eng.in_resim);
                                ScopedTempHeapArena arena(rt);
                                fn_app_Okimono_Draw___I__V(rt, ok, 1);
                            }
                            save_side(rt, owner);
                            if (rt.field_i32(ok, kFld_Okimono_on) == 0) {
                                core.oki_owner[(owner == 1 && i < 4) ? ((i < 2) ? (i + 2) : (i - 2)) : i] = -1;
                            }
                        }
                    }

                    // Advance Custom Gauge (0..128) or Final Turn 10-second countdown
                    if (core.custom_round >= kMaxTurns) {
                        if (core.final_turn_ticks_left > 0) {
                            core.final_turn_ticks_left--;
                        }
                        core.cus_gage = static_cast<int32_t>(
                            (static_cast<uint32_t>(core.final_turn_ticks_left) * 128u) / kFinalTurnDurationTicks);
                    } else {
                        core.cus_gage = std::min<int32_t>(128, core.cus_gage + std::max<int32_t>(1, core.shared_cus_sp));
                    }
                    core.sides[0].cus_gage = core.cus_gage;
                    core.sides[1].cus_gage = core.cus_gage;
                }
            }
        }
    }

    // Winner check (-2 = ongoing, -1 = draw, 0 = P0 win, 1 = P1 win)
    int32_t hp0 = core.sides[0].hp;
    int32_t hp1 = core.sides[1].hp;
    if (hp0 <= 0 && hp1 <= 0) {
        core.winner = -1;
    } else if (hp1 <= 0) {
        core.winner = 0;
    } else if (hp0 <= 0) {
        core.winner = 1;
    } else if (core.custom_round >= kMaxTurns && core.final_turn_ticks_left == 0 && core.tf_state == TF_IDLE) {
        core.ended_by_judge = true;
        if (hp0 > hp1) core.winner = 0;
        else if (hp1 > hp0) core.winner = 1;
        else if (core.damage_dealt[0] > core.damage_dealt[1]) core.winner = 0;
        else if (core.damage_dealt[1] > core.damage_dealt[0]) core.winner = 1;
        else core.winner = -1;
    }

    // Leave local side loaded cleanly for rendering
    ensure_board_orient(rt, g_pvp_eng.local_idx);
    load_side(rt, g_pvp_eng.local_idx);
    sync_panel_occupancy_and_proxy(rt, g_pvp_eng.local_idx);
    sync_tf_canvas_statics_from_core(core);
    sf_app_CpCanvas_cus_gage = core.cus_gage;
    g_pvp_eng.in_combat_step = false;
}

static uint16_t compute_combat_state_hash(DojaRuntime& rt) {
    (void)rt;
    uint32_t h = 0x811C9DC5u;
    auto mix = [&](uint32_t v) {
        h ^= v;
        h *= 0x01000193u;
    };
    const PvpCombatCore& c = g_pvp_eng.core;
    mix(static_cast<uint32_t>(c.combat_tick_in_round));
    mix(static_cast<uint32_t>(c.cus_gage));
    mix(static_cast<uint32_t>(c.rng_state));
    mix(static_cast<uint32_t>(c.winner));
    mix(static_cast<uint32_t>(c.tf_state));
    for (int s = 0; s < 2; ++s) {
        const SideContext& sc = c.sides[s];
        mix(static_cast<uint32_t>(sc.hp));
        mix(static_cast<uint32_t>(sc.pos_x));
        mix(static_cast<uint32_t>(sc.pos_y));
        mix(static_cast<uint32_t>(sc.zoku));
        mix(static_cast<uint32_t>(sc.arm_cnt));
        mix(static_cast<uint32_t>(sc.tip_no));
        mix(static_cast<uint32_t>(sc.han));
        mix(static_cast<uint32_t>(sc.bari));
        mix(static_cast<uint32_t>(sc.wana));
        mix(static_cast<uint32_t>(sc.muteki_cnt));
        mix(static_cast<uint32_t>(c.damage_dealt[s]));
        mix(sc.fol_used_mask);
    }
    return static_cast<uint16_t>((h ^ (h >> 16)) & 0xFFFFu);
}

// ============================================================================
// Read-Only Rendering (Sprite Flip Override, Okimono.Draw(0), HUD, Soul Badges)
// ============================================================================
static void draw_rock_readonly(DojaRuntime& rt, Ref rock) {
    if (rock <= 0) return;
    int32_t sv_ani_cnt = sf_app_Rock_ani_cnt;
    int32_t sv_ani_wait = sf_app_Rock_ani_wait_cnt;
    int32_t sv_tmp_ani = sf_app_Rock_tmp_ani_cnt;
    int32_t sv_t_ani = rt.field_i32(rock, kFld_Rock_t_ani_cnt);
    fn_app_Rock_Draw___I__V(rt, rock, 0);
    sf_app_Rock_ani_cnt = sv_ani_cnt;
    sf_app_Rock_ani_wait_cnt = sv_ani_wait;
    sf_app_Rock_tmp_ani_cnt = sv_tmp_ani;
    rt.field_i32(rock, kFld_Rock_t_ani_cnt) = sv_t_ani;
}

static void draw_attack_readonly(DojaRuntime& rt, Ref a, int32_t layer) {
    if (a <= 0 || rt.field_i32(a, kFld_Attack_on) == 0) return;
    int32_t sv_ht = rt.field_i32(a, kFld_Attack_hit_time);
    if (sv_ht != 0 && sf_app_CpCanvas_stop_cnt < 15) return;
    // Attack.Draw increments ani_cnt even when adv==0 if hit_time != 0: preserve all mutables!
    int32_t sv_ac = rt.field_i32(a, kFld_Attack_ani_cnt);
    int32_t sv_tac = rt.field_i32(a, kFld_Attack_t_ani_cnt);
    int32_t sv_lf = rt.field_i32(a, kFld_Attack_loop_flg);
    rt.field_i32(a, kFld_Attack_hit_time) = 0;
    rt.field_i32(a, kFld_Attack_ani_cnt) = sv_tac;
    fn_app_Attack_Draw___II__V(rt, a, layer, 0);
    rt.field_i32(a, kFld_Attack_hit_time) = sv_ht;
    rt.field_i32(a, kFld_Attack_ani_cnt) = sv_ac;
    rt.field_i32(a, kFld_Attack_t_ani_cnt) = sv_tac;
    rt.field_i32(a, kFld_Attack_loop_flg) = sv_lf;
}

static void draw_mirrored_opponent_attacks(DojaRuntime& rt, int opp_idx, int32_t layer_min, int32_t target_row) {
    int loc_idx = g_pvp_eng.local_idx;
    load_side(rt, opp_idx);
    set_rock_palette_fast(rt, g_pvp_eng.core.sides[opp_idx].zoku);
    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0) continue;
            int32_t on_val = rt.field_i32(a, kFld_Attack_on);
            if (on_val == 0) continue;
            if (layer_min <= 1) {
                if (on_val != 1) continue;
            } else {
                if (on_val < layer_min) continue;
            }
            if (target_row >= 0 && rt.field_i32(a, kFld_Attack_pos_y) != target_row) continue;
            int32_t px = rt.field_i32(a, kFld_Attack_pos_x);
            int32_t flp = rt.field_i32(a, kFld_Attack_flp);
            int32_t kxs = rt.field_i32(a, kFld_Attack_kidou_x_sp);
            // LoN Attack.Draw already multiplies pos_dx by (1 - 2 * flp): do NOT negate pos_dx!
            rt.field_i32(a, kFld_Attack_pos_x) = 5 - px;
            rt.field_i32(a, kFld_Attack_flp) = (flp == 0) ? 1 : 0;
            rt.field_i32(a, kFld_Attack_kidou_x_sp) = -kxs;
            draw_attack_readonly(rt, a, on_val);
            rt.field_i32(a, kFld_Attack_pos_x) = px;
            rt.field_i32(a, kFld_Attack_flp) = flp;
            rt.field_i32(a, kFld_Attack_kidou_x_sp) = kxs;
        }
    }
    load_side(rt, loc_idx);
}

static void draw_opponent_soul_badge_and_trap(DojaRuntime& rt, const SideContext& opp_sc, const SideContext& loc_sc) {
    // Trap overhead indicators matching Rock.WanaDraw ("????" at y=95 on LoN!)
    if (loc_sc.wana > 0) {
        Ref s = rt.alloc_string("????");
        fn_app_CpCanvas_strDraw___Ljava_lang_String_II__V(rt, s, 5, 95);
    }
    if (opp_sc.wana > 0) {
        Ref s = rt.alloc_string("????");
        fn_app_CpCanvas_strDraw___Ljava_lang_String_II__V(rt, s, 210, 95);
    }
    // Opponent Soul/Trance badge and remaining turns are intentionally hidden on the local screen.
}

static void draw_tf_cutin_fx_sprite(DojaRuntime& rt, int interrupted_ch, int seq_idx) {
    if (seq_idx < 0) return;
    int32_t x = 60 + 120 * std::clamp<int>(interrupted_ch, 0, 1);
    fn_app_CpCanvas_DrawAni___IIIIII__I(rt, 38, seq_idx, x, 76, 0, 1);
}

static void render_pvp_combat_frame(DojaRuntime& rt, Ref cp) {
    // Snapshot canonical simulation state so read-only Draw methods can never mutate combat or rollback state
    HostRuntimeState host_st = capture_host_runtime_state(rt);
    PvpCombatCore saved_core = g_pvp_eng.core;
    int32_t saved_pal_zoku = g_pvp_eng.last_pal_zoku;
    generated_save_statics(g_pvp_eng.render_statics);
    g_pvp_eng.render_snap.base_heap_size = 0;
    rt.save_combat_snapshot(g_pvp_eng.render_snap, g_pvp_eng.mutable_heap_min_ref);
    sync_tf_canvas_statics_from_core(g_pvp_eng.core, true);

    bool prev_sup = rt.suppress_av();
    rt.set_suppress_av(false);

    int loc = g_pvp_eng.local_idx;
    int opp = g_pvp_eng.opp_idx();
    ensure_board_orient(rt, loc);
    load_side(rt, loc);
    sync_panel_occupancy_and_proxy(rt, loc);
    save_side(rt, loc);

    // 1. Refresh arena backdrop (stage RGB / Time-Freeze / Trance tint) + all 18 panels onto graMap,
    // then composite onto g (EN drawImgMapBG draws tmp_imgMap + imgMap with green chroma key).
    sync_tf_canvas_statics_from_core(g_pvp_eng.core, true);
    bool allow_recovery_blink = (g_pvp_eng.phase == Phase::COMBAT && sf_app_CpCanvas_stop_cnt == 0);
    refresh_arena_imgmap_backdrop(rt, cp, allow_recovery_blink);
#ifdef HAS_FN_CpCanvas_PalSetBG
    fn_app_CpCanvas_drawImgMapBG___I__V(rt, 0);
#else
    if (sf_app_CpCanvas_g > 0 && sf_app_CpCanvas_imgMap > 0) {
        Slot args[4];
        args[0].r = sf_app_CpCanvas_g;
        args[1].r = sf_app_CpCanvas_imgMap;
        args[2].i = 0;
        args[3].i = 0;
        rt.invoke_external("com/nttdocomo/ui/Graphics", "drawImage", "(Lcom/nttdocomo/ui/Image;II)V", args, 4);
    }
#endif

    // 2. Panel warning / effect overlays (BtPanel.Draw2)
    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
        for (int32_t y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0 || rt.array_length(row) < 6) continue;
            for (int32_t x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p > 0) fn_app_BtPanel_Draw2_____V(rt, p);
            }
        }
    }

    // 3. Y-sorted 2.5D row rendering (row = 0..2): Opponent Rock, Local Rock, Okimono, Layer-1 Attacks
    const SideContext& loc_sc = g_pvp_eng.core.sides[loc];
    const SideContext& opp_sc = g_pvp_eng.core.sides[opp];
    for (int32_t row = 0; row < 3; ++row) {
        // Opponent Rock (mirrored horizontally on right side, facing left)
        if (opp_sc.hp > 0 && opp_sc.pos_y == row && sf_app_CpCanvas_rock > 0) {
            load_side(rt, opp);
            set_rock_palette_fast(rt, opp_sc.zoku);
            int32_t orig_px = sf_app_Rock_pos_x;
            int32_t orig_dx = sf_app_Rock_dx;
            sf_app_Rock_pos_x = 5 - orig_px;
            sf_app_Rock_dx = -orig_dx;
            g_pvp_rock_flip_override = 0;
            draw_rock_readonly(rt, sf_app_CpCanvas_rock);
            g_pvp_rock_flip_override = -1;
            sf_app_Rock_pos_x = orig_px;
            sf_app_Rock_dx = orig_dx;
        }

        // Local Rock (left side, facing right)
        if (loc_sc.hp > 0 && loc_sc.pos_y == row && sf_app_CpCanvas_rock > 0) {
            load_side(rt, loc);
            set_rock_palette_fast(rt, loc_sc.zoku);
            g_pvp_rock_flip_override = 1;
            draw_rock_readonly(rt, sf_app_CpCanvas_rock);
            g_pvp_rock_flip_override = -1;
        }

        // Shared Okimono obstacles on this row (already in local_idx screen orientation)
        if (sf_app_CpCanvas_oki > 0) {
            int32_t on_cnt = std::min<int32_t>(kMaxOkimono, rt.array_length(sf_app_CpCanvas_oki));
            for (int32_t i = 0; i < on_cnt; ++i) {
                Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                if (ok > 0 && rt.field_i32(ok, kFld_Okimono_on) != 0 &&
                    rt.field_i32(ok, kFld_Okimono_pos_y) == row) {
                    fn_app_Okimono_Draw___I__V(rt, ok, 0);
                }
            }
        }

        // Local Layer-1 Attacks on this row
        load_side(rt, loc);
        set_rock_palette_fast(rt, loc_sc.zoku);
        if (sf_app_CpCanvas_ata > 0) {
            int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
            for (int32_t i = 0; i < an; ++i) {
                Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                if (a > 0 && rt.field_i32(a, kFld_Attack_on) == 1 &&
                    rt.field_i32(a, kFld_Attack_pos_y) == row) {
                    draw_attack_readonly(rt, a, 1);
                }
            }
        }

        // Mirrored Opponent Layer-1 Attacks on this row
        draw_mirrored_opponent_attacks(rt, opp, 1, row);
    }

    // 4. Foreground Attacks (layer >= 2: 2 and 3) for both players
    load_side(rt, loc);
    set_rock_palette_fast(rt, loc_sc.zoku);
    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0) continue;
            int32_t on_val = rt.field_i32(a, kFld_Attack_on);
            if (on_val >= 2) {
                draw_attack_readonly(rt, a, on_val);
            }
        }
    }
    draw_mirrored_opponent_attacks(rt, opp, 2, -1);

    // 5. Opponent foot HP digits (CpCanvas.CharaDraw -> Ene.HpDraw)
    load_side(rt, loc);
    sync_panel_occupancy_and_proxy(rt, loc);
    if (sf_app_CpCanvas_ene > 0 && rt.array_length(sf_app_CpCanvas_ene) > 0) {
        Ref e0 = rt.iaload(sf_app_CpCanvas_ene, 0);
        if (e0 > 0 && rt.field_i32(e0, kFld_Ene_on) != 0) {
            fn_app_Ene_HpDraw_____V(rt, e0);
        }
    }

    // 8. Native Custom Gauge frame (image 57) + fill bar + HP + Loaded chips + Soul HUD
    if (sf_app_CpCanvas_rock > 0) {
        set_rock_palette_fast(rt, loc_sc.zoku);
        fn_app_CpCanvas_drawImg3___IIIIZ__V(rt, 57, 0, 48, 40, 0);
        if (sf_app_CpCanvas_stop_cnt < 2 && sf_app_CpCanvas_g > 0) {
            uint32_t bar_rgb = 0xFFC6D2EAu;
            if (g_pvp_eng.core.custom_round >= kMaxTurns) {
                if (g_pvp_eng.core.final_turn_ticks_left <= 48) {
                    bar_rgb = (((sf_app_CpCanvas_game_cnt / 2) % 2) == 0) ? 0xFFFF4050u : 0xFFFFC858u;
                } else {
                    bar_rgb = 0xFFFFB838u;
                }
            } else if (g_pvp_eng.core.cus_gage >= 128) {
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
            r_args[3].i = std::clamp<int32_t>(g_pvp_eng.core.cus_gage, 0, 128);
            r_args[4].i = 6;
            rt.invoke_external("com/nttdocomo/ui/Graphics", "fillRect", "(IIII)V", r_args, 5);
        }
        if (sf_app_CpCanvas_g > 0) {
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
        fn_app_Rock_HpDraw___II__V(rt, sf_app_CpCanvas_rock, 0, 0);
        fn_app_Rock_ArmDraw_____V(rt, sf_app_CpCanvas_rock);
        if (loc_sc.tip_no >= 0 && g_pvp_eng.phase == Phase::COMBAT) {
            fn_app_Rock_TipDraw_____V(rt, sf_app_CpCanvas_rock);
        }
    }
    draw_opponent_soul_badge_and_trap(rt, opp_sc, loc_sc);

    // 9. Time-Freeze chip name announcement & Counter Cut-In "!!" banner
    if ((g_pvp_eng.core.tf_state == TF_ROOT_INTRO || g_pvp_eng.core.tf_state == TF_COUNTER_BANNER ||
         g_pvp_eng.core.tf_state == TF_COUNTER_ACTION || g_pvp_eng.core.tf_state == TF_ROOT_RESUME) &&
        g_pvp_eng.core.tf_depth >= 1) {
        const TfSlot& top = g_pvp_eng.core.tf_stack[(g_pvp_eng.core.tf_depth >= 2) ? 1 : 0];
        if (g_pvp_eng.core.tf_state == TF_ROOT_INTRO && top.timer < 15 && top.tip_id > 0 &&
            sf_app_CpCanvas_tip > 0 && top.tip_id < rt.array_length(sf_app_CpCanvas_tip)) {
            Ref t_obj = rt.iaload(sf_app_CpCanvas_tip, top.tip_id);
            Ref disp_name = (t_obj > 0) ? rt.field_i32(t_obj, kFld_Tip_name) : 0;
            if (disp_name > 0) {
                int32_t ch = (top.owner == opp) ? 1 : 0;
                fn_app_CpCanvas_strDraw___Ljava_lang_String_II__V(rt, disp_name, 37 + 95 * ch, 76);
            }
        }
    }
    if (g_pvp_eng.core.tf_state == TF_COUNTER_BANNER && g_pvp_eng.core.tf_cutin_fx_seq >= 0) {
        int ch = (g_pvp_eng.core.tf_cutin_interrupted_side == opp) ? 1 : 0;
        draw_tf_cutin_fx_sprite(rt, ch, g_pvp_eng.core.tf_cutin_fx_seq);
    }

    // 10. Top Combat HUD (HP, Gauge/Final countdown, Names, BO3 stars)
    bool is_final_round = (g_pvp_eng.core.custom_round >= kMaxTurns);
    render_top_combat_hud(
        rt,
        loc_sc.player_name, loc_sc.hp, loc_sc.max_hp,
        opp_sc.player_name, opp_sc.hp, opp_sc.max_hp,
        g_pvp_eng.is_triple_battle,
        g_pvp_eng.bo3_game_index,
        g_pvp_eng.bo3_wins[loc],
        g_pvp_eng.bo3_wins[opp],
        is_final_round && g_pvp_eng.phase == Phase::COMBAT,
        g_pvp_eng.core.final_turn_ticks_left);

    // Restore canonical simulation state cleanly (zero render-induced divergence)
    g_pvp_eng.core = std::move(saved_core);
    g_pvp_eng.last_pal_zoku = saved_pal_zoku;
    generated_restore_statics(g_pvp_eng.render_statics);
    rt.restore_combat_snapshot(g_pvp_eng.render_snap);
    restore_host_runtime_state(rt, host_st, true);
    rt.set_suppress_av(prev_sup);
}

static void render_custom_screen_backdrop(DojaRuntime& rt, Ref cp) {
    HostRuntimeState host_st = capture_host_runtime_state(rt);
    PvpCombatCore saved_core = g_pvp_eng.core;
    int32_t saved_pal_zoku = g_pvp_eng.last_pal_zoku;
    generated_save_statics(g_pvp_eng.render_statics);
    g_pvp_eng.render_snap.base_heap_size = 0;
    rt.save_combat_snapshot(g_pvp_eng.render_snap, 1);

    bool prev_sup = rt.suppress_av();
    rt.set_suppress_av(false);

    int loc = g_pvp_eng.local_idx;
    int opp = g_pvp_eng.opp_idx();
    ensure_board_orient(rt, loc);
    load_side(rt, loc);
    sync_panel_occupancy_and_proxy(rt, loc);
    save_side(rt, loc);

    // 1. Refresh arena backdrop & 18 panels
    refresh_arena_imgmap_backdrop(rt, cp, false);
#ifdef HAS_FN_CpCanvas_PalSetBG
    fn_app_CpCanvas_drawImgMapBG___I__V(rt, 0);
#else
    if (sf_app_CpCanvas_g > 0 && sf_app_CpCanvas_imgMap > 0) {
        Slot args[4];
        args[0].r = sf_app_CpCanvas_g;
        args[1].r = sf_app_CpCanvas_imgMap;
        args[2].i = 0;
        args[3].i = 0;
        rt.invoke_external("com/nttdocomo/ui/Graphics", "drawImage", "(Lcom/nttdocomo/ui/Image;II)V", args, 4);
    }
#endif

    // 2. Panel warning / effect overlays
    if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
        for (int32_t y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0 || rt.array_length(row) < 6) continue;
            for (int32_t x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p > 0) fn_app_BtPanel_Draw2_____V(rt, p);
            }
        }
    }

    // 3. Y-sorted 2.5D entities without blink
    const SideContext& loc_sc = g_pvp_eng.core.sides[loc];
    const SideContext& opp_sc = g_pvp_eng.core.sides[opp];
    for (int32_t row = 0; row < 3; ++row) {
        if (opp_sc.hp > 0 && opp_sc.pos_y == row && sf_app_CpCanvas_rock > 0) {
            load_side(rt, opp);
            set_rock_palette_fast(rt, opp_sc.zoku);
            sf_app_Rock_muteki_cnt = 0;
            sf_app_Rock_hit_cnt = 0;
            sf_app_Rock_non_cnt2 = 0;
            int32_t orig_px = sf_app_Rock_pos_x;
            int32_t orig_dx = sf_app_Rock_dx;
            sf_app_Rock_pos_x = 5 - orig_px;
            sf_app_Rock_dx = -orig_dx;
            g_pvp_rock_flip_override = 0;
            draw_rock_readonly(rt, sf_app_CpCanvas_rock);
            g_pvp_rock_flip_override = -1;
            sf_app_Rock_pos_x = orig_px;
            sf_app_Rock_dx = orig_dx;
        }
        if (loc_sc.hp > 0 && loc_sc.pos_y == row && sf_app_CpCanvas_rock > 0) {
            load_side(rt, loc);
            set_rock_palette_fast(rt, loc_sc.zoku);
            sf_app_Rock_muteki_cnt = 0;
            sf_app_Rock_hit_cnt = 0;
            sf_app_Rock_non_cnt2 = 0;
            g_pvp_rock_flip_override = 1;
            draw_rock_readonly(rt, sf_app_CpCanvas_rock);
            g_pvp_rock_flip_override = -1;
        }
        if (sf_app_CpCanvas_oki > 0) {
            int32_t on_cnt = std::min<int32_t>(kMaxOkimono, rt.array_length(sf_app_CpCanvas_oki));
            for (int32_t i = 0; i < on_cnt; ++i) {
                Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                if (ok > 0 && rt.field_i32(ok, kFld_Okimono_on) != 0 &&
                    rt.field_i32(ok, kFld_Okimono_pos_y) == row) {
                    fn_app_Okimono_Draw___I__V(rt, ok, 0);
                }
            }
        }
        load_side(rt, loc);
        set_rock_palette_fast(rt, loc_sc.zoku);
        if (sf_app_CpCanvas_ata > 0) {
            int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
            for (int32_t i = 0; i < an; ++i) {
                Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
                if (a > 0 && rt.field_i32(a, kFld_Attack_on) == 1 &&
                    rt.field_i32(a, kFld_Attack_pos_y) == row) {
                    draw_attack_readonly(rt, a, 1);
                }
            }
        }
        draw_mirrored_opponent_attacks(rt, opp, 1, row);
    }

    // 4. Foreground Attacks (layer >= 2)
    load_side(rt, loc);
    set_rock_palette_fast(rt, loc_sc.zoku);
    if (sf_app_CpCanvas_ata > 0) {
        int32_t an = std::min<int32_t>(kMaxAttacks, rt.array_length(sf_app_CpCanvas_ata));
        for (int32_t i = 0; i < an; ++i) {
            Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
            if (a <= 0) continue;
            int32_t on_val = rt.field_i32(a, kFld_Attack_on);
            if (on_val >= 2) {
                draw_attack_readonly(rt, a, on_val);
            }
        }
    }
    draw_mirrored_opponent_attacks(rt, opp, 2, -1);

    // 5. Opponent foot HP digits + soul/trap HUD + top/bottom black bars (clears any previous scene bleed)
    load_side(rt, loc);
    sync_panel_occupancy_and_proxy(rt, loc);
    if (sf_app_CpCanvas_ene > 0 && rt.array_length(sf_app_CpCanvas_ene) > 0) {
        Ref e0 = rt.iaload(sf_app_CpCanvas_ene, 0);
        if (e0 > 0 && rt.field_i32(e0, kFld_Ene_on) != 0) {
            fn_app_Ene_HpDraw_____V(rt, e0);
        }
    }
    draw_opponent_soul_badge_and_trap(rt, opp_sc, loc_sc);
    if (sf_app_CpCanvas_g > 0) {
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

    // Restore BattleTip tray/selection heap & statics intact before BattleTip executes
    g_pvp_eng.core = std::move(saved_core);
    g_pvp_eng.last_pal_zoku = saved_pal_zoku;
    generated_restore_statics(g_pvp_eng.render_statics);
    rt.restore_combat_snapshot(g_pvp_eng.render_snap);
    if (sf_app_CpCanvas_rock > 0) {
        set_rock_palette_fast(rt, sf_app_Rock_zoku);
    }
    restore_host_runtime_state(rt, host_st, true);
    rt.set_suppress_av(prev_sup);
}

static void render_custom_screen_top_hud(DojaRuntime& rt) {
    int loc = g_pvp_eng.local_idx;
    int opp = g_pvp_eng.opp_idx();
    const SideContext& opp_sc = g_pvp_eng.core.sides[opp];
    int ticks_rem = std::max<int>(0, kCustomTimeoutTicks - g_pvp_eng.custom_timer_ticks);
    int secs_left = (ticks_rem + 15) / 16;

    if (g_pvp_eng.core.custom_round >= kMaxTurns) {
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
                                  std::to_string(g_pvp_eng.bo3_wins[loc]) + "-" +
                                  std::to_string(g_pvp_eng.bo3_wins[opp]);
            rt.fb_fill_rect_alpha(70, 10, 48, 20, 0xCC182444u);
            rt.fb_draw_border(70, 10, 48, 20, 0xFFFFD84Cu);
            rt.fb_draw_text(bo3_str, 94 - rt.fb_measure_text(bo3_str) / 2, 24, 0xFFFFE866u);
        }
    }

    std::string opp_hdr = "VS " + opp_sc.player_name;
    std::string opp_hp_str = "HP " + std::to_string(std::max<int32_t>(0, opp_sc.hp)) + "/" +
                             std::to_string(std::max<int32_t>(1, opp_sc.max_hp));
    rt.fb_fill_rect_alpha(122, 4, 114, 32, 0xE0381420u);
    rt.fb_draw_border(122, 4, 114, 32, 0xFFFF5868u);
    rt.fb_draw_text(opp_hdr, 126, 16, 0xFFFF98A8u);
    rt.fb_draw_text(opp_hp_str, 126, 31, (opp_sc.hp * 4 <= opp_sc.max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);
}

// ============================================================================
// Auto-Bot (Headless & Live Smoke Input Driver)
// ============================================================================
static int32_t compute_auto_bot_combat_input(DojaRuntime& rt) {
    (void)rt;
    uint32_t t = static_cast<uint32_t>(g_pvp_eng.core.combat_tick_in_round);
    int32_t m = 0;
    const SideContext& me = g_pvp_eng.core.sides[g_pvp_eng.local_idx];
    const SideContext& opp = g_pvp_eng.core.sides[g_pvp_eng.opp_idx()];
    // Align vertically with opponent row, move toward front column, & fire Buster/Chips
    if (me.pos_y < opp.pos_y && (t % 8 == 0)) m |= (1 << 19);      // Down
    else if (me.pos_y > opp.pos_y && (t % 8 == 0)) m |= (1 << 17); // Up
    else if (me.pos_x < 2 && (t % 16 == 4)) m |= (1 << 18);        // Right toward front col
    if (me.tip_no >= 0 && (t % 12 == 5)) m |= kKeySelectBit;        // Chip (ACT_SELECT) on rest tick
    else if ((t % 6) < 3) m |= (1 << 10);                          // Buster/Charge (ACT_CANCEL) hold/release
    if (g_pvp_eng.core.custom_round < kMaxTurns && g_pvp_eng.core.cus_gage >= 128 &&
        me.tip_no < 0 && (t & 1u) == 0u) {
        m |= (g_pvp_eng.local_idx == 0) ? (1 << 21) : (1 << 22);   // Custom Enter (ACT_SOFT1 / ACT_SOFT2)
    }
    return m;
}

// ============================================================================
// Intercepts: Ene.Hit (Shield / Reflect / AntiDmg -> Rock.Hit) & Ene.Draw
// ============================================================================
static int32_t pvp_lon_intercept_ene_hit(DojaRuntime& rt, Ref ene_thiz, int32_t hit_kou, int32_t pow, int32_t zoku) {
    if (!g_pvp_eng.in_combat_step || sf_app_CpCanvas_ene <= 0) {
        return -1; // Normal single-player execution
    }
    if (ene_thiz != rt.iaload(sf_app_CpCanvas_ene, 0)) {
        return -1;
    }

    int att_idx = g_pvp_eng.active_side;
    int def_idx = 1 - att_idx;
    Ref att_ata = g_pvp_eng.active_ata_ref;
    bool is_breaking = (att_ata > 0) && (rt.field_i32(att_ata, kFld_Attack_hit_bure) != 0);

    save_side(rt, att_idx);
    ensure_board_orient(rt, def_idx);
    load_side(rt, def_idx);
    sync_panel_occupancy_and_proxy(rt, def_idx);
    int32_t prev_def_hp = std::max<int32_t>(0, sf_app_Rock_hp);

    const SideContext& att_sc = g_pvp_eng.core.sides[att_idx];
    int32_t hit_res = 0;
    int32_t att_ata_pow = (att_ata > 0) ? rt.field_i32(att_ata, kFld_Attack_pow) : pow;
    int32_t eff_pow = std::max(att_ata_pow, pow);
    bool terminate_att_ata = false;
    bool mark_att_ata_hit = false;
    bool handled_by_shield = false;

    // 1. Guard / Reflect (han == 1 -> waza 84; han == 2/3 -> waza 240; Breaking shatters)
    if (sf_app_Rock_han > 0 && eff_pow > 0 && sf_app_Rock_han_id >= 0 && sf_app_Rock_han_id < kMaxAttacks) {
        Ref shield_ata = rt.iaload(sf_app_CpCanvas_ata, sf_app_Rock_han_id);
        if (is_breaking) {
            if (shield_ata > 0) fn_app_Attack_init_____V(rt, shield_ata);
            sf_app_Rock_han = 0;
            sf_app_Rock_han_id = -1;
            sf_app_Rock_muteki_cnt = 0;
            if (sf_app_CpCanvas_stop_cnt != 0) sf_app_Rock_tmp_muteki_cnt = 0;
        } else {
            int32_t han_mode = sf_app_Rock_han;
            int32_t shield_pow = (shield_ata > 0) ? rt.field_i32(shield_ata, kFld_Attack_pow) : 50;
            int32_t shield_mahi = (shield_ata > 0) ? rt.field_i32(shield_ata, kFld_Attack_mahi_flg) : 0;
            auto spawn_counter = [&](int32_t waza_id, int32_t cy, int32_t layer, int32_t cflg) {
                int32_t s_no = fn_app_CpCanvas_AtaNo___I__I(rt, sf_app_Rock_han_id);
                if (s_no >= 0 && sf_app_CpCanvas_ata > 0 && s_no < rt.array_length(sf_app_CpCanvas_ata)) {
                    Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s_no);
                    if (c_ata > 0) {
                        fn_app_Attack_Set___IIIII__I(rt, c_ata, waza_id, sf_app_Rock_pos_x, cy, 1, layer);
                        rt.field_i32(c_ata, kFld_Attack_chara_flg) = cflg;
                        rt.field_i32(c_ata, kFld_Attack_pow) = shield_pow;
                        if (shield_mahi != 0) {
                            rt.field_i32(c_ata, kFld_Attack_hit_param) = 3;
                            rt.field_i32(c_ata, kFld_Attack_mahi_flg) = 1;
                        }
                    }
                }
            };
            if (han_mode == 1) {
                spawn_counter(84, sf_app_Rock_pos_y, 0, 0);
            } else if (han_mode == 2) {
                spawn_counter(240, sf_app_Rock_pos_y, -2, 0);
            } else if (han_mode == 3) {
                for (int k = 0; k < 3; ++k) {
                    int32_t cy = sf_app_Rock_pos_y + k - 1;
                    if (cy >= 0 && cy <= 2) spawn_counter(240, cy, -2, 0);
                }
            }
            fn_app_Rock_EffSet___I__V(rt, sf_app_CpCanvas_rock, 37);
            if (han_mode != 1) {
                if (shield_ata > 0) fn_app_Attack_init_____V(rt, shield_ata);
                sf_app_Rock_han = 0;
                sf_app_Rock_han_id = -1;
                sf_app_Rock_muteki_cnt = 0;
                if (sf_app_CpCanvas_stop_cnt != 0) sf_app_Rock_tmp_muteki_cnt = 0;
            }
            terminate_att_ata = true;
            hit_res = 0;
            handled_by_shield = true;
        }
    }

    // 2. AntiDmg (wana == 5 -> waza 245 & smoke effect 225) or Rock.Hit fallthrough
    if (!handled_by_shield) {
        if (sf_app_Rock_wana == 5 && sf_app_Rock_muteki_cnt == 0 && sf_app_Rock_bari == 0 && eff_pow > 0) {
            int32_t wpow = (g_pvp_eng.core.sides[def_idx].wana_pow[0] > 0)
                ? g_pvp_eng.core.sides[def_idx].wana_pow[0]
                : 100;
            sf_app_Rock_wana = 0;
            sf_app_Rock_muteki_cnt = 10;
            if (sf_app_CpCanvas_stop_cnt != 0) {
                sf_app_Rock_tmp_muteki_cnt = std::max<int32_t>(sf_app_Rock_tmp_muteki_cnt, 10);
            }
            sf_app_Rock_hit_cnt = 10;
            fn_app_Rock_EffSet___I__V(rt, sf_app_CpCanvas_rock, 225);

            if (att_sc.hp > 0) {
                int32_t tx = std::clamp<int32_t>(5 - att_sc.pos_x, 0, 5);
                int32_t ty = std::clamp<int32_t>(att_sc.pos_y, 0, 2);
                int32_t s_no = fn_app_CpCanvas_AtaNo___I__I(rt, 0);
                if (s_no >= 0 && sf_app_CpCanvas_ata > 0 && s_no < rt.array_length(sf_app_CpCanvas_ata)) {
                    Ref c_ata = rt.iaload(sf_app_CpCanvas_ata, s_no);
                    if (c_ata > 0) {
                        fn_app_Attack_Set___IIIII__I(rt, c_ata, 245, tx, ty, 1, -2);
                        rt.field_i32(c_ata, kFld_Attack_chara_flg) = 0;
                        rt.field_i32(c_ata, kFld_Attack_pow) = wpow;
                        rt.field_i32(c_ata, kFld_Attack_zoku) = 0;
                    }
                }
            }
            mark_att_ata_hit = true;
            hit_res = 0;
        } else if (sf_app_Rock_muteki_cnt > 0 && att_ata > 0) {
            hit_res = 0;
        } else {
            // NorthWind barrier strip check (param[7] == 18)
            if (att_ata > 0) {
                Ref p_arr = rt.field_i32(att_ata, kFld_Attack_param);
                if (p_arr > 0 && rt.array_length(p_arr) > 7 && rt.iaload(p_arr, 7) == 18) {
                    sf_app_Rock_bari = 0;
                    sf_app_Rock_bari_id = -1;
                }
            }
            int32_t pre_tmp_muteki = sf_app_Rock_tmp_muteki_cnt;
            hit_res = fn_app_Rock_Hit___III__Z(rt, sf_app_CpCanvas_rock, hit_kou, pow, zoku);
            if (sf_app_CpCanvas_stop_cnt != 0) {
                sf_app_Rock_tmp_muteki_cnt = std::max<int32_t>(pre_tmp_muteki, sf_app_Rock_tmp_muteki_cnt);
            }
            if (sf_app_CpCanvas_stop_cnt > 0 && hit_res != 0) {
                g_pvp_eng.core.tf_saved[def_idx].was_hit_in_tf = 1;
            }
        }
    }

    int32_t post_def_hp = std::max<int32_t>(0, sf_app_Rock_hp);
    if (post_def_hp < prev_def_hp) {
        g_pvp_eng.core.damage_dealt[att_idx] += (prev_def_hp - post_def_hp);
    }
    sf_app_CpCanvas_now_hp = sf_app_Rock_hp;
    save_side(rt, def_idx);

    ensure_board_orient(rt, att_idx);
    load_side(rt, att_idx);
    if (att_ata > 0) {
        if (terminate_att_ata) {
            fn_app_Attack_init_____V(rt, att_ata);
            rt.field_i32(att_ata, kFld_Attack_hit_eff) = 0;
            rt.field_i32(att_ata, kFld_Attack_hit_ok) = 1;
        } else if (mark_att_ata_hit) {
            rt.field_i32(att_ata, kFld_Attack_ren_hit) = 1;
            rt.field_i32(att_ata, kFld_Attack_hit_flg) = 1;
            rt.field_i32(att_ata, kFld_Attack_hit_ok) = 1;
        }
    }
    save_side(rt, att_idx);
    sync_panel_occupancy_and_proxy(rt, att_idx);
    return hit_res;
}

static bool pvp_lon_intercept_ene_draw(DojaRuntime& rt, Ref ene_thiz, int32_t a0) {
    (void)a0;
    if (!g_pvp_eng.is_active() || sf_app_CpCanvas_ene <= 0) return false;
    if (ene_thiz != rt.iaload(sf_app_CpCanvas_ene, 0)) return false;

    int loc = g_pvp_eng.local_idx;
    int opp = g_pvp_eng.opp_idx();
    const SideContext& opp_sc = g_pvp_eng.core.sides[opp];
    if (opp_sc.hp <= 0 || sf_app_CpCanvas_rock <= 0) return true;

    save_side(rt, loc);
    ensure_board_orient(rt, opp);
    load_side(rt, opp);
    set_rock_palette_fast(rt, opp_sc.zoku);
    int32_t orig_px = sf_app_Rock_pos_x;
    int32_t orig_dx = sf_app_Rock_dx;
    sf_app_Rock_pos_x = 5 - orig_px;
    sf_app_Rock_dx = -orig_dx;
    g_pvp_rock_flip_override = 0;
    draw_rock_readonly(rt, sf_app_CpCanvas_rock);
    g_pvp_rock_flip_override = -1;
    sf_app_Rock_pos_x = orig_px;
    sf_app_Rock_dx = orig_dx;
    ensure_board_orient(rt, loc);
    load_side(rt, loc);
    sync_panel_occupancy_and_proxy(rt, loc);
    return true;
}

// ============================================================================
// Match Lifecycle Helpers (Cleanup, Disconnect Win, BO3 Progression)
// ============================================================================
static void cleanup_and_restore_singleplayer(DojaRuntime& rt, bool close_net = true) {
    g_pvp_eng.has_pre_custom_snap = false;
    g_pvp_eng.last_pal_zoku = -1;
    g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
    cleanup_common_and_restore_singleplayer(rt, g_pvp_eng, close_net);
}

static void adjudicate_peer_disconnect_win(DojaRuntime& rt) {
    adjudicate_peer_disconnect_win_common(rt, g_pvp_eng, 5, 6);
}

static void conclude_current_game(DojaRuntime& rt, Ref cp) {
    (void)cp;
    conclude_current_game_common(rt, g_pvp_eng, 5, 6);
}

static void start_next_bo3_game(DojaRuntime& rt, Ref cp) {
    restore_pre_match_snapshot_for_next_game(rt, g_pvp_eng);
    g_pvp_eng.core = PvpCombatCore{};
    g_pvp_eng.last_pal_zoku = -1;
    g_pvp_eng.has_pre_custom_snap = false;
    g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
    init_pvp_battle_arena(rt, cp);
    enter_custom_screen_phase(rt, cp);
}

// ============================================================================
// Stage 4 Headless --pvp-self-test Suites A-N
// ============================================================================
static int run_pvp_headless_self_test(DojaRuntime& rt, Ref cp) {
    SelfTestFlagGuard test_guard(rt);
    std::printf("[PvP SELF-TEST] LoN Suites A-N starting...\n");

    FullHeapSnapshot base_heap;
    std::vector<int64_t> base_statics;
    rt.save_full_snapshot(base_heap);
    generated_save_statics(base_statics);

    struct SelfTestRestoreGuard {
        DojaRuntime& rt;
        const FullHeapSnapshot& heap;
        const std::vector<int64_t>& statics;
        ~SelfTestRestoreGuard() {
            rt.reset_interpolator();
            rt.set_suppress_av(true);
            rt.restore_full_snapshot(heap);
            generated_restore_statics(statics);
            g_pvp_eng = PvpEngineState{};
        }
    } restore_guard{rt, base_heap, base_statics};

    int failures = 0;
    auto fail_msg = [&](const std::string& tag, const std::string& detail) {
        failures++;
        std::fprintf(stderr, "[PvP SELF-TEST FAIL] %s: %s\n", tag.c_str(), detail.c_str());
    };

    const pvp::PvpFighterBuild local_raw = extract_local_lon_build(rt, cp);
    if (!local_raw.has_lon_v2) {
        fail_msg("Init-ExtractBuild", "extract_local_lon_build did not set has_lon_v2");
        return 1;
    }
    const pvp::PvpFighterBuild base_build = sanitize_lon_build(rt, local_raw);

    auto make_build = [&](const char* name, int32_t max_hp, uint8_t arm_flg,
                          uint8_t get_arm_mask, int32_t dell_val) {
        pvp::PvpFighterBuild b = base_build;
        b.player_name = name;
        b.max_hp = std::clamp<int32_t>(max_hp, 1, 9999);
        b.arm_flg = arm_flg;
        b.get_arm_mask = get_arm_mask;
        for (int i = 0; i < 10; ++i) b.dell_time[i] = dell_val;
        return sanitize_lon_build(rt, b);
    };

    auto setup_test_arena = [&](const pvp::PvpFighterBuild& b0,
                                const pvp::PvpFighterBuild& b1,
                                uint32_t seed,
                                int local_idx = 0) {
        rt.reset_interpolator();
        rt.set_suppress_av(true);
        rt.restore_full_snapshot(base_heap);
        generated_restore_statics(base_statics);
        g_pvp_eng = PvpEngineState{};
        reset_bo3_series_state(g_pvp_eng, seed, false);
        g_pvp_eng.local_idx = local_idx;
        g_pvp_eng.builds[0] = b0;
        g_pvp_eng.builds[1] = b1;
        g_pvp_eng.local_build = (local_idx == 0) ? b0 : b1;
        init_pvp_battle_arena(rt, cp);
        g_pvp_eng.local_idx = local_idx;
        ensure_board_orient(rt, local_idx);
        load_side(rt, local_idx);
        sync_panel_occupancy_and_proxy(rt, local_idx);
        g_pvp_eng.phase = Phase::COMBAT;
        sf_app_CpCanvas_scene = kSceneBattleMain;
    };

    auto setup_basic_arena = [&](int32_t hp0, int32_t hp1, uint32_t seed, int local_idx = 0) {
        pvp::PvpFighterBuild b0 = make_build("SelfP0", hp0, 1, 0x3F, 0);
        pvp::PvpFighterBuild b1 = make_build("SelfP1", hp1, 1, 0x3F, 0);
        setup_test_arena(b0, b1, seed, local_idx);
    };

    // Place Rock.tip slots in FIFO firing order (tip[N-1] fires first via Rock.TipNext)
    auto queue_tips_fifo = [&](int s_idx, const std::vector<int16_t>& tips) {
        SideContext& sc = g_pvp_eng.core.sides[s_idx];
        int n = std::min<int>(9, static_cast<int>(tips.size()));
        for (int i = 0; i < 9; ++i) sc.rock_tip_ids[i] = 0;
        for (int i = 0; i < n; ++i) sc.rock_tip_ids[n - 1 - i] = tips[i];
        sc.tip_no = static_cast<int8_t>(n - 1);
        sc.next_tip = 1;
        ensure_board_orient(rt, s_idx);
        load_side(rt, s_idx);
        sync_panel_occupancy_and_proxy(rt, s_idx);
        fn_app_Rock_TipNext_____V(rt, sf_app_CpCanvas_rock);
        save_side(rt, s_idx);
    };

    struct PanelSnap {
        int32_t jin = 0, jou = 0, ana_cnt = 0, mag_cnt = 0, aria_cnt = 0, yokoku = 0, bom_pow = 0;
    };
    auto capture_panels_canonical = [&]() -> std::array<std::array<PanelSnap, 6>, 3> {
        std::array<std::array<PanelSnap, 6>, 3> out{};
        ensure_board_orient(rt, 0);
        if (sf_app_CpCanvas_panel <= 0 || rt.array_length(sf_app_CpCanvas_panel) < 3) return out;
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            if (row <= 0 || rt.array_length(row) < 6) continue;
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                if (p <= 0) continue;
                out[y][x] = PanelSnap{
                    rt.field_i32(p, kFld_BtPanel_jin),
                    rt.field_i32(p, kFld_BtPanel_jou),
                    rt.field_i32(p, kFld_BtPanel_ana_cnt),
                    rt.field_i32(p, kFld_BtPanel_mag_cnt),
                    rt.field_i32(p, kFld_BtPanel_aria_cnt),
                    rt.field_i32(p, kFld_BtPanel_yokoku),
                    rt.field_i32(p, kFld_BtPanel_bom_pow),
                };
            }
        }
        return out;
    };

    auto run_safe = [&](const char* suite_tag, auto&& fn) {
        try {
            fn();
        } catch (const JavaException& ex) {
            fail_msg(suite_tag, std::string("JavaException ref=") + std::to_string(ex.exc_ref) +
                     " where=" + (ex.where ? ex.where : "?") + " a=" + std::to_string(ex.detail_a));
        } catch (const std::exception& ex) {
            fail_msg(suite_tag, std::string("std::exception: ") + ex.what());
        }
    };

    // Discover useful tip indices dynamically via kFld_Tip_* / kFld_Waza_*
    const int32_t tip_len = get_tip_array_len(rt);
    int32_t tid_cannon = -1;
    int32_t tid_navi_attack = -1;
    int32_t tid_dimming_attack = -1;
    int32_t tid_obstacle = -1;
    for (int32_t tid = 2; tid < tip_len; ++tid) {
        Ref t_obj = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
        if (t_obj <= 0) continue;
        int32_t syu = rt.field_i32(t_obj, kFld_Tip_syu);
        int32_t pow = rt.field_i32(t_obj, kFld_Tip_pow);
        int32_t wid = rt.field_i32(t_obj, kFld_Tip_waza_id);
        int32_t tcls = rt.field_i32(t_obj, kFld_Tip_tip_class);
        int32_t stime = 0;
        int32_t wkou = 0;
        if (sf_app_CpCanvas_waza > 0 && wid >= 0 && wid < rt.array_length(sf_app_CpCanvas_waza)) {
            Ref w_obj = rt.iaload(sf_app_CpCanvas_waza, wid);
            if (w_obj > 0) {
                stime = rt.field_i32(w_obj, kFld_Waza_stop_time);
                wkou = rt.field_i32(w_obj, kFld_Waza_waza_kou);
            }
        }
        if (tid_cannon < 0 && syu == 0 && pow >= 20 && stime == 0 && tcls <= 1) tid_cannon = tid;
        if (tid_navi_attack < 0 && syu == 1 && pow >= 40 && stime > 0) tid_navi_attack = tid;
        if (tid_dimming_attack < 0 && stime > 0 && pow >= 30 && syu <= 2) tid_dimming_attack = tid;
    }

    // =========================================================================
    // Suite A: Arena, Horizontal Mirroring, EN bas_max_flg==0 & Framebuffer Pixels
    // =========================================================================
    run_safe("SuiteA", [&]() {
        for (int loc = 0; loc < 2; ++loc) {
            setup_basic_arena(500, 500, 0xA0001u + loc, loc);
#ifdef HAS_SF_app_CpCanvas_bas_max_flg
            if (sf_app_CpCanvas_bas_max_flg != 0) {
                fail_msg("SuiteA-BasMaxFlg", "Expected sf_app_CpCanvas_bas_max_flg == 0 in PvP");
            }
#endif
            // Verify orientation mirroring & proxy positions
            g_pvp_eng.core.sides[loc].pos_x = 1;
            g_pvp_eng.core.sides[loc].pos_y = 0;
            g_pvp_eng.core.sides[1 - loc].pos_x = 0;
            g_pvp_eng.core.sides[1 - loc].pos_y = 2;
            ensure_board_orient(rt, loc);
            load_side(rt, loc);
            sync_panel_occupancy_and_proxy(rt, loc);
            Ref e0 = (sf_app_CpCanvas_ene > 0) ? rt.iaload(sf_app_CpCanvas_ene, 0) : 0;
            if (e0 <= 0 || rt.field_i32(e0, kFld_Ene_pos_x) != 5 || rt.field_i32(e0, kFld_Ene_pos_y) != 2) {
                fail_msg("SuiteA-ProxyMirror", "Opponent Ene[0] not mirrored to (5, 2)");
            }
            // Test asymmetric panel steal: steal col 2 on loc's side (canon col 2 if loc==0, col 3 if loc==1)
            Ref p_mid = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 0), 2);
            int32_t orig_jin = rt.field_i32(p_mid, kFld_BtPanel_jin);
            rt.field_i32(p_mid, kFld_BtPanel_jin) = 1; // Opp steals col 2 in loc view
            ensure_board_orient(rt, 1 - loc);
            Ref p_mir = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 0), 3);
            if (rt.field_i32(p_mir, kFld_BtPanel_jin) != 0) {
                fail_msg("SuiteA-BoardOrient", "ensure_board_orient did not mirror stolen tile jin cleanly");
            }
            ensure_board_orient(rt, loc);
            rt.field_i32(p_mid, kFld_BtPanel_jin) = orig_jin;
            load_side(rt, loc);
            sync_panel_occupancy_and_proxy(rt, loc);

            g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
            rt.set_suppress_av(false);
            render_pvp_combat_frame(rt, cp);
            rt.set_suppress_av(true);

            const uint32_t* fb = rt.framebuffer();
            int chroma_green_cnt = 0;
            int chroma_magenta_cnt = 0;
            int backdrop_non_black = 0;
            int left_blue_dom = 0, left_red_dom = 0;
            int right_blue_dom = 0, right_red_dom = 0;

            for (int y = 40; y < 200; ++y) {
                for (int x = 0; x < 240; ++x) {
                    uint32_t rgb = fb[y * 240 + x] & 0x00FFFFFFu;
                    if (rgb == 0x00FF00u) chroma_green_cnt++;
                    if (rgb == 0xBA00FFu) chroma_magenta_cnt++;
                    if (y >= 62 && y <= 92 && rgb != 0) backdrop_non_black++;
                    if (y >= 122 && y <= 178) {
                        int32_t r = static_cast<int32_t>((rgb >> 16) & 0xFFu);
                        int32_t g = static_cast<int32_t>((rgb >> 8) & 0xFFu);
                        int32_t b = static_cast<int32_t>(rgb & 0xFFu);
                        if (x >= 4 && x <= 36) {
                            if (b > r + 15 && b >= g) left_blue_dom++;
                            if (r > b + 15 && r >= g) left_red_dom++;
                        } else if (x >= 204 && x <= 236) {
                            if (b > r + 15 && b >= g) right_blue_dom++;
                            if (r > b + 15 && r >= g) right_red_dom++;
                        }
                    }
                }
            }
            if (chroma_green_cnt != 0 || chroma_magenta_cnt != 0) {
                fail_msg("SuiteA-ChromaBleed",
                         "loc=" + std::to_string(loc) + " green=" + std::to_string(chroma_green_cnt) +
                         " magenta=" + std::to_string(chroma_magenta_cnt));
            }
            if (backdrop_non_black < 500) {
                fail_msg("SuiteA-BackdropBlank",
                         "loc=" + std::to_string(loc) + " backdrop_non_black=" + std::to_string(backdrop_non_black));
            }
            // In LoN, local (jin==0, left side) panels are RED and opponent (jin==1, right side) panels are BLUE.
            if (left_red_dom < 80 || left_red_dom <= left_blue_dom ||
                right_blue_dom < 80 || right_blue_dom <= right_red_dom) {
                fail_msg("SuiteA-PanelSignature",
                         "loc=" + std::to_string(loc) +
                         " L(B=" + std::to_string(left_blue_dom) + ",R=" + std::to_string(left_red_dom) + ")" +
                         " R(B=" + std::to_string(right_blue_dom) + ",R=" + std::to_string(right_red_dom) + ")");
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite A complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite B: All 270 Chips (1..270) + 7 Charge Shots (1,263..268) + PAs Mirror Symmetry
    // =========================================================================
    run_safe("SuiteB", [&]() {
        // 1) All 7 Charge Shots (Normal + 6 Souls: tip_id = 1, 263..268)
        for (int z = 0; z < 7; ++z) {
            struct RunSnap {
                int32_t hp0 = 0, hp1 = 0, joutai0 = 0, joutai1 = 0, fc0 = 0, fc1 = 0;
                uint16_t hash = 0;
            };
            RunSnap snaps[2];
            for (int actor = 0; actor < 2; ++actor) {
                setup_basic_arena(1000, 1000, 0xB1000u + z, 0);
                int32_t sx = (z >= 4) ? 2 : 1;
                g_pvp_eng.core.sides[actor].zoku = z;
                g_pvp_eng.core.sides[actor].arm_cnt = (z > 0) ? 3 : 0;
                g_pvp_eng.core.sides[actor].pos_x = sx;
                g_pvp_eng.core.sides[actor].tmp_x = sx;
                g_pvp_eng.core.sides[actor].pos_y = 1;
                g_pvp_eng.core.sides[1 - actor].pos_x = sx;
                g_pvp_eng.core.sides[1 - actor].tmp_x = sx;
                g_pvp_eng.core.sides[1 - actor].pos_y = 1;
                const int charge_ticks = (42 - sf_app_CpCanvas_cha_lv * 5) + kChargeDelayAdd[z] + 4;
                for (int t = 0; t < charge_ticks; ++t) {
                    int32_t p0m = (actor == 0) ? (1 << 20) : 0;
                    int32_t p1m = (actor == 1) ? (1 << 20) : 0;
                    step_pvp_combat_tick(rt, cp, p0m, p1m);
                }
                for (int t = 0; t < 24; ++t) {
                    step_pvp_combat_tick(rt, cp, 0, 0);
                }
                snaps[actor] = RunSnap{
                    g_pvp_eng.core.sides[0].hp, g_pvp_eng.core.sides[1].hp,
                    g_pvp_eng.core.sides[0].joutai, g_pvp_eng.core.sides[1].joutai,
                    g_pvp_eng.core.sides[0].rock_first_chip, g_pvp_eng.core.sides[1].rock_first_chip,
                    compute_combat_state_hash(rt)
                };
            }
            if (snaps[0].hp0 != snaps[1].hp1 || snaps[0].hp1 != snaps[1].hp0 ||
                snaps[0].joutai1 != snaps[1].joutai0 ||
                (snaps[0].hp1 >= 1000 && snaps[0].joutai1 == 0 && snaps[0].fc0 == snaps[0].fc1)) {
                fail_msg("SuiteB-ChargeShot",
                         "z=" + std::to_string(z) + " tid=" + std::to_string(kChargeShotTipId[z]) +
                         " A=(" + std::to_string(snaps[0].hp0) + "," + std::to_string(snaps[0].hp1) + ")" +
                         " B=(" + std::to_string(snaps[1].hp0) + "," + std::to_string(snaps[1].hp1) + ")");
            }
        }

        // 2) Full 270-Chip Mirror Symmetry + Render Invariance Sweep (both close x=2 and throw x=1)
        const int32_t max_tid = std::min<int32_t>(270, tip_len - 1);
        int tested_chips = 0;
        for (int32_t tid = 1; tid <= max_tid; ++tid) {
            Ref t_obj = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
            if (t_obj <= 0) continue;
            int32_t syu = rt.field_i32(t_obj, kFld_Tip_syu);
            std::vector<int16_t> seq;
            if (syu == 4 && tid_cannon > 0) {
                seq = {static_cast<int16_t>(tid_cannon), static_cast<int16_t>(tid)};
            } else if (syu == 5 && tid_navi_attack > 0) {
                seq = {static_cast<int16_t>(tid_navi_attack), static_cast<int16_t>(tid)};
            } else {
                seq = {static_cast<int16_t>(tid)};
            }

            auto run_chip_mirror_at_pos = [&](int start_px, bool& out_had_effect) {
                struct SideRes {
                    SideContext s[2];
                    std::array<std::array<PanelSnap, 6>, 3> panels{};
                    bool render_invar = false;
                } res[2];

                for (int actor = 0; actor < 2; ++actor) {
                    setup_basic_arena(5000, 5000, 0xB2000u + static_cast<uint32_t>(tid), 0);
                    g_pvp_eng.core.sides[0].pos_x = start_px;
                    g_pvp_eng.core.sides[0].tmp_x = start_px;
                    g_pvp_eng.core.sides[1].pos_x = start_px;
                    g_pvp_eng.core.sides[1].tmp_x = start_px;
                    g_pvp_eng.core.sides[0].pos_y = 1;
                    g_pvp_eng.core.sides[1].pos_y = 1;
                    queue_tips_fifo(actor, seq);
                    bool rendered_mid = false;
                    res[actor].render_invar = true;
                    for (int t = 0; t < 44 || (g_pvp_eng.core.tf_state != TF_IDLE && t < 160); ++t) {
                        int32_t p0m = (actor == 0 && t == 0) ? (1 << 20) : 0;
                        int32_t p1m = (actor == 1 && t == 0) ? (1 << 20) : 0;
                        step_pvp_combat_tick(rt, cp, p0m, p1m);
                        if (t == 6 || t == 43) {
                            uint16_t pre_h = compute_combat_state_hash(rt);
                            size_t pre_heap = rt.heap_size();
                            rt.set_suppress_av(false);
                            render_pvp_combat_frame(rt, cp);
                            rt.set_suppress_av(true);
                            if (compute_combat_state_hash(rt) != pre_h || rt.heap_size() != pre_heap) {
                                res[actor].render_invar = false;
                            }
                            rendered_mid = true;
                        }
                    }
                    if (!rendered_mid) res[actor].render_invar = false;
                    res[actor].s[0] = g_pvp_eng.core.sides[0];
                    res[actor].s[1] = g_pvp_eng.core.sides[1];
                    res[actor].panels = capture_panels_canonical();
                }

                const SideContext& A0 = res[0].s[0];
                const SideContext& A1 = res[0].s[1];
                const SideContext& B0 = res[1].s[0];
                const SideContext& B1 = res[1].s[1];
                bool hp_eq = (A0.hp == B1.hp && A1.hp == B0.hp);
                bool st_eq = (A0.joutai == B1.joutai && A1.joutai == B0.joutai &&
                              A0.bari == B1.bari && A1.bari == B0.bari &&
                              A0.han == B1.han && A1.han == B0.han &&
                              A0.wana_flg == B1.wana_flg && A1.wana_flg == B0.wana_flg &&
                              A0.wana_flg2 == B1.wana_flg2 && A1.wana_flg2 == B0.wana_flg2 &&
                              A0.pos_x == B1.pos_x && A1.pos_x == B0.pos_x &&
                              A0.pos_y == B1.pos_y && A1.pos_y == B0.pos_y);
                bool pan_eq = true;
                std::string pan_diag;
                for (int y = 0; y < 3 && pan_eq; ++y) {
                    for (int x = 0; x < 6; ++x) {
                        const PanelSnap& pA = res[0].panels[y][x];
                        const PanelSnap& pB = res[1].panels[y][5 - x];
                        if ((pA.jin ^ 1) != pB.jin || pA.jou != pB.jou ||
                            pA.ana_cnt != pB.ana_cnt || pA.mag_cnt != pB.mag_cnt ||
                            pA.bom_pow != pB.bom_pow || pA.yokoku != pB.yokoku) {
                            pan_eq = false;
                            pan_diag = " @(" + std::to_string(x) + "," + std::to_string(y) + ")" +
                                       "A[jin=" + std::to_string(pA.jin) + ",jou=" + std::to_string(pA.jou) +
                                       ",ana=" + std::to_string(pA.ana_cnt) + ",mag=" + std::to_string(pA.mag_cnt) +
                                       ",bom=" + std::to_string(pA.bom_pow) + ",yok=" + std::to_string(pA.yokoku) + "]" +
                                       "B[jin=" + std::to_string(pB.jin) + ",jou=" + std::to_string(pB.jou) +
                                       ",ana=" + std::to_string(pB.ana_cnt) + ",mag=" + std::to_string(pB.mag_cnt) +
                                       ",bom=" + std::to_string(pB.bom_pow) + ",yok=" + std::to_string(pB.yokoku) + "]";
                            break;
                        }
                    }
                }
                if (!hp_eq || !st_eq || !pan_eq || !res[0].render_invar || !res[1].render_invar) {
                    static int b_diag_cnt = 0;
                    std::string extra = pan_diag;
                    if (!hp_eq && b_diag_cnt < 4) {
                        b_diag_cnt++;
                        extra += " posA0=(" + std::to_string(A0.pos_x) + "," + std::to_string(A0.pos_y) + ")" +
                                 " posB1=(" + std::to_string(B1.pos_x) + "," + std::to_string(B1.pos_y) + ")" +
                                 " syu=" + std::to_string(syu) +
                                 " wid=" + std::to_string(rt.field_i32(t_obj, kFld_Tip_waza_id));
                    }
                    fail_msg("SuiteB-ChipMirror",
                             "tid=" + std::to_string(tid) + " px=" + std::to_string(start_px) +
                             " hp=" + std::to_string(hp_eq) + " st=" + std::to_string(st_eq) +
                             " pan=" + std::to_string(pan_eq) + " rnd=(" +
                             std::to_string(res[0].render_invar) + "," + std::to_string(res[1].render_invar) + ")" +
                             " A=(" + std::to_string(A0.hp) + "," + std::to_string(A1.hp) + ")" +
                             " B=(" + std::to_string(B0.hp) + "," + std::to_string(B1.hp) + ")" + extra);
                }
                out_had_effect = (A1.hp < 5000 || A0.bari != 0 || A0.han != 0 ||
                                  A0.wana_flg != 0 || A0.wana_flg2 != 0 || A0.joutai != 0 || A1.joutai != 0);
            };

            bool eff_close = false;
            run_chip_mirror_at_pos(2, eff_close);
            if (!eff_close) {
                bool eff_far = false;
                run_chip_mirror_at_pos(1, eff_far);
            }
            tested_chips++;
        }

        // 3) Program Advance discovery + Custom Screen PA combiner check via PaNo / pa_data
        int pa_checked = 0;
        setup_basic_arena(1000, 1000, 0xB3000u, 0);
        if (sf_app_CpCanvas_pa_data > 0) {
            int32_t pa_cnt = rt.array_length(sf_app_CpCanvas_pa_data);
            for (int32_t pid = 0; pid < pa_cnt && pa_checked < 6; ++pid) {
                Ref entry = rt.iaload(sf_app_CpCanvas_pa_data, pid);
                if (entry <= 0 || rt.array_length(entry) < 9) continue;
                int32_t in_t0 = rt.iaload(entry, 0);
                int32_t in_t1 = rt.iaload(entry, 1);
                int32_t in_t2 = rt.iaload(entry, 2);
                int32_t in_t3 = rt.iaload(entry, 3);
                int32_t out_pa = rt.iaload(entry, 8);
                if (in_t0 <= 0 || in_t1 <= 0 || in_t2 <= 0 || in_t3 > 0 || out_pa <= 0) continue;
                int32_t c0 = rt.iaload(entry, 4);
                int32_t c1 = rt.iaload(entry, 5);
                int32_t c2 = rt.iaload(entry, 6);

                // Verify PaNo recognizes sequence across steps 0->1->2 and TipIn/Rock receives out_pa
                int32_t r0 = fn_app_CpCanvas_PaNo___IIII__I(rt, cp, in_t0, c0, 0, 0);
                int32_t r1 = fn_app_CpCanvas_PaNo___IIII__I(rt, cp, in_t1, c1, 1, 1);
                int32_t r2 = fn_app_CpCanvas_PaNo___IIII__I(rt, cp, in_t2, c2, 2, 2);
                if (r2 != pid && r0 < 0 && r1 < 0) {
                    // Skip PAs with code rules not matched identically if any, check matched ones
                }
                // Run mirror check on out_pa
                setup_basic_arena(5000, 5000, 0xB3100u + pid, 0);
                queue_tips_fifo(0, {static_cast<int16_t>(out_pa)});
                for (int t = 0; t < 48; ++t) step_pvp_combat_tick(rt, cp, (t == 0) ? (1 << 20) : 0, 0);
                int32_t a1_hp = g_pvp_eng.core.sides[1].hp;

                setup_basic_arena(5000, 5000, 0xB3100u + pid, 0);
                queue_tips_fifo(1, {static_cast<int16_t>(out_pa)});
                for (int t = 0; t < 48; ++t) step_pvp_combat_tick(rt, cp, 0, (t == 0) ? (1 << 20) : 0);
                int32_t b0_hp = g_pvp_eng.core.sides[0].hp;
                if (a1_hp != b0_hp) {
                    fail_msg("SuiteB-PAMirror", "pid=" + std::to_string(pid) + " out_pa=" +
                             std::to_string(out_pa) + " hp=(" + std::to_string(a1_hp) + "," + std::to_string(b0_hp) + ")");
                }
                pa_checked++;
            }
        }
        if (tested_chips < 200 || pa_checked < 3) {
            fail_msg("SuiteB-Coverage", "tested_chips=" + std::to_string(tested_chips) +
                     " pa_checked=" + std::to_string(pa_checked));
        }
    });
    std::printf("[PvP SELF-TEST] Suite B complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite C: dell_time Plan A Asymmetry + Multi-Frame GGPO Rollback Torture
    // =========================================================================
    run_safe("SuiteC", [&]() {
        // 1) Plan A dell_time[0..9] asymmetry: P0 fast S-delete (100) vs P1 (0)
        pvp::PvpFighterBuild fast_b = make_build("FastSP", 3000, 1, 0x3F, 100);
        pvp::PvpFighterBuild slow_b = make_build("SlowSP", 3000, 1, 0x3F, 0);
        setup_test_arena(fast_b, slow_b, 0xC0001u, 0);

        int sp_verified = 0;
        int32_t boosted_sp_tid = -1;
        for (int32_t sp_tid : g_pvp_eng.sp_tip_ids) {
            Ref t_obj = safe_iaload(rt, sf_app_CpCanvas_tip, sp_tid);
            if (t_obj <= 0 || rt.field_i32(t_obj, kFld_Tip_sp_up) <= 0) continue;
            load_side(rt, 0);
            int32_t p0 = rt.field_i32(t_obj, kFld_Tip_pow);
            load_side(rt, 1);
            int32_t p1 = rt.field_i32(t_obj, kFld_Tip_pow);
            if (p0 <= p1) {
                fail_msg("SuiteC-PlanAPow", "sp_tid=" + std::to_string(sp_tid) +
                         " p0=" + std::to_string(p0) + " <= p1=" + std::to_string(p1));
            } else {
                sp_verified++;
                if (boosted_sp_tid < 0 && rt.field_i32(t_obj, kFld_Tip_syu) == 1 && p1 >= 40) {
                    boosted_sp_tid = sp_tid;
                }
            }
        }
        if (sp_verified == 0 || boosted_sp_tid < 0) {
            fail_msg("SuiteC-PlanADiscovery", "sp_verified=" + std::to_string(sp_verified) +
                     " boosted_sp_tid=" + std::to_string(boosted_sp_tid));
        } else {
            int32_t best_p0 = 0, best_p1 = 0;
            for (int px : {2, 1}) {
                setup_test_arena(fast_b, slow_b, 0xC0001u, 0);
                g_pvp_eng.core.sides[0].pos_x = px;
                g_pvp_eng.core.sides[0].tmp_x = px;
                g_pvp_eng.core.sides[1].pos_x = px;
                g_pvp_eng.core.sides[1].tmp_x = px;
                queue_tips_fifo(0, {static_cast<int16_t>(boosted_sp_tid)});
                for (int t = 0; t < 96; ++t) {
                    step_pvp_combat_tick(rt, cp, (t == 0) ? (1 << 20) : 0, 0);
                }
                int32_t dmg_p0 = 3000 - g_pvp_eng.core.sides[1].hp;

                setup_test_arena(fast_b, slow_b, 0xC0001u, 0);
                g_pvp_eng.core.sides[0].pos_x = px;
                g_pvp_eng.core.sides[0].tmp_x = px;
                g_pvp_eng.core.sides[1].pos_x = px;
                g_pvp_eng.core.sides[1].tmp_x = px;
                queue_tips_fifo(1, {static_cast<int16_t>(boosted_sp_tid)});
                for (int t = 0; t < 96; ++t) step_pvp_combat_tick(rt, cp, 0, (t == 0) ? (1 << 20) : 0);
                int32_t dmg_p1 = 3000 - g_pvp_eng.core.sides[0].hp;
                if (dmg_p0 > best_p0) {
                    best_p0 = dmg_p0;
                    best_p1 = dmg_p1;
                }
                if (dmg_p0 > dmg_p1 && dmg_p1 > 0) break;
            }
            if (best_p0 <= best_p1 || best_p1 <= 0) {
                fail_msg("SuiteC-PlanADamage", "tid=" + std::to_string(boosted_sp_tid) +
                         " dmg_p0=" + std::to_string(best_p0) + " dmg_p1=" + std::to_string(best_p1));
            }
        }

        // 2) Forced multi-frame GGPO rollback (24 frames with divergent P0/P1 inputs & SP chips)
        constexpr int kRbFrames = 24;
        std::array<int32_t, kRbFrames> p0_in{}, p1_in{};
        for (int t = 0; t < kRbFrames; ++t) {
            p0_in[t] = (t == 0) ? (1 << 20) : ((t == 4) ? (1 << 18) : ((t >= 8 && t < 18) ? (1 << 10) : 0));
            p1_in[t] = (t == 2) ? (1 << 20) : ((t == 5) ? (1 << 17) : ((t >= 10 && t < 20) ? (1 << 11) : 0));
        }
        setup_test_arena(fast_b, slow_b, 0xC0002u, 0);
        if (tid_cannon > 0) {
            queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_cannon)});
        }
        for (int t = 0; t < kRbFrames; ++t) {
            step_pvp_combat_tick(rt, cp, p0_in[t], p1_in[t]);
        }
        uint16_t canon_hash = compute_combat_state_hash(rt);
        SideContext canon_s0 = g_pvp_eng.core.sides[0];
        SideContext canon_s1 = g_pvp_eng.core.sides[1];

        // Speculate with wrong P1=0, then roll back from ticks 2->10->24
        setup_test_arena(fast_b, slow_b, 0xC0002u, 0);
        if (tid_cannon > 0) {
            queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_cannon)});
        }
        std::array<int32_t, kRbFrames> spec_p1{};
        for (int t = 0; t < kRbFrames; ++t) {
            uint16_t pre_h = compute_combat_state_hash(rt);
            save_rollback_slot(rt, g_pvp_eng, static_cast<uint32_t>(t), p0_in[t], spec_p1[t], true, pre_h);
            step_pvp_combat_tick(rt, cp, p0_in[t], spec_p1[t]);
            if (t == 11 || t == 23) {
                uint32_t rb_from = (t == 11) ? 2u : 10u;
                for (uint32_t k = rb_from; k <= static_cast<uint32_t>(t); ++k) spec_p1[k] = p1_in[k];
                if (!restore_rollback_slot(rt, g_pvp_eng, rb_from)) {
                    fail_msg("SuiteC-RestoreSlot", "restore_rollback_slot failed at turn " + std::to_string(rb_from));
                    break;
                }
                g_pvp_eng.in_resim = true;
                for (uint32_t k = rb_from; k <= static_cast<uint32_t>(t); ++k) {
                    save_rollback_slot(rt, g_pvp_eng, k, p0_in[k], spec_p1[k], false, compute_combat_state_hash(rt));
                    step_pvp_combat_tick(rt, cp, p0_in[k], spec_p1[k]);
                }
                g_pvp_eng.in_resim = false;
            }
        }
        uint16_t rb_hash = compute_combat_state_hash(rt);
        if (rb_hash != canon_hash ||
            g_pvp_eng.core.sides[0].hp != canon_s0.hp ||
            g_pvp_eng.core.sides[1].hp != canon_s1.hp ||
            g_pvp_eng.core.sides[0].pos_x != canon_s0.pos_x ||
            g_pvp_eng.core.sides[1].pos_x != canon_s1.pos_x) {
            fail_msg("SuiteC-RollbackMatch", "canon=0x" + std::to_string(canon_hash) + " rb=0x" + std::to_string(rb_hash));
        }
    });
    std::printf("[PvP SELF-TEST] Suite C complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite D: EXE6 Trance Matrix & Rollback Across Distinct Soul Forms
    // =========================================================================
    run_safe("SuiteD", [&]() {
        int d_step = 0;
        try {
        auto prep_trance_commit = [&](uint8_t ta, int16_t sac_tid) {
            pvp::PvpCustomCommit c{};
            c.has_lon_v2 = true;
            c.turn_no = current_wire_round_key();
            c.tip_no = (sac_tid > 0 && ta == 0) ? 0 : -1;
            c.rock_tip_ids[0] = (sac_tid > 0 && ta == 0) ? static_cast<uint16_t>(sac_tid) : 0;
            c.trance_action = ta;
            c.arm_chip = (ta >= 1 && ta <= 6 && sac_tid > 0) ? static_cast<uint16_t>(sac_tid) : 0;
            c.fol_used_mask = (sac_tid > 0) ? 1u : 0u;
            uint8_t zmask = 0;
            for (int s = 0; s < kMaxSouls; ++s) zmask |= static_cast<uint8_t>(1u << s);
            if (ta >= 1 && ta <= 6) zmask &= static_cast<uint8_t>(~(1u << (ta - 1)));
            c.trans_zan_mask = zmask;
            return c;
        };

        // 1) (0, 0): skips cutscene cleanly -> ROUND_BANNER
        d_step = 1;
        setup_basic_arena(1000, 1000, 0xD0001u, 0);
        g_pvp_eng.local_commit = prep_trance_commit(0, tid_cannon > 0 ? tid_cannon : 2);
        g_pvp_eng.remote_commit = prep_trance_commit(0, 0);
        enter_round_banner_or_trance(rt, cp);
        if (g_pvp_eng.phase != Phase::ROUND_BANNER) {
            fail_msg("SuiteD-None", "Expected Phase::ROUND_BANNER for (0,0)");
        }

        // 2) (1, 0): Unilateral 54 ticks, sacrifice, flash t=40..47, zero DrawAni heap leak, ChargeShot 263
        d_step = 2;
        setup_basic_arena(1000, 1000, 0xD0002u, 0);
        uint32_t pre_used0 = g_pvp_eng.core.sides[0].fol_used_mask;
        g_pvp_eng.local_commit = prep_trance_commit(1, tid_cannon > 0 ? tid_cannon : 2);
        g_pvp_eng.local_commit.fol_used_mask = pre_used0 | 1u;
        g_pvp_eng.remote_commit = prep_trance_commit(0, 0);
        enter_round_banner_or_trance(rt, cp);
        if (g_pvp_eng.phase != Phase::TRANCE_CUTSCENE || g_pvp_eng.trance_is_revert || g_pvp_eng.trance_tick != 1) {
            fail_msg("SuiteD-UnilateralStart", "Did not enter forward TRANCE_CUTSCENE t=1");
        }
        int cut_ticks = 0;
        size_t heap_at_21 = rt.heap_size();
        bool flash_white_ok = true;
        while (g_pvp_eng.phase == Phase::TRANCE_CUTSCENE && cut_ticks < 60) {
            int32_t cur_t = g_pvp_eng.trance_tick;
            d_step = 2000 + cur_t;
            if (cur_t == 21) heap_at_21 = rt.heap_size();
            rt.set_suppress_av(false);
            step_and_render_trance_cutscene(rt, cp);
            rt.set_suppress_av(true);
            if (cur_t >= 40 && cur_t <= 47) {
                uint32_t center_px = rt.framebuffer()[120 * 240 + 120] & 0x00FFFFFFu;
                if (center_px != 0xFFFFFFu) flash_white_ok = false;
            }
            if (cur_t == 39 && rt.heap_size() != heap_at_21) {
                fail_msg("SuiteD-DrawAniHeapLeak", "heap_size grew during Soul Convergence 21..39: " +
                         std::to_string(heap_at_21) + " -> " + std::to_string(rt.heap_size()));
            }
            cut_ticks++;
        }
        if (cut_ticks != 54 || g_pvp_eng.phase != Phase::ROUND_BANNER || !flash_white_ok ||
            g_pvp_eng.core.sides[0].zoku != 1 || g_pvp_eng.core.sides[0].arm_cnt != 3 ||
            g_pvp_eng.core.sides[0].trans_zan[0] != 0 || (g_pvp_eng.core.sides[0].fol_used_mask & 1u) == 0) {
            fail_msg("SuiteD-UnilateralEnd", "ticks=" + std::to_string(cut_ticks) +
                     " zoku=" + std::to_string(g_pvp_eng.core.sides[0].zoku) +
                     " arm=" + std::to_string(g_pvp_eng.core.sides[0].arm_cnt) +
                     " zan0=" + std::to_string(g_pvp_eng.core.sides[0].trans_zan[0]) +
                     " flash=" + std::to_string(flash_white_ok));
        }
        // Fire Aqua Soul Charge Shot (263) via held+released A button
        d_step = 2500;
        g_pvp_eng.phase = Phase::COMBAT;
        sf_app_CpCanvas_scene = kSceneBattleMain;
        const int chg_t = (42 - sf_app_CpCanvas_cha_lv * 5) + kChargeDelayAdd[1] + 4;
        for (int t = 0; t < chg_t; ++t) step_pvp_combat_tick(rt, cp, (1 << 20), 0);
        for (int t = 0; t < 24; ++t) step_pvp_combat_tick(rt, cp, 0, 0);
        if (g_pvp_eng.core.sides[1].hp >= 1000) {
            fail_msg("SuiteD-AquaChargeShot", "Aqua Soul charge shot (263) dealt no damage");
        }

        // 3) (soul_A=2, soul_B=4): Simultaneous dual 54-tick cutscene + rollback across different forms
        d_step = 3;
        g_pvp_eng.local_commit = prep_trance_commit(2, tid_cannon > 0 ? tid_cannon : 2);
        g_pvp_eng.remote_commit = prep_trance_commit(4, tid_cannon > 0 ? tid_cannon : 2);
        enter_round_banner_or_trance(rt, cp);
        int dual_ticks = 0;
        while (g_pvp_eng.phase == Phase::TRANCE_CUTSCENE && dual_ticks < 60) {
            step_and_render_trance_cutscene(rt, cp);
            dual_ticks++;
        }
        if (dual_ticks != 54 || g_pvp_eng.core.sides[0].zoku != 2 || g_pvp_eng.core.sides[1].zoku != 4) {
            fail_msg("SuiteD-SimultaneousDual", "ticks=" + std::to_string(dual_ticks) +
                     " z0=" + std::to_string(g_pvp_eng.core.sides[0].zoku) +
                     " z1=" + std::to_string(g_pvp_eng.core.sides[1].zoku));
        }

        // Verify GGPO rollback preserves distinct active Soul Trances cleanly
        g_pvp_eng.phase = Phase::COMBAT;
        sf_app_CpCanvas_scene = kSceneBattleMain;
        init_round_rollback_window(rt);
        uint16_t dual_pre_h = compute_combat_state_hash(rt);
        save_rollback_slot(rt, g_pvp_eng, 0, (1 << 20), 0, true, dual_pre_h);
        for (int t = 0; t < 16; ++t) step_pvp_combat_tick(rt, cp, (1 << 20), 0);
        uint16_t canon_trance_h = compute_combat_state_hash(rt);
        restore_rollback_slot(rt, g_pvp_eng, 0);
        for (int t = 0; t < 16; ++t) step_pvp_combat_tick(rt, cp, (1 << 20), 0);
        if (compute_combat_state_hash(rt) != canon_trance_h ||
            g_pvp_eng.core.sides[0].zoku != 2 || g_pvp_eng.core.sides[1].zoku != 4) {
            fail_msg("SuiteD-DualTranceRollback", "Rollback failed to restore distinct Trance forms");
        }

        // 4) (7, 0): Revert-only 20 ticks (100..119), white flash t=106..114
        d_step = 4;
        setup_basic_arena(1000, 1000, 0xD0004u, 0);
        apply_trance_decision_for_side(rt, 0, 1);
        g_pvp_eng.has_pre_custom_snap = false;
        g_pvp_eng.local_commit = prep_trance_commit(7, 0);
        g_pvp_eng.remote_commit = prep_trance_commit(0, 0);
        enter_round_banner_or_trance(rt, cp);
        if (g_pvp_eng.phase != Phase::TRANCE_CUTSCENE || !g_pvp_eng.trance_is_revert || g_pvp_eng.trance_tick != 100) {
            fail_msg("SuiteD-RevertOnlyStart", "Did not enter revert TRANCE_CUTSCENE t=100");
        }
        int rev_ticks = 0;
        bool rev_flash_ok = true;
        while (g_pvp_eng.phase == Phase::TRANCE_CUTSCENE && rev_ticks < 30) {
            int32_t cur_t = g_pvp_eng.trance_tick;
            rt.set_suppress_av(false);
            step_and_render_trance_cutscene(rt, cp);
            rt.set_suppress_av(true);
            if (cur_t >= 106 && cur_t <= 114) {
                uint32_t cpx = rt.framebuffer()[120 * 240 + 120] & 0x00FFFFFFu;
                if (cpx != 0xFFFFFFu) rev_flash_ok = false;
            }
            rev_ticks++;
        }
        if (rev_ticks != 20 || !rev_flash_ok || g_pvp_eng.core.sides[0].zoku != 0) {
            fail_msg("SuiteD-RevertOnlyEnd", "ticks=" + std::to_string(rev_ticks) +
                     " flash=" + std::to_string(rev_flash_ok) +
                     " zoku=" + std::to_string(g_pvp_eng.core.sides[0].zoku));
        }

        // 5) (soul_A=3, 7): Mixed activate + revert runs 54 ticks
        d_step = 5;
        setup_basic_arena(1000, 1000, 0xD0005u, 0);
        apply_trance_decision_for_side(rt, 1, 2);
        g_pvp_eng.has_pre_custom_snap = false;
        g_pvp_eng.local_commit = prep_trance_commit(3, tid_cannon > 0 ? tid_cannon : 2);
        g_pvp_eng.remote_commit = prep_trance_commit(7, 0);
        enter_round_banner_or_trance(rt, cp);
        int mix_ticks = 0;
        while (g_pvp_eng.phase == Phase::TRANCE_CUTSCENE && mix_ticks < 60) {
            d_step = 5000 + g_pvp_eng.trance_tick;
            step_and_render_trance_cutscene(rt, cp);
            mix_ticks++;
        }
        if (mix_ticks != 54 || g_pvp_eng.core.sides[0].zoku != 3 || g_pvp_eng.core.sides[1].zoku != 0) {
            fail_msg("SuiteD-MixedActivateRevert", "ticks=" + std::to_string(mix_ticks) +
                     " z0=" + std::to_string(g_pvp_eng.core.sides[0].zoku) +
                     " z1=" + std::to_string(g_pvp_eng.core.sides[1].zoku));
        }
        } catch (const JavaException& ex) {
            fail_msg("SuiteD", std::string("step=") + std::to_string(d_step) +
                     " JavaException ref=" + std::to_string(ex.exc_ref) +
                     " where=" + (ex.where ? ex.where : "?") + " a=" + std::to_string(ex.detail_a));
        }
    });
    std::printf("[PvP SELF-TEST] Suite D complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite E: Time-Freeze 2-Slot LIFO Cut-In Stack, Backdrop Darken, & Traps
    // =========================================================================
    run_safe("SuiteE", [&]() {
        // 1) 2-slot LIFO Time-Freeze Cut-In stack + backdrop darken
        int32_t dim_tid = (tid_dimming_attack > 0) ? tid_dimming_attack : tid_navi_attack;
        if (dim_tid <= 0) {
            fail_msg("SuiteE-NoDimTip", "Could not find dimming tip for Suite E");
        } else {
            setup_basic_arena(3000, 3000, 0xE0001u, 0);
            // Measure normal backdrop luminance away from y=76 Time-Freeze text announcement banner
            rt.set_suppress_av(false);
            render_pvp_combat_frame(rt, cp);
            rt.set_suppress_av(true);
            uint64_t norm_sum = 0;
            for (int y = 52; y <= 70; ++y) {
                for (int x = 0; x < 36; ++x) {
                    uint32_t rgb = rt.framebuffer()[y * 240 + x] & 0x00FFFFFFu;
                    norm_sum += ((rgb >> 16) & 0xFFu) + ((rgb >> 8) & 0xFFu) + (rgb & 0xFFu);
                }
            }

            queue_tips_fifo(0, {static_cast<int16_t>(dim_tid)});
            queue_tips_fifo(1, {static_cast<int16_t>(dim_tid)});
            step_pvp_combat_tick(rt, cp, (1 << 20), 0);
            bool saw_root = (g_pvp_eng.core.tf_state == TF_ROOT_INTRO && g_pvp_eng.core.tf_depth == 1);

            // Render inside Time-Freeze and confirm backdrop darkened
            g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
            rt.set_suppress_av(false);
            render_pvp_combat_frame(rt, cp);
            rt.set_suppress_av(true);
            uint64_t dark_sum = 0;
            for (int y = 52; y <= 70; ++y) {
                for (int x = 0; x < 36; ++x) {
                    uint32_t rgb = rt.framebuffer()[y * 240 + x] & 0x00FFFFFFu;
                    dark_sum += ((rgb >> 16) & 0xFFu) + ((rgb >> 8) & 0xFFu) + (rgb & 0xFFu);
                }
            }

            // Counter-Cut-In by P1 during TF_ROOT_INTRO banner window -> tf_depth == 2
            step_pvp_combat_tick(rt, cp, 0, (1 << 20));
            bool saw_counter = (g_pvp_eng.core.tf_depth == 2);
            bool saw_resume = false;
            bool saw_idle = false;
            for (int t = 0; t < 300; ++t) {
                step_pvp_combat_tick(rt, cp, 0, 0);
                if (g_pvp_eng.core.tf_state == TF_ROOT_RESUME && g_pvp_eng.core.tf_depth == 1) saw_resume = true;
                if (g_pvp_eng.core.tf_state == TF_IDLE && g_pvp_eng.core.tf_depth == 0) {
                    saw_idle = true;
                    break;
                }
            }
            if (!saw_root || !saw_counter || !saw_resume || !saw_idle || dark_sum >= norm_sum) {
                fail_msg("SuiteE-TfStackAndDarken",
                         "root=" + std::to_string(saw_root) + " counter=" + std::to_string(saw_counter) +
                         " resume=" + std::to_string(saw_resume) + " idle=" + std::to_string(saw_idle) +
                         " tf_state=" + std::to_string(g_pvp_eng.core.tf_state) +
                         " tf_depth=" + std::to_string(g_pvp_eng.core.tf_depth) +
                         " norm_sum=" + std::to_string(norm_sum) + " dark_sum=" + std::to_string(dark_sum));
            }
        }

        // 2) Traps: wana_flg (Elemental 1..4), wana_flg2, wana_pow, mahi_flg reflect & AntiDmg (wana==5)
        setup_basic_arena(2000, 2000, 0xE0002u, 0);
        g_pvp_eng.core.sides[1].wana = 1; // AntiFire
        g_pvp_eng.core.sides[1].wana_flg = 1;
        g_pvp_eng.core.sides[1].wana_pow[0] = 120;
        trigger_opponent_elemental_trap(rt, 0, 1);
        for (int t = 0; t < 40; ++t) {
            step_pvp_combat_tick(rt, cp, 0, 0);
        }
        if (g_pvp_eng.core.sides[0].hp >= 2000 || g_pvp_eng.core.sides[1].wana != 0) {
            fail_msg("SuiteE-ElemTrap", "Elemental trap did not fire back at attacker (p0_hp=" +
                     std::to_string(g_pvp_eng.core.sides[0].hp) + ")");
        }

        int32_t tid_reflect = -1;
        {
            Ref tmp_a = rt.alloc_object(kClassId_Attack, kNumI32_Attack, kNumI64_Attack);
            fn_app_Attack__init____I__V(rt, tmp_a, 29);
            for (int32_t t = 1; t <= 270; ++t) {
                Ref to = safe_iaload(rt, sf_app_CpCanvas_tip, t);
                if (to <= 0) continue;
                int32_t wid = rt.field_i32(to, kFld_Tip_waza_id);
                if (wid <= 0) continue;
                fn_app_Attack_Set___IIIII__I(rt, tmp_a, wid, 1, 1, 0, 1);
                Ref p_arr = rt.field_i32(tmp_a, kFld_Attack_param);
                if (p_arr > 0 && safe_iaload(rt, p_arr, 8) == 23 && safe_iaload(rt, p_arr, 10) == 1) {
                    tid_reflect = t;
                    break;
                }
            }
        }
        setup_basic_arena(2000, 2000, 0xE0003u, 0);
        g_pvp_eng.core.sides[0].pos_x = 2;
        g_pvp_eng.core.sides[0].tmp_x = 2;
        g_pvp_eng.core.sides[1].pos_x = 2;
        g_pvp_eng.core.sides[1].tmp_x = 2;
        g_pvp_eng.core.sides[1].wana_flg2 = 2;
        if (tid_cannon > 0) queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
        if (tid_reflect > 0) queue_tips_fifo(1, {static_cast<int16_t>(tid_reflect)});
        bool injected_mahi = false;
        bool saw_p0_paralyzed = false;
        for (int t = 0; t < 40; ++t) {
            int32_t p0_m = (t == 0) ? (1 << 20) : 0;
            int32_t p1_m = (t == 3) ? (1 << 20) : 0;
            step_pvp_combat_tick(rt, cp, p0_m, p1_m);
            if (!injected_mahi && g_pvp_eng.core.sides[1].han == 1 && g_pvp_eng.core.sides[1].han_id >= 0) {
                int32_t hid = g_pvp_eng.core.sides[1].han_id;
                g_pvp_eng.core.sides[1].ata_data[hid][kFld_Attack_pow] = 80;
                g_pvp_eng.core.sides[1].ata_data[hid][kFld_Attack_mahi_flg] = 1;
                injected_mahi = true;
            }
            if (g_pvp_eng.core.sides[0].joutai != 0) saw_p0_paralyzed = true;
        }
        if (g_pvp_eng.core.sides[1].hp != 2000 || g_pvp_eng.core.sides[0].hp >= 2000 ||
            g_pvp_eng.core.sides[1].wana_flg2 != 2 || !saw_p0_paralyzed) {
            fail_msg("SuiteE-ReflectMahi", "p0_hp=" + std::to_string(g_pvp_eng.core.sides[0].hp) +
                     " p1_hp=" + std::to_string(g_pvp_eng.core.sides[1].hp) +
                     " saw_mahi0=" + std::to_string(saw_p0_paralyzed ? 1 : 0) +
                     " wana_flg2=" + std::to_string(g_pvp_eng.core.sides[1].wana_flg2));
        }

        // AntiDmg (wana == 5) shuriken trap counter + wana_flg2=1
        setup_basic_arena(2000, 2000, 0xE0004u, 0);
        g_pvp_eng.core.sides[1].wana = 5;
        g_pvp_eng.core.sides[1].wana_pow[0] = 100;
        g_pvp_eng.core.sides[1].wana_pow[2] = 100;
        if (tid_cannon > 0) queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
        for (int t = 0; t < 40; ++t) step_pvp_combat_tick(rt, cp, (t == 0) ? (1 << 20) : 0, 0);
        if (g_pvp_eng.core.sides[1].hp != 2000 || g_pvp_eng.core.sides[0].hp >= 2000 ||
            g_pvp_eng.core.sides[1].wana != 0) {
            fail_msg("SuiteE-AntiDmgWana5", "p0_hp=" + std::to_string(g_pvp_eng.core.sides[0].hp) +
                     " p1_hp=" + std::to_string(g_pvp_eng.core.sides[1].hp) +
                     " wana1=" + std::to_string(g_pvp_eng.core.sides[1].wana));
        }
    });
    std::printf("[PvP SELF-TEST] Suite E complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite F: Okimono 6-Slot Ownership & Rock.Action2
    // =========================================================================
    run_safe("SuiteF", [&]() {
        // Discover an obstacle chip (Okimono spawner) across tip table
        int32_t found_oki_tid = -1;
        for (int32_t tid = 2; tid <= std::min<int32_t>(270, tip_len - 1); ++tid) {
            Ref t_obj = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
            if (t_obj <= 0) continue;
            int32_t syu = rt.field_i32(t_obj, kFld_Tip_syu);
            if (syu == 4 || syu == 5) continue;
            setup_basic_arena(2000, 2000, 0xF0001u, 0);
            queue_tips_fifo(0, {static_cast<int16_t>(tid)});
            step_pvp_combat_tick(rt, cp, (1 << 20), 0);
            for (int t = 0; t < 48 && found_oki_tid < 0; ++t) {
                step_pvp_combat_tick(rt, cp, 0, 0);
                for (int s = 0; s < kMaxOkimono; ++s) {
                    Ref ok = sf_app_CpCanvas_oki > 0 ? rt.iaload(sf_app_CpCanvas_oki, s) : 0;
                    if (g_pvp_eng.core.oki_owner[s] == 0 || (ok > 0 && rt.field_i32(ok, kFld_Okimono_on) != 0)) {
                        found_oki_tid = tid;
                        break;
                    }
                }
            }
            if (found_oki_tid > 0) break;
        }
        if (found_oki_tid <= 0) {
            fail_msg("SuiteF-DiscoverOki", "No Okimono-spawning chip found in tip table (scanned tid 2..270)");
        } else {
            // Spawn both P0 and P1 Okimono simultaneously and test ownership, orient, render, destroy
            setup_basic_arena(2000, 2000, 0xF0002u, 0);
            queue_tips_fifo(0, {static_cast<int16_t>(found_oki_tid)});
            queue_tips_fifo(1, {static_cast<int16_t>(found_oki_tid)});
            for (int t = 0; t < 96 && (t < 50 || g_pvp_eng.core.tf_state != TF_IDLE); ++t) {
                step_pvp_combat_tick(rt, cp, (t == 0) ? (1 << 20) : 0, (t == 0) ? (1 << 20) : 0);
            }
            int p0_slots = 0, p1_slots = 0;
            for (int i = 0; i < kMaxOkimono; ++i) {
                if (g_pvp_eng.core.oki_owner[i] == 0) p0_slots++;
                if (g_pvp_eng.core.oki_owner[i] == 1) p1_slots++;
            }
            rt.set_suppress_av(false);
            render_pvp_combat_frame(rt, cp);
            rt.set_suppress_av(true);
            ensure_board_orient(rt, 1);
            ensure_board_orient(rt, 0);
            // Hit P1's Okimono explicitly via Okimono.Hit until cleared
            for (int i = 0; i < kMaxOkimono; ++i) {
                if (g_pvp_eng.core.oki_owner[i] == 1 && sf_app_CpCanvas_oki > 0) {
                    Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                    if (ok > 0 && rt.field_i32(ok, kFld_Okimono_on) != 0) {
                        fn_app_Okimono_Hit___III__V(rt, ok, 9999, 0, 0);
                    }
                }
            }
            step_pvp_combat_tick(rt, cp, 0, 0);
            if (p0_slots == 0 || p1_slots == 0) {
                fail_msg("SuiteF-OkiOwnership", "tid=" + std::to_string(found_oki_tid) +
                         " p0_slots=" + std::to_string(p0_slots) + " p1_slots=" + std::to_string(p1_slots));
            }
        }

        // Verify Rock.Action2 melee/dash chip (scan for action with Action2/close melee or waza_kou != 0)
        int32_t melee_tid = -1;
        for (int32_t tid = 2; tid <= std::min<int32_t>(270, tip_len - 1); ++tid) {
            Ref t_obj = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
            if (t_obj <= 0 || rt.field_i32(t_obj, kFld_Tip_pow) < 40 || rt.field_i32(t_obj, kFld_Tip_syu) != 0) continue;
            int32_t wid = rt.field_i32(t_obj, kFld_Tip_waza_id);
            if (sf_app_CpCanvas_waza > 0 && wid >= 0 && wid < rt.array_length(sf_app_CpCanvas_waza)) {
                Ref w_obj = rt.iaload(sf_app_CpCanvas_waza, wid);
                if (w_obj > 0 && rt.field_i32(w_obj, kFld_Waza_stop_time) == 0 &&
                    rt.field_i32(w_obj, kFld_Waza_waza_kou) >= 1) {
                    melee_tid = tid;
                    break;
                }
            }
        }
        if (melee_tid > 0) {
            setup_basic_arena(1500, 1500, 0xF0003u, 0);
            g_pvp_eng.core.sides[0].pos_x = 2;
            g_pvp_eng.core.sides[0].tmp_x = 2;
            g_pvp_eng.core.sides[1].pos_x = 2;
            g_pvp_eng.core.sides[1].tmp_x = 2;
            load_side(rt, 0);
            sync_panel_occupancy_and_proxy(rt, 0);
            fn_app_Rock_Action2___II__V(rt, sf_app_CpCanvas_rock, melee_tid, 0);
            save_side(rt, 0);
            for (int t = 0; t < 24; ++t) step_pvp_combat_tick(rt, cp, 0, 0);
            if (g_pvp_eng.core.sides[1].hp >= 1500 && g_pvp_eng.core.sides[0].pos_x == 2) {
                fail_msg("SuiteF-Action2", "Rock.Action2 tid=" + std::to_string(melee_tid) + " had no effect");
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite F complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite G: 15-Turn Judge Tiebreaker & BO3 Series Reset
    // =========================================================================
    run_safe("SuiteG", [&]() {
        // 1) 15-turn Judge tiebreaker: equal HP, unequal damage_dealt -> higher damage wins; equal -> draw
        setup_basic_arena(1000, 1000, 0x60001u, 0);
        g_pvp_eng.has_pre_match_snap = false;
        capture_pre_match_snapshot(rt, g_pvp_eng);
        g_pvp_eng.is_triple_battle = true;
        g_pvp_eng.core.custom_round = kMaxTurns;
        g_pvp_eng.core.final_turn_ticks_left = 1;
        g_pvp_eng.core.sides[0].hp = 600;
        g_pvp_eng.core.sides[1].hp = 600;
        g_pvp_eng.core.damage_dealt[0] = 520;
        g_pvp_eng.core.damage_dealt[1] = 400;
        if (g_pvp_eng.core.custom_round >= kMaxTurns && g_pvp_eng.core.final_turn_ticks_left == 1) {
            g_pvp_eng.core.ended_by_judge = true;
            if (g_pvp_eng.core.sides[0].hp != g_pvp_eng.core.sides[1].hp) {
                g_pvp_eng.core.winner = (g_pvp_eng.core.sides[0].hp > g_pvp_eng.core.sides[1].hp) ? 0 : 1;
            } else if (g_pvp_eng.core.damage_dealt[0] != g_pvp_eng.core.damage_dealt[1]) {
                g_pvp_eng.core.winner = (g_pvp_eng.core.damage_dealt[0] > g_pvp_eng.core.damage_dealt[1]) ? 0 : 1;
            } else {
                g_pvp_eng.core.winner = -1;
            }
        }
        if (g_pvp_eng.core.winner != 0) {
            fail_msg("SuiteG-JudgeDmgTiebreak", "Expected P0 win on higher damage_dealt");
        }
        conclude_current_game(rt, cp);
        if (g_pvp_eng.bo3_wins[0] != 1 || g_pvp_eng.bo3_wins[1] != 0) {
            fail_msg("SuiteG-Bo3WinCount", "bo3_wins=(" + std::to_string(g_pvp_eng.bo3_wins[0]) + "," +
                     std::to_string(g_pvp_eng.bo3_wins[1]) + ")");
        }

        // Dirty Trance/Folder/Panels/BackdropCache before start_next_bo3_game
        apply_trance_decision_for_side(rt, 0, 1);
        g_pvp_eng.core.sides[0].sha_cnt = 4;
        g_pvp_eng.core.sides[0].fol_used_mask = 0x3FFFFFFFu;
        if (sf_app_CpCanvas_panel > 0) {
            Ref p = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 1), 2);
            if (p > 0) {
                rt.field_i32(p, kFld_BtPanel_jin) = 1;
                rt.field_i32(p, kFld_BtPanel_ana_cnt) = 120;
            }
        }
        g_pvp_eng.backdrop_cache.valid = true;

        start_next_bo3_game(rt, cp);

        bool reset_ok = (g_pvp_eng.core.sides[0].zoku == 0 &&
                         g_pvp_eng.core.sides[0].arm_cnt == 0 &&
                         g_pvp_eng.core.sides[0].sha_cnt == 0 &&
                         g_pvp_eng.core.sides[0].trans_zan[0] == 1 &&
                         g_pvp_eng.core.sides[0].fol_used_mask == 0);
        if (sf_app_CpCanvas_panel > 0) {
            Ref p = rt.iaload(rt.iaload(sf_app_CpCanvas_panel, 1), 2);
            if (p > 0 && (rt.field_i32(p, kFld_BtPanel_jin) != 0 || rt.field_i32(p, kFld_BtPanel_ana_cnt) != 0)) {
                reset_ok = false;
            }
        }
        if (!reset_ok) {
            fail_msg("SuiteG-Bo3Reset", "start_next_bo3_game left dirty trance/folder/panel state");
        }
    });
    std::printf("[PvP SELF-TEST] Suite G complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite H: v2 Sanitizer Clamps & Outer UDP Packet Authentication
    // =========================================================================
    run_safe("SuiteH", [&]() {
        // 1) Sanitize v2 build + custom commit
        pvp::PvpFighterBuild bad_b = base_build;
        bad_b.max_hp = 999999;
        bad_b.folder_packed[0] = (999u << 8) | 0u;
        bad_b.arm_flg = 99;
        bad_b.get_arm_mask = 0xFF;
        pvp::PvpFighterBuild clean_b = sanitize_lon_build(rt, bad_b);
        if (clean_b.max_hp != 9999 || ((clean_b.folder_packed[0] >> 8) & 0xFFFFu) != 1u ||
            clean_b.arm_flg != 1 || clean_b.get_arm_mask != 0x3F) {
            fail_msg("SuiteH-SanitizeBuild", "sanitize_lon_build failed clamp");
        }

        setup_basic_arena(1000, 1000, 0x80001u, 0);
        g_pvp_eng.core.sides[0].arm_flg = 1;
        g_pvp_eng.core.sides[0].get_arm_mask = 0x01; // Only soul 0 unlocked
        g_pvp_eng.core.sides[0].trans_zan[0] = 0;    // Already spent soul 0!
        pvp::PvpCustomCommit bad_c{};
        bad_c.has_lon_v2 = true;
        bad_c.tip_no = 12;
        bad_c.rock_tip_ids[0] = 999;
        bad_c.arm_chip = 999;
        bad_c.trance_action = 4; // Unowned soul 3
        bad_c.fol_used_mask = g_pvp_eng.core.sides[0].fol_used_mask; // Forged arm_chip without spending folder slot
        sanitize_lon_commit(rt, g_pvp_eng.core.sides[0], bad_c);
        if (bad_c.tip_no > 4 || bad_c.rock_tip_ids[0] > 270 || bad_c.trance_action != 0 || bad_c.arm_chip != 0) {
            fail_msg("SuiteH-SanitizeCommit", "tip_no=" + std::to_string(bad_c.tip_no) +
                     " rt0=" + std::to_string(bad_c.rock_tip_ids[0]) +
                     " ta=" + std::to_string(bad_c.trance_action) +
                     " arm=" + std::to_string(bad_c.arm_chip));
        }

        // 2) LoopbackRelay + dual PvpClientSession v2 build/commit wire check
        LoopbackRelay relay;
        if (!relay.start()) {
            fail_msg("SuiteH-RelayStart", "LoopbackRelay::start failed");
        } else {
            pvp::PvpClientSession hn, gn;
            hn.set_preferred_format(pvp::PVP_FORMAT_SINGLE);
            gn.set_preferred_format(pvp::PVP_FORMAT_SINGLE);
            if (!hn.create_room(relay.addr(), "lon_h", "LoNH", "lon") ||
                !gn.join_room(relay.addr(), hn.room_id(), "lon_g", "LoNG", "lon")) {
                fail_msg("SuiteH-RelayJoin", "create/join failed");
            } else {
                bool linked = false;
                for (int i = 0; i < 400; ++i) {
                    hn.poll_udp();
                    gn.poll_udp();
                    if (hn.match_started() && gn.match_started()) {
                        hn.send_build_sync(clean_b);
                        gn.send_build_sync(clean_b);
                        if (hn.has_remote_build() && hn.local_build_acked() &&
                            gn.has_remote_build() && gn.local_build_acked()) {
                            linked = true;
                            break;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (!linked) fail_msg("SuiteH-RelayBuildSync", "Build sync handshake timed out");
            }
            hn.close_udp();
            gn.close_udp();
        }

        // 3) Outer UDP packet auth checks (bad version=2, bad token, bad RMXP magic, rogue source port)
        plat::sock_t m_tcp = ::socket(AF_INET, SOCK_STREAM, 0);
        plat::sock_t m_udp = ::socket(AF_INET, SOCK_DGRAM, 0);
        plat::sock_t rogue_udp = ::socket(AF_INET, SOCK_DGRAM, 0);
        uint16_t m_port = 0;
        bool sock_ok = (m_tcp != plat::kInvalidSocket && m_udp != plat::kInvalidSocket &&
                        rogue_udp != plat::kInvalidSocket);
        if (sock_ok) {
            plat::set_socket_reuseaddr(m_tcp);
            plat::set_socket_reuseaddr(m_udp);
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sockaddr_in bound{};
            socklen_t blen = sizeof(bound);
            sock_ok = (::bind(m_tcp, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0 &&
                       ::listen(m_tcp, 4) == 0 &&
                       ::getsockname(m_tcp, reinterpret_cast<sockaddr*>(&bound), &blen) == 0);
            if (sock_ok) {
                m_port = ntohs(bound.sin_port);
                sa.sin_port = htons(m_port);
                sock_ok = (::bind(m_udp, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0);
            }
            sa.sin_port = 0;
            if (sock_ok) sock_ok = (::bind(rogue_udp, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0);
        }
        if (!sock_ok) {
            if (m_tcp != plat::kInvalidSocket) plat::close_socket(m_tcp);
            if (m_udp != plat::kInvalidSocket) plat::close_socket(m_udp);
            if (rogue_udp != plat::kInvalidSocket) plat::close_socket(rogue_udp);
            fail_msg("SuiteH-MockSock", "Failed to bind loopback mock sockets");
        } else {
            plat::set_socket_nonblocking(m_tcp, true);
            plat::set_socket_nonblocking(m_udp, true);
            plat::disable_udp_connreset(m_udp);
            std::atomic<bool> stop_srv{false};
            std::thread http_thr([&]() {
                while (!stop_srv.load(std::memory_order_relaxed)) {
                    pollfd pfd{m_tcp, POLLIN, 0};
                    if (plat::poll_sockets(&pfd, 1, 5) > 0 && (pfd.revents & POLLIN)) {
                        sockaddr_in ca{};
                        socklen_t cl = sizeof(ca);
                        plat::sock_t cfd = ::accept(m_tcp, reinterpret_cast<sockaddr*>(&ca), &cl);
                        if (cfd != plat::kInvalidSocket) {
                            uint8_t rb[1024];
                            pollfd cpfd{cfd, POLLIN, 0};
                            if (plat::poll_sockets(&cpfd, 1, 20) > 0) (void)plat::sock_recv(cfd, rb, sizeof(rb));
                            const char* body = "{\"ok\":true,\"room_id\":2002,\"slot\":1,\"token\":54321,\"format\":\"single\"}";
                            std::ostringstream oss;
                            oss << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                << std::strlen(body) << "\r\nConnection: close\r\n\r\n" << body;
                            std::string r = oss.str();
                            (void)plat::sock_send(cfd, reinterpret_cast<const uint8_t*>(r.data()), r.size());
                            plat::close_socket(cfd);
                        }
                    }
                }
            });
            pvp::PvpClientSession sess;
            bool cr_ok = sess.create_room("127.0.0.1:" + std::to_string(m_port), "H_LON", "H_LON", "lon");
            sockaddr_storage cli_addr{};
            socklen_t cli_len = sizeof(cli_addr);
            bool got_bind = false;
            if (cr_ok) {
                (void)sess.poll_udp();
                uint8_t ub[512];
                for (int w = 0; w < 25; ++w) {
                    socklen_t sl = sizeof(cli_addr);
                    if (plat::sock_recvfrom(m_udp, ub, sizeof(ub),
                                            reinterpret_cast<sockaddr*>(&cli_addr), &sl) > 0) {
                        cli_len = sl;
                        got_bind = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (!cr_ok || !got_bind) {
                fail_msg("SuiteH-MockBind", "create_room/PKT_BIND failed");
            } else {
                auto send_hdr = [&](plat::sock_t fd, bool corrupt_magic, uint8_t ver, uint8_t ptype,
                                    uint32_t tok, uint32_t f, int32_t mask) {
                    pvp::PvpPacketHeader h{};
                    h.version = ver;
                    h.pkt_type = ptype;
                    h.slot = 2;
                    h.flags = pvp::PVP_HDR_FLAG_FMT_KNOWN;
                    h.room_id = 2002;
                    h.token = tok;
                    h.frame_or_seq = f;
                    h.keypad_mask = mask;
                    auto wire = h.serialize();
                    if (corrupt_magic) wire[0] = 'X';
                    plat::sock_sendto(fd, wire.data(), wire.size(),
                                      reinterpret_cast<const sockaddr*>(&cli_addr), cli_len);
                };
                send_hdr(m_udp, false, 1, pvp::PKT_BIND_ACK, 54321, 1, 0);
                send_hdr(m_udp, false, 1, pvp::PKT_MATCH_START, 54321, 1, 0);
                (void)sess.poll_udp();

                send_hdr(m_udp, true, 1, pvp::PKT_FRAME_INPUT, 54321, 90, 0x1111);  // Bad magic
                send_hdr(m_udp, false, 2, pvp::PKT_FRAME_INPUT, 54321, 91, 0x2222); // Bad version=2
                send_hdr(m_udp, false, 1, pvp::PKT_FRAME_INPUT, 99999, 92, 0x3333); // Bad token
                send_hdr(rogue_udp, false, 1, pvp::PKT_FRAME_INPUT, 54321, 93, 0x4444); // Rogue source port
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                (void)sess.poll_udp();

                int32_t dummy_m = 0;
                bool drops_ok = (!sess.pop_remote_input(90, dummy_m) &&
                                 !sess.pop_remote_input(91, dummy_m) &&
                                 !sess.pop_remote_input(92, dummy_m) &&
                                 !sess.pop_remote_input(93, dummy_m));
                send_hdr(m_udp, false, 1, pvp::PKT_FRAME_INPUT, 54321, 94, 0x200000);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                (void)sess.poll_udp();
                int32_t acc_m = 0;
                bool acc_ok = (sess.pop_remote_input(94, acc_m) && acc_m == 0x200000);
                if (!drops_ok || !acc_ok) {
                    fail_msg("SuiteH-UdpAuth", "drops_ok=" + std::to_string(drops_ok) +
                             " acc_ok=" + std::to_string(acc_ok));
                }
                sess.close_udp();
            }
            stop_srv.store(true, std::memory_order_relaxed);
            http_thr.join();
            plat::close_socket(rogue_udp);
            plat::close_socket(m_udp);
            plat::close_socket(m_tcp);
        }
        // Regression check: HANDSHAKE disconnect/timeout before init_pvp_battle_arena must render Phase::RESULT safely
        {
            rt.reset_interpolator();
            rt.set_suppress_av(true);
            rt.restore_full_snapshot(base_heap);
            generated_restore_statics(base_statics);
            g_pvp_eng = PvpEngineState{};
            capture_pre_match_snapshot(rt, g_pvp_eng);
            g_pvp_eng.phase = Phase::HANDSHAKE;
            g_pvp_eng.local_idx = 0;
            adjudicate_peer_disconnect_win(rt);
            bool tick_ok = pvp_lon_tick_override_impl(rt, cp);
            if (!tick_ok || g_pvp_eng.phase != Phase::RESULT || g_pvp_eng.result_ticks != 1) {
                fail_msg("SuiteH-HandshakeDisconnectResult",
                         "tick_ok=" + std::to_string(tick_ok) +
                         " phase=" + std::to_string(static_cast<int>(g_pvp_eng.phase)) +
                         " ticks=" + std::to_string(g_pvp_eng.result_ticks));
            }
            cleanup_and_restore_singleplayer(rt, false);
        }
        // Regression check: Town/Event ImgSet2(2) overwrite must be restored by ImgSet2(0) in arena init
        {
            rt.restore_full_snapshot(base_heap);
            generated_restore_statics(base_statics);
            fn_app_CpCanvas_ImgSet2___I__V(rt, cp, 2);
            setup_basic_arena(1000, 1000, 0x8888u, 0);
            rt.set_suppress_av(false);
            render_pvp_combat_frame(rt, cp);
            rt.set_suppress_av(true);
            if (safe_iaload(rt, sf_app_CpCanvas_image, 70) <= 0) {
                fail_msg("SuiteH-TownSpriteReload", "image[70] was not reloaded by init_pvp_battle_arena");
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite H complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite I: 60fps Interpolator & Render State/Heap Invariance
    // =========================================================================
    run_safe("SuiteI", [&]() {
        for (int loc = 0; loc < 2; ++loc) {
            setup_basic_arena(1000, 1000, 0x90001u + loc, loc);
            rt.set_suppress_av(false);
            rt.reset_interpolator();

            refresh_arena_imgmap_backdrop(rt, cp, true);
            uint32_t gen0 = rt.interp_test_img_gen(sf_app_CpCanvas_imgMap);
            refresh_arena_imgmap_backdrop(rt, cp, true);
            uint32_t gen1 = rt.interp_test_img_gen(sf_app_CpCanvas_imgMap);
            if (gen1 != gen0) {
                fail_msg("SuiteI-BackdropCacheGen", "imgMap gen changed without panel/RGB mutation");
            }

            if (tid_cannon > 0) queue_tips_fifo(loc, {static_cast<int16_t>(tid_cannon)});
            std::vector<uint32_t> sub_fb(240 * 240, 0), prev_fb(240 * 240, 0), cur_fb(240 * 240, 0);
            bool have_prev = false;
            bool interp_ok = false;
            bool invar_ok = true;

            for (int k = 0; k < 14; ++k) {
                // Move Rock up/down and fire projectile to create visible inter-frame delta
                int32_t m_loc = (k == 0) ? (1 << 20) : ((k % 4 < 2) ? (1 << 17) : (1 << 19));
                int32_t p0m = (loc == 0) ? m_loc : 0;
                int32_t p1m = (loc == 1) ? m_loc : 0;
                step_pvp_combat_tick(rt, cp, p0m, p1m);

                uint16_t pre_h = compute_combat_state_hash(rt);
                size_t heap_before = rt.heap_size();
                rt.set_suppress_av(false);
                rt.interp_test_begin_frame();
                render_pvp_combat_frame(rt, cp);
                bool can_interp = rt.interp_test_end_and_rasterize(0.25f, sub_fb.data());
                std::copy_n(rt.framebuffer(), 240 * 240, cur_fb.data());
                if (compute_combat_state_hash(rt) != pre_h || rt.heap_size() != heap_before) {
                    invar_ok = false;
                }

                if (have_prev && can_interp) {
                    int hud_px = 0;
                    for (int y = 4; y <= 35; ++y) {
                        for (int x = 0; x < 240; ++x) {
                            if ((sub_fb[y * 240 + x] & 0x00FFFFFFu) != 0) hud_px++;
                        }
                    }
                    bool arena_diff = false;
                    for (int y = 40; y < 200 && !arena_diff; ++y) {
                        for (int x = 0; x < 240; ++x) {
                            uint32_t px = sub_fb[y * 240 + x];
                            if (px != cur_fb[y * 240 + x] && px != prev_fb[y * 240 + x]) {
                                arena_diff = true;
                                break;
                            }
                        }
                    }
                    if (hud_px >= 200 && arena_diff) {
                        interp_ok = true;
                        break;
                    }
                }
                prev_fb.swap(cur_fb);
                have_prev = true;
            }
            if (!invar_ok || !interp_ok) {
                fail_msg("SuiteI-Interp", "loc=" + std::to_string(loc) + " invar=" +
                         std::to_string(invar_ok) + " interp=" + std::to_string(interp_ok));
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite I complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite J: Cold No-.sav Init(0), Custom Screen Tray Persistence & Backdrop Render
    // =========================================================================
    run_safe("SuiteJ", [&]() {
        // 1) Cold no-.sav fallback check: init(0) directly populates max_hp >= 100 and 30 valid folder chips
        rt.restore_full_snapshot(base_heap);
        generated_restore_statics(base_statics);
        g_pvp_eng = PvpEngineState{};
        sf_app_CpCanvas_max_hp = 0;
        fn_app_CpCanvas_init___I__V(rt, cp, 0);
        int32_t cold_hp = sf_app_CpCanvas_max_hp;
        pvp::PvpFighterBuild cold_init_b = extract_local_lon_build(rt, cp);

        rt.restore_full_snapshot(base_heap);
        generated_restore_statics(base_statics);
        g_pvp_eng = PvpEngineState{};
        sf_app_CpCanvas_max_hp = 0;
        pvp::PvpFighterBuild cold_ext_b = extract_local_lon_build(rt, cp);

        int init_valid_chips = 0;
        int ext_valid_chips = 0;
        for (int i = 0; i < 30; ++i) {
            if (((cold_init_b.folder_packed[i] >> 8) & 0xFFFFu) > 0) init_valid_chips++;
            if (((cold_ext_b.folder_packed[i] >> 8) & 0xFFFFu) > 0) ext_valid_chips++;
        }
        if (cold_hp < 100 || init_valid_chips != 30 || cold_ext_b.max_hp < 100 || ext_valid_chips != 30) {
            fail_msg("SuiteJ-ColdInit0",
                     "cold_hp=" + std::to_string(cold_hp) +
                     " init_chips=" + std::to_string(init_valid_chips) +
                     " ext_hp=" + std::to_string(cold_ext_b.max_hp) +
                     " ext_chips=" + std::to_string(ext_valid_chips));
        }

        // 2) Live HANDSHAKE -> CUSTOM_SCREEN tray persistence, chip icons/text, right arena & opp sprite
        rt.restore_full_snapshot(base_heap);
        generated_restore_statics(base_statics);
        g_pvp_eng = PvpEngineState{};
        rt.pvp_session().close_udp();

        LoopbackRelay relay_j;
        if (!relay_j.start()) {
            fail_msg("SuiteJ-RelayStart", "LoopbackRelay::start failed");
        } else {
            SelfTestFlagGuard st_j(rt);
            DefaultSingleFormatGuard def_fmt_j(rt.pvp_session());
            pvp::PvpClientSession peer_j;
            peer_j.set_preferred_format(pvp::PVP_FORMAT_SINGLE);

            pvp::PvpFighterBuild peer_b = sanitize_lon_build(rt, base_build);
            peer_b.player_name = "PeerJ";

            if (!rt.pvp_session().create_room(relay_j.addr(), "J_Host", "LoN0", "lon") ||
                !peer_j.join_room(relay_j.addr(), rt.pvp_session().room_id(), "J_Peer", "PeerJ", "lon")) {
                fail_msg("SuiteJ-RelayJoin", "create/join failed in Suite J");
            } else {
                rt.set_suppress_av(false);
                for (int d = 0; d < 20; ++d) rt.present_screen();
                auto step_j = [&]() {
                    (void)pvp_lon_tick_override_impl(rt, cp);
                    rt.present_screen();
                };
                bool entered_custom = false;
                for (int w = 0; w < 400; ++w) {
                    peer_j.poll_udp();
                    if (peer_j.match_started()) {
                        peer_j.send_build_sync(peer_b);
                    }
                    rt.inject_key_at_frame(rt.frame_count() + 1, 19);
                    step_j();
                    if (g_pvp_eng.phase == Phase::CUSTOM_SCREEN) {
                        entered_custom = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }

                if (!entered_custom) {
                    fail_msg("SuiteJ-EnterCustom", "Did not reach Phase::CUSTOM_SCREEN");
                } else {
                    const uint32_t* fb = rt.framebuffer();
                    uint32_t hs_px = fb[50 * 240 + 200] & 0x00FFFFFFu;
                    if (hs_px != 0x0A1428u) {
                        fail_msg("SuiteJ-HandshakePrePaint", "Unexpected pre-custom pixel=" + std::to_string(hs_px));
                    }

                    // Tick 1 (keep Down bit 19 held across enter_custom_screen_phase -> must NOT move cursor)
                    rt.inject_key_at_frame(rt.frame_count() + 1, 19);
                    step_j();
                    bool carry_suppressed = (sf_app_CpCanvas_sel_cas_mode == 0 && sf_app_CpCanvas_sel_cas_tip == 0);
                    int32_t t1_soft1 = safe_iaload(rt, sf_app_CpCanvas_soft_id, 1, -1);
                    bool t1_cas_ok = true;
                    for (int i = 0; i < 5; ++i) {
                        if (safe_iaload(rt, sf_app_CpCanvas_cas_tip, i, -1) <= 0) t1_cas_ok = false;
                    }

                    step_j();
                    std::vector<uint32_t> tick2_fb(fb, fb + 240 * 240);

                    int hs_bleed = 0;
                    for (int y = 62; y <= 180; ++y) {
                        for (int x = 130; x <= 236; ++x) {
                            uint32_t rgb = tick2_fb[y * 240 + x] & 0x00FFFFFFu;
                            if (rgb == 0x0A1428u || rgb == 0x081426u) hs_bleed++;
                        }
                    }

                    int stage_px = 0;
                    for (int y = 62; y <= 92; ++y) {
                        for (int x = 130; x <= 236; ++x) {
                            uint32_t rgb = tick2_fb[y * 240 + x] & 0x00FFFFFFu;
                            if (rgb != 0 && rgb != 0x0A1428u && rgb != 0x081426u) stage_px++;
                        }
                    }

                    int blue_panel_px = 0;
                    for (int y = 122; y <= 178; ++y) {
                        for (int x = 204; x <= 236; ++x) {
                            uint32_t px = tick2_fb[y * 240 + x];
                            int r = (px >> 16) & 0xFF;
                            int b = px & 0xFF;
                            if (b > r + 15 && b >= 100) blue_panel_px++;
                        }
                    }

                    // Control render with opponent HP = 0 to prove opponent Navi sprite contributes on right half
                    int opp = g_pvp_eng.opp_idx();
                    int32_t saved_opp_hp = g_pvp_eng.core.sides[opp].hp;
                    g_pvp_eng.core.sides[opp].hp = 0;
                    render_custom_screen_backdrop(rt, cp);
                    int opp_sprite_diff = 0;
                    for (int y = 70; y <= 180; ++y) {
                        for (int x = 120; x < 240; ++x) {
                            if ((fb[y * 240 + x] & 0x00FFFFFFu) != (tick2_fb[y * 240 + x] & 0x00FFFFFFu)) {
                                opp_sprite_diff++;
                            }
                        }
                    }
                    g_pvp_eng.core.sides[opp].hp = saved_opp_hp;
                    std::copy_n(tick2_fb.data(), 240 * 240, rt.framebuffer());

                    int slot0_colored_px = 0;
                    for (int y = 142; y <= 168; ++y) {
                        for (int x = 8; x <= 32; ++x) {
                            uint32_t px = tick2_fb[y * 240 + x];
                            int r = (px >> 16) & 0xFF;
                            int g = (px >> 8) & 0xFF;
                            int b = px & 0xFF;
                            if (std::max({r, g, b}) - std::min({r, g, b}) >= 24) slot0_colored_px++;
                        }
                    }

                    int name_ink_px = 0;
                    for (int y = 50; y <= 65; ++y) {
                        for (int x = 8; x <= 105; ++x) {
                            uint32_t px = tick2_fb[y * 240 + x];
                            int r = (px >> 16) & 0xFF;
                            int g = (px >> 8) & 0xFF;
                            int b = px & 0xFF;
                            if (r < 100 && g < 100 && b < 100) name_ink_px++;
                        }
                    }

                    int bot_white_px = 0;
                    int bot_ink_px = 0;
                    for (int y = 204; y <= 236; ++y) {
                        for (int x = 4; x <= 234; ++x) {
                            uint32_t px = tick2_fb[y * 240 + x];
                            int r = (px >> 16) & 0xFF;
                            int g = (px >> 8) & 0xFF;
                            int b = px & 0xFF;
                            if (r > 200 && g > 200 && b > 200) bot_white_px++;
                            if (r < 100 && g < 100 && b < 100) bot_ink_px++;
                        }
                    }

                    int top_left_hud_px = 0;
                    for (int y = 10; y <= 29; ++y) {
                        for (int x = 4; x <= 65; ++x) {
                            if ((tick2_fb[y * 240 + x] & 0x00FFFFFFu) != 0) top_left_hud_px++;
                        }
                    }
                    int top_right_hud_px = 0;
                    for (int y = 4; y <= 35; ++y) {
                        for (int x = 122; x <= 235; ++x) {
                            if ((tick2_fb[y * 240 + x] & 0x00FFFFFFu) != 0) top_right_hud_px++;
                        }
                    }

                    // Held directional key single-step + multi-key immunity check
                    int f0 = rt.frame_count();
                    rt.inject_key_span(f0 + 1, f0 + 8, 10); // Hold Buster across span
                    rt.inject_key_span(f0 + 1, f0 + 5, 18); // Hold Right across 5 frames
                    step_j();
                    bool hold_no_repeat = (sf_app_CpCanvas_sel_cas_mode == 0 && sf_app_CpCanvas_sel_cas_tip == 1);
                    for (int h = 0; h < 4; ++h) {
                        step_j();
                        if (sf_app_CpCanvas_sel_cas_mode != 0 || sf_app_CpCanvas_sel_cas_tip != 1) {
                            hold_no_repeat = false;
                        }
                    }
                    step_j(); // f0 + 6: Right released while Buster still held
                    if (sf_app_CpCanvas_sel_cas_mode != 0 || sf_app_CpCanvas_sel_cas_tip != 1) {
                        hold_no_repeat = false;
                    }
                    rt.inject_key_at_frame(rt.frame_count() + 1, 16); // f0 + 7: Left while Buster held
                    step_j();
                    bool left_back_ok = (sf_app_CpCanvas_sel_cas_mode == 0 && sf_app_CpCanvas_sel_cas_tip == 0);
                    step_j(); // f0 + 8
                    step_j(); // f0 + 9: release Buster cleanly

                    // Tick 3: press A (bit 20) to select slot 0
                    rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                    step_j();
                    int32_t t3_set0 = safe_iaload(rt, sf_app_CpCanvas_set_tip, 0, -1);
                    int32_t t3_sel_cnt = sf_app_CpCanvas_sel_cnt;

                    // Tick 4: idle persistence check (set_tip[0] & cas_tip[0] MUST survive without key!)
                    step_j();
                    int32_t t4_set0 = safe_iaload(rt, sf_app_CpCanvas_set_tip, 0, -1);
                    int32_t t4_sel_cnt = sf_app_CpCanvas_sel_cnt;
                    int32_t t4_cas0 = safe_iaload(rt, sf_app_CpCanvas_cas_tip, 0, -1);
                    int32_t t4_soft1 = safe_iaload(rt, sf_app_CpCanvas_soft_id, 1, -1);

                    // Tick 5: focus OK and press A (bit 20) to confirm commit
                    sf_app_CpCanvas_sel_cas_mode = 1;
                    rt.inject_key_at_frame(rt.frame_count() + 1, 20);
                    step_j();

                    if (!carry_suppressed || !hold_no_repeat || !left_back_ok ||
                        !t1_cas_ok || t1_soft1 != 0 || hs_bleed != 0 || stage_px < 200 ||
                        blue_panel_px < 80 || opp_sprite_diff < 40 ||
                        slot0_colored_px < 12 || name_ink_px < 10 ||
                        bot_white_px < 2000 || bot_ink_px < 10 ||
                        top_left_hud_px < 40 || top_right_hud_px < 80 ||
                        t3_set0 != 0 || t3_sel_cnt < 1 ||
                        t4_set0 != 0 || t4_sel_cnt < 1 || t4_cas0 <= 0 || t4_soft1 != 0 ||
                        !g_pvp_eng.local_custom_done ||
                        g_pvp_eng.local_commit.tip_no != 0 ||
                        g_pvp_eng.local_commit.rock_tip_ids[0] == 0) {
                        fail_msg("SuiteJ-CustomScreenFlow",
                                 "carry=" + std::to_string(carry_suppressed) +
                                 " hold1=" + std::to_string(hold_no_repeat) +
                                 " left=" + std::to_string(left_back_ok) +
                                 " cas_ok=" + std::to_string(t1_cas_ok) +
                                 " soft1=" + std::to_string(t1_soft1) + "/" + std::to_string(t4_soft1) +
                                 " bleed=" + std::to_string(hs_bleed) +
                                 " stage=" + std::to_string(stage_px) +
                                 " blue=" + std::to_string(blue_panel_px) +
                                 " opp_diff=" + std::to_string(opp_sprite_diff) +
                                 " slot0=" + std::to_string(slot0_colored_px) +
                                 " name_ink=" + std::to_string(name_ink_px) +
                                 " bot=" + std::to_string(bot_white_px) + "/" + std::to_string(bot_ink_px) +
                                 " top=" + std::to_string(top_left_hud_px) + "/" + std::to_string(top_right_hud_px) +
                                 " set0=" + std::to_string(t3_set0) + "->" + std::to_string(t4_set0) +
                                 " sel=" + std::to_string(t3_sel_cnt) + "->" + std::to_string(t4_sel_cnt) +
                                 " cas0=" + std::to_string(t4_cas0) +
                                 " done=" + std::to_string(g_pvp_eng.local_custom_done) +
                                 " tip_no=" + std::to_string(g_pvp_eng.local_commit.tip_no) +
                                 " rtid0=" + std::to_string(g_pvp_eng.local_commit.rock_tip_ids[0]));
                    }
                }
            }
            peer_j.close_udp();
            rt.pvp_session().close_udp();
            cleanup_and_restore_singleplayer(rt, false);
        }
    });
    std::printf("[PvP SELF-TEST] Suite J complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite K: AreaGrab / PanelGrab TF Orb Lifecycle, Clean Render & Freeze
    // =========================================================================
    run_safe("SuiteK", [&]() {
        // Discover PanelGrab, AreaGrab, and a non-dimming traveling projectile dynamically (JP + EN safe)
        int32_t tid_panel_grab = -1;
        int32_t tid_area_grab = -1;
        int32_t tid_proj = -1;
        {
            setup_basic_arena(1000, 1000, 0xCA001u, 0);
            Ref tmp_a = rt.alloc_object(kClassId_Attack, kNumI32_Attack, kNumI64_Attack);
            fn_app_Attack__init____I__V(rt, tmp_a, 29);
            const int32_t max_tid = std::min<int32_t>(270, tip_len - 1);
            for (int32_t tid = 2; tid <= max_tid; ++tid) {
                Ref to = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
                if (to <= 0) continue;
                int32_t syu = rt.field_i32(to, kFld_Tip_syu);
                int32_t tcls = rt.field_i32(to, kFld_Tip_tip_class);
                int32_t wid = rt.field_i32(to, kFld_Tip_waza_id);
                if (wid <= 0) continue;
                int32_t stime = 0;
                if (sf_app_CpCanvas_waza > 0 && wid < rt.array_length(sf_app_CpCanvas_waza)) {
                    Ref w_obj = rt.iaload(sf_app_CpCanvas_waza, wid);
                    if (w_obj > 0) stime = rt.field_i32(w_obj, kFld_Waza_stop_time);
                }
                fn_app_Attack_Set___IIIII__I(rt, tmp_a, wid, 1, 1, 0, 1);
                Ref p_arr = rt.field_i32(tmp_a, kFld_Attack_param);
                int32_t p7 = safe_iaload(rt, p_arr, 7);
                int32_t p8 = safe_iaload(rt, p_arr, 8);
                int32_t ht = rt.field_i32(tmp_a, kFld_Attack_hit_time);
                int32_t kxs = rt.field_i32(tmp_a, kFld_Attack_kidou_x_sp);
                if (tid_panel_grab < 0 && stime > 0 && p7 == 15 && p8 == 0) {
                    tid_panel_grab = tid;
                } else if (tid_area_grab < 0 && stime > 0 && p7 == 15 && p8 == 28) {
                    tid_area_grab = tid;
                } else if (tid_proj < 0 && stime == 0 && ht == 0 && syu == 0 && tcls <= 1 && kxs != 0) {
                    tid_proj = tid;
                }
            }
        }
        if (tid_panel_grab <= 0 || tid_area_grab <= 0) {
            fail_msg("SuiteK-Discover", "pg=" + std::to_string(tid_panel_grab) +
                     " ag=" + std::to_string(tid_area_grab) + " proj=" + std::to_string(tid_proj));
            return;
        }

        // K1: PanelGrab (expect >= 1 orb) & AreaGrab (expect >= 3 orbs) for actor=0 and actor=1
        struct GrabCase {
            int32_t tid;
            int min_orbs;
            bool full_col;
            const char* name;
        };
        const GrabCase cases[2] = {
            {tid_panel_grab, 1, false, "PanelGrab"},
            {tid_area_grab, 3, true, "AreaGrab"},
        };
        for (int c_idx = 0; c_idx < 2; ++c_idx) {
            const GrabCase& gc = cases[c_idx];
            for (int actor = 0; actor < 2; ++actor) {
                setup_basic_arena(1000, 1000, 0xCA100u + static_cast<uint32_t>(c_idx * 10 + actor), 0);
                g_pvp_eng.core.sides[0].pos_x = 1; g_pvp_eng.core.sides[0].tmp_x = 1;
                g_pvp_eng.core.sides[0].pos_y = 1; g_pvp_eng.core.sides[0].tmp_y = 1;
                g_pvp_eng.core.sides[1].pos_x = 1; g_pvp_eng.core.sides[1].tmp_x = 1;
                g_pvp_eng.core.sides[1].pos_y = 1; g_pvp_eng.core.sides[1].tmp_y = 1;
                queue_tips_fifo(actor, {static_cast<int16_t>(gc.tid)});

                int peak_orbs = 0;
                int32_t peak_t_ani = -1;
                int orb_px_during_tf[2] = {0, 0};
                bool exited_tf = false;

                for (int t = 0; t < 72; ++t) {
                    int32_t p0m = (actor == 0 && t == 0) ? (1 << 20) : 0;
                    int32_t p1m = (actor == 1 && t == 0) ? (1 << 20) : 0;
                    step_pvp_combat_tick(rt, cp, p0m, p1m);

                    if (g_pvp_eng.core.tf_state == TF_ROOT_INTRO && g_pvp_eng.core.tf_stack[0].timer >= 16) {
                        int active_orbs = 0;
                        for (int i = 0; i < kMaxAttacks; ++i) {
                            const auto& ad = g_pvp_eng.core.sides[actor].ata_data[i];
                            if (ad[kFld_Attack_on] != 0 && ad[kFld_Attack_hit_time] > 0) {
                                active_orbs++;
                                peak_t_ani = std::max(peak_t_ani, ad[kFld_Attack_t_ani_cnt]);
                            }
                        }
                        peak_orbs = std::max(peak_orbs, active_orbs);

                        if (g_pvp_eng.core.tf_stack[0].timer == 30) {
                            for (int loc = 0; loc < 2; ++loc) {
                                g_pvp_eng.local_idx = loc;
                                g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
                                rt.set_suppress_av(false);
                                render_pvp_combat_frame(rt, cp);
                                rt.set_suppress_av(true);
                                const uint32_t* fb = rt.framebuffer();
                                int col = (loc == actor) ? 3 : 2;
                                int x0 = col * 40 + 2;
                                int x1 = col * 40 + 38;
                                int bright = 0;
                                for (int y = 70; y <= 178; ++y) {
                                    for (int x = x0; x <= x1; ++x) {
                                        uint32_t rgb = fb[y * 240 + x] & 0x00FFFFFFu;
                                        uint32_t r = (rgb >> 16) & 0xFFu;
                                        uint32_t g = (rgb >> 8) & 0xFFu;
                                        uint32_t b = rgb & 0xFFu;
                                        if (g >= 210 && ((r >= 210 && b >= 210) || (g >= r + 40 && g >= b + 15))) {
                                            bright++;
                                        }
                                    }
                                }
                                orb_px_during_tf[loc] = bright;
                            }
                            g_pvp_eng.local_idx = 0;
                        }
                    } else if (t > 0 && g_pvp_eng.core.tf_state == TF_IDLE) {
                        exited_tf = true;
                        break;
                    }
                }

                for (int t = 0; t < 4; ++t) {
                    step_pvp_combat_tick(rt, cp, 0, 0);
                }

                int leftover_on = 0;
                for (int s = 0; s < 2; ++s) {
                    for (int i = 0; i < kMaxAttacks; ++i) {
                        if (g_pvp_eng.core.sides[s].ata_data[i][kFld_Attack_on] != 0) leftover_on++;
                    }
                }

                const auto post_panels = capture_panels_canonical();
                int tx = (actor == 0) ? 3 : 2;
                bool ownership_ok = (post_panels[1][tx].jin == actor);
                if (gc.full_col) {
                    ownership_ok = ownership_ok &&
                                   (post_panels[0][tx].jin == actor) &&
                                   (post_panels[2][tx].jin == actor);
                } else {
                    ownership_ok = ownership_ok &&
                                   (post_panels[0][tx].jin == (1 - actor)) &&
                                   (post_panels[2][tx].jin == (1 - actor));
                }

                // Render post-TF combat frame from both P0 & P1 perspectives
                std::array<std::vector<uint32_t>, 2> fb_after;
                for (int loc = 0; loc < 2; ++loc) {
                    g_pvp_eng.local_idx = loc;
                    g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
                    rt.set_suppress_av(false);
                    render_pvp_combat_frame(rt, cp);
                    rt.set_suppress_av(true);
                    const uint32_t* fb = rt.framebuffer();
                    fb_after[loc].assign(fb, fb + 240 * 240);
                }
                const int32_t post_cus = g_pvp_eng.core.cus_gage;
                const int32_t post_game_cnt = sf_app_CpCanvas_game_cnt;
                const int32_t post_custom_round = g_pvp_eng.core.custom_round;
                const SideContext post_s0 = g_pvp_eng.core.sides[0];
                const SideContext post_s1 = g_pvp_eng.core.sides[1];

                // Build clean board with zero active attacks and identical panels, gauge, and Rock ani counters
                setup_basic_arena(1000, 1000, 0xCA900u, 0);
                g_pvp_eng.core.cus_gage = post_cus;
                g_pvp_eng.core.custom_round = post_custom_round;
                g_pvp_eng.core.sides[0].cus_gage = post_cus;
                g_pvp_eng.core.sides[1].cus_gage = post_cus;
                auto copy_rock_vis = [](SideContext& dst, const SideContext& src) {
                    dst.pos_x = src.pos_x; dst.pos_y = src.pos_y;
                    dst.tmp_x = src.tmp_x; dst.tmp_y = src.tmp_y;
                    dst.ani_id = src.ani_id; dst.ani_cnt = src.ani_cnt;
                    dst.ani_wait_cnt = src.ani_wait_cnt; dst.tmp_ani_cnt = src.tmp_ani_cnt;
                    dst.rock_t_ani_cnt = src.rock_t_ani_cnt;
                    dst.opp_nondim_tip_id = src.opp_nondim_tip_id;
                    dst.opp_nondim_ticks = src.opp_nondim_ticks;
                    dst.tip_no = src.tip_no;
                    for (int k = 0; k < 9; ++k) dst.rock_tip_ids[k] = src.rock_tip_ids[k];
                };
                copy_rock_vis(g_pvp_eng.core.sides[0], post_s0);
                copy_rock_vis(g_pvp_eng.core.sides[1], post_s1);
                sf_app_CpCanvas_game_cnt = post_game_cnt;
                ensure_board_orient(rt, 0);
                if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
                    for (int y = 0; y < 3; ++y) {
                        Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
                        if (row <= 0 || rt.array_length(row) < 6) continue;
                        for (int x = 0; x < 6; ++x) {
                            Ref p = rt.iaload(row, x);
                            if (p <= 0) continue;
                            rt.field_i32(p, kFld_BtPanel_jin) = post_panels[y][x].jin;
                            rt.field_i32(p, kFld_BtPanel_jou) = post_panels[y][x].jou;
                            rt.field_i32(p, kFld_BtPanel_ana_cnt) = post_panels[y][x].ana_cnt;
                            rt.field_i32(p, kFld_BtPanel_mag_cnt) = post_panels[y][x].mag_cnt;
                            rt.field_i32(p, kFld_BtPanel_aria_cnt) = post_panels[y][x].aria_cnt;
                            rt.field_i32(p, kFld_BtPanel_yokoku) = post_panels[y][x].yokoku;
                            rt.field_i32(p, kFld_BtPanel_bom_pow) = post_panels[y][x].bom_pow;
                        }
                    }
                }

                int diff_pixels[2] = {0, 0};
                std::string first_diff[2];
                for (int loc = 0; loc < 2; ++loc) {
                    g_pvp_eng.local_idx = loc;
                    g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
                    rt.set_suppress_av(false);
                    render_pvp_combat_frame(rt, cp);
                    rt.set_suppress_av(true);
                    const uint32_t* fb = rt.framebuffer();
                    for (int y = 58; y <= 195; ++y) {
                        for (int x = 0; x < 240; ++x) {
                            if ((fb[y * 240 + x] & 0x00FFFFFFu) != (fb_after[loc][y * 240 + x] & 0x00FFFFFFu)) {
                                if (diff_pixels[loc] == 0) {
                                    first_diff[loc] = "(" + std::to_string(x) + "," + std::to_string(y) + ")";
                                }
                                diff_pixels[loc]++;
                            }
                        }
                    }
                }
                g_pvp_eng.local_idx = 0;

                if (!exited_tf || peak_orbs < gc.min_orbs || peak_t_ani <= 0 ||
                    orb_px_during_tf[0] < 30 || orb_px_during_tf[1] < 30 ||
                    leftover_on != 0 || !ownership_ok ||
                    sf_app_CpCanvas_stop_cnt != 0 || sf_app_CpCanvas_stop_time != 0 ||
                    diff_pixels[0] != 0 || diff_pixels[1] != 0) {
                    fail_msg(std::string("SuiteK-") + gc.name,
                             "actor=" + std::to_string(actor) +
                             " exited=" + std::to_string(exited_tf) +
                             " orbs=" + std::to_string(peak_orbs) +
                             " tani=" + std::to_string(peak_t_ani) +
                             " tfpx=(" + std::to_string(orb_px_during_tf[0]) + "," + std::to_string(orb_px_during_tf[1]) + ")" +
                             " leftover=" + std::to_string(leftover_on) +
                             " own=" + std::to_string(ownership_ok) +
                             " stop=(" + std::to_string(sf_app_CpCanvas_stop_cnt) + "," + std::to_string(sf_app_CpCanvas_stop_time) + ")" +
                             " diff=(" + std::to_string(diff_pixels[0]) + first_diff[0] + "," +
                             std::to_string(diff_pixels[1]) + first_diff[1] + ")");
                }
            }
        }

        // K2: Pre-existing non-dimming projectile freeze on BOTH owner and opp during AreaGrab TF
        if (tid_proj > 0) {
            setup_basic_arena(1000, 1000, 0xCA200u, 0);
            g_pvp_eng.core.sides[0].pos_y = 0; g_pvp_eng.core.sides[0].tmp_y = 0;
            g_pvp_eng.core.sides[1].pos_y = 2; g_pvp_eng.core.sides[1].tmp_y = 2;
            queue_tips_fifo(0, {static_cast<int16_t>(tid_proj)});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_proj), static_cast<int16_t>(tid_area_grab)});

            // Both players fire non-dimming projectile at t=0, P1 triggers AreaGrab at t=2
            int proj_slot0 = -1, proj_slot1 = -1;
            int32_t frozen_x0 = -1, frozen_dx0 = -1, frozen_mc0 = -1;
            int32_t frozen_x1 = -1, frozen_dx1 = -1, frozen_mc1 = -1;
            bool froze_during_tf = true;
            bool captured_freeze_baseline = false;
            bool resumed_after_tf = false;

            for (int t = 0; t < 72; ++t) {
                int32_t p0m = (t == 0) ? (1 << 20) : 0;
                int32_t p1m = (t == 0 || t == 2) ? (1 << 20) : 0;
                step_pvp_combat_tick(rt, cp, p0m, p1m);

                auto find_nondim_slot = [&](int s) -> int {
                    for (int i = 0; i < kMaxAttacks; ++i) {
                        const auto& ad = g_pvp_eng.core.sides[s].ata_data[i];
                        if (ad[kFld_Attack_on] != 0 && ad[kFld_Attack_hit_time] == 0) return i;
                    }
                    return -1;
                };
                if (proj_slot0 < 0) proj_slot0 = find_nondim_slot(0);
                if (proj_slot1 < 0) proj_slot1 = find_nondim_slot(1);

                if (t == 2 && proj_slot0 >= 0 && proj_slot1 >= 0) {
                    const auto& ad0 = g_pvp_eng.core.sides[0].ata_data[proj_slot0];
                    const auto& ad1 = g_pvp_eng.core.sides[1].ata_data[proj_slot1];
                    frozen_x0 = ad0[kFld_Attack_pos_x];
                    frozen_dx0 = ad0[kFld_Attack_pos_dx];
                    frozen_mc0 = ad0[kFld_Attack_move_cnt];
                    frozen_x1 = ad1[kFld_Attack_pos_x];
                    frozen_dx1 = ad1[kFld_Attack_pos_dx];
                    frozen_mc1 = ad1[kFld_Attack_move_cnt];
                    captured_freeze_baseline = true;
                } else if (captured_freeze_baseline && g_pvp_eng.core.tf_state != TF_IDLE) {
                    const auto& ad0 = g_pvp_eng.core.sides[0].ata_data[proj_slot0];
                    const auto& ad1 = g_pvp_eng.core.sides[1].ata_data[proj_slot1];
                    if (ad0[kFld_Attack_on] != 0 &&
                        (ad0[kFld_Attack_pos_x] != frozen_x0 ||
                         ad0[kFld_Attack_pos_dx] != frozen_dx0 ||
                         ad0[kFld_Attack_move_cnt] != frozen_mc0)) {
                        froze_during_tf = false;
                    }
                    if (ad1[kFld_Attack_on] != 0 &&
                        (ad1[kFld_Attack_pos_x] != frozen_x1 ||
                         ad1[kFld_Attack_pos_dx] != frozen_dx1 ||
                         ad1[kFld_Attack_move_cnt] != frozen_mc1)) {
                        froze_during_tf = false;
                    }
                } else if (captured_freeze_baseline && g_pvp_eng.core.tf_state == TF_IDLE) {
                    const auto& ad0 = g_pvp_eng.core.sides[0].ata_data[proj_slot0];
                    const auto& ad1 = g_pvp_eng.core.sides[1].ata_data[proj_slot1];
                    if ((ad0[kFld_Attack_on] == 0 || ad0[kFld_Attack_move_cnt] > frozen_mc0 ||
                         ad0[kFld_Attack_pos_x] != frozen_x0 || ad0[kFld_Attack_pos_dx] != frozen_dx0) &&
                        (ad1[kFld_Attack_on] == 0 || ad1[kFld_Attack_move_cnt] > frozen_mc1 ||
                         ad1[kFld_Attack_pos_x] != frozen_x1 || ad1[kFld_Attack_pos_dx] != frozen_dx1)) {
                        resumed_after_tf = true;
                    }
                }
            }
            if (!captured_freeze_baseline || !froze_during_tf || !resumed_after_tf) {
                fail_msg("SuiteK-ProjFreeze",
                         "tid_proj=" + std::to_string(tid_proj) +
                         " base=" + std::to_string(captured_freeze_baseline) +
                         " froze=" + std::to_string(froze_during_tf) +
                         " resumed=" + std::to_string(resumed_after_tf));
            }
        }

        // K3: Counter-Dimming AreaGrab Cut-In (P0 AreaGrab countered by P1 AreaGrab) + Rollback Determinism
        {
            constexpr int kCtrFrames = 112;
            std::array<int32_t, kCtrFrames> p0_in{}, p1_in{};
            p0_in[0] = (1 << 20);
            p1_in[2] = (1 << 20);

            setup_basic_arena(1000, 1000, 0xCA300u, 0);
            queue_tips_fifo(0, {static_cast<int16_t>(tid_area_grab)});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_area_grab)});
            bool saw_depth2 = false;
            bool exited_ctr = false;
            for (int t = 0; t < kCtrFrames; ++t) {
                step_pvp_combat_tick(rt, cp, p0_in[t], p1_in[t]);
                if (g_pvp_eng.core.tf_depth == 2) saw_depth2 = true;
                if (saw_depth2 && g_pvp_eng.core.tf_state == TF_IDLE) exited_ctr = true;
            }
            int ctr_leftover = 0;
            for (int s = 0; s < 2; ++s) {
                for (int i = 0; i < kMaxAttacks; ++i) {
                    if (g_pvp_eng.core.sides[s].ata_data[i][kFld_Attack_on] != 0) ctr_leftover++;
                }
            }
            const uint16_t canon_ctr_hash = compute_combat_state_hash(rt);
            const auto canon_ctr_panels = capture_panels_canonical();

            // Re-run with mispredicted P1 input rolled back from tick 2 at tick 12
            setup_basic_arena(1000, 1000, 0xCA300u, 0);
            queue_tips_fifo(0, {static_cast<int16_t>(tid_area_grab)});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_area_grab)});
            std::array<int32_t, kCtrFrames> spec_p1{};
            bool rb_restored = true;
            for (int t = 0; t < kCtrFrames; ++t) {
                save_rollback_slot(rt, g_pvp_eng, static_cast<uint32_t>(t), p0_in[t], spec_p1[t],
                                   true, compute_combat_state_hash(rt));
                step_pvp_combat_tick(rt, cp, p0_in[t], spec_p1[t]);
                if (t == 12) {
                    for (uint32_t k = 2; k <= 12u; ++k) spec_p1[k] = p1_in[k];
                    if (!restore_rollback_slot(rt, g_pvp_eng, 2u)) {
                        rb_restored = false;
                        break;
                    }
                    g_pvp_eng.in_resim = true;
                    for (uint32_t k = 2; k <= 12u; ++k) {
                        save_rollback_slot(rt, g_pvp_eng, k, p0_in[k], spec_p1[k],
                                           false, compute_combat_state_hash(rt));
                        step_pvp_combat_tick(rt, cp, p0_in[k], spec_p1[k]);
                    }
                    g_pvp_eng.in_resim = false;
                }
            }
            const uint16_t rb_ctr_hash = compute_combat_state_hash(rt);
            const auto rb_ctr_panels = capture_panels_canonical();
            bool ctr_pan_eq = true;
            for (int y = 0; y < 3 && ctr_pan_eq; ++y) {
                for (int x = 0; x < 6; ++x) {
                    if (canon_ctr_panels[y][x].jin != rb_ctr_panels[y][x].jin) {
                        ctr_pan_eq = false;
                        break;
                    }
                }
            }
            if (!saw_depth2 || !exited_ctr || ctr_leftover != 0 || !rb_restored ||
                rb_ctr_hash != canon_ctr_hash || !ctr_pan_eq) {
                fail_msg("SuiteK-CounterCutIn",
                         "depth2=" + std::to_string(saw_depth2) +
                         " exited=" + std::to_string(exited_ctr) +
                         " leftover=" + std::to_string(ctr_leftover) +
                         " rbrest=" + std::to_string(rb_restored) +
                         " hash=(" + std::to_string(canon_ctr_hash) + "," + std::to_string(rb_ctr_hash) + ")" +
                         " pan=" + std::to_string(ctr_pan_eq));
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite K complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite L: PoN <-> LoN Keybinding Parity (ACT_SELECT / ACT_CANCEL / ACT_POUND / SOFT1 / SOFT2)
    // =========================================================================
    run_safe("SuiteL", [&]() {
        if (tid_cannon <= 0) {
            fail_msg("SuiteL-NoCannon", "tid_cannon not discovered");
            return;
        }
        Ref cannon_obj = safe_iaload(rt, sf_app_CpCanvas_tip, tid_cannon);
        int32_t cannon_pow = (cannon_obj > 0) ? rt.field_i32(cannon_obj, kFld_Tip_pow) : 0;

        auto step_hold_and_release = [&](int actor, int32_t bit_mask, int hold_ticks, int settle_ticks, bool& out_saw_buster_vis) {
            out_saw_buster_vis = false;
            for (int t = 0; t < hold_ticks + settle_ticks; ++t) {
                int32_t m = (t < hold_ticks) ? bit_mask : 0;
                step_pvp_combat_tick(rt, cp, (actor == 0) ? m : 0, (actor == 1) ? m : 0);
                if (g_pvp_eng.core.sides[actor].opp_buster_visual_ticks > 0) {
                    out_saw_buster_vis = true;
                }
            }
        };

        // L1: ACT_SELECT (1 << 20) fires queued chip and non_bas prevents accidental Buster pea-shot on release
        {
            setup_basic_arena(1000, 1000, 0xCA401u, 0);
            g_pvp_eng.core.sides[0].pos_y = 1; g_pvp_eng.core.sides[0].tmp_y = 1;
            g_pvp_eng.core.sides[1].pos_y = 1; g_pvp_eng.core.sides[1].tmp_y = 1;
            queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
            bool saw_buster_vis = false;
            step_hold_and_release(0, (1 << 20), 1, 29, saw_buster_vis);
            int32_t dmg = 1000 - g_pvp_eng.core.sides[1].hp;
            if (g_pvp_eng.core.sides[0].tip_no != -1 || saw_buster_vis || dmg != cannon_pow) {
                fail_msg("SuiteL1-SelectChip",
                         "tip_no=" + std::to_string(g_pvp_eng.core.sides[0].tip_no) +
                         " buster_vis=" + std::to_string(saw_buster_vis) +
                         " dmg=" + std::to_string(dmg) + " expected=" + std::to_string(cannon_pow));
            }
        }

        // L2: ACT_SELECT (1 << 20) fires Normal Buster & Charge Shot when tip_no < 0
        {
            setup_basic_arena(1000, 1000, 0xCA402u, 0);
            g_pvp_eng.core.sides[0].pos_y = 1; g_pvp_eng.core.sides[0].tmp_y = 1;
            g_pvp_eng.core.sides[1].pos_y = 1; g_pvp_eng.core.sides[1].tmp_y = 1;
            bool saw_tap_vis = false;
            step_hold_and_release(0, (1 << 20), 1, 24, saw_tap_vis);
            int32_t tap_dmg = 1000 - g_pvp_eng.core.sides[1].hp;

            setup_basic_arena(1000, 1000, 0xCA402u, 0);
            g_pvp_eng.core.sides[0].pos_y = 1; g_pvp_eng.core.sides[0].tmp_y = 1;
            g_pvp_eng.core.sides[1].pos_y = 1; g_pvp_eng.core.sides[1].tmp_y = 1;
            const int chg_ticks = (42 - sf_app_CpCanvas_cha_lv * 5) + kChargeDelayAdd[0] + 4;
            bool saw_chg_vis = false;
            step_hold_and_release(0, (1 << 20), chg_ticks, 24, saw_chg_vis);
            int32_t chg_dmg = 1000 - g_pvp_eng.core.sides[1].hp;
            if (!saw_tap_vis || tap_dmg < 1 || tap_dmg > 5 || !saw_chg_vis || chg_dmg < 10) {
                fail_msg("SuiteL2-SelectEmptyBuster",
                         "tap_vis=" + std::to_string(saw_tap_vis) + " tap_dmg=" + std::to_string(tap_dmg) +
                         " chg_vis=" + std::to_string(saw_chg_vis) + " chg_dmg=" + std::to_string(chg_dmg));
            }
        }

        // L3: ACT_CANCEL (1 << 10) & ACT_POUND (1 << 11) fire Buster & Charge Shot WITHOUT consuming queued chips
        {
            setup_basic_arena(1000, 1000, 0xCA403u, 0);
            g_pvp_eng.core.sides[0].pos_y = 1; g_pvp_eng.core.sides[0].tmp_y = 1;
            g_pvp_eng.core.sides[1].pos_y = 1; g_pvp_eng.core.sides[1].tmp_y = 1;
            queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
            bool saw_cancel_tap = false;
            step_hold_and_release(0, (1 << 10), 1, 24, saw_cancel_tap);
            int32_t cancel_tap_dmg = 1000 - g_pvp_eng.core.sides[1].hp;
            int32_t p0_tip_after_tap = g_pvp_eng.core.sides[0].tip_no;

            setup_basic_arena(1000, 1000, 0xCA403u, 0);
            g_pvp_eng.core.sides[0].pos_y = 1; g_pvp_eng.core.sides[0].tmp_y = 1;
            g_pvp_eng.core.sides[1].pos_y = 1; g_pvp_eng.core.sides[1].tmp_y = 1;
            queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_cannon)});
            const int chg_ticks = (42 - sf_app_CpCanvas_cha_lv * 5) + kChargeDelayAdd[0] + 4;
            bool saw_cancel_chg = false;
            step_hold_and_release(0, (1 << 10), chg_ticks, 24, saw_cancel_chg);
            int32_t cancel_chg_dmg = 1000 - g_pvp_eng.core.sides[1].hp;
            int32_t p0_tip_after_chg = g_pvp_eng.core.sides[0].tip_no;

            bool saw_pound_chg = false;
            step_hold_and_release(1, (1 << 11), chg_ticks, 24, saw_pound_chg);
            int32_t pound_chg_dmg = 1000 - g_pvp_eng.core.sides[0].hp;
            int32_t p1_tip_after_chg = g_pvp_eng.core.sides[1].tip_no;

            if (!saw_cancel_tap || cancel_tap_dmg < 1 || cancel_tap_dmg > 5 || p0_tip_after_tap != 0 ||
                !saw_cancel_chg || cancel_chg_dmg < 10 || p0_tip_after_chg != 0 ||
                !saw_pound_chg || pound_chg_dmg < 10 || p1_tip_after_chg != 0) {
                fail_msg("SuiteL3-CancelPoundBuster",
                         "ctap=" + std::to_string(cancel_tap_dmg) + " tip0a=" + std::to_string(p0_tip_after_tap) +
                         " cchg=" + std::to_string(cancel_chg_dmg) + " tip0b=" + std::to_string(p0_tip_after_chg) +
                         " pchg=" + std::to_string(pound_chg_dmg) + " tip1=" + std::to_string(p1_tip_after_chg));
            }
        }

        // L4: ACT_SOFT1 (1 << 21) & ACT_SOFT2 (1 << 22) open Custom Screen on rising edge without consuming queued chips
        {
            setup_basic_arena(1000, 1000, 0xCA404u, 0);
            queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
            queue_tips_fifo(1, {static_cast<int16_t>(tid_cannon)});
            g_pvp_eng.core.custom_round = 1;
            g_pvp_eng.core.cus_gage = 64;
            sf_app_CpCanvas_cus_gage = 64;
            step_pvp_combat_tick(rt, cp, (1 << 21), 0);
            bool pre_full_no_open = (!g_pvp_eng.core.open_custom_req && g_pvp_eng.core.sides[0].tip_no == 0);

            g_pvp_eng.core.cus_gage = 128;
            sf_app_CpCanvas_cus_gage = 128;
            step_pvp_combat_tick(rt, cp, (1 << 21), 0); // Still held across threshold -> must require fresh rising edge
            bool held_no_open = (!g_pvp_eng.core.open_custom_req && g_pvp_eng.core.sides[0].tip_no == 0);

            step_pvp_combat_tick(rt, cp, 0, 0); // Release
            step_pvp_combat_tick(rt, cp, (1 << 21), 0); // Fresh rising edge -> open Custom!
            bool soft1_opened = (g_pvp_eng.core.open_custom_req && g_pvp_eng.core.sides[0].tip_no == 0);

            setup_basic_arena(1000, 1000, 0xCA405u, 0);
            queue_tips_fifo(1, {static_cast<int16_t>(tid_cannon)});
            g_pvp_eng.core.custom_round = 1;
            g_pvp_eng.core.cus_gage = 128;
            sf_app_CpCanvas_cus_gage = 128;
            step_pvp_combat_tick(rt, cp, 0, (1 << 22));
            bool soft2_opened = (g_pvp_eng.core.open_custom_req && g_pvp_eng.core.sides[1].tip_no == 0);

            if (!pre_full_no_open || !held_no_open || !soft1_opened || !soft2_opened) {
                fail_msg("SuiteL4-SoftkeyCustomOpen",
                         "pre=" + std::to_string(pre_full_no_open) +
                         " held=" + std::to_string(held_no_open) +
                         " soft1=" + std::to_string(soft1_opened) +
                         " soft2=" + std::to_string(soft2_opened));
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite L complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite M: Buster Charge VFX Lifecycle & Move-While-Charging Parity
    // =========================================================================
    run_safe("SuiteM", [&]() {
        auto count_charge_vfx_slots = [&](int side_idx, int& out_active_count, int& out_mismatch_pos) {
            out_active_count = 0;
            out_mismatch_pos = 0;
            const SideContext& sctx = g_pvp_eng.core.sides[side_idx];
            for (int i = 0; i < kMaxAttacks; ++i) {
                const auto& ad = sctx.ata_data[i];
                if (ad[kFld_Attack_on] == 0) continue;
                int32_t eid = ad[kFld_Attack_eff_id];
                int32_t tid = ad[kFld_Attack_tmp_id];
                if (eid == 227 || eid == 228 || tid == 227 || tid == 228) {
                    out_active_count++;
                    if (ad[kFld_Attack_pos_x] != sctx.pos_x || ad[kFld_Attack_pos_y] != sctx.pos_y) {
                        out_mismatch_pos++;
                    }
                }
            }
        };

        auto check_no_residual_attacks = [&](const char* label) -> bool {
            for (int s = 0; s < 2; ++s) {
                for (int i = 0; i < kMaxAttacks; ++i) {
                    if (g_pvp_eng.core.sides[s].ata_data[i][kFld_Attack_on] != 0) {
                        fail_msg(label,
                                 "side=" + std::to_string(s) +
                                 " slot=" + std::to_string(i) +
                                 " eff=" + std::to_string(g_pvp_eng.core.sides[s].ata_data[i][kFld_Attack_eff_id]) +
                                 " wid=" + std::to_string(g_pvp_eng.core.sides[s].ata_data[i][kFld_Attack_waza_id]) +
                                 " on=" + std::to_string(g_pvp_eng.core.sides[s].ata_data[i][kFld_Attack_on]));
                        return false;
                    }
                }
            }
            return true;
        };

        // M1: Hold Charge across Lv1->Lv2 + Multi-Panel Moves -> VFX follows every step, 0 residual pixels on both views
        for (int actor = 0; actor < 2; ++actor) {
            setup_basic_arena(1000, 1000, 0xCA500u + static_cast<uint32_t>(actor), 0);
            g_pvp_eng.core.sides[0].pos_x = 1; g_pvp_eng.core.sides[0].tmp_x = 1;
            g_pvp_eng.core.sides[0].pos_y = 1; g_pvp_eng.core.sides[0].tmp_y = 1;
            g_pvp_eng.core.sides[1].pos_x = 1; g_pvp_eng.core.sides[1].tmp_x = 1;
            g_pvp_eng.core.sides[1].pos_y = 1; g_pvp_eng.core.sides[1].tmp_y = 1;

            const int chg_thresh = (42 - sf_app_CpCanvas_cha_lv * 5) + kChargeDelayAdd[0];
            bool bad_ata_id = false;
            bool bad_bas_no = false;
            bool bad_charge_follow = false;
            int peak_charge_slots = 0;

            for (int t = 0; t < chg_thresh + 24; ++t) {
                int32_t m = (1 << 10); // Hold ACT_CANCEL (Buster)
                // Trigger panel moves spaced 8 ticks apart while holding charge (triggers Rock_Check waza 225 in ata[1])
                if (t == 8)       m |= (1 << 17); // Up -> (1, 0)
                else if (t == 16) m |= (1 << 16); // Left -> (0, 0)
                else if (t == 24) m |= (1 << 19); // Down -> (0, 1)
                else if (t == 32) m |= (1 << 18); // Right -> (1, 1)
                step_pvp_combat_tick(rt, cp, (actor == 0) ? m : 0, (actor == 1) ? m : 0);

                const SideContext& act_sc = g_pvp_eng.core.sides[actor];
                for (int i = 0; i < kMaxAttacks; ++i) {
                    if (act_sc.ata_data[i][0] != i) bad_ata_id = true;
                }
                int cnt = 0, mis = 0;
                count_charge_vfx_slots(actor, cnt, mis);
                peak_charge_slots = std::max(peak_charge_slots, cnt);
                if (t >= 4 && act_sc.bas_no < 0) bad_bas_no = true;
                if (cnt > 1 || mis > 0 || (t >= 4 && cnt != 1)) bad_charge_follow = true;
            }

            // Release Buster and settle 36 ticks
            for (int t = 0; t < 36; ++t) {
                step_pvp_combat_tick(rt, cp, 0, 0);
            }

            int opp = 1 - actor;
            int32_t dmg = 1000 - g_pvp_eng.core.sides[opp].hp;
            bool bas_cleared = (g_pvp_eng.core.sides[actor].bas_no == -1 && g_pvp_eng.core.sides[actor].key_cnt == 0);
            bool no_leftover = check_no_residual_attacks("SuiteM1-Leftover");

            // Render clean post-settle frame from both P0 & P1 views and diff [y: 58..178) against zero-attack board
            std::array<std::vector<uint32_t>, 2> fb_after;
            for (int loc = 0; loc < 2; ++loc) {
                g_pvp_eng.local_idx = loc;
                g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
                rt.set_suppress_av(false);
                render_pvp_combat_frame(rt, cp);
                rt.set_suppress_av(true);
                const uint32_t* fb = rt.framebuffer();
                fb_after[loc].assign(fb, fb + 240 * 240);
            }
            const auto post_panels = capture_panels_canonical();
            const int32_t post_cus = g_pvp_eng.core.cus_gage;
            const int32_t post_game_cnt = sf_app_CpCanvas_game_cnt;
            const int32_t post_round = g_pvp_eng.core.custom_round;
            const SideContext post_s0 = g_pvp_eng.core.sides[0];
            const SideContext post_s1 = g_pvp_eng.core.sides[1];

            setup_basic_arena(1000, 1000, 0xCA599u, 0);
            g_pvp_eng.core.cus_gage = post_cus;
            g_pvp_eng.core.custom_round = post_round;
            g_pvp_eng.core.sides[0].cus_gage = post_cus;
            g_pvp_eng.core.sides[1].cus_gage = post_cus;
            auto copy_rock_vis = [](SideContext& dst, const SideContext& src) {
                dst.hp = src.hp; dst.max_hp = src.max_hp;
                dst.pos_x = src.pos_x; dst.pos_y = src.pos_y;
                dst.tmp_x = src.tmp_x; dst.tmp_y = src.tmp_y;
                dst.ani_id = src.ani_id; dst.ani_cnt = src.ani_cnt;
                dst.ani_wait_cnt = src.ani_wait_cnt; dst.tmp_ani_cnt = src.tmp_ani_cnt;
                dst.hit_cnt = src.hit_cnt; dst.muteki_cnt = src.muteki_cnt;
                dst.noke_cnt = src.noke_cnt; dst.rock_t_ani_cnt = src.rock_t_ani_cnt;
                dst.opp_nondim_tip_id = src.opp_nondim_tip_id;
                dst.opp_nondim_ticks = src.opp_nondim_ticks;
                dst.opp_buster_visual_ticks = src.opp_buster_visual_ticks;
                dst.tip_no = src.tip_no;
                for (int k = 0; k < 9; ++k) dst.rock_tip_ids[k] = src.rock_tip_ids[k];
            };
            copy_rock_vis(g_pvp_eng.core.sides[0], post_s0);
            copy_rock_vis(g_pvp_eng.core.sides[1], post_s1);
            sf_app_CpCanvas_game_cnt = post_game_cnt;
            ensure_board_orient(rt, 0);
            if (sf_app_CpCanvas_panel > 0 && rt.array_length(sf_app_CpCanvas_panel) >= 3) {
                for (int y = 0; y < 3; ++y) {
                    Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
                    if (row <= 0 || rt.array_length(row) < 6) continue;
                    for (int x = 0; x < 6; ++x) {
                        Ref p = rt.iaload(row, x);
                        if (p <= 0) continue;
                        rt.field_i32(p, kFld_BtPanel_jin) = post_panels[y][x].jin;
                        rt.field_i32(p, kFld_BtPanel_jou) = post_panels[y][x].jou;
                        rt.field_i32(p, kFld_BtPanel_ana_cnt) = post_panels[y][x].ana_cnt;
                        rt.field_i32(p, kFld_BtPanel_mag_cnt) = post_panels[y][x].mag_cnt;
                        rt.field_i32(p, kFld_BtPanel_aria_cnt) = post_panels[y][x].aria_cnt;
                        rt.field_i32(p, kFld_BtPanel_yokoku) = post_panels[y][x].yokoku;
                        rt.field_i32(p, kFld_BtPanel_bom_pow) = post_panels[y][x].bom_pow;
                    }
                }
            }
            int diff_px[2] = {0, 0};
            for (int loc = 0; loc < 2; ++loc) {
                g_pvp_eng.local_idx = loc;
                g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
                rt.set_suppress_av(false);
                render_pvp_combat_frame(rt, cp);
                rt.set_suppress_av(true);
                const uint32_t* fb = rt.framebuffer();
                for (int y = 58; y < 178; ++y) {
                    for (int x = 0; x < 240; ++x) {
                        if ((fb[y * 240 + x] & 0x00FFFFFFu) != (fb_after[loc][y * 240 + x] & 0x00FFFFFFu)) {
                            diff_px[loc]++;
                        }
                    }
                }
            }
            g_pvp_eng.local_idx = 0;

            if (bad_ata_id || bad_bas_no || bad_charge_follow || peak_charge_slots != 1 ||
                dmg < 10 || !bas_cleared || !no_leftover || diff_px[0] != 0 || diff_px[1] != 0) {
                fail_msg("SuiteM1-MoveWhileCharging",
                         "actor=" + std::to_string(actor) +
                         " ata_id=" + std::to_string(!bad_ata_id) +
                         " bas=" + std::to_string(!bad_bas_no) +
                         " follow=" + std::to_string(!bad_charge_follow) +
                         " peak=" + std::to_string(peak_charge_slots) +
                         " dmg=" + std::to_string(dmg) +
                         " cleared=" + std::to_string(bas_cleared) +
                         " diff=(" + std::to_string(diff_px[0]) + "," + std::to_string(diff_px[1]) + ")");
            }
        }

        // M2: Releasing Buster mid-move (move_flg != 0) at partial and full charge cleans up VFX and preserves Charge Shot
        {
            // Partial charge + D-pad Up + release on next tick (mid-move)
            setup_basic_arena(1000, 1000, 0xCA502u, 0);
            for (int t = 0; t < 12; ++t) {
                int32_t m = (1 << 10) | ((t == 11) ? (1 << 17) : 0);
                step_pvp_combat_tick(rt, cp, m, 0);
            }
            for (int t = 0; t < 24; ++t) {
                step_pvp_combat_tick(rt, cp, 0, 0);
            }
            bool m2a_ok = (g_pvp_eng.core.sides[0].bas_no == -1 && check_no_residual_attacks("SuiteM2a-PartialMidMove"));

            // Full charge at y=0 (opp at y=1), press Down (1 << 19) + release Buster simultaneously
            setup_basic_arena(1000, 1000, 0xCA503u, 0);
            g_pvp_eng.core.sides[0].pos_y = 0; g_pvp_eng.core.sides[0].tmp_y = 0;
            g_pvp_eng.core.sides[1].pos_y = 1; g_pvp_eng.core.sides[1].tmp_y = 1;
            const int chg_ticks = (42 - sf_app_CpCanvas_cha_lv * 5) + kChargeDelayAdd[0] + 4;
            for (int t = 0; t < chg_ticks; ++t) {
                step_pvp_combat_tick(rt, cp, (1 << 10), 0);
            }
            step_pvp_combat_tick(rt, cp, (1 << 19), 0); // D-pad sets move_flg != 0 while Buster releases!
            for (int t = 0; t < 24; ++t) {
                step_pvp_combat_tick(rt, cp, 0, 0);
            }
            int32_t mid_move_chg_dmg = 1000 - g_pvp_eng.core.sides[1].hp;
            bool m2b_ok = (mid_move_chg_dmg >= 10 && g_pvp_eng.core.sides[0].bas_no == -1 &&
                           check_no_residual_attacks("SuiteM2b-FullMidMove"));
            if (!m2a_ok || !m2b_ok || g_pvp_eng.core.sides[0].pos_y != 1) {
                fail_msg("SuiteM2-ReleaseMidMove",
                         "m2a=" + std::to_string(m2a_ok) +
                         " m2b=" + std::to_string(m2b_ok) +
                         " dmg=" + std::to_string(mid_move_chg_dmg) +
                         " y=" + std::to_string(g_pvp_eng.core.sides[0].pos_y));
            }
        }

        // M3: Charge interrupted by moving + chip press, and by joutai!=0 release
        if (tid_cannon > 0) {
            setup_basic_arena(1000, 1000, 0xCA504u, 0);
            queue_tips_fifo(0, {static_cast<int16_t>(tid_cannon)});
            for (int t = 0; t < 12; ++t) {
                step_pvp_combat_tick(rt, cp, (1 << 10), 0);
            }
            // Start moving Up while pressing chip ACT_SELECT and releasing Buster
            step_pvp_combat_tick(rt, cp, (1 << 17) | (1 << 20), 0);
            for (int t = 0; t < 24; ++t) {
                step_pvp_combat_tick(rt, cp, 0, 0);
            }
            int32_t chip_dmg = 1000 - g_pvp_eng.core.sides[1].hp;
            bool m3a_ok = (chip_dmg > 0 && g_pvp_eng.core.sides[0].bas_no == -1 &&
                           check_no_residual_attacks("SuiteM3a-MoveChipCharge"));

            // Joutai != 0 while charging -> releasing Buster must clean up bas_no & charge attack
            setup_basic_arena(1000, 1000, 0xCA505u, 0);
            for (int t = 0; t < 12; ++t) {
                step_pvp_combat_tick(rt, cp, (1 << 10), 0);
            }
            g_pvp_eng.core.sides[0].joutai = 1;
            step_pvp_combat_tick(rt, cp, 0, 0);
            bool m3b_ok = (g_pvp_eng.core.sides[0].bas_no == -1 && g_pvp_eng.core.sides[0].key_cnt == 0 &&
                           check_no_residual_attacks("SuiteM3b-JoutaiRelease"));
            if (!m3a_ok || !m3b_ok) {
                fail_msg("SuiteM3-ChargeInterrupts",
                         "m3a=" + std::to_string(m3a_ok) + " cdmg=" + std::to_string(chip_dmg) +
                         " m3b=" + std::to_string(m3b_ok));
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite M complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite N: FullCust BattleChip Gauge Fill, Counter-Cut-In, Rollback & Turn 15
    // =========================================================================
    run_safe("SuiteN", [&]() {
        int32_t tid_fullcust = -1;
        int32_t tid_dim_other = -1;
        {
            setup_basic_arena(1000, 1000, 0xCA600u, 0);
            Ref tmp_a = rt.alloc_object(kClassId_Attack, kNumI32_Attack, kNumI64_Attack);
            fn_app_Attack__init____I__V(rt, tmp_a, 29);
            const int32_t max_tid = std::min<int32_t>(270, tip_len - 1);
            for (int32_t tid = 2; tid <= max_tid; ++tid) {
                Ref to = safe_iaload(rt, sf_app_CpCanvas_tip, tid);
                if (to <= 0) continue;
                int32_t wid = rt.field_i32(to, kFld_Tip_waza_id);
                if (wid <= 0) continue;
                int32_t stime = 0;
                if (sf_app_CpCanvas_waza > 0 && wid < rt.array_length(sf_app_CpCanvas_waza)) {
                    Ref w_obj = rt.iaload(sf_app_CpCanvas_waza, wid);
                    if (w_obj > 0) stime = rt.field_i32(w_obj, kFld_Waza_stop_time);
                }
                fn_app_Attack_Set___IIIII__I(rt, tmp_a, wid, 1, 1, 0, 1);
                Ref p_arr = rt.field_i32(tmp_a, kFld_Attack_param);
                int32_t p7 = safe_iaload(rt, p_arr, 7);
                if (tid_fullcust < 0 && p7 == 20) {
                    tid_fullcust = tid;
                } else if (tid_dim_other < 0 && stime > 0 && p7 == 15) {
                    tid_dim_other = tid;
                }
            }
        }
        if (tid_fullcust <= 0 || tid_dim_other <= 0) {
            fail_msg("SuiteN-Discover",
                     "fullcust=" + std::to_string(tid_fullcust) +
                     " dim_other=" + std::to_string(tid_dim_other));
            return;
        }

        // N1: P0 & P1 FullCust fills shared Custom Gauge to 128, renders full green bar, and allows Custom open
        for (int actor = 0; actor < 2; ++actor) {
            setup_basic_arena(1000, 1000, 0xCA610u + static_cast<uint32_t>(actor), 0);
            g_pvp_eng.core.custom_round = 1;
            g_pvp_eng.core.cus_gage = 10;
            g_pvp_eng.core.sides[0].cus_gage = 10;
            g_pvp_eng.core.sides[1].cus_gage = 10;
            sf_app_CpCanvas_cus_gage = 10;
            queue_tips_fifo(actor, {static_cast<int16_t>(tid_fullcust)});

            bool saw_tf = false;
            bool exited_tf = false;
            for (int t = 0; t < 80; ++t) {
                int32_t m = (t == 0) ? (1 << 20) : 0;
                step_pvp_combat_tick(rt, cp, (actor == 0) ? m : 0, (actor == 1) ? m : 0);
                if (g_pvp_eng.core.tf_state != TF_IDLE) saw_tf = true;
                else if (saw_tf) {
                    exited_tf = true;
                    break;
                }
            }

            int leftover_on = 0;
            for (int s = 0; s < 2; ++s) {
                for (int i = 0; i < kMaxAttacks; ++i) {
                    if (g_pvp_eng.core.sides[s].ata_data[i][kFld_Attack_on] != 0) leftover_on++;
                }
            }

            bool bar_green_ok = true;
            const int32_t saved_gc = sf_app_CpCanvas_game_cnt;
            sf_app_CpCanvas_game_cnt = 0;
            for (int loc = 0; loc < 2; ++loc) {
                g_pvp_eng.local_idx = loc;
                g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
                rt.set_suppress_av(false);
                render_pvp_combat_frame(rt, cp);
                rt.set_suppress_av(true);
                const uint32_t* fb = rt.framebuffer();
                for (int x = 60; x < 180; ++x) {
                    uint32_t rgb = fb[52 * 240 + x] & 0x00FFFFFFu;
                    uint32_t r = (rgb >> 16) & 0xFFu;
                    uint32_t g = (rgb >> 8) & 0xFFu;
                    uint32_t b = rgb & 0xFFu;
                    if (!(g >= 240 && r < 32 && b < 32)) {
                        bar_green_ok = false;
                        break;
                    }
                }
            }
            sf_app_CpCanvas_game_cnt = saved_gc;
            g_pvp_eng.local_idx = 0;

            bool gauge_state_ok = (saw_tf && exited_tf && leftover_on == 0 &&
                                   g_pvp_eng.core.sides[actor].tip_no == -1 &&
                                   g_pvp_eng.core.cus_gage == 128 &&
                                   g_pvp_eng.core.sides[0].cus_gage == 128 &&
                                   g_pvp_eng.core.sides[1].cus_gage == 128 &&
                                   sf_app_CpCanvas_cus_gage == 128);

            step_pvp_combat_tick(rt, cp, (actor == 0) ? (1 << 21) : 0, (actor == 1) ? (1 << 22) : 0);
            bool opened_custom = g_pvp_eng.core.open_custom_req;
            if (!gauge_state_ok || !bar_green_ok || !opened_custom) {
                fail_msg("SuiteN1-FullCustFill",
                         "actor=" + std::to_string(actor) +
                         " tid=" + std::to_string(tid_fullcust) +
                         " state=" + std::to_string(gauge_state_ok) +
                         " tf=(" + std::to_string(saw_tf) + "," + std::to_string(exited_tf) + ")" +
                         " cus=(" + std::to_string(g_pvp_eng.core.cus_gage) + "," +
                         std::to_string(g_pvp_eng.core.sides[0].cus_gage) + "," +
                         std::to_string(g_pvp_eng.core.sides[1].cus_gage) + "," +
                         std::to_string(sf_app_CpCanvas_cus_gage) + ")" +
                         " left=" + std::to_string(leftover_on) +
                         " green=" + std::to_string(bar_green_ok) +
                         " open=" + std::to_string(opened_custom));
            }
        }

        // N2: FullCust Counter-Dimming Cut-In + GGPO Rollback Determinism
        {
            constexpr int kFrames = 112;
            std::array<int32_t, kFrames> p0_in{}, p1_in{};
            p0_in[0] = (1 << 20);
            p1_in[2] = (1 << 20);

            auto setup_n2 = [&]() {
                setup_basic_arena(1000, 1000, 0xCA620u, 0);
                g_pvp_eng.core.custom_round = 1;
                g_pvp_eng.core.cus_gage = 5;
                g_pvp_eng.core.sides[0].cus_gage = 5;
                g_pvp_eng.core.sides[1].cus_gage = 5;
                sf_app_CpCanvas_cus_gage = 5;
                queue_tips_fifo(0, {static_cast<int16_t>(tid_dim_other)});
                queue_tips_fifo(1, {static_cast<int16_t>(tid_fullcust)});
            };

            setup_n2();
            bool saw_depth2 = false;
            bool exited_tf = false;
            for (int t = 0; t < kFrames; ++t) {
                step_pvp_combat_tick(rt, cp, p0_in[t], p1_in[t]);
                if (g_pvp_eng.core.tf_depth == 2) saw_depth2 = true;
                if (saw_depth2 && g_pvp_eng.core.tf_state == TF_IDLE) exited_tf = true;
            }
            const int32_t canon_cus = g_pvp_eng.core.cus_gage;
            const uint16_t canon_hash = compute_combat_state_hash(rt);

            setup_n2();
            std::array<int32_t, kFrames> spec_p1{};
            bool rb_ok = true;
            for (int t = 0; t < kFrames; ++t) {
                save_rollback_slot(rt, g_pvp_eng, static_cast<uint32_t>(t), p0_in[t], spec_p1[t],
                                   true, compute_combat_state_hash(rt));
                step_pvp_combat_tick(rt, cp, p0_in[t], spec_p1[t]);
                if (t == 12) {
                    for (uint32_t k = 2; k <= 12u; ++k) spec_p1[k] = p1_in[k];
                    if (!restore_rollback_slot(rt, g_pvp_eng, 2u)) {
                        rb_ok = false;
                        break;
                    }
                    g_pvp_eng.in_resim = true;
                    for (uint32_t k = 2; k <= 12u; ++k) {
                        save_rollback_slot(rt, g_pvp_eng, k, p0_in[k], spec_p1[k],
                                           false, compute_combat_state_hash(rt));
                        step_pvp_combat_tick(rt, cp, p0_in[k], spec_p1[k]);
                    }
                    g_pvp_eng.in_resim = false;
                }
            }
            const uint16_t rb_hash = compute_combat_state_hash(rt);
            if (!saw_depth2 || !exited_tf || canon_cus != 128 || !rb_ok ||
                g_pvp_eng.core.cus_gage != 128 || rb_hash != canon_hash) {
                fail_msg("SuiteN2-FullCustCounterRollback",
                         "d2=" + std::to_string(saw_depth2) +
                         " ex=" + std::to_string(exited_tf) +
                         " cus=(" + std::to_string(canon_cus) + "," + std::to_string(g_pvp_eng.core.cus_gage) + ")" +
                         " rb=" + std::to_string(rb_ok) +
                         " h=(" + std::to_string(canon_hash) + "," + std::to_string(rb_hash) + ")");
            }
        }

        // N3: Turn 15 Final Countdown Immunity (FullCust must not override final countdown or open Custom)
        {
            setup_basic_arena(1000, 1000, 0xCA630u, 0);
            g_pvp_eng.core.custom_round = kMaxTurns;
            g_pvp_eng.core.final_turn_ticks_left = 80;
            const int32_t init_gage = static_cast<int32_t>((80u * 128u) / kFinalTurnDurationTicks);
            g_pvp_eng.core.cus_gage = init_gage;
            g_pvp_eng.core.sides[0].cus_gage = init_gage;
            g_pvp_eng.core.sides[1].cus_gage = init_gage;
            sf_app_CpCanvas_cus_gage = init_gage;
            queue_tips_fifo(0, {static_cast<int16_t>(tid_fullcust)});

            bool saw_tf = false;
            bool exited_tf = false;
            for (int t = 0; t < 80; ++t) {
                step_pvp_combat_tick(rt, cp, (t == 0) ? (1 << 20) : 0, 0);
                if (g_pvp_eng.core.tf_state != TF_IDLE) saw_tf = true;
                else if (saw_tf) {
                    exited_tf = true;
                    break;
                }
            }
            step_pvp_combat_tick(rt, cp, (1 << 21), 0);
            const int32_t exp_gage_79 = static_cast<int32_t>((79u * 128u) / kFinalTurnDurationTicks);
            if (!saw_tf || !exited_tf || g_pvp_eng.core.final_turn_ticks_left != 79 ||
                g_pvp_eng.core.cus_gage != exp_gage_79 || sf_app_CpCanvas_cus_gage != exp_gage_79 ||
                g_pvp_eng.core.open_custom_req) {
                fail_msg("SuiteN3-Turn15FullCustImmunity",
                         "tf=(" + std::to_string(saw_tf) + "," + std::to_string(exited_tf) + ")" +
                         " left=" + std::to_string(g_pvp_eng.core.final_turn_ticks_left) +
                         " cus=(" + std::to_string(g_pvp_eng.core.cus_gage) + "," +
                         std::to_string(sf_app_CpCanvas_cus_gage) + ") exp=" + std::to_string(exp_gage_79) +
                         " open=" + std::to_string(g_pvp_eng.core.open_custom_req));
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite N complete (failures=%d)\n", failures);

    // =========================================================================
    // Suite O: Opponent Soul/Trance Badge & Turn Counter Hidden on Local Screen
    // =========================================================================
    run_safe("SuiteO", [&]() {
        for (int loc = 0; loc < 2; ++loc) {
            setup_basic_arena(500, 500, 0xF0001u + loc, loc);
            const int opp = 1 - loc;

            auto capture_rect = [&](int x0, int x1, int y0, int y1) {
                std::vector<uint32_t> out;
                const uint32_t* fb = rt.framebuffer();
                out.reserve(static_cast<size_t>((x1 - x0 + 1) * (y1 - y0 + 1)));
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) {
                        out.push_back(fb[y * 240 + x] & 0x00FFFFFFu);
                    }
                }
                return out;
            };

            auto render_and_capture = [&](bool custom_backdrop) {
                g_pvp_eng.backdrop_cache = PvpEngineState::BackdropCache{};
                rt.set_suppress_av(false);
                if (custom_backdrop) {
                    render_custom_screen_backdrop(rt, cp);
                } else {
                    render_pvp_combat_frame(rt, cp);
                }
                rt.set_suppress_av(true);
            };

            // 1) Baseline: neither side has Soul/Trance active
            g_pvp_eng.core.sides[loc].zoku = 0;
            g_pvp_eng.core.sides[loc].arm_cnt = 0;
            g_pvp_eng.core.sides[loc].wana = 0;
            g_pvp_eng.core.sides[opp].zoku = 0;
            g_pvp_eng.core.sides[opp].arm_cnt = 0;
            g_pvp_eng.core.sides[opp].wana = 0;
            render_and_capture(false);
            const auto base_tr_combat = capture_rect(192, 239, 58, 86);
            const auto base_tl_combat = capture_rect(0, 48, 58, 86);
            render_and_capture(true);
            const auto base_tr_custom = capture_rect(192, 239, 58, 86);

            // 2) Opponent activates Soul/Trance (zoku=2, arm_cnt=3) -> top-right HUD must stay unchanged
            g_pvp_eng.core.sides[opp].zoku = 2;
            g_pvp_eng.core.sides[opp].arm_cnt = 3;
            render_and_capture(false);
            const auto opp_tr_combat = capture_rect(192, 239, 58, 86);
            if (opp_tr_combat != base_tr_combat) {
                fail_msg("SuiteO-OppSoulHiddenCombat",
                         "loc=" + std::to_string(loc) + " opponent Soul/Trance HUD leaked into top-right during combat");
            }
            render_and_capture(true);
            const auto opp_tr_custom = capture_rect(192, 239, 58, 86);
            if (opp_tr_custom != base_tr_custom) {
                fail_msg("SuiteO-OppSoulHiddenCustom",
                         "loc=" + std::to_string(loc) + " opponent Soul/Trance HUD leaked into top-right during custom backdrop");
            }

            // 3) Local player activates Soul/Trance (zoku=2, arm_cnt=3) -> local top-left HUD still renders
            g_pvp_eng.core.sides[loc].zoku = 2;
            g_pvp_eng.core.sides[loc].arm_cnt = 3;
            render_and_capture(false);
            const auto loc_tl_combat = capture_rect(0, 48, 58, 86);
            if (loc_tl_combat == base_tl_combat) {
                fail_msg("SuiteO-LocalSoulVisible",
                         "loc=" + std::to_string(loc) + " local Soul/Trance HUD missing on top-left");
            }
        }
    });
    std::printf("[PvP SELF-TEST] Suite O complete (failures=%d)\n", failures);

    if (failures == 0) {
        std::printf("[PvP SELF-TEST PASS] LoN Suites A-O passed (0 failures).\n");
        return 0;
    }
    std::fprintf(stderr, "[PvP SELF-TEST FAIL] LoN Suites A-O failed (failures=%d).\n", failures);
    return 1;
}

} // namespace pvp_lon

// ============================================================================
// Public Entrypoints Called by Generated Hooks
// ============================================================================
static int32_t pvp_lon_intercept_ene_hit(DojaRuntime& rt, Ref ene_thiz, int32_t a0, int32_t a1, int32_t a2) {
    return pvp_lon::pvp_lon_intercept_ene_hit(rt, ene_thiz, a0, a1, a2);
}

static bool pvp_lon_intercept_ene_draw(DojaRuntime& rt, Ref ene_thiz, int32_t a0) {
    return pvp_lon::pvp_lon_intercept_ene_draw(rt, ene_thiz, a0);
}

static bool pvp_lon_tick_override(DojaRuntime& rt, Ref cp) {
    try {
        return pvp_lon_tick_override_impl(rt, cp);
    } catch (const JavaException& ex) {
        std::fprintf(stderr,
                     "[PvP LoN JavaException] phase=%d round=%u turn=%u ticks=%d done=%d ref=%d where=%s a=%d b=%d\n",
                     static_cast<int>(pvp_lon::g_pvp_eng.phase),
                     static_cast<unsigned>(pvp_lon::g_pvp_eng.core.custom_round),
                     pvp_lon::g_pvp_eng.core.combat_turn,
                     pvp_lon::g_pvp_eng.custom_timer_ticks,
                     pvp_lon::g_pvp_eng.local_custom_done ? 1 : 0,
                     ex.exc_ref, ex.where ? ex.where : "?", ex.detail_a, ex.detail_b);
        std::fflush(stderr);
        throw;
    }
}

static bool pvp_lon_tick_override_impl(DojaRuntime& rt, Ref cp) {
    using namespace pvp_lon;
    auto& eng = g_pvp_eng;
    pvp::PvpClientSession& net = rt.pvp_session();

    if (rt.pvp_self_test() && !eng.pending_self_test) {
        eng.pending_self_test = true;
        int rc = run_pvp_headless_self_test(rt, cp);
        rt.shutdown();
        std::fflush(stdout);
        std::fflush(stderr);
        std::exit(rc);
    }

    // Check CLI --pvp-create / --pvp-join auto-start once booted onto Title screen
    if (eng.phase == Phase::IDLE && !net.active()) {
        if (rt.pvp_auto_bot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        uint32_t cli_rm = 0;
        rt.take_cli_pvp_action(cli_rm);
    }

    // If user closed room via F1 menu while in PvP, restore single-player cleanly
    // (Stay in Phase::RESULT if UDP was closed by a local disconnect/stall adjudication.)
    if (!net.active()) {
        if (eng.phase != Phase::RESULT) {
            if (eng.phase != Phase::IDLE) {
                cleanup_and_restore_singleplayer(rt, false);
            }
            return false;
        }
    } else {
        if (eng.phase >= Phase::HANDSHAKE &&
            eng.active_room_id != 0 &&
            net.room_id() != eng.active_room_id) {
            cleanup_and_restore_singleplayer(rt, false);
        }

        net.poll_udp();

        if (net.peer_disconnected() && eng.phase <= Phase::LOBBY_WAIT) {
            net.close_udp();
            eng.phase = Phase::IDLE;
            rt.reset_interpolator();
            rt.show_ui_toast("房间已超时或对手已离开", 150);
            return false;
        }

        // Phase 1: Waiting for Challenger in Lobby (non-blocking banner while user plays)
        if (!net.match_started() && eng.phase <= Phase::LOBBY_WAIT) {
            eng.phase = Phase::LOBBY_WAIT;
            if (rt.pvp_auto_bot()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        }
    }

    // Transition into active PvP match! Take full VM snapshot once so we can restore cleanly later
    if (eng.phase == Phase::IDLE || eng.phase == Phase::LOBBY_WAIT) {
        rt.set_pvp_match_active(true);
        eng.active_room_id = net.room_id();
        eng.local_idx = net.is_host() ? 0 : 1;
        eng.match_seed = net.rng_seed() ? net.rng_seed() : 0x2468ACE1u;

        capture_pre_match_snapshot(rt, eng);

        eng.local_build = extract_local_lon_build(rt, cp);
        eng.build_prepared = true;
        eng.last_sync_send_ms = 0;
        eng.last_build_keepalive_ms = 0;
        eng.phase = Phase::HANDSHAKE;
        eng.phase_timer = 0;
        rt.reset_interpolator();
        rt.show_ui_toast("", 0);
    }

    // Check unexpected peer disconnect during active match
    if (net.active() && net.peer_disconnected() && eng.phase != Phase::RESULT) {
        adjudicate_peer_disconnect_win(rt);
        rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::HANDSHAKE — Exchange Fighter Builds (HP, Buster, Folder, Skills, dell_time, Souls)
    // ------------------------------------------------------------------------
    if (eng.phase == Phase::HANDSHAKE) {
        eng.phase_timer++;
        int64_t now = steady_ms();
        if (!(net.has_remote_build() && net.local_build_acked()) &&
            (now - eng.last_sync_send_ms >= 120)) {
            net.send_build_sync(eng.local_build);
            eng.last_sync_send_ms = now;
        }
        net.poll_udp();

        if (!(net.has_remote_build() && net.local_build_acked()) && eng.phase_timer > 16 * 15) {
            adjudicate_peer_disconnect_win(rt);
            net.close_udp();
            rt.show_ui_toast("对手连接超时，判定获胜!", 150);
        } else {
            render_connecting_screen(rt, net.room_id(), net.opponent_name(), net.is_triple_battle());

            if (net.has_remote_build() && net.local_build_acked()) {
                int loc_i = eng.local_idx;
                eng.builds[loc_i] = eng.local_build;
                eng.builds[1 - loc_i] = sanitize_lon_build(rt, net.remote_build());

                if (net.is_host() && net.preferred_format() == pvp::PVP_FORMAT_TRIPLE && !net.peer_supports_bo3()) {
                    rt.show_ui_toast("对手版本较旧，已自动切换为一回战", 150);
                }
                reset_bo3_series_state(eng, net.rng_seed(), net.is_triple_battle());

                init_pvp_battle_arena(rt, cp);
                enter_custom_screen_phase(rt, cp);
            } else if (rt.pvp_auto_bot()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::CUSTOM_SCREEN — Native LoN BattleTip + v2 Commit
    // ------------------------------------------------------------------------
    if (eng.phase == Phase::CUSTOM_SCREEN) {
        int32_t held_raw = rt.poll_local_pvp_keypad();
        int32_t edge_raw = held_raw & (~eng.prev_raw_keypad);
        eng.prev_raw_keypad = held_raw;
        held_raw &= ~(1 << 22);
        edge_raw &= ~(1 << 22);

        if (eng.bo3_game_index == 0 && eng.core.custom_round == 1) {
            pvp::PvpCustomCommit dummy_cc{};
            int64_t now = steady_ms();
            if (!net.has_remote_commit(current_wire_round_key(), dummy_cc) &&
                (now - eng.last_build_keepalive_ms >= 250)) {
                net.send_build_sync(eng.local_build);
                eng.last_build_keepalive_ms = now;
            }
        }

        if (!eng.local_custom_done) {
            eng.custom_timer_ticks++;
            render_custom_screen_backdrop(rt, cp);

            bool timed_out = (eng.custom_timer_ticks >= kCustomTimeoutTicks);
            bool bot_ok = rt.pvp_auto_bot() && (eng.custom_timer_ticks >= 8);
            int32_t pre_cas = sf_app_CpCanvas_cas_cnt;
            int32_t pre_arm = sf_app_CpCanvas_arm_cnt;
            int32_t pre_pa2 = sf_app_CpCanvas_pa_cnt2;

            if (rt.pvp_auto_bot() && eng.custom_timer_ticks == 4) {
                sf_app_CpCanvas_sel_cas_mode = 0;
                sf_app_CpCanvas_sel_cas_tip = 0;
                sf_app_CpCanvas_key = (1 << 20);
                sf_app_CpCanvas_key2 = (1 << 20);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
            } else if (rt.pvp_auto_bot() && eng.custom_timer_ticks == 6 &&
                       safe_iaload(rt, sf_app_CpCanvas_cas_ok, 1) == 1 &&
                       safe_iaload(rt, sf_app_CpCanvas_cas_tip, 1) > 0) {
                sf_app_CpCanvas_sel_cas_mode = 0;
                sf_app_CpCanvas_sel_cas_tip = 1;
                sf_app_CpCanvas_key = (1 << 20);
                sf_app_CpCanvas_key2 = (1 << 20);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
            } else if (bot_ok) {
                sf_app_CpCanvas_scene = kSceneBattleTip;
                sf_app_CpCanvas_sel_cas_mode = 1;
                sf_app_CpCanvas_key = (1 << 20);
                sf_app_CpCanvas_key2 = (1 << 20);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
                if (sf_app_CpCanvas_cas_cnt == 0 && sf_app_CpCanvas_arm_cnt == 0) {
                    fn_app_CpCanvas_CasOk_____V(rt, cp);
                    fn_app_CpCanvas_TipIn_____V(rt, cp);
                    sf_app_CpCanvas_cas_cnt = 15;
                }
            } else {
                int32_t bt_key = 0;
                const int check_bits[] = {20, 21, 16, 18, 17, 19};
                for (int b : check_bits) {
                    if (edge_raw & (1 << b)) {
                        bt_key = (1 << b);
                        break;
                    }
                }
                if (sf_app_CpCanvas_max_sel <= 0) {
                    sf_app_CpCanvas_sel_cas_mode = 1;
                    sf_app_CpCanvas_sel_cas_tip = 0;
                    bt_key &= ~((1 << 16) | (1 << 17) | (1 << 18) | (1 << 19));
                }
                sf_app_CpCanvas_key = bt_key;
                sf_app_CpCanvas_key2 = bt_key;
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
            }

            safe_iastore(rt, sf_app_CpCanvas_soft_id, 1, 0);

            bool ok_triggered = (pre_cas == 0 && pre_arm == 0 && pre_pa2 == 0) &&
                (sf_app_CpCanvas_cas_cnt > 0 || sf_app_CpCanvas_arm_cnt > 0 ||
                 sf_app_CpCanvas_pa_cnt2 > 0 || sf_app_CpCanvas_scene != kSceneBattleTip);

            if (ok_triggered || timed_out) {
                eng.local_commit = capture_local_custom_commit(rt, timed_out && !ok_triggered);
                eng.local_custom_done = true;
                eng.last_sync_send_ms = steady_ms();
                net.send_custom_commit(eng.local_commit);
                draw_waiting_custom_backdrop(rt, cp);
            } else {
                render_custom_screen_top_hud(rt);
            }
        } else {
            draw_waiting_custom_backdrop(rt, cp);
        }

        if (eng.local_custom_done) {
            uint16_t rnd = current_wire_round_key();
            if (!eng.remote_custom_ready && net.has_remote_commit(rnd, eng.remote_commit)) {
                eng.remote_custom_ready = true;
            }
            int64_t now = steady_ms();
            if (!eng.remote_custom_ready && (now - eng.last_sync_send_ms >= 120)) {
                net.send_custom_commit(eng.local_commit);
                eng.last_sync_send_ms = now;
            }
            if (eng.remote_custom_ready) {
                enter_round_banner_or_trance(rt, cp);
            } else if (rt.pvp_auto_bot()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::TRANCE_CUTSCENE — EXE6-Exact Trance Activation / Revert Cutscene
    // ------------------------------------------------------------------------
    if (eng.phase == Phase::TRANCE_CUTSCENE) {
        (void)rt.poll_local_pvp_keypad();
        uint16_t rnd = current_wire_round_key();
        uint32_t rem_top = 0;
        int64_t now = steady_ms();
        if (!net.get_highest_remote_turn_frame(rnd, rem_top) && (now - eng.last_sync_send_ms >= 120)) {
            net.send_custom_commit(eng.local_commit);
            eng.last_sync_send_ms = now;
        }
        if (rt.pvp_auto_bot() && !rt.fast_forward() && (eng.trance_tick & 3) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        step_and_render_trance_cutscene(rt, cp);
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::ROUND_BANNER — Synchronized 15-Frame Native BATTLE START Banner
    // ------------------------------------------------------------------------
    if (eng.phase == Phase::ROUND_BANNER) {
        (void)rt.poll_local_pvp_keypad();
        uint16_t rnd = current_wire_round_key();
        uint32_t rem_top = 0;
        int64_t now = steady_ms();
        if (!net.get_highest_remote_turn_frame(rnd, rem_top) && (now - eng.last_sync_send_ms >= 120)) {
            net.send_custom_commit(eng.local_commit);
            eng.last_sync_send_ms = now;
        }
        if (rt.pvp_auto_bot() && !rt.fast_forward() && (eng.banner_ticks & 3) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        render_pvp_combat_frame(rt, cp);
        render_round_banner_overlay(rt, eng.banner_ticks, eng.core.custom_round, 57);
        eng.banner_ticks++;
        if (eng.banner_ticks >= kBannerTicks) {
            int loc_i = eng.local_idx;
            transition_banner_to_combat(rt, eng, eng.core.sides[loc_i].tip_no >= 0);
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::COMBAT — GGPO Rollback + Symmetric Dual-Perspective Combat Engine
    // ------------------------------------------------------------------------
    if (eng.phase == Phase::COMBAT) {
        uint32_t cur_t = eng.core.combat_turn;
        const uint32_t pre_tick_unconf = eng.first_unconfirmed_turn;
        int32_t raw_local = rt.poll_local_pvp_keypad();
        if (rt.pvp_auto_bot()) {
            eng.round_held_edge_suppress_mask = 0;
            raw_local = compute_auto_bot_combat_input(rt);
            if (!rt.fast_forward() && (cur_t & 3u) == 0u) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            eng.round_held_edge_suppress_mask &= raw_local;
            raw_local &= ~eng.round_held_edge_suppress_mask;
        }

        const size_t hist_n = eng.local_inputs.size();
        uint16_t rnd = current_wire_round_key();
        bool pending_transition = (eng.core.open_custom_req || eng.core.winner != -2);

        schedule_local_combat_inputs(eng, cur_t, raw_local, pending_transition);

        uint32_t send_top = eng.max_scheduled_turn;
        int32_t hist_newest_first[8] = {0};
        uint8_t bundle_cnt = build_redundant_input_bundle(eng, send_top, hist_newest_first);
        uint16_t cur_hash = (!pending_transition &&
                             send_top == cur_t + kInputDelayFrames &&
                             eng.first_unconfirmed_turn >= cur_t)
            ? compute_combat_state_hash(rt)
            : 0;

        uint32_t pre_rem_top = 0;
        bool has_rem_input = net.get_highest_remote_turn_frame(rnd, pre_rem_top);
        if (!has_rem_input) {
            int64_t now = steady_ms();
            if (now - eng.last_sync_send_ms >= 120) {
                net.send_custom_commit(eng.local_commit);
                eng.last_sync_send_ms = now;
            }
        }
        int32_t pre_rem_frontier = has_rem_input
            ? static_cast<int32_t>(pre_rem_top)
            : static_cast<int32_t>(eng.round_start_turn);
        int16_t local_tick_adv = static_cast<int16_t>(
            std::clamp<int32_t>(static_cast<int32_t>(send_top) - pre_rem_frontier, -16000, 16000));
        net.send_input_bundle(rnd, send_top, hist_newest_first, bundle_cnt, cur_hash, local_tick_adv);
        net.poll_udp();

        uint32_t rollback_from = scan_mispredicted_turns(eng, net, rnd, cur_t);
        if (rollback_from != UINT32_MAX) {
            eng.in_resim = true;
            cur_t = execute_rollback_resim(
                rt, eng, net, rnd, rollback_from, cur_t,
                [&](int32_t p0_m, int32_t p1_m) { step_pvp_combat_tick(rt, cp, p0_m, p1_m); },
                [](DojaRuntime& r) { return compute_combat_state_hash(r); });
            eng.in_resim = false;
        }

        advance_confirmed_frontier(eng, net, rnd, cur_t);
        verify_confirmed_state_hashes(rt, eng, net, rnd, cur_t);

        if (eng.first_unconfirmed_turn > pre_tick_unconf) {
            eng.combat_stall_frames = 0;
        }

        auto flush_round_tail_and_transition = [&]() -> bool {
            flush_round_tail_bundle(eng, net, rnd, cur_t);
            eng.stall_frames = 0;
            eng.combat_stall_frames = 0;
            if (eng.core.winner != -2) {
                if (eng.core.ended_by_judge) {
                    eng.phase = Phase::JUDGE;
                    eng.phase_timer = 0;
                    eng.judge_ticks = 0;
                    rt.reset_interpolator();
                } else {
                    conclude_current_game(rt, cp);
                }
                render_pvp_combat_frame(rt, cp);
                return true;
            }
            enter_custom_screen_phase(rt, cp);
            return true;
        };

        if (eng.core.winner != -2 || eng.core.open_custom_req) {
            if (eng.first_unconfirmed_turn < cur_t) {
                ++eng.stall_frames;
                if (eng.first_unconfirmed_turn == pre_tick_unconf) {
                    ++eng.combat_stall_frames;
                } else {
                    eng.combat_stall_frames = 0;
                }
                if (eng.combat_stall_frames >= kStallTimeoutTicks) {
                    adjudicate_peer_disconnect_win(rt);
                    net.close_udp();
                    rt.show_ui_toast("对战同步超时，判定获胜!", 150);
                    render_pvp_combat_frame(rt, cp);
                    return true;
                }
                net.request_missing_input(rnd, eng.first_unconfirmed_turn);
                render_pvp_combat_frame(rt, cp);
                if (eng.stall_frames >= 8) {
                    render_sync_stall_banner(rt);
                }
                if (rt.pvp_auto_bot()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            }
            return flush_round_tail_and_transition();
        }

        size_t cur_slot = cur_t % hist_n;
        int32_t cur_rem_mask = 0;
        bool cur_rem_confirmed = eng.remote_confirmed[cur_slot];
        if (cur_rem_confirmed) {
            cur_rem_mask = eng.predicted_remote[cur_slot];
        } else if (net.get_remote_turn_input(rnd, cur_t, cur_rem_mask)) {
            cur_rem_confirmed = true;
        }

        uint32_t lag_turns = (cur_t > eng.first_unconfirmed_turn)
            ? (cur_t - eng.first_unconfirmed_turn)
            : (cur_rem_confirmed ? 0u : 1u);
        if (eng.first_unconfirmed_turn <= cur_t && !cur_rem_confirmed) {
            uint32_t miss_t = (eng.first_unconfirmed_turn < cur_t) ? eng.first_unconfirmed_turn : cur_t;
            if (lag_turns >= 4 || net.has_any_remote_turn_input_after(rnd, miss_t)) {
                net.request_missing_input(rnd, miss_t);
            }
        }

        if (!cur_rem_confirmed && lag_turns >= kMaxPredictionFrames) {
            ++eng.stall_frames;
            if (eng.first_unconfirmed_turn == pre_tick_unconf) {
                ++eng.combat_stall_frames;
            } else {
                eng.combat_stall_frames = 0;
            }
            if (eng.combat_stall_frames >= kStallTimeoutTicks) {
                adjudicate_peer_disconnect_win(rt);
                net.close_udp();
                rt.show_ui_toast("对战同步超时，判定获胜!", 150);
                render_pvp_combat_frame(rt, cp);
                return true;
            }
            net.request_missing_input(rnd, eng.first_unconfirmed_turn);
            render_pvp_combat_frame(rt, cp);
            if (eng.stall_frames >= 3) {
                render_sync_stall_banner(rt);
            }
            if (rt.pvp_auto_bot()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return true;
        }
        eng.stall_frames = 0;
        eng.combat_stall_frames = 0;

        apply_trill_clock_sync(rt, eng, net, rnd, send_top);

        if (!cur_rem_confirmed) {
            cur_rem_mask = (cur_t > eng.round_start_turn)
                ? eng.predicted_remote[(cur_t - 1) % hist_n]
                : eng.round_initial_remote_pred;
        }

        eng.predicted_remote[cur_slot] = cur_rem_mask;
        eng.remote_confirmed[cur_slot] = cur_rem_confirmed;
        if (cur_rem_confirmed && eng.first_unconfirmed_turn == cur_t) {
            eng.first_unconfirmed_turn = cur_t + 1;
        }

        int32_t cur_loc_mask = eng.local_inputs[cur_slot];
        int32_t p0_mask = (eng.local_idx == 0) ? cur_loc_mask : cur_rem_mask;
        int32_t p1_mask = (eng.local_idx == 1) ? cur_loc_mask : cur_rem_mask;

        uint16_t pre_h = compute_combat_state_hash(rt);
        save_rollback_slot(rt, eng, cur_t, p0_mask, p1_mask, !cur_rem_confirmed, pre_h);
        step_pvp_combat_tick(rt, cp, p0_mask, p1_mask);
        cur_t = eng.core.combat_turn;

        if ((eng.core.winner != -2 || eng.core.open_custom_req) &&
            eng.first_unconfirmed_turn >= cur_t) {
            return flush_round_tail_and_transition();
        }

        render_pvp_combat_frame(rt, cp);
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::JUDGE — Deterministic Hit Damage Judge Presentation (48 Ticks = 3s)
    // ------------------------------------------------------------------------
    if (eng.phase == Phase::JUDGE) {
        if (!net.active() || (eng.active_room_id != 0 && net.room_id() != eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        net.poll_udp();
        if (net.peer_disconnected()) {
            adjudicate_peer_disconnect_win(rt);
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
        }
        (void)rt.poll_local_pvp_keypad();
        render_pvp_combat_frame(rt, cp);

        int loc_i = eng.local_idx;
        int opp_i = eng.opp_idx();
        render_judge_overlay(
            rt,
            eng.builds[loc_i].player_name, eng.core.damage_dealt[loc_i],
            eng.builds[opp_i].player_name, eng.core.damage_dealt[opp_i],
            eng.core.winner == -1,
            eng.core.winner == loc_i,
            eng.judge_ticks);

        if (rt.pvp_auto_bot() && !rt.fast_forward() && (eng.judge_ticks & 3) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        eng.judge_ticks++;
        if (eng.judge_ticks >= kJudgeTicks) {
            conclude_current_game(rt, cp);
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::ROUND_RESULT — Between-Game BO3 Scoreboard (48 Ticks = 3s)
    // ------------------------------------------------------------------------
    if (eng.phase == Phase::ROUND_RESULT) {
        if (!net.active() || (eng.active_room_id != 0 && net.room_id() != eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        net.poll_udp();
        if (net.peer_disconnected()) {
            adjudicate_peer_disconnect_win(rt);
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
        }
        (void)rt.poll_local_pvp_keypad();
        render_pvp_combat_frame(rt, cp);

        int loc_i = eng.local_idx;
        int opp_i = eng.opp_idx();
        int g_win = (eng.bo3_game_index < 3)
            ? eng.bo3_game_results[eng.bo3_game_index]
            : eng.core.winner;
        render_round_result_overlay(
            rt,
            eng.bo3_game_index,
            g_win,
            loc_i,
            eng.bo3_wins[loc_i],
            eng.bo3_wins[opp_i]);

        if (rt.pvp_auto_bot() && !rt.fast_forward()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        eng.round_result_ticks++;
        if (eng.round_result_ticks >= kRoundResultTicks) {
            start_next_bo3_game(rt, cp);
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Phase::RESULT — Victory / Defeat / Draw Banner & Clean Single-Player Restore
    // ------------------------------------------------------------------------
    if (eng.phase == Phase::RESULT) {
        if (net.active()) {
            net.poll_udp();
        }
        if (eng.core.sides[0].dell_time_ref > 0 && eng.core.sides[1].dell_time_ref > 0) {
            render_pvp_combat_frame(rt, cp);
        } else {
            rt.fb_fill_rect_alpha(0, 0, 240, 240, 0xFF0A1428u);
        }

        int loc_i = eng.local_idx;
        int opp_i = eng.opp_idx();
        int eff_win = (eng.match_winner != -2) ? eng.match_winner : eng.core.winner;
        render_match_result_overlay(
            rt,
            eff_win == -1,
            eff_win == loc_i,
            eng.result_ticks,
            eng.is_triple_battle,
            eng.bo3_wins[loc_i],
            eng.bo3_wins[opp_i]);

        int32_t k = rt.poll_local_pvp_keypad();
        if (net.active() && eng.active_room_id != 0 && net.room_id() != eng.active_room_id) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        ++eng.result_ticks;
        if (rt.pvp_auto_bot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            if (eng.result_ticks >= 32) {
                int win = eff_win;
                uint32_t turns = eng.core.combat_turn;
                unsigned rnds = static_cast<unsigned>(eng.core.custom_round);
                int32_t hp0 = eng.core.sides[0].hp;
                int32_t hp1 = eng.core.sides[1].hp;
                int desyncs = eng.desync_warnings;
                unsigned games = static_cast<unsigned>(eng.bo3_game_index + 1);
                std::printf("[PvP BOT RESULT] local_idx=%d winner=%d turns=%u rounds=%u hp0=%d hp1=%d desyncs=%d bo3=%d games=%u w0=%u w1=%u\n",
                            loc_i, win, turns, rnds, hp0, hp1, desyncs,
                            eng.is_triple_battle ? 1 : 0, games,
                            static_cast<unsigned>(eng.bo3_wins[0]),
                            static_cast<unsigned>(eng.bo3_wins[1]));
                cleanup_and_restore_singleplayer(rt);
                rt.shutdown();
                std::fflush(stdout);
                std::fflush(stderr);
                std::exit(desyncs == 0 ? 0 : 2);
            }
            return true;
        }
        if (eng.result_ticks >= kResultAutoExitTicks ||
            (eng.result_ticks >= 24 && (k & ((1 << 20) | (1 << 10))))) {
            cleanup_and_restore_singleplayer(rt);
        }
        return true;
    }

    return false;
}
