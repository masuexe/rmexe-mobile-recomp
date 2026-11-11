// ============================================================================
// runtime/pvp_pon_engine.inl
// Symmetric 2-Player Online PvP NetBattle Engine for Rockman EXE PoN (JP/EN)
// Included inside namespace rmexe at the bottom of *_gen.cpp when is_pon_pvp.
// ============================================================================

} // namespace rmexe

#include "pvp_protocol.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace rmexe {
namespace pvp_pon {

enum class Phase : uint8_t {
    IDLE = 0,
    LOBBY_WAIT,
    HANDSHAKE,
    CUSTOM_SCREEN,
    COMBAT,
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

    int32_t prev_input_mask = 0;
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
static constexpr uint32_t kMaxPredictionFrames = 64;
static constexpr size_t kInputHistorySize = 4096;
static constexpr uint32_t kInputDelayFrames = 1;

struct PvpEngineState {
    Phase phase = Phase::IDLE;
    uint32_t active_room_id = 0;
    int local_idx = 0; // 0 = Host (Left/Red), 1 = Joiner (Right/Blue)
    uint32_t match_seed = 0x13579BDFu;
    size_t mutable_heap_min_ref = 1;

    // Pre-match full VM snapshot for clean restoration after match ends
    bool has_pre_match_snap = false;
    FullHeapSnapshot pre_match_heap;
    std::vector<int64_t> pre_match_statics;

    // Build sync & Custom Screen state
    pvp::PvpFighterBuild local_build{};
    pvp::PvpFighterBuild builds[2]{};
    bool build_prepared = false;
    int64_t last_sync_send_ms = 0;

    bool local_custom_done = false;
    pvp::PvpCustomCommit local_commit{};
    int custom_timer_ticks = 0;

    // Lockstep & GGPO Rollback state
    PvpCombatCore core{};
    std::array<int32_t, kInputHistorySize> local_inputs{};
    std::array<int32_t, kInputHistorySize> predicted_remote{};
    std::array<bool, kInputHistorySize> remote_confirmed{};
    std::array<RollbackSlot, kRollbackWindow> rb_ring{};
    FastCombatSnapshot render_snap{};
    std::vector<int64_t> render_statics{};
    uint32_t round_start_turn = 0;
    uint32_t first_unconfirmed_turn = 0;
    uint32_t max_scheduled_turn = 0;
    uint32_t last_hash_checked_turn = 0;
    int desync_warnings = 0;
    int stall_frames = 0;

    // Result banner timer
    int result_ticks = 0;

    // Re-entrancy flags for Ene.Hit hook
    bool in_combat_step = false;
    int active_side = 0;
};

static PvpEngineState g_pvp_eng;

static int64_t steady_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
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
    sf_app_CpCanvas_combo = s.combo;
    sf_app_CpCanvas_combo_time = s.combo_time;

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
                }
            }
        }
    }
    if (sf_app_CpCanvas_oki > 0) {
        for (int i = 0; i < 6; ++i) {
            Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
            if (ok <= 0 || rt.field_i32(ok, 1) == 0) continue;
            rt.field_i32(ok, 9) = 5 - rt.field_i32(ok, 9);    // pos_x
            rt.field_i32(ok, 11) = -rt.field_i32(ok, 11);     // pos_dx
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
            rt.field_i32(e0, 24) = opp.joutai;
            rt.field_i32(e0, 25) = opp.muteki_cnt;
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

    std::string nick = rt.player_nickname();
    b.player_name = nick.empty() ? "NAVI" : nick.substr(0, 24);

    rt.set_suppress_av(prev_sup);
    return b;
}

static void init_side_context(DojaRuntime& rt, Ref cp, int idx, const pvp::PvpFighterBuild& b) {
    SideContext& s = g_pvp_eng.core.sides[idx];
    s = SideContext{};

    // Allocate per-side Folder, tip_list[30], skill_kouka[29], Rock, Attack[30], and Rock static arrays
    s.tip_list_ref = rt.alloc_int_array(30);
    for (int i = 0; i < 30; ++i) {
        rt.iaload(s.tip_list_ref, i) = b.folder_packed[i];
    }

    s.fol_ref = rt.alloc_object(8, 9, 0); // app/Folder (class_id=8, 9 fields)
    fn_app_Folder__init______V(rt, s.fol_ref);
    fn_app_Folder_init_____V(rt, s.fol_ref);
    Ref fol_tip_id = rt.field_i32(s.fol_ref, 0);
    Ref fol_used = rt.field_i32(s.fol_ref, 1);
    for (int i = 0; i < 30; ++i) {
        rt.iaload(fol_tip_id, i) = i;
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
    sf_app_CpCanvas_max_sel = std::clamp<int32_t>(5 + b.skill_kouka[6], 1, 8);
    sf_app_CpCanvas_regu_flg = b.regu_flg ? 1 : 0;
    sf_app_CpCanvas_sec_cha = 0;
    sf_app_CpCanvas_pa_no = -1;
    sf_app_CpCanvas_key_cnt = 0;
    sf_app_CpCanvas_cus_sp = 1;
    sf_app_CpCanvas_combo = 1;
    sf_app_CpCanvas_combo_time = 0;

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

    // Apply start-of-battle NaviCust skills (Barrier, Custom+, Mega+, etc.)
    int32_t cus_mod = fn_app_CpCanvas_SkillRun___I__I(rt, cp, 0);
    (void)cus_mod;
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

    // Start battle music (Navi Battle BGM = 3)
    fn_app_CpCanvas_Audio___II__V(rt, 3, 1);

    g_pvp_eng.local_inputs.fill(0);
    g_pvp_eng.predicted_remote.fill(0);
    g_pvp_eng.remote_confirmed.fill(false);
    for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
    g_pvp_eng.round_start_turn = 0;
    g_pvp_eng.first_unconfirmed_turn = 0;
    g_pvp_eng.max_scheduled_turn = 0;
    g_pvp_eng.last_hash_checked_turn = 0;
    g_pvp_eng.desync_warnings = 0;
    g_pvp_eng.stall_frames = 0;
}

static void set_rock_palette_fast(DojaRuntime& rt, int32_t zoku);

static void enter_custom_screen_phase(DojaRuntime& rt, Ref cp) {
    g_pvp_eng.phase = Phase::CUSTOM_SCREEN;
    g_pvp_eng.core.custom_round++;
    g_pvp_eng.core.open_custom_req = false;
    g_pvp_eng.local_custom_done = false;
    g_pvp_eng.custom_timer_ticks = 30 * 16; // 30 seconds at 16 Hz
    g_pvp_eng.last_sync_send_ms = 0;

    int loc_i = g_pvp_eng.local_idx;
    ensure_board_orient(rt, loc_i);
    // Discard any leftover chips from the previous combat round on both sides when Custom Screen opens
    g_pvp_eng.core.sides[0].tip_no = -1;
    g_pvp_eng.core.sides[0].next_tip = 0;
    g_pvp_eng.core.sides[1].tip_no = -1;
    g_pvp_eng.core.sides[1].next_tip = 0;
    load_side(rt, loc_i);
    set_rock_palette_fast(rt, sf_app_Rock_zoku);
    // Prevent local Navi from appearing invisible if Custom Screen opened on an odd blink frame
    sf_app_Rock_muteki_cnt = 0;
    sf_app_Rock_hit_cnt = 0;
    sf_app_Rock_non_cnt2 = 0;
    sf_app_Rock_tip_no = -1;
    sf_app_Rock_next_tip = 0;
    sync_panel_occupancy_and_proxy(rt, loc_i);

    // Redraw all 18 panels into imgMap for local perspective backdrop during Custom Screen
    for (int y = 0; y < 3; ++y) {
        Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
        for (int x = 0; x < 6; ++x) {
            fn_app_BtPanel_Draw___IZ__V(rt, rt.iaload(row, x), -1, 1);
        }
    }

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
    if (g_pvp_eng.core.custom_round == 1 && sf_app_CpCanvas_cas_tip > 0) {
        int32_t clen = rt.array_length(sf_app_CpCanvas_cas_tip);
        for (int32_t i = 0; i < clen; ++i) rt.iaload(sf_app_CpCanvas_cas_tip, i) = -1;
    } else if (g_pvp_eng.core.custom_round > 1 && sf_app_CpCanvas_cas_tip > 0 && sf_app_CpCanvas_fol_tip_no > 0) {
        // Keep fol_tip_no aligned with compacted cas_tip slots (fol_tip_id[k] == k in PvP) so ADD in Round 2+ returns the right chip
        int32_t n = std::min<int32_t>(rt.array_length(sf_app_CpCanvas_cas_tip), rt.array_length(sf_app_CpCanvas_fol_tip_no));
        for (int32_t i = 0; i < n; ++i) {
            int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, i);
            if (ct >= 0) rt.iaload(sf_app_CpCanvas_fol_tip_no, i) = ct;
        }
    }

    Ref fol_obj = rt.iaload(sf_app_CpCanvas_fol, 0);
    fn_app_Folder_RandSet___I__V(rt, fol_obj, sf_app_CpCanvas_max_sel);
}

static pvp::PvpCustomCommit capture_local_custom_commit(DojaRuntime& rt) {
    pvp::PvpCustomCommit c{};
    c.turn_no = g_pvp_eng.core.custom_round;
    c.tip_no = static_cast<int8_t>(std::clamp(sf_app_Rock_tip_no, -1, 8));
    c.max_sel = static_cast<uint8_t>(std::clamp(sf_app_CpCanvas_max_sel, 1, 10));
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

    uint32_t mask = 0;
    Ref fol_obj = rt.iaload(sf_app_CpCanvas_fol, 0);
    if (fol_obj > 0) {
        Ref used_arr = rt.field_i32(fol_obj, 1);
        for (int i = 0; i < 30; ++i) {
            if (rt.iaload(used_arr, i) != 0) {
                mask |= (1u << i);
            }
        }
    }
    c.fol_used_mask = mask;
    return c;
}

static void apply_custom_commit_to_side(DojaRuntime& rt, int idx, const pvp::PvpCustomCommit& c) {
    load_side(rt, idx);
    sf_app_Rock_tip_no = c.tip_no;
    sf_app_CpCanvas_max_sel = std::clamp<int32_t>(c.max_sel, 1, 10);
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
    if (fol_obj > 0) {
        Ref used_arr = rt.field_i32(fol_obj, 1);
        int zan = 30;
        for (int i = 0; i < 30; ++i) {
            int u = ((c.fol_used_mask >> i) & 1u) ? 1 : 0;
            rt.iaload(used_arr, i) = u;
            if (u) --zan;
        }
        rt.field_i32(fol_obj, 2) = zan;
    }

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

// Compute 16-bit FNV-1a hash of canonical combat state for lockstep verification
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
        mix(sc.tip_no);
        mix(sc.bari);
        mix(sc.joutai);
        mix(sc.muteki_cnt);
    }
    mix(sf_app_CpCanvas_cus_gage);
    mix(sf_app_CpCanvas_stop_cnt);
    mix(sf_app_CpCanvas_stop_time);
    mix(g_pvp_eng.core.stop_owner);
    if (sf_app_CpCanvas_panel > 0) {
        for (int y = 0; y < 3; ++y) {
            Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
            for (int x = 0; x < 6; ++x) {
                Ref p = rt.iaload(row, x);
                mix(rt.field_i32(p, 3)); // jin
                mix(rt.field_i32(p, 4)); // jou
            }
        }
    }
    return static_cast<uint16_t>((h >> 16) ^ (h & 0xFFFFu));
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
    g_pvp_eng.core = sl.core;
    generated_restore_statics(sl.statics);
    rt.restore_combat_snapshot(sl.heap_snap);
    return true;
}

static void step_single_side(DojaRuntime& rt, Ref cp, int s_idx, int32_t mask) {
    g_pvp_eng.active_side = s_idx;
    ensure_board_orient(rt, s_idx);
    load_side(rt, s_idx);
    sync_panel_occupancy_and_proxy(rt, s_idx);

    SideContext& s = g_pvp_eng.core.sides[s_idx];
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
                fn_app_Rock_Action___I__V(rt, sf_app_CpCanvas_rock, 0);
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
                if (sf_app_CpCanvas_key_cnt > cha_thresh) {
                    static const int32_t kChargeTips[5] = {1, 209, 210, 211, 212};
                    buster_tip_id = kChargeTips[std::clamp(sf_app_Edit_r_zoku, 0, 4)];
                }
                int32_t saved_tip_no = sf_app_Rock_tip_no;
                sf_app_Rock_tip_no = 5;
                rt.iaload(rt.field_i32(sf_app_CpCanvas_rock, 0), 5) = rt.iaload(sf_app_CpCanvas_tip, buster_tip_id);
                fn_app_Rock_Action___I__V(rt, sf_app_CpCanvas_rock, 0);
                sf_app_Rock_tip_no = saved_tip_no;
                sf_app_CpCanvas_key_cnt = -4;
            }
        }
        sf_app_Rock_non_bas = 0;
    }

    rt.set_keypad_override(true, mask);
    sf_app_CpCanvas_key = 0;

    // Step Rock movement & chip execution
    fn_app_Rock_Move_____V(rt, sf_app_CpCanvas_rock);
    fn_app_CpCanvas_PaneHit___I__Z(rt, cp, 0);

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
    SideContext& opp = g_pvp_eng.core.sides[1 - s_idx];
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
            fn_app_Attack_HitGo___I__V(rt, a, -1);
        }
    }
    for (int i = 0; i < 30; ++i) {
        Ref a = rt.iaload(sf_app_CpCanvas_ata, i);
        if (rt.field_i32(a, 38) != 0) {
            fn_app_Attack_Move___I__V(rt, a, 1);
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
            fn_app_Attack_HitGo___I__V(rt, a, 0);
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

    // Sync game_cnt & seed RNG deterministically per tick so panel ticks and random attacks stay 100% in sync
    sf_app_CpCanvas_game_cnt = static_cast<int32_t>(g_pvp_eng.core.combat_turn);
    if (sf_app_CpCanvas_rand > 0) {
        uint64_t seed = (static_cast<uint64_t>(g_pvp_eng.match_seed) +
                         static_cast<uint64_t>(g_pvp_eng.core.combat_turn) * 0x9E3779B97F4A7C15ULL) &
                        ((1ULL << 48) - 1ULL);
        rt.get(sf_app_CpCanvas_rand).rng_seed = seed;
    }

    // Check simultaneous Custom Screen request when Custom Gauge is full (128) & no Time-Freeze
    if (sf_app_CpCanvas_cus_gage >= 128 && sf_app_CpCanvas_stop_cnt == 0) {
        int32_t p0_new = p0_mask & ~g_pvp_eng.core.sides[0].prev_input_mask;
        int32_t p1_new = p1_mask & ~g_pvp_eng.core.sides[1].prev_input_mask;
        constexpr int32_t kCustomTriggerBits = (1 << 21) | (1 << 22); // SOFT1 or SOFT2
        if ((p0_new & kCustomTriggerBits) || (p1_new & kCustomTriggerBits)) {
            g_pvp_eng.core.sides[0].prev_input_mask = p0_mask;
            g_pvp_eng.core.sides[1].prev_input_mask = p1_mask;
            g_pvp_eng.core.open_custom_req = true;
            g_pvp_eng.core.combat_turn++;
            ensure_board_orient(rt, 0);
            g_pvp_eng.in_combat_step = false;
            rt.set_suppress_av(prev_sup);
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
        int owner = (g_pvp_eng.core.stop_owner == 1) ? 1 : 0;

        if (sf_app_CpCanvas_stop_cnt == 1) {
            // Rewind mid-frame Rock animation counters (MotionBack) and snapshot mercy i-frames
            // into tmp_muteki_cnt on both fighters so multi-hit Navi chips don't grant mercy
            // invincibility mid-combo, while restoring i-frames cleanly when Time-Freeze ends.
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
                fn_app_Attack_Move___I__V(rt, a, 0);
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
                fn_app_Attack_HitGo___I__V(rt, a, 0);
            }
        }
        if (sf_app_CpCanvas_cus_sp > g_pvp_eng.core.shared_cus_sp) {
            g_pvp_eng.core.shared_cus_sp = std::clamp<int32_t>(sf_app_CpCanvas_cus_sp, 1, 4);
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
            sf_app_CpCanvas_key = 0;
            g_pvp_eng.core.stop_owner = -1;
        } else {
            save_side(rt, owner);
        }
    } else {
        // Alternate execution priority each tick for 100% frame-tie fairness
        int first = static_cast<int>(g_pvp_eng.core.combat_turn & 1u);
        int second = 1 - first;
        int32_t masks[2] = {p0_mask, p1_mask};

        step_single_side(rt, cp, first, masks[first]);
        if (sf_app_CpCanvas_stop_cnt == 0) {
            step_single_side(rt, cp, second, masks[second]);
        }

        if (sf_app_CpCanvas_stop_cnt == 0) {
            // Step shared obstacles & panels once per tick in canonical P0 orientation
            ensure_board_orient(rt, 0);
            load_side(rt, 0);
            sync_panel_occupancy_and_proxy(rt, 0);

            for (int i = 0; i < 6; ++i) {
                Ref ok = rt.iaload(sf_app_CpCanvas_oki, i);
                if (ok > 0 && rt.field_i32(ok, 1) != 0) {
                    fn_app_Okimono_Move_____V(rt, ok);
                    fn_app_Okimono_Draw_____V(rt, ok);
                }
            }
            for (int x = 0; x < 3; ++x) {
                for (int y = 0; y < 3; ++y) {
                    fn_app_BtPanel_Move_____V(rt, rt.iaload(rt.iaload(sf_app_CpCanvas_panel, y), x));
                }
            }
            for (int x = 5; x > 2; --x) {
                for (int y = 0; y < 3; ++y) {
                    fn_app_BtPanel_Move_____V(rt, rt.iaload(rt.iaload(sf_app_CpCanvas_panel, y), x));
                }
            }
            save_side(rt, 0);

            // Advance Custom Gauge
            if (sf_app_CpCanvas_cus_gage < 128) {
                sf_app_CpCanvas_cus_gage = std::min<int32_t>(128, sf_app_CpCanvas_cus_gage + g_pvp_eng.core.shared_cus_sp);
            }
        }
    }

    ensure_board_orient(rt, 0);
    if (sf_app_CpCanvas_soft_id > 0) {
        int32_t sid = (sf_app_CpCanvas_cus_gage >= 128) ? 1 : 0;
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

    g_pvp_eng.core.combat_turn++;
    g_pvp_eng.in_combat_step = false;
    rt.set_suppress_av(prev_sup);
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
        int32_t orig_dx = rt.field_i32(a, 5);
        int32_t orig_flp = rt.field_i32(a, 40);

        rt.field_i32(a, 3) = 5 - orig_px;
        rt.field_i32(a, 5) = -orig_dx;
        rt.field_i32(a, 40) = 1 - orig_flp;

        draw_attack_readonly(rt, a, layer);

        rt.field_i32(a, 3) = orig_px;
        rt.field_i32(a, 5) = orig_dx;
        rt.field_i32(a, 40) = orig_flp;
    }
}

static void render_pvp_combat_frame(DojaRuntime& rt, Ref cp) {
    // Snapshot canonical simulation state so PoN Draw methods cannot mutate any combat state
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

    // 1. Refresh all 18 battle panels into imgMap & blit imgMap to screen Graphics
    for (int y = 0; y < 3; ++y) {
        Ref row = rt.iaload(sf_app_CpCanvas_panel, y);
        for (int x = 0; x < 6; ++x) {
            fn_app_BtPanel_Draw___IZ__V(rt, rt.iaload(row, x), -1, 1);
        }
    }
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
            int32_t saved_dx = sf_app_Rock_dx;
            sf_app_Rock_pos_x = 5 - saved_px;
            sf_app_Rock_dx = -saved_dx;
            g_pvp_rock_flip_override = 0; // Face Left!
            fn_app_Rock_Draw___I__V(rt, sf_app_CpCanvas_rock, 0);
            g_pvp_rock_flip_override = -1;
            sf_app_Rock_pos_x = saved_px;
            sf_app_Rock_dx = saved_dx;
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
    load_side(rt, loc_i);
    set_rock_palette_fast(rt, sf_app_Rock_zoku);
    fn_app_CpCanvas_drawImg3___IIIIZ__V(rt, 43, 0, 48, 40, 0);
    {
        uint32_t bar_rgb = 0xFFC6D2EAu;
        if (sf_app_CpCanvas_cus_gage >= 128) {
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
    fn_app_Rock_WanaDraw_____V(rt, sf_app_CpCanvas_rock);
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

    // Time-Freeze chip name banner if active
    if (sf_app_CpCanvas_stop_cnt > 0 && sf_app_CpCanvas_stop_cnt <= 16 && sf_app_CpCanvas_stop_name > 0) {
        std::string chip_nm = rt.get(sf_app_CpCanvas_stop_name).str;
        int w = rt.fb_measure_text(chip_nm);
        rt.fb_fill_rect_alpha(40, 74, 160, 20, 0xCC101838u);
        rt.fb_draw_border(40, 74, 160, 20, 0xFFFFE066u);
        rt.fb_draw_text(chip_nm, 120 - w / 2, 88, 0xFFFFFFFFu);
    }

    // 6. Top HUD Bar: Local Name + HP vs Opponent Name + HP + Live Ping
    const SideContext& loc_sc = g_pvp_eng.core.sides[loc_i];
    const SideContext& opp_sc = g_pvp_eng.core.sides[opp_i];
    std::string loc_name = g_pvp_eng.builds[loc_i].player_name;
    std::string opp_name = g_pvp_eng.builds[opp_i].player_name;

    rt.fb_fill_rect_alpha(3, 4, 104, 32, 0xE014203Cu);
    rt.fb_draw_border(3, 4, 104, 32, 0xFF4C88FFu);
    rt.fb_draw_text(loc_name, 7, 16, 0xFF88CCFFu);
    std::string loc_hp_str = "HP " + std::to_string(std::max(0, loc_sc.hp)) + "/" + std::to_string(loc_sc.max_hp);
    rt.fb_draw_text(loc_hp_str, 7, 31, (loc_sc.hp * 4 <= loc_sc.max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);

    rt.fb_draw_text("VS", 113, 24, 0xFFFFD84Cu);

    rt.fb_fill_rect_alpha(133, 4, 104, 32, 0xE0381420u);
    rt.fb_draw_border(133, 4, 104, 32, 0xFFFF5868u);
    rt.fb_draw_text(opp_name, 137, 16, 0xFFFF98A8u);
    std::string opp_hp_str = "HP " + std::to_string(std::max(0, opp_sc.hp)) + "/" + std::to_string(opp_sc.max_hp);
    rt.fb_draw_text(opp_hp_str, 137, 31, (opp_sc.hp * 4 <= opp_sc.max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);

    // Restore canonical simulation state (100% read-only rendering, zero heap growth)
    g_pvp_eng.core = std::move(saved_core);
    generated_restore_statics(g_pvp_eng.render_statics);
    rt.restore_combat_snapshot(g_pvp_eng.render_snap);
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
    g_pvp_eng.active_room_id = 0;
    g_pvp_eng.build_prepared = false;
}

} // namespace pvp_pon

// ============================================================================
// Intercept Ene.Hit(III)Z on ene[0] proxy and route directly to defender Rock.Hit
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

    save_side(rt, att_idx);
    ensure_board_orient(rt, def_idx);
    load_side(rt, def_idx);

    int32_t hit_res = 0;
    int32_t counter_pow = 0;
    int32_t counter_zoku = 0;

    // Check Shield / Reflect (sf_app_Rock_han > 0)
    if (sf_app_Rock_han > 0 && pow > 0) {
        fn_app_Rock_EffSet___I__V(rt, sf_app_CpCanvas_rock, 12);
        if (sf_app_Rock_han == 2) {
            counter_pow = 100; // Reflect damage back!
        }
        hit_res = 0;
    }
    // Check Kawarimi Magic trap (sf_app_Rock_wana == 5)
    else if (sf_app_Rock_wana == 5 && pow >= 10) {
        sf_app_Rock_wana = 0;
        if (sf_app_CpCanvas_stop_cnt != 0) sf_app_Rock_tmp_muteki_cnt = 30;
        else sf_app_Rock_muteki_cnt = 30;
        fn_app_Rock_EffSet___I__V(rt, sf_app_CpCanvas_rock, 39);
        counter_pow = 100;
        hit_res = 0;
    }
    // Check Elemental Traps (sf_app_Rock_wana == 1..4: Anti-Fire/Aqua/Elec/Wood)
    else if (sf_app_Rock_wana >= 1 && sf_app_Rock_wana <= 4 && sf_app_Rock_wana == zoku && pow >= 10) {
        counter_zoku = sf_app_Rock_wana;
        counter_pow = 200;
        sf_app_Rock_wana = 0;
        if (sf_app_CpCanvas_stop_cnt != 0) sf_app_Rock_tmp_muteki_cnt = 30;
        else sf_app_Rock_muteki_cnt = 30;
        fn_app_Rock_EffSet___I__V(rt, sf_app_CpCanvas_rock, 39);
        hit_res = 0;
    } else {
        hit_res = fn_app_Rock_Hit___III__Z(rt, sf_app_CpCanvas_rock, hit_kou, pow, zoku);
    }

    sf_app_CpCanvas_now_hp = sf_app_Rock_hp;
    save_side(rt, def_idx);

    // Restore attacker context & proxy
    ensure_board_orient(rt, att_idx);
    load_side(rt, att_idx);
    if (counter_pow > 0) {
        fn_app_Rock_Hit___III__Z(rt, sf_app_CpCanvas_rock, 1, counter_pow, counter_zoku);
        sf_app_CpCanvas_now_hp = sf_app_Rock_hp;
        save_side(rt, att_idx);
    }
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

    // Check CLI --pvp-create / --pvp-join auto-start once booted onto Title screen
    if (g_pvp_eng.phase == Phase::IDLE && !rt.pvp_session().active()) {
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
        g_pvp_eng.phase = Phase::HANDSHAKE;
        rt.show_ui_toast("", 0);
    }

    // Check unexpected peer disconnect during active match
    if (net.peer_disconnected() && g_pvp_eng.phase != Phase::RESULT) {
        g_pvp_eng.core.winner = g_pvp_eng.local_idx;
        g_pvp_eng.phase = Phase::RESULT;
        g_pvp_eng.result_ticks = 0;
        rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
    }

    // ------------------------------------------------------------------------
    // Phase::HANDSHAKE — Exchange .sav Fighter Builds (HP, Buster, Folder, Skills)
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::HANDSHAKE) {
        int64_t now = steady_ms();
        if (!net.local_build_acked() && (now - g_pvp_eng.last_sync_send_ms >= 120)) {
            net.send_build_sync(g_pvp_eng.local_build);
            g_pvp_eng.last_sync_send_ms = now;
        }
        net.poll_udp();

        // Draw cyber-grid connecting screen
        rt.fb_fill_rect_alpha(0, 0, 240, 240, 0xFF0A1428u);
        rt.fb_fill_rect_alpha(16, 68, 208, 104, 0xF0142448u);
        rt.fb_draw_border(16, 68, 208, 104, 0xFF5898FFu);
        std::string t1 = "NETBATTLE 联机对战准备中";
        std::string t2 = "房间 #" + std::to_string(net.room_id()) + "  VS  " + net.opponent_name();
        std::string t3 = "正在同步双方存档牌组与领航员改造...";
        rt.fb_draw_text(t1, 120 - rt.fb_measure_text(t1) / 2, 94, 0xFFFFE066u);
        rt.fb_draw_text(t2, 120 - rt.fb_measure_text(t2) / 2, 122, 0xFFFFFFFFu);
        rt.fb_draw_text(t3, 120 - rt.fb_measure_text(t3) / 2, 148, 0xFF88CCFFu);

        if (net.has_remote_build() && net.local_build_acked()) {
            int loc_i = g_pvp_eng.local_idx;
            g_pvp_eng.builds[loc_i] = g_pvp_eng.local_build;
            g_pvp_eng.builds[1 - loc_i] = net.remote_build();

            init_pvp_battle_arena(rt, cp);
            enter_custom_screen_phase(rt, cp);
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
            g_pvp_eng.core.winner = g_pvp_eng.local_idx;
            g_pvp_eng.phase = Phase::RESULT;
            g_pvp_eng.result_ticks = 0;
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
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
                            if (st >= 0 && st < clim && rt.iaload(sf_app_CpCanvas_cas_tip, st) >= 0) {
                                rt.iaload(sf_app_CpCanvas_set_tip, valid_sel++) = st;
                            }
                        }
                    }
                    sf_app_CpCanvas_sel_cnt = valid_sel;
                    if (valid_sel > 0) {
                        fn_app_CpCanvas_TipIn_____V(rt, cp);
                        for (int32_t i = 0; i < 10; ++i) {
                            if (rt.iaload(sf_app_CpCanvas_cas_tip, i) < 0) {
                                for (int32_t j = i + 1; j < 10; ++j) {
                                    int32_t ct = rt.iaload(sf_app_CpCanvas_cas_tip, j);
                                    if (ct >= 0) {
                                        rt.iaload(sf_app_CpCanvas_cas_tip, i) = ct;
                                        rt.iaload(sf_app_CpCanvas_cas_ok, i) = 1;
                                        rt.iaload(sf_app_CpCanvas_cas_tip, j) = -1;
                                        rt.iaload(sf_app_CpCanvas_cas_ok, j) = 0;
                                        break;
                                    }
                                }
                            }
                        }
                    } else {
                        sf_app_Rock_tip_no = -1;
                    }
                    sf_app_CpCanvas_sel_cnt = 0;
                    sf_app_CpCanvas_cas_cnt = 0;
                    sf_app_CpCanvas_pa_cnt2 = 0;
                    sf_app_CpCanvas_sec_cha = 0;
                    sf_app_CpCanvas_key = 0;
                    sf_app_CpCanvas_scene = 1;
                }
            } else {
                sf_app_CpCanvas_key = bt_key;
                rt.set_keypad_override(true, local_mask);
                fn_app_CpCanvas_BattleTip_____V(rt, cp);
                rt.set_keypad_override(false, 0);
                // Hide the "[S] 逃げる" softkey label in PvP Custom Screen
                if (sf_app_CpCanvas_soft_id > 0 && rt.array_length(sf_app_CpCanvas_soft_id) > 1) {
                    rt.iaload(sf_app_CpCanvas_soft_id, 1) = 0;
                }
            }

            // Draw Custom Screen timer on top-left & Opponent Name/HP card on top-right
            int secs_left = std::max(0, (g_pvp_eng.custom_timer_ticks + 15) / 16);
            std::string t_str = "选牌 " + std::to_string(secs_left) + "s";
            rt.fb_fill_rect_alpha(4, 10, 62, 20, 0xCC101830u);
            rt.fb_draw_border(4, 10, 62, 20, 0xFF5898FFu);
            rt.fb_draw_text(t_str, 10, 24, (secs_left <= 5) ? 0xFFFF6868u : 0xFFFFE066u);

            int opp_i = 1 - loc_i;
            const SideContext& opp_sc = g_pvp_eng.core.sides[opp_i];
            std::string opp_hdr = "VS " + g_pvp_eng.builds[opp_i].player_name;
            std::string opp_hp_str = "HP " + std::to_string(std::max(0, opp_sc.hp)) + "/" + std::to_string(opp_sc.max_hp);
            rt.fb_fill_rect_alpha(122, 4, 114, 32, 0xE0381420u);
            rt.fb_draw_border(122, 4, 114, 32, 0xFFFF5868u);
            rt.fb_draw_text(opp_hdr, 126, 16, 0xFFFF98A8u);
            rt.fb_draw_text(opp_hp_str, 126, 31, (opp_sc.hp * 4 <= opp_sc.max_hp) ? 0xFFFF6868u : 0xFFFFFFFFu);

            if (sf_app_CpCanvas_scene == 1) {
                // Local player confirmed chips ("OK!") -> Capture commit!
                g_pvp_eng.local_commit = capture_local_custom_commit(rt);
                g_pvp_eng.local_custom_done = true;
                g_pvp_eng.last_sync_send_ms = 0;
            }
        }

        if (g_pvp_eng.local_custom_done) {
            int64_t now = steady_ms();
            uint16_t rnd = g_pvp_eng.core.custom_round;
            if (!net.is_custom_acked(rnd) && (now - g_pvp_eng.last_sync_send_ms >= 100)) {
                net.send_custom_commit(g_pvp_eng.local_commit);
                g_pvp_eng.last_sync_send_ms = now;
            }
            net.poll_udp();

            pvp::PvpCustomCommit rem_commit{};
            if (net.has_remote_commit(rnd, rem_commit) && net.is_custom_acked(rnd)) {
                // Both players committed & ACKed! Apply both hands in canonical order (P0 then P1)
                int loc_i = g_pvp_eng.local_idx;
                const pvp::PvpCustomCommit& c0 = (loc_i == 0) ? g_pvp_eng.local_commit : rem_commit;
                const pvp::PvpCustomCommit& c1 = (loc_i == 1) ? g_pvp_eng.local_commit : rem_commit;

                ensure_board_orient(rt, 0);
                apply_custom_commit_to_side(rt, 0, c0);
                apply_custom_commit_to_side(rt, 1, c1);

                g_pvp_eng.core.sides[0].prev_input_mask = 0;
                g_pvp_eng.core.sides[1].prev_input_mask = 0;
                sf_app_CpCanvas_cus_gage = 0;
                sf_app_CpCanvas_scene = 1;
                sf_app_CpCanvas_stop_cnt = 0;
                sf_app_CpCanvas_stop_time = 0;

                // Reset rollback ring & per-round tracking at the start of each combat round
                for (auto& sl : g_pvp_eng.rb_ring) sl.turn = UINT32_MAX;
                uint32_t cur_t = g_pvp_eng.core.combat_turn;
                const size_t hist_n = g_pvp_eng.local_inputs.size();
                g_pvp_eng.round_start_turn = cur_t;
                g_pvp_eng.first_unconfirmed_turn = cur_t + 1;
                g_pvp_eng.max_scheduled_turn = cur_t;
                g_pvp_eng.stall_frames = 0;
                for (uint32_t d = 0; d <= kInputDelayFrames + 8; ++d) {
                    size_t slot = (cur_t + d) % hist_n;
                    g_pvp_eng.local_inputs[slot] = 0;
                    g_pvp_eng.predicted_remote[slot] = 0;
                    g_pvp_eng.remote_confirmed[slot] = (d == 0);
                }

                g_pvp_eng.phase = Phase::COMBAT;
                render_pvp_combat_frame(rt, cp);
                return true;
            }

            // Render arena behind "Waiting for opponent..." banner
            render_pvp_combat_frame(rt, cp);
            rt.fb_fill_rect_alpha(28, 102, 184, 36, 0xE6101C38u);
            rt.fb_draw_border(28, 102, 184, 36, 0xFF68A8FFu);
            std::string wait_msg = "等待对手选牌确认...";
            rt.fb_draw_text(wait_msg, 120 - rt.fb_measure_text(wait_msg) / 2, 125, 0xFFFFE878u);
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
            g_pvp_eng.core.winner = g_pvp_eng.local_idx;
            g_pvp_eng.phase = Phase::RESULT;
            g_pvp_eng.result_ticks = 0;
            rt.show_ui_toast("对手连接已断开，判定获胜!", 150);
            render_pvp_combat_frame(rt, cp);
            return true;
        }

        const size_t hist_n = g_pvp_eng.local_inputs.size();
        uint16_t rnd = g_pvp_eng.core.custom_round;
        bool pending_transition = (g_pvp_eng.core.open_custom_req || g_pvp_eng.core.winner != -2);

        // Schedule new local inputs immutably: never overwrite an already-scheduled turn during a stall!
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
        net.send_input_bundle(rnd, send_top, hist_newest_first, bundle_cnt, cur_hash);
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
                        : 0;
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
                if (u > g_pvp_eng.round_start_turn) {
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
                g_pvp_eng.phase = Phase::RESULT;
                g_pvp_eng.result_ticks = 0;
                uint8_t res_code = (g_pvp_eng.core.winner == -1)
                    ? 3
                    : ((g_pvp_eng.core.winner == g_pvp_eng.local_idx) ? 1 : 2);
                net.send_match_end(res_code);
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
            std::string sync_txt = "网络同步中...";
            rt.fb_fill_rect_alpha(72, 108, 96, 22, 0xD8101830u);
            rt.fb_draw_border(72, 108, 96, 22, 0xFFFFB84Cu);
            rt.fb_draw_text(sync_txt, 120 - rt.fb_measure_text(sync_txt) / 2, 123, 0xFFFFE088u);
            return true;
        }
        g_pvp_eng.stall_frames = 0;

        if (!cur_rem_confirmed) {
            cur_rem_mask = (cur_t > g_pvp_eng.round_start_turn)
                ? g_pvp_eng.predicted_remote[(cur_t - 1) % hist_n]
                : 0;
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
    // Phase::RESULT — Victory / Defeat / Draw Banner & Clean Single-Player Restore
    // ------------------------------------------------------------------------
    if (g_pvp_eng.phase == Phase::RESULT) {
        render_pvp_combat_frame(rt, cp);

        bool is_draw = (g_pvp_eng.core.winner == -1);
        bool is_win = (g_pvp_eng.core.winner == g_pvp_eng.local_idx);
        uint32_t border_col = is_draw ? 0xFFFFD866u : (is_win ? 0xFF58FF98u : 0xFFFF6070u);
        std::string headline = is_draw ? "DRAW GAME - 平局!" : (is_win ? "YOU WIN!! 对战胜利!" : "YOU LOSE... 对战落败");
        std::string subline = "按 决定键 返回游戏 (" + std::to_string(std::max(0, (96 - g_pvp_eng.result_ticks) / 16)) + "s)";

        rt.fb_fill_rect_alpha(20, 86, 200, 56, 0xEE0C162Cu);
        rt.fb_draw_border(20, 86, 200, 56, border_col);
        rt.fb_draw_text(headline, 120 - rt.fb_measure_text(headline) / 2, 110, border_col);
        rt.fb_draw_text(subline, 120 - rt.fb_measure_text(subline) / 2, 132, 0xFFD8E6FFu);

        int32_t k = rt.poll_local_pvp_keypad();
        if (!net.active() || (g_pvp_eng.active_room_id != 0 && net.room_id() != g_pvp_eng.active_room_id)) {
            cleanup_and_restore_singleplayer(rt, false);
            return false;
        }
        ++g_pvp_eng.result_ticks;
        if (g_pvp_eng.result_ticks >= 96 || (g_pvp_eng.result_ticks >= 24 && (k & ((1 << 20) | (1 << 10))))) {
            cleanup_and_restore_singleplayer(rt);
        }
        return true;
    }

    return false;
}
