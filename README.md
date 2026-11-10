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
  - **240×240 Software Rasterizer**: Full support for `Graphics.drawImage`, `drawScaledImage`, `setFlipMode`, `setOrigin`, `fillRect`, `drawRect`, offscreen `Image` targets, and color-key transparency.
  - **Hardware-Style Indexed GIF89a & Palette Swapping**: Built-in LZW GIF decoder for `PalettedImage` and `MediaImage`, supporting real-time `Palette.setEntry` / `setPalette` swaps used for enemy element coloring, damage flashes, and battle transitions.
  - **Embedded ZIP / `JarInflater` & ScratchPad Loader**: Stream-based `PK\x03\x04` deflate extractor (`zlib`) with automatic detection of the 64-byte DoJa SDK `.sp` header (`511,964` / `512,064` bytes).
  - **Copy-on-Write Save Overlay (`.sav`)**: Keeps original `.jar` and `.sp` files strictly read-only while persisting player save data to `.sav` overlays.
  - **Shift-JIS (`CP932`) $\leftrightarrow$ `UTF-8` & CJK Font Rendering**: Automatic string transcoding via `iconv` and crisp 12px Japanese/English glyph rendering via `libfreetype`.

---

## Repository Layout

```text
rmexe-mobile-recomp/
├── ARCHITECTURE.md          # Full technical specification (60 FPS interpolation & GGPO PvP design)
├── CMakeLists.txt           # CMake build system for all 4 game variants
├── assets/                  # Place user-supplied .jar, .sp, and .jam files here (gitignored)
│   └── README.md            # Directory structure instructions for JP and EN assets
├── runtime/
│   ├── doja_runtime.hpp     # Deterministic heap, slot types, and DoJaRuntime interface
│   ├── doja_runtime.cpp     # Clean-room DoJa/CLDC runtime, GIF/ZIP decoders, SDL2 host
│   └── main.cpp             # CLI entry point, headless smoke-test & frame capture harness
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

### Default Keyboard Controls (GBA-Style Mapping)

| Action | DoJa Key | Keyboard Keys |
|---|---|---|
| Move Up / Down / Left / Right | `KEY_UP` / `DOWN` / `LEFT` / `RIGHT` | Arrow Keys or `W` `A` `S` `D` |
| **A Button** (Confirm / Use BattleChip / Fire Buster) | `KEY_SELECT` (`0x14`) | `Z`, `J`, `Space`, `Enter` |
| **B Button** (Cancel / Charge & Fire Mega Buster) | `KEY_ASTERISK` (`0x0A`) | `X`, `K` |
| **L Button** (Left Softkey / Open Custom Screen in Battle) | `KEY_SOFT1` (`0x15`) | `Q`, `U`, `C` |
| **R Button** (Right Softkey / Open PET Menu) | `KEY_SOFT2` (`0x16`) | `E`, `I`, `V`, `Backspace` |
| Numeric Keypad (`0`–`9`) | `KEY_0`–`KEY_9` (`0x00`–`0x09`) | `0`–`9` |
| Save & Quit | — | `Esc` |

---

## Architecture & Roadmap

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the complete engineering design, including:
- **Phase 1**: Static Bytecode & ScratchPad Survey (`tools/survey_doja.py`) — **Completed**
- **Phase 2**: Ahead-of-Time JVM-to-C++20 Recompiler & SDL2 Native Runtime (`tools/recomp_jar.py`, `runtime/`) — **Completed**
- **Phase 3**: High-Frame-Rate (60+ FPS) Presentation & Sub-Tick Interpolation Engine
- **Phase 4**: Real-Time PvP NetBattle Expansion with GGPO Rollback Netcode
