#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rmexe::audio {

// ============================================================================
// Authentic Yamaha MA-3 (YMU762) / MA-5 (YMU765) Multi-Waveform FM Engine
// ============================================================================
enum YmWaveSelect : uint8_t {
    WS_SINE        = 0, // Standard Sine
    WS_HALF_SINE   = 1, // Positive Half-Sine (OPL/MA woodwind & pluck)
    WS_ABS_SINE    = 2, // Full-Wave Rectified Sine (even harmonics, guitar/reed)
    WS_PULSE_SINE  = 3, // Quarter-Period Pulse Sine (clav/metallic bite)
    WS_ALT_2X_SINE = 4, // OPL3/MA-3 Double-Freq Half-Sine (formant/vocal)
    WS_ABS_2X_SINE = 5, // OPL3/MA-3 Double-Freq Abs-Sine
    WS_COUNT       = 6
};

struct YmWaveRom {
    static constexpr int TABLE_BITS = 12;
    static constexpr int TABLE_SIZE = 1 << TABLE_BITS; // 4096
    static constexpr int TABLE_MASK = TABLE_SIZE - 1;

    float mod_tbl[WS_COUNT][TABLE_SIZE]; // Raw waveforms for phase modulation
    float car_tbl[WS_COUNT][TABLE_SIZE]; // DC-balanced waveforms for carrier output

    YmWaveRom() {
        constexpr double kTwoPi = 6.28318530717958647692;
        for (int w = 0; w < WS_COUNT; ++w) {
            double dc_sum = 0.0;
            for (int i = 0; i < TABLE_SIZE; ++i) {
                double p = static_cast<double>(i) / TABLE_SIZE;
                double v = 0.0;
                switch (w) {
                    case WS_SINE:
                        v = std::sin(kTwoPi * p);
                        break;
                    case WS_HALF_SINE:
                        v = (p < 0.5) ? std::sin(kTwoPi * p) : 0.0;
                        break;
                    case WS_ABS_SINE:
                        v = std::abs(std::sin(kTwoPi * p));
                        break;
                    case WS_PULSE_SINE:
                        v = (std::fmod(p, 0.5) < 0.25) ? std::abs(std::sin(kTwoPi * p)) : 0.0;
                        break;
                    case WS_ALT_2X_SINE:
                        v = (p < 0.5) ? std::sin(2.0 * kTwoPi * p) : 0.0;
                        break;
                    case WS_ABS_2X_SINE:
                        v = (p < 0.5) ? std::abs(std::sin(2.0 * kTwoPi * p)) : 0.0;
                        break;
                }
                mod_tbl[w][i] = static_cast<float>(v);
                dc_sum += v;
            }
            float dc_mean = static_cast<float>(dc_sum / TABLE_SIZE);
            for (int i = 0; i < TABLE_SIZE; ++i) {
                car_tbl[w][i] = mod_tbl[w][i] - dc_mean;
            }
        }
    }

    inline float sample_mod(uint8_t ws, double phase_cycles) const {
        int idx = static_cast<int>(phase_cycles * TABLE_SIZE) & TABLE_MASK;
        return mod_tbl[ws][idx];
    }

    inline float sample_car(uint8_t ws, double phase_cycles) const {
        int idx = static_cast<int>(phase_cycles * TABLE_SIZE) & TABLE_MASK;
        return car_tbl[ws][idx];
    }
};

inline const YmWaveRom& ym_rom() {
    static const YmWaveRom kRom;
    return kRom;
}

// 4-Operator (Dual 2-Op Stack) Yamaha MA-3 / MA-5 Voice Definition
struct YmFmPatch {
    // Pair 1: Modulator 1 -> Carrier 1
    float ratio_c1 = 1.0f;
    float ratio_m1 = 1.0f;
    uint8_t ws_c1 = WS_SINE;
    uint8_t ws_m1 = WS_SINE;
    float fb1 = 0.0f;            // Self-feedback in cycles (0.0 .. 0.30)
    float idx1_atk = 0.25f;      // Peak phase modulation depth (in cycles; 0.25 cycles = pi/2 rad)
    float idx1_sus = 0.15f;      // Sustained modulation depth (in cycles)
    float mod1_dec = 8.0f;       // Modulator decay rate (1/s)
    bool brass_swell = false;    // Lip-bloom modulator ramp for acoustic brass

    // Pair 2: Modulator 2 -> Carrier 2 (0 mix2 = pure 2-Op voice)
    float ratio_c2 = 1.0f;
    float ratio_m2 = 1.0f;
    uint8_t ws_c2 = WS_SINE;
    uint8_t ws_m2 = WS_SINE;
    float fb2 = 0.0f;
    float idx2_atk = 0.0f;
    float idx2_sus = 0.0f;
    float mod2_dec = 12.0f;
    float detune2_cents = 0.0f;  // Unison / chorus detune for MA-5 4-op richness
    float mix2 = 0.0f;

    // Carrier ADSR + D2R + Key Scaling
    float atk_sec = 0.003f;
    float dec1_rate = 10.0f;
    float sus_level = 0.80f;
    float dec2_rate = 0.0f;      // Decay rate while note is held (piano/guitar strings)
    float rel_rate = 45.0f;
    float ksl = 0.18f;           // Key-scale high-note softening
    float breath_chiff = 0.0f;   // Woodwind/flute attack breath noise
    float gain = 1.0f;
};

inline YmFmPatch get_ymu762_patch(int prog) {
    YmFmPatch p{};
    int gm = std::clamp(prog, 0, 127);

    if (gm <= 1) {
        // 0, 1: Acoustic / Bright Piano (MA-3 FM Piano: warm 1:1 string + 1:7 hammer tine)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.06f;
        p.idx1_atk = (gm == 1) ? 0.42f : 0.34f; p.idx1_sus = 0.07f; p.mod1_dec = 9.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 7.0f;
        p.idx2_atk = 0.24f; p.idx2_sus = 0.0f; p.mod2_dec = 28.0f; p.mix2 = 0.28f;
        p.atk_sec = 0.0018f; p.dec1_rate = 7.5f; p.sus_level = 0.58f;
        p.dec2_rate = 2.1f; p.rel_rate = 26.0f; p.ksl = 0.28f; p.gain = 1.08f;
    } else if (gm <= 7) {
        // 2..7: Electric Piano / DX7-style FM Tine Piano / Clav
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.04f;
        p.idx1_atk = 0.26f; p.idx1_sus = 0.05f; p.mod1_dec = 7.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 14.0f;
        p.idx2_atk = 0.20f; p.idx2_sus = 0.01f; p.mod2_dec = 20.0f; p.mix2 = 0.25f;
        p.atk_sec = 0.002f; p.dec1_rate = 6.0f; p.sus_level = 0.62f;
        p.dec2_rate = 1.6f; p.rel_rate = 22.0f; p.ksl = 0.30f; p.gain = 1.05f;
    } else if (gm == 10 || gm == 8 || gm == 9 || gm == 11 || gm == 14) {
        // 8..11, 14: Celesta / Glockenspiel / Music Box / Vibraphone / Tubular Bells
        p.ratio_c1 = 1.0f; p.ratio_m1 = 3.5f; p.fb1 = 0.02f;
        p.idx1_atk = 0.36f; p.idx1_sus = 0.04f; p.mod1_dec = 12.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 7.0f;
        p.idx2_atk = 0.18f; p.idx2_sus = 0.0f; p.mod2_dec = 30.0f; p.mix2 = 0.22f;
        p.atk_sec = 0.0012f; p.dec1_rate = 8.0f; p.sus_level = 0.35f;
        p.dec2_rate = 3.2f; p.rel_rate = 14.0f; p.ksl = 0.32f; p.gain = 0.95f;
    } else if (gm >= 12 && gm <= 15) {
        // 12, 13, 15: Marimba / Xylophone / Dulcimer (Woody hollow 1:4 FM mallet)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 4.0f; p.ws_m1 = WS_HALF_SINE;
        p.idx1_atk = 0.42f; p.idx1_sus = 0.02f; p.mod1_dec = 22.0f;
        p.atk_sec = 0.0012f; p.dec1_rate = 14.0f; p.sus_level = 0.22f;
        p.dec2_rate = 5.5f; p.rel_rate = 24.0f; p.ksl = 0.25f; p.gain = 1.12f;
    } else if (gm >= 16 && gm <= 23) {
        // 16..23: Drawbar / Percussive / Rock Organ / Accordion
        p.ratio_c1 = 1.0f; p.ratio_m1 = 2.0f; p.fb1 = 0.05f;
        p.idx1_atk = 0.24f; p.idx1_sus = 0.20f; p.mod1_dec = 10.0f;
        p.ratio_c2 = 3.0f; p.ratio_m2 = 1.0f;
        p.idx2_atk = 0.08f; p.idx2_sus = 0.06f; p.mix2 = 0.32f;
        p.atk_sec = 0.003f; p.dec1_rate = 12.0f; p.sus_level = 0.90f;
        p.rel_rate = 45.0f; p.gain = 0.95f;
    } else if (gm == 24) {
        // 24: Acoustic Guitar (nylon) - warm plucked FM string
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.07f;
        p.idx1_atk = 0.36f; p.idx1_sus = 0.06f; p.mod1_dec = 13.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 2.0f;
        p.idx2_atk = 0.20f; p.idx2_sus = 0.02f; p.mod2_dec = 20.0f; p.mix2 = 0.25f;
        p.atk_sec = 0.002f; p.dec1_rate = 9.0f; p.sus_level = 0.52f;
        p.dec2_rate = 2.8f; p.rel_rate = 24.0f; p.ksl = 0.24f; p.gain = 1.08f;
    } else if (gm == 25 || (gm >= 26 && gm <= 28)) {
        // 25..28: Acoustic Guitar (steel) / Clean & Muted Electric Guitar
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.10f;
        p.idx1_atk = 0.46f; p.idx1_sus = 0.10f; p.mod1_dec = 14.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 3.0f; p.ws_m2 = WS_HALF_SINE;
        p.idx2_atk = 0.30f; p.idx2_sus = 0.03f; p.mod2_dec = 24.0f;
        p.detune2_cents = 3.5f; p.mix2 = 0.34f;
        p.atk_sec = 0.0016f; p.dec1_rate = 9.5f; p.sus_level = (gm == 28) ? 0.18f : 0.56f;
        p.dec2_rate = (gm == 28) ? 8.0f : 2.5f; p.rel_rate = 26.0f; p.ksl = 0.24f; p.gain = 1.05f;
    } else if (gm >= 29 && gm <= 31) {
        // 29..31: Overdriven / Distortion Guitar / Harmonics (High-index OPL/MA FM Rock Guitar)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.ws_c1 = WS_ABS_SINE; p.fb1 = 0.19f;
        p.idx1_atk = 0.62f; p.idx1_sus = 0.42f; p.mod1_dec = 10.0f;
        p.ratio_c2 = 2.0f; p.ratio_m2 = 1.0f; p.fb2 = 0.14f;
        p.idx2_atk = 0.40f; p.idx2_sus = 0.28f; p.detune2_cents = 6.0f; p.mix2 = 0.38f;
        p.atk_sec = 0.0025f; p.dec1_rate = 8.0f; p.sus_level = 0.82f;
        p.dec2_rate = 0.6f; p.rel_rate = 32.0f; p.ksl = 0.22f; p.gain = 0.92f;
    } else if (gm >= 32 && gm <= 35) {
        // 32..35: Acoustic / Finger / Picked / Fretless Bass
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.11f;
        p.idx1_atk = 0.42f; p.idx1_sus = 0.14f; p.mod1_dec = 14.0f;
        p.atk_sec = 0.002f; p.dec1_rate = 10.0f; p.sus_level = 0.70f;
        p.dec2_rate = 1.5f; p.rel_rate = 38.0f; p.ksl = 0.10f; p.gain = 1.18f;
    } else if (gm == 36 || gm == 37) {
        // 36, 37: Slap Bass 1 & 2 (Signature Yamaha FM Slap Bass: rubbery 1:1 + metallic pop!)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.15f;
        p.idx1_atk = 0.56f; p.idx1_sus = 0.17f; p.mod1_dec = 18.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 3.0f; p.ws_m2 = WS_PULSE_SINE;
        p.idx2_atk = 0.44f; p.idx2_sus = 0.02f; p.mod2_dec = 34.0f; p.mix2 = 0.36f;
        p.atk_sec = 0.0014f; p.dec1_rate = 12.0f; p.sus_level = 0.66f;
        p.dec2_rate = 2.0f; p.rel_rate = 42.0f; p.ksl = 0.10f; p.gain = 1.18f;
    } else if (gm == 38) {
        // 38: Synth Bass 1 (Backbone of PoN & LoN! Snappy, punchy Yamaha MA-3 FM Synth Bass)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.16f;
        p.idx1_atk = 0.54f; p.idx1_sus = 0.18f; p.mod1_dec = 19.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 2.0f; p.ws_m2 = WS_HALF_SINE;
        p.idx2_atk = 0.36f; p.idx2_sus = 0.10f; p.mod2_dec = 22.0f; p.mix2 = 0.38f;
        p.atk_sec = 0.0015f; p.dec1_rate = 13.0f; p.sus_level = 0.74f;
        p.dec2_rate = 0.8f; p.rel_rate = 48.0f; p.ksl = 0.08f; p.gain = 1.22f;
    } else if (gm == 39) {
        // 39: Synth Bass 2 (Resonant FM Saw/Pulse Bass)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.18f;
        p.idx1_atk = 0.60f; p.idx1_sus = 0.25f; p.mod1_dec = 15.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 2.0f;
        p.idx2_atk = 0.32f; p.idx2_sus = 0.16f; p.detune2_cents = 4.0f; p.mix2 = 0.35f;
        p.atk_sec = 0.0018f; p.dec1_rate = 11.0f; p.sus_level = 0.78f;
        p.dec2_rate = 0.5f; p.rel_rate = 46.0f; p.ksl = 0.08f; p.gain = 1.18f;
    } else if (gm == 46 || gm == 45) {
        // 45, 46: Pizzicato Strings / Orchestral Harp (127 notes in PoN!)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.04f;
        p.idx1_atk = 0.28f; p.idx1_sus = 0.03f; p.mod1_dec = 14.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 3.0f;
        p.idx2_atk = 0.18f; p.idx2_sus = 0.0f; p.mod2_dec = 26.0f; p.mix2 = 0.24f;
        p.atk_sec = 0.0018f; p.dec1_rate = 8.5f; p.sus_level = 0.42f;
        p.dec2_rate = 2.8f; p.rel_rate = 18.0f; p.ksl = 0.26f; p.gain = 1.05f;
    } else if (gm >= 40 && gm <= 51) {
        // 40..44, 48..51: Violin / Strings Ensemble / Synth Strings (Lush 4-Op Chorus FM Strings)
        bool synth_str = (gm >= 50);
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.13f;
        p.idx1_atk = synth_str ? 0.34f : 0.28f; p.idx1_sus = 0.24f; p.mod1_dec = 6.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 2.0f; p.fb2 = 0.08f;
        p.idx2_atk = 0.22f; p.idx2_sus = 0.18f; p.detune2_cents = synth_str ? 7.5f : 5.5f;
        p.mix2 = 0.55f;
        p.atk_sec = synth_str ? 0.018f : 0.028f;
        p.dec1_rate = 5.0f; p.sus_level = 0.88f;
        p.rel_rate = 20.0f; p.ksl = 0.22f; p.gain = 0.88f;
    } else if (gm >= 52 && gm <= 54) {
        // 52..54: Choir Aahs / Voice Oohs / Synth Voice
        p.ratio_c1 = 1.0f; p.ratio_m1 = 2.0f; p.ws_c1 = WS_ALT_2X_SINE;
        p.idx1_atk = 0.18f; p.idx1_sus = 0.14f; p.mod1_dec = 5.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 1.0f; p.detune2_cents = 5.0f; p.mix2 = 0.45f;
        p.atk_sec = 0.024f; p.dec1_rate = 5.0f; p.sus_level = 0.86f; p.rel_rate = 20.0f; p.gain = 0.92f;
    } else if (gm == 55) {
        // 55: Orchestra Hit (Classic Capcom Power-Fifth FM Stab!)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.16f;
        p.idx1_atk = 0.52f; p.idx1_sus = 0.18f; p.mod1_dec = 14.0f;
        p.ratio_c2 = 1.4983f; p.ratio_m2 = 1.4983f; p.fb2 = 0.14f;
        p.idx2_atk = 0.44f; p.idx2_sus = 0.14f; p.mod2_dec = 14.0f;
        p.detune2_cents = 4.0f; p.mix2 = 0.65f;
        p.atk_sec = 0.002f; p.dec1_rate = 9.0f; p.sus_level = 0.38f;
        p.dec2_rate = 5.5f; p.rel_rate = 22.0f; p.ksl = 0.18f; p.gain = 1.05f;
    } else if (gm >= 56 && gm <= 63) {
        // 56..63: Trumpet / Trombone / Brass Section / Synth Brass 1 & 2
        bool synth_br = (gm >= 62);
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = synth_br ? 0.16f : 0.12f;
        p.idx1_atk = synth_br ? 0.48f : 0.38f; p.idx1_sus = 0.28f; p.mod1_dec = 9.0f;
        p.brass_swell = !synth_br;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 1.0f; p.fb2 = 0.12f;
        p.idx2_atk = 0.34f; p.idx2_sus = 0.22f; p.detune2_cents = synth_br ? 6.5f : 3.5f;
        p.mix2 = synth_br ? 0.48f : 0.28f;
        p.atk_sec = synth_br ? 0.006f : 0.012f; p.dec1_rate = 8.0f; p.sus_level = 0.84f;
        p.rel_rate = 30.0f; p.ksl = 0.20f; p.gain = 0.96f;
    } else if (gm >= 64 && gm <= 70) {
        // 64..70: Sax / Oboe (68) / English Horn / Bassoon
        p.ratio_c1 = 1.0f; p.ratio_m1 = 2.0f; p.ws_m1 = WS_PULSE_SINE; p.fb1 = 0.08f;
        p.idx1_atk = 0.34f; p.idx1_sus = 0.26f; p.mod1_dec = 10.0f;
        p.atk_sec = 0.008f; p.dec1_rate = 7.0f; p.sus_level = 0.86f;
        p.rel_rate = 32.0f; p.ksl = 0.22f; p.gain = 0.98f;
    } else if (gm == 71) {
        // 71: Clarinet (312 notes in LoN! Classic 1:3 odd-harmonic woody Yamaha FM Clarinet)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 3.0f; p.fb1 = 0.04f;
        p.idx1_atk = 0.30f; p.idx1_sus = 0.22f; p.mod1_dec = 9.0f;
        p.atk_sec = 0.009f; p.dec1_rate = 7.0f; p.sus_level = 0.86f;
        p.rel_rate = 32.0f; p.ksl = 0.24f; p.breath_chiff = 0.04f; p.gain = 1.00f;
    } else if (gm >= 72 && gm <= 79) {
        // 72..79: Piccolo / Flute (73) / Recorder (74) / Pan Flute (75) / Blown Bottle (76)
        p.ratio_c1 = 1.0f; p.ratio_m1 = 2.0f; p.fb1 = 0.03f;
        p.idx1_atk = 0.16f; p.idx1_sus = 0.08f; p.mod1_dec = 12.0f;
        p.atk_sec = 0.012f; p.dec1_rate = 6.0f; p.sus_level = 0.88f;
        p.rel_rate = 28.0f; p.ksl = 0.25f; p.breath_chiff = 0.08f; p.gain = 1.05f;
    } else if (gm == 80) {
        // 80: Lead 1 (Square) - Signature Rockman EXE Main Lead!
        // Pure odd-harmonic Yamaha 1:2 FM Square + subtle MA-5 stereo/detune pair
        p.ratio_c1 = 1.0f; p.ratio_m1 = 2.0f; p.fb1 = 0.02f;
        p.idx1_atk = 0.30f; p.idx1_sus = 0.25f; p.mod1_dec = 12.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 2.0f;
        p.idx2_atk = 0.24f; p.idx2_sus = 0.21f; p.detune2_cents = 3.5f; p.mix2 = 0.28f;
        p.atk_sec = 0.0022f; p.dec1_rate = 9.0f; p.sus_level = 0.84f;
        p.rel_rate = 42.0f; p.ksl = 0.20f; p.gain = 0.98f;
    } else if (gm == 81) {
        // 81: Lead 2 (Sawtooth) - Signature Rockman EXE Counter/Battle Lead!
        // Warm Yamaha 1:1 feedback FM Sawtooth + MA-5 unison detune
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.16f;
        p.idx1_atk = 0.39f; p.idx1_sus = 0.29f; p.mod1_dec = 11.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 1.0f; p.fb2 = 0.14f;
        p.idx2_atk = 0.33f; p.idx2_sus = 0.25f; p.detune2_cents = 5.5f; p.mix2 = 0.40f;
        p.atk_sec = 0.0025f; p.dec1_rate = 9.0f; p.sus_level = 0.84f;
        p.rel_rate = 40.0f; p.ksl = 0.22f; p.gain = 0.94f;
    } else if (gm == 82 || gm == 83) {
        // 82, 83: Lead 3 (Calliope) / Chiff Lead
        p.ratio_c1 = 1.0f; p.ratio_m1 = 2.0f;
        p.idx1_atk = 0.24f; p.idx1_sus = 0.14f; p.mod1_dec = 14.0f;
        p.ratio_c2 = 2.0f; p.ratio_m2 = 4.0f;
        p.idx2_atk = 0.15f; p.idx2_sus = 0.06f; p.mix2 = 0.26f;
        p.atk_sec = 0.005f; p.dec1_rate = 8.0f; p.sus_level = 0.84f;
        p.rel_rate = 34.0f; p.breath_chiff = 0.07f; p.gain = 0.98f;
    } else if (gm == 87) {
        // 87: Lead 8 (Bass + Lead) - Heavy Capcom Battle / Boss FM Unison Lead!
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.16f;
        p.idx1_atk = 0.42f; p.idx1_sus = 0.30f; p.mod1_dec = 12.0f;
        p.ratio_c2 = 0.5f; p.ratio_m2 = 1.0f; p.fb2 = 0.12f;
        p.idx2_atk = 0.38f; p.idx2_sus = 0.22f; p.detune2_cents = 3.0f; p.mix2 = 0.50f;
        p.atk_sec = 0.002f; p.dec1_rate = 10.0f; p.sus_level = 0.85f;
        p.rel_rate = 42.0f; p.ksl = 0.18f; p.gain = 0.96f;
    } else {
        // 84..86, 88..117: Synth Leads / Polysynth Pads / Ethnic
        p.ratio_c1 = 1.0f; p.ratio_m1 = 1.0f; p.fb1 = 0.12f;
        p.idx1_atk = 0.32f; p.idx1_sus = 0.22f; p.mod1_dec = 8.0f;
        p.ratio_c2 = 1.0f; p.ratio_m2 = 2.0f;
        p.idx2_atk = 0.20f; p.idx2_sus = 0.15f; p.detune2_cents = 6.0f; p.mix2 = 0.42f;
        p.atk_sec = (gm >= 88 && gm <= 95) ? 0.018f : 0.003f;
        p.dec1_rate = 8.0f; p.sus_level = 0.84f;
        p.rel_rate = 28.0f; p.ksl = 0.20f; p.gain = 0.94f;
    }

    return p;
}

} // namespace rmexe::audio
