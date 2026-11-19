<!-- reviewed-through: ad06ac4 (2026-11-18) -->
# LESSONS.md — 工程教训与回归防线

> **3-Day 晋升规则**：同一类教训在 **3 个不同日期** 重复出现时，必须晋升为 `AGENTS.md` 中的硬性规范，或在 `tools/gate.py` 中落地为自动化门禁检查。

## 已沉淀教训

### [2026-11-18 | `ad06ac4`] PvP 钩子不得在 `LOBBY_WAIT` / `HANDSHAKE` 阶段拦截单机，退出须全量复位
- **根因**：`is_active()` 仅判断 `phase != Phase::IDLE`，导致处于 `LOBBY_WAIT` 或尚未建场的 `HANDSHAKE` 阶段时，`pvp_lon_intercept_ene_draw` 拦截了单机 `Ene.Draw`；且 `reset_common_exit_fields` 未清零 `eng.core`，`cleanup_and_restore_singleplayer` 未复位 `g_pvp_in_ene_hit_passthrough`。
- **对策**：战场拦截统一以 `is_arena_ready()` 为门控；退出清理必须复位 `eng.core = CoreT{}` 与全部全局 passthrough 标志，并在 PoN/LoN 自测中保留单机大厅回归用例（已纳入 `tools/gate.py` 契约检查）。

### [2026-11-17 ~ 2026-11-18 | `322a0ba`, `5bc6689`, `adad312`, `95b8ce4`, `dc98868`] GGPO 回滚与时间冻结链须保证快照完备与每帧清理
- **根因**：时间冻结（Dim-Chip / Counter Chain）与回滚重演期间，部分瞬态字段（如 `BtPanel.yokoku`、`tmp_muteki_cnt`、Buster 枪口/姿势视觉状态、Custom 槽、召唤隐藏状态）未纳入快照或未在规范逻辑帧边界清理，导致视觉错位或状态残留。
- **对策**：任何在战斗 tick 中读写的瞬态字段必须明确归属快照结构体或在规范 tick 边界显式清理/晋升，并由 `--pvp-self-test` 300 帧混沌回滚测试覆盖。

### [2026-11-15 | `4a13773`] 协议、鉴权与双区（JP/EN）差异须端到端联测
- **根因**：EN 版特有逻辑（如 Buster MAX 防护）及伪造 UDP 包边界条件，仅靠单端或单区自测无法暴露。
- **对策**：改动 `common/pvp_protocol.hpp`、`server/rmexe_server.cpp` 或 `runtime/pvp_*_engine.inl` 后，必须运行 `test_pvp_pon.py` 与 `test_pvp_lon.py` 覆盖 JP/EN 跨区对战与畸形包 fuzz 用例。
