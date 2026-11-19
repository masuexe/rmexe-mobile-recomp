# AGENTS.md — rmexe-mobile-recomp 工程规范

面向 *Rockman EXE: Phantom of Network (PoN)* 与 *Legend of Network (LoN)* 的静态重编译（JVM `.class` -> C++20）与原生 SDL2 运行时工程规范。

## 1. 构建与统一门禁

```bash
# 配置与全量构建（需 SDL2、zlib、Python 3.8+、C++20 编译器）
cmake -B build
cmake --build build -j$(nproc)

# 统一门禁（默认：静态契约检查 + CMake 构建 + PoN/LoN 完整 PvP QA 套件）
python3 tools/gate.py

# 快速门禁（静态契约检查 + CMake 构建 + 四端 --pvp-self-test 无头自测）
python3 tools/gate.py --quick
```

- 原始游戏文件（`.jar` / `.sp` / `.jam`）由用户放置于 `assets/`（已 gitignore），**严禁提交任何受版权保护的游戏资产**。

## 2. 核心模块边界

| 目录/模块 | 职责 | 边界约束 |
|---|---|---|
| `common/` | 跨平台 Socket、Capcom/PvP 协议、FM 音频、`localization.hpp`、`stb_truetype.h` | 保持纯头文件/无 SDL2 与游戏全局状态依赖 |
| `runtime/` | DoJa/CLDC 原生运行时（`doja_runtime.*`、`main.cpp`、`net_config.hpp`）、60 FPS 插值、双链字体引擎、PvP 引擎（`pvp_common.*`、`pvp_pon_engine.inl`、`pvp_lon_engine.inl`） | 保持 `.jar`/`.sp` 只读（写操作重定向至 `.sav`）；保证 `FullHeapSnapshot` / `FastCombatSnapshot` 确定性以支持 GGPO 回滚 |
| `server/` | 单端口（默认 `8765`）TCP HTTP/1.1 + UDP 联机服务器（`rmexe_server.cpp`） | 修改协议须同步检查 `common/pvp_protocol.hpp` 与 `test_pvp_*.py` |
| `tools/` | `recomp_jar.py`、`survey_doja.py`、`extract_strings.py`、`test_pvp_pon.py`、`test_pvp_lon.py`、`gate.py` | 保持 Python 3 标准库兼容，新增工具须通过 `gate.py` 语法检查 |

## 3. 开发与验证铁律

1. **Sibling Sweep（联机改动必验单机，共享改动必验双作双区）**：
   - 凡涉及 PvP 状态机（`LOBBY_WAIT` / `HANDSHAKE` / `CUSTOM_SCREEN` / `COMBAT` / 退出清理）或全局拦截钩子（如 `Ene.Draw` / `Ene.Hit`）的修改，**必须**验证单机路径未被拦截且退出 PvP 后状态完全复原。
   - 凡涉及 `pvp_common.*`、字体、i18n overlay 或运行时公共渲染/输入逻辑的修改，**必须**同时覆盖 **PoN 与 LoN**、**JP 与 EN** 四个目标。
2. **提交说明记录实跑结果**：
   - 非琐碎提交（feat / fix / refactor）须在 commit body 中记录实际执行过的构建与测试命令及结果（例如 `Verified: python3 tools/gate.py` 或具体 `--pvp-self-test` / `--dump-ppm` 验证范围），禁止只写空 body 或口头声称已测。
3. **经验沉淀**：
   - 踩坑与返工教训记录至 `LESSONS.md`，遵循 3-day 晋升规则。
