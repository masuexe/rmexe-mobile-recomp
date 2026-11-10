# RMEXE-MOBILE-RECOMP: SYSTEM DESIGN SPECIFICATION

**Target Software:** *Rockman EXE: Phantom of Network* (PoN) and *Rockman EXE: Legend of Network* (LoN)

---

## 1. Purpose and Scope

This document defines the technical plan for the `rmexe-mobile-recomp` project.
The project translates two DoJa mobile games (*PoN* and *LoN*) into native PC programs.

The system has four primary goals:
1. Run *PoN* and *LoN* natively on modern operating systems without an external emulator.
2. Keep copyright compliance. The project does not include original game code or assets.
3. Increase the display frame rate from 15 FPS to 60 FPS (and higher refresh rates).
4. Add real-time player-versus-player (PvP) online battles with GGPO rollback netcode.

---

## 2. Terminology

This specification uses the following technical terms:

* **JVM bytecode** — Compiled instruction format inside `.jar` class files.
* **DoJa API** — NTT DoCoMo Java library (`com.nttdocomo.*` and CLDC classes).
* **ScratchPad (`.sp`)** — Binary storage file for game chapters, maps, graphics, audio, and save data.
* **JAM file (`.jam`)** — Text descriptor file for an i-mode Java application.
* **MFi / MLD (`.mld`)** — Melody Format for i-mode audio files for Yamaha MA-series sound chips.
* **Static recompiler** — Build tool that translates JVM bytecode into C++ source code.
* **Native runtime** — C++ host library that provides graphics, audio, input, and network functions.
* **Linear arena heap** — Contiguous memory block that stores all runtime objects with 32-bit offsets.
* **Frame interpolation** — Visual calculation of intermediate positions between two logic ticks.
* **Sub-tick mode** — High-frequency logic update mode at 60 Hz (four sub-ticks per 15 Hz tick).
* **Custom Screen** — Turn-based BattleChip selection menu during combat.
* **GGPO rollback** — Deterministic peer-to-peer network synchronization method with state snapshots.

---

## 3. Analysis of Original Game Files

The preserved *PoN* and *LoN* releases use the DoJa platform. Each game contains three parts:
1. **JAM file (`.jam`):** Defines application metadata and ScratchPad segment sizes.
2. **Archive file (`.jar`):** Contains standard JVM bytecode. The bytecode keeps class structures, method signatures, and data types. It does not use raw memory pointers or hardware CPU registers.
3. **ScratchPad (`.sp`):** Stores episodic game assets and save data at fixed byte offsets.

The games communicate with the mobile phone only through the narrow DoJa API. Because of this clean boundary, static recompilation of *PoN* and *LoN* is simpler than recompilation of 16-bit or 64-bit console machine code.

---

## 4. Native Recompilation Pipeline

The system separates the build tools from the proprietary game data. The user must supply the original `.jar`, `.sp`, and `.jam` files.

### 4.1 Static Recompiler (JVM Bytecode to C++)

1. The static recompiler reads all `.class` files from the user's `.jar` file.
2. The tool reads a symbol map file (`.toml`) to assign readable names to obfuscated classes, methods, and fields.
3. The tool translates JVM bytecode instructions into structured C++ functions.
4. A C++ compiler links these generated functions with the native runtime to produce a standalone executable.

With symbol map files, one static recompiler and one native runtime can support Japanese, English, and future localized patches for both *PoN* and *LoN*.

### 4.2 Native Runtime Components

* **Graphics and Input (`SDL3`):** Implements `com.nttdocomo.ui.Canvas` and `Graphics`. Supports integer scaling, widescreen borders, and gamepad button mapping.
* **Audio Synthesizer (`MLD` Engine):** Decodes MFi / MLD files through a software synthesizer for Yamaha FM and ADPCM channels. The audio layer also supports optional external `.ogg` music tracks.
* **Copy-on-Write Storage:** Keeps the original `.sp` file in read-only mode. The runtime redirects all save data writes to a separate `.sav` file.
* **Local Mock Server:** Intercepts `javax.microedition.io.Connector` HTTP requests. A local virtual server unlocks offline event BattleChips, chip traders, and virus battle rankings.
* **External Font and Text Hook:** Intercepts text draw calls to support UTF-8 external translation tables and custom bitmap fonts.

---

## 5. High Frame Rate Design (15 FPS to 60+ FPS)

The original games use a fixed logic loop at 15 Hz (66.7 ms per tick). The game code adds integer speeds and timers on each tick without a time delta variable. A direct change to the loop timer causes the game to run at four times normal speed.

To prevent speed errors, the native runtime separates logic updates from screen draws. The system provides two frame rate modes:

### 5.1 Mode A: 15 Hz Logic with 60+ FPS Frame Interpolation (Default)

1. The game logic continues to run once every 66.7 ms (15 Hz).
2. The renderer assigns a unique ID tag to each active sprite, projectile, UI panel, and camera view.
3. The renderer records the previous coordinates `(prevX, prevY)` and current coordinates `(currX, currY)` on each logic tick.
4. Between two logic ticks, the renderer draws intermediate frames with a time ratio `alpha` (`0.0` to `1.0`).

Mode A produces smooth 60 FPS or 144 FPS camera movement, projectile motion, and menu transitions. Mode A does not change game rules, AI timers, or network synchronization.

### 5.2 Mode B: 60 Hz Hybrid Sub-Tick Mode (Low Input Latency)

At 15 Hz, button inputs have up to 66.7 ms of latency. Mode B reduces input latency to 16.7 ms to match Game Boy Advance titles:

1. The main loop samples player inputs and runs player state updates at 60 Hz.
2. Movement velocities use fixed-point values (one quarter of the 15 Hz step per sub-tick).
3. Player timers (charge shot, invincibility, and Custom gauge) multiply their target counts by four.
4. Legacy AI scripts that expect 15 Hz counters advance only once every four sub-ticks (`subTick % 4 == 0`).

---

## 6. Memory Management for Deterministic Rollback

Standard Java programs create temporary objects and rely on non-deterministic garbage collection (GC). Standard C++ heap pointers (`malloc` / `new`) cannot roll back safely with raw memory copies.

To solve this problem, the native runtime uses a **linear arena heap**:
1. The runtime allocates one fixed contiguous byte array (for example, 4 MB) at startup.
2. Generated C++ classes store 32-bit integer offsets into this array instead of 64-bit raw pointers.
3. A deterministic mark-compact collector runs only at fixed tick boundaries when memory usage exceeds a threshold.
4. A complete battle state snapshot requires only a fast `memcpy` of the active arena range.

---

## 7. Real-Time PvP NetBattle Design

The original mobile games had no real-time PvP code because early cellular networks had high HTTP latency. The combat engine, however, uses a deterministic 6x3 grid state machine. The project expands this engine into a two-player system.

### 7.1 Dual-Player Combat Engine Changes

1. **Dual Player Instances:** Remove singleton assumptions from the MegaMan player class. Create `Player[0]` on the left 3x3 grid and `Player[1]` on the right 3x3 grid with mirrored horizontal coordinates.
2. **Symmetric BattleChip Logic:** Audit all BattleChip routines. Replace hardcoded `+X` projectile directions and enemy-array targets with relative team parameters (`ownerSide` and `opponentSide`).
3. **Standalone Deck Sandbox:** Provide a dedicated NetBattle folder editor so players can build folders and NaviCust setups without single-player save progress.

### 7.2 Two-Phase Network Protocol

Online matches operate in two synchronized phases:

1. **Custom Screen Phase (Reliable Channel):**
   * Each player selects BattleChips and confirms the turn locally.
   * Both clients exchange selected chip ID arrays and transformation flags over a reliable packet channel.
   * When both clients confirm, combat starts on the same logic tick with a shared random seed.
2. **Real-Time Combat Phase (GGPO Rollback over UDP):**
   * On each logic tick, clients exchange an 8-bit input mask.
   * In **15 Hz Classic Rules**, a 120 ms network round-trip needs only 1 to 2 frames of rollback.
   * In **60 Hz Low-Latency Rules**, both clients lock to Mode B and use 4 to 8 frames of rollback.
   * Both players in a match must use the same logic frequency rule (`15 Hz` or `60 Hz`). Frame interpolation remains independent on each client.

---

## 8. Development Phases

* **Phase 1 — Bytecode Survey:** Analyze `.jar` and `.sp` structures for *PoN* and *LoN*. Map the main loop, DoJa API calls, and combat classes.
* **Phase 2 — Single-Player Recompiler:** Build the JVM-to-C++ static recompiler, linear arena heap, SDL3 backend, MLD synthesizer, and copy-on-write save layer.
* **Phase 3 — 60 FPS Upgrade:** Add sprite tagging and 60+ FPS frame interpolation, then build the optional 60 Hz sub-tick mode.
* **Phase 4 — Local PvP & Symmetry Audit:** Instantiate two local players on one screen and verify symmetric behavior for every BattleChip.
* **Phase 5 — Online NetBattle:** Integrate GGPO rollback netcode, Custom Screen synchronization, and the NetBattle deck sandbox.
