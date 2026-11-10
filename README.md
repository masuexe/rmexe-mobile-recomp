# rmexe-mobile-recomp

Static AOT Recompiler (`JVM .class` $\rightarrow$ `C++20`) and Native SDL2 Runtime for the Japanese DoCoMo i-αppli (DoJa) flip-phone titles:

- ***Rockman EXE: Phantom of Network*** (*MegaMan Battle Network: Phantom of Network*, 2004–2009)
- ***Rockman EXE: Legend of Network*** (*MegaMan Battle Network: Legend of Network*, 2006–2009)

Supports both the **original Japanese releases** (`PoN_jp`, `LoN_jp`) and the **English fan-translated patches** (`PoN`, `LoN`), compiling each `.jar` ahead-of-time into a standalone native executable with zero external JVM or DoJa emulator dependencies.

---

## Features

- **Ahead-of-Time JVM Bytecode to C++20 Static Recompiler ([`tools/recomp_jar.py`](tools/recomp_jar.py))**:
  - Parses all class files in `PoN.jar` (14 classes, 187K lines of generated C++) and `LoN.jar` (13 classes, 200K lines of generated C++).
  - Translates every JVM instruction into deterministic C++20 basic-block functions with zero-cost exception range dispatch (`try`/`catch` over `JavaException`).
- **Deterministic Handle-Based Memory Arena ([`runtime/doja_runtime.hpp`](runtime/doja_runtime.hpp))**:
  - All objects and arrays are stored in a contiguous handle table (`Ref = int32_t`), enabling fast O(1) state snapshotting for rollback netcode and sub-tick interpolation.
- **Clean-Room Native DoJa + CLDC 1.1 Runtime ([`runtime/doja_runtime.cpp`](runtime/doja_runtime.cpp))**:
  - **240×240 Software Rasterizer & 60 FPS Motion Interpolator**: Full support for `Graphics.drawImage`, `drawScaledImage`, `setFlipMode`, `setOrigin`, `fillRect`, `drawRect`, offscreen `Image` targets, and color-key transparency. Includes a display-list sub-frame motion interpolator (`FrameInterpolator`) that delivers smooth **60 FPS** isometric map scrolling and sprite movement over the deterministic 15 Hz logic loop without altering game speed, with instant **60 FPS / 15 FPS** switching (`F3`, `F1` menu, `--fps 60|15`, `controls.cfg`).
  - **Hardware-Style Indexed GIF89a & Palette Swapping**: Built-in LZW GIF decoder for `PalettedImage` and `MediaImage`, supporting real-time `Palette.setEntry` / `setPalette` swaps used for enemy element coloring, damage flashes, and battle transitions.
  - **Authentic Flip-Phone Yamaha MA-3 (`YMU762`) / MA-5 (`YMU765`) Stereo FM Audio ([`common/ymu762_fm.hpp`](common/ymu762_fm.hpp))**: 44.1 kHz stereo MFi2/MFi5 (`.mld`) synthesizer featuring 6-waveform 4-Operator (dual 2-Op stack) phase-modulation FM instrument patches, a 26-note MA-3 drum kit, dedicated *Rockman EXE* battle SE synthesis, panpot/expression/12-bit pitch-bend/vibrato timelines, MA-5 stereo reflection DSP, click-free loop crossfading, and FNV-1a deduplicated on-demand synthesis.
  - **Embedded ZIP / `JarInflater` & ScratchPad Loader**: Stream-based `PK\x03\x04` deflate extractor (`zlib`) with automatic detection of the 64-byte DoJa SDK `.sp` header (`511,964` / `512,064` bytes).
  - **Copy-on-Write Save Overlay (`.sav`)**: Keeps original `.jar` and `.sp` files strictly read-only while persisting player save data, downloaded quests, and seasonal courses to `.sav` overlays.
  - **Dual-Mode Capcom i-mode Network & Unified PvP Server ([`common/capcom_protocol.hpp`](common/capcom_protocol.hpp), [`common/pvp_protocol.hpp`](common/pvp_protocol.hpp), [`server/rmexe_server.cpp`](server/rmexe_server.cpp))**:
    - **Built-in Offline Fallback (Zero-Config Single-Player)**: Simulates all original Capcom i-mode endpoints (`buyreg`, server date, `scck` episodic unlocks, `pts` portal points, `DataSave`/`Load` cloud backups, `exesr` Top-10 Virus Busting leaderboard & rival chip folders, and `dataget.php` seasonal 10-wave courses) locally in `capcom_local.db`.
    - **Standalone Unified Server (`rmexe_server`)**: Multiplexes **TCP HTTP/1.1** (multi-user Capcom cloud saves, live global Virus Busting leaderboard, and PvP room matchmaking) and **UDP** (low-latency real-time PvP NetBattle frame-input lockstep/rollback relay) on a single port (`8765`).
  - **Shift-JIS (`CP932`) $\leftrightarrow$ `UTF-8` & CJK Font Rendering**: Automatic string transcoding via `iconv` and crisp 12px Japanese/English glyph rendering via `libfreetype`.

---

## Repository Layout

```text
rmexe-mobile-recomp/
├── ARCHITECTURE.md          # Full technical specification (60 FPS interpolation & GGPO PvP design)
├── CMakeLists.txt           # CMake build system for rmexe_server and all 4 game variants
├── assets/                  # Place user-supplied .jar, .sp, and .jam files here (gitignored)
│   └── README.md            # Directory structure instructions for JP and EN assets
├── common/
│   ├── capcom_protocol.hpp  # Capcom i-mode binary protocol & persistent cloud/leaderboard engine
│   ├── pvp_protocol.hpp     # PvP matchmaking REST + 28-byte UDP real-time frame-sync protocol
│   └── ymu762_fm.hpp        # Yamaha MA-3 (YMU762) / MA-5 (YMU765) 6-waveform ROM & 4-Op FM patches
├── runtime/
│   ├── doja_runtime.hpp     # Deterministic heap, slot types, and DoJaRuntime interface
│   ├── doja_runtime.cpp     # Clean-room DoJa/CLDC runtime, MA-3/MA-5 FM audio, 60 FPS interpolator, UI
│   ├── net_config.hpp       # CLI/config network, FPS mode & player identity bridge
│   └── main.cpp             # CLI entry point, headless smoke-test & frame capture harness
├── server/
│   └── rmexe_server.cpp     # Standalone single-port TCP (HTTP/1.1) + UDP unified server
└── tools/
    ├── survey_doja.py       # Phase 1 bytecode & ScratchPad static analyzer
    └── recomp_jar.py        # Phase 2 JVM .class -> C++20 ahead-of-time static recompiler
```

> **Note on Copyrighted Assets**: This repository contains **zero** copyrighted Capcom game code or assets. You must supply your own legally obtained `.jar` and `.sp` files in `assets/` prior to building.

---

## Building

### Prerequisites (Linux / WSL2)

- C++20 compiler (`g++ >= 10` or `clang++ >= 12`)
- `cmake >= 3.16`, `python3 >= 3.8`
- `libsdl2-dev`, `zlib1g-dev`, `libfreetype6`

### 1. Place Game Assets

Place the original Japanese and/or English patched files in `assets/` as documented in [`assets/README.md`](assets/README.md):

| Target Executable | `.jar` Path | `.sp` Path |
|---|---|---|
| `pon_jp_recomp` | `assets/PoN_jp/bin/PoN.jar` | `assets/PoN_jp/sp/PoN.sp` |
| `pon_en_recomp` | `assets/PoN/bin/PoN.jar` | `assets/PoN/sp/PoN.sp` |
| `lon_jp_recomp` | `assets/LoN_jp/bin/LoN.jar` | `assets/LoN_jp/sp/LoN.sp` |
| `lon_en_recomp` | `assets/LoN/bin/LoN.jar` | `assets/LoN/sp/LoN.sp` |

### 2. Configure and Build with CMake

```bash
cmake -B build
cmake --build build -j$(nproc)
```

---

## Running & Controls

```bash
./build/pon_jp_recomp --scale 3 --fps 60
./build/pon_en_recomp --scale 3 --fps 60
./build/lon_jp_recomp --scale 3 --fps 60
./build/lon_en_recomp --scale 3 --fps 60
```

### Unified Server (`rmexe_server`) & Online / Offline Modes

By default, all four game clients run in **Offline Simulation Mode**, handling Capcom i-mode cloud backups, site points, seasonal ranking courses, and NPC rival leaderboards locally without needing an external server.

To host shared cloud saves, a live multi-player PoN Virus Busting Top-10 leaderboard, and PvP NetBattle rooms on a single port (`8765` TCP + UDP):

```bash
# Start the unified server (persists data to server_data/capcom_state.db)
./build/rmexe_server --port 8765 --data-dir server_data

# Connect any JP or EN client to the unified server (or toggle Online Mode in the F1 menu)
./build/pon_jp_recomp --server 127.0.0.1:8765 --name LAN
```

### Default Controls & In-Game Keybinding Menu

Press **`F1`** or **`Esc`** (or **`Start`** on a Gamepad) at any time during gameplay to open the interactive **Keybinding & Emulator Settings Menu**, where you can remap all keyboard slots and controller buttons, switch between **60 FPS Smooth Interpolation** and **15 FPS Original Frame Rate**, adjust master volume, configure Online/Offline server settings, or toggle the flip-phone Softkey Bar. All settings are automatically saved to `controls.cfg`.

| Action | DoJa Key | Default Keyboard | Default Gamepad |
|---|---|---|---|
| Move Up / Down / Left / Right | `KEY_UP` / `DOWN` / `LEFT` / `RIGHT` | Arrow Keys or `W` `A` `S` `D` | D-Pad / Left Stick |
| **A Button** (Confirm / Use BattleChip) | `KEY_SELECT` (`0x14`) | `Z`, `J` (plus `Enter`, `Space`) | `A` / Cross |
| **B Button** (Cancel / Mega Buster) | `KEY_ASTERISK` (`0x0A`) | `X`, `K` | `B` / Circle |
| **L Button** (Left Softkey / Custom) | `KEY_SOFT1` (`0x15`) | `Q`, `U` | `LB` / `L1` |
| **R Button** (Right Softkey / PET Menu) | `KEY_SOFT2` (`0x16`) | `E`, `I` (plus `Backspace`) | `RB` / `R1` |
| **Pound / #** | `KEY_POUND` (`0x0B`) | `C`, `L` | `X` / Square |
| **Hold Fast-Forward (Turbo)** | — | `Tab`, `` ` `` | `Y` / Triangle |
| Numeric Keypad (`0`–`9`) | `KEY_0`–`KEY_9` (`0x00`–`0x09`) | `0`–`9` / Numpad `0`–`9` | — |

### Emulator Hotkeys

| Hotkey | Function |
|---|---|
| `F1` / `Esc` / `Start` | Open Pause, Keybinding & Settings Menu (includes Save & Quit) |
| `Tab` (hold) | Hold for ~3x Fast-Forward |
| `F2` | Toggle Flip-Phone Bottom Softkey Bar (`240×260` $\leftrightarrow$ `240×240`) |
| `F3` | Toggle Frame Rate Mode (`60 FPS Smooth Interpolation` $\leftrightarrow$ `15 FPS Original`) |
| `-` / `=` / `F9` (`M`) | Decrease / Increase Master Volume / Toggle Mute |
| `F11` / `Alt+Enter` | Toggle Desktop Fullscreen |
| `F12` | Save 240×240 BMP Screenshot |

---

## Architecture & Roadmap

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the complete engineering design, including:
- **Phase 1**: Static Bytecode & ScratchPad Survey (`tools/survey_doja.py`) — **Completed**
- **Phase 2**: Ahead-of-Time JVM-to-C++20 Recompiler, SDL2 Native Runtime & Yamaha MA-3/MA-5 Stereo FM Audio (`tools/recomp_jar.py`, `runtime/`, `common/ymu762_fm.hpp`) — **Completed**
- **Phase 3**: High-Frame-Rate (60 FPS) Display-List Motion Interpolation & `60 FPS / 15 FPS` Toggle — **Completed**
- **Phase 4**: Real-Time PvP NetBattle Expansion with GGPO Rollback Netcode (Unified `rmexe_server` TCP+UDP transport ready; combat engine dual-player hook next)
