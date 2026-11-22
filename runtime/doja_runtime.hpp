#pragma once

#include "assist_config.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <iostream>

namespace rmexe::pvp {
class PvpClientSession;
}

namespace rmexe {

// 32-bit handle into the deterministic object table (0 == null)
using Ref = int32_t;

union Slot {
    int32_t i;
    int64_t j;
    Ref r;
    Slot() : j(0) {}
    explicit Slot(int32_t v) : j(0) { i = v; }
    explicit Slot(int64_t v) : j(v) {}
};

struct JavaException : public std::exception {
    Ref exc_ref;
    const char* where;
    int32_t detail_a;
    int32_t detail_b;
    explicit JavaException(Ref r = 1, const char* w = "java", int32_t a = 0, int32_t b = 0)
        : exc_ref(r), where(w), detail_a(a), detail_b(b) {}
    const char* what() const noexcept override { return "JavaException"; }
};

enum class ObjKind : uint8_t {
    Null = 0,
    ClassInstance,
    IntArray,
    ByteArray,
    RefArray,
    String,
    StringBuffer,
    Random,
    InputStream,
    OutputStream,
    Graphics,
    Image,
    PalettedImage,
    Palette,
    Font,
    AudioPresenter,
    MediaSound,
    MediaImage,
    JarInflater,
    HttpConnection,
    Dialog,
    Calendar,
    Date,
    Boolean,
    PrintStream,
    ExceptionObj,
};

struct ImageData {
    int width = 0;
    int height = 0;
    bool is_paletted = false;
    bool transparent_enabled = true;
    int transparent_index = -1;
    uint32_t transparent_color = 0xFF000000u;
    Ref palette_ref = 0;
    Ref graphics_ref = 0;
    std::vector<uint8_t> indices;     // For PalettedImage (8-bit indexed)
    std::vector<uint32_t> argb;       // For mutable offscreen Image (240x240)
    std::vector<uint8_t> raw_gif;     // Original GIF bytes for lazy/palette re-decode
    std::vector<uint32_t> pix_text;   // Lazy-allocated premultiplied text layer (width * height)
    std::vector<uint32_t> hd_text;    // Lazy-allocated HD premultiplied text layer ((width*hd_scale) * (height*hd_scale))
};

struct PaletteData {
    uint32_t entries[256] = {0};
    int count = 256;
};

struct StreamData {
    std::vector<uint8_t> buffer;
    size_t pos = 0;
    int sp_write_offset = -1;
    int sp_write_length = 0;
};

struct JarInflaterData {
    std::vector<uint8_t> zip_bytes;
};

struct HeapObject {
    ObjKind kind = ObjKind::Null;
    uint16_t class_id = 0;
    std::vector<int32_t> fields_i32;   // Instance fields (or int[] / Ref[] elements)
    std::vector<int64_t> fields_i64;   // 64-bit instance fields if any
    std::vector<uint8_t> bytes;        // byte[] elements
    std::string str;                   // For String / StringBuffer / URL
    uint64_t rng_seed = 0;             // For java.util.Random
    int32_t aux_i32 = 0;               // Graphics color / flip / AudioPresenter id / etc.
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t flip_mode = 0;
    Ref target_image = 0;              // Graphics target Image (0 = screen framebuffer)
    std::shared_ptr<ImageData> img;
    std::shared_ptr<PaletteData> pal;
    std::shared_ptr<StreamData> stream;
    std::shared_ptr<JarInflaterData> inflater;
};

struct FullHeapSnapshot {
    std::vector<HeapObject> heap;
    std::vector<std::pair<Ref, PaletteData>> palettes;
    std::vector<std::pair<Ref, std::vector<uint32_t>>> offscreen_argb;
    std::vector<std::pair<Ref, std::vector<uint32_t>>> offscreen_pix_text;
    std::vector<std::pair<Ref, std::vector<uint32_t>>> offscreen_hd_text;
    std::vector<std::pair<Ref, Ref>> paletted_image_refs;
};

struct FastCombatObjState {
    Ref ref = 0;
    std::vector<int32_t> fields_i32;
    std::vector<int64_t> fields_i64;
    uint64_t rng_seed = 0;
};

struct FastCombatSnapshot {
    size_t base_heap_size = 0;
    std::vector<FastCombatObjState> states;
    std::vector<std::pair<Ref, Ref>> paletted_image_refs;
    std::vector<HeapObject> extra_objs;
};

struct InjectedKeySpan {
    int start_frame = 0;
    int end_frame = 0;
    int doja_key = 0;
};

class DojaRuntime;

struct GameModule {
    void (*static_init)(DojaRuntime& rt) = nullptr;
    Ref (*create_app_main)(DojaRuntime& rt) = nullptr;
    void (*start_app)(DojaRuntime& rt, Ref app_ref) = nullptr;
    void (*process_event)(DojaRuntime& rt, Ref canvas_ref, int32_t type, int32_t param) = nullptr;
};

struct SaveSlotSummary {
    bool exists = false;
    bool is_autosave = false;
    int64_t timestamp = 0;
    int32_t sina_no = 0;
    int32_t back_menu = 5; // 3 = Cyberworld (FieldMain), 5 = Real World (Machi)
    int32_t map_no = 0;
    int32_t machi_no = 0;
    int32_t now_hp = 0;
    int32_t max_hp = 0;
    int32_t zenny = 0;
    std::string location_name;
};

// Runtime host interface implemented in doja_runtime.cpp
class DojaRuntime {
public:
    DojaRuntime();
    ~DojaRuntime();

    bool init(const std::string& jar_path, const std::string& sp_path, const std::string& jam_path, int scale = 3, const std::string& window_title = "");
    void shutdown();
    void save_sav_overlay_public() { save_sav_overlay(); }

    // Arena / Object allocation
    Ref alloc_object(uint16_t class_id, size_t num_i32_fields, size_t num_i64_fields = 0);
    Ref alloc_int_array(int32_t count);
    Ref alloc_byte_array(int32_t count);
    Ref alloc_ref_array(int32_t count);
    Ref alloc_multi_int_array(int32_t dim1, int32_t dim2);
    Ref alloc_multi_byte_array(int32_t dim1, int32_t dim2);
    Ref alloc_multi_int_array3(int32_t dim1, int32_t dim2, int32_t dim3);
    Ref alloc_string(const std::string& utf8);

    HeapObject& get(Ref r) {
        if (r <= 0 || static_cast<size_t>(r) >= heap_.size()) {
            throw JavaException(1, "get", r, static_cast<int32_t>(heap_.size()));
        }
        return heap_[r];
    }

    int32_t array_length(Ref r);

    // Field & array access helpers
    int32_t& field_i32(Ref obj, size_t idx) {
        HeapObject& o = get(obj);
        if (idx >= o.fields_i32.size()) {
            throw JavaException(1, "field_i32", static_cast<int32_t>(idx), static_cast<int32_t>(o.fields_i32.size()));
        }
        return o.fields_i32[idx];
    }
    int64_t& field_i64(Ref obj, size_t idx) {
        HeapObject& o = get(obj);
        if (idx >= o.fields_i64.size()) {
            throw JavaException(1, "field_i64", static_cast<int32_t>(idx), static_cast<int32_t>(o.fields_i64.size()));
        }
        return o.fields_i64[idx];
    }
    int32_t& iaload(Ref arr, int32_t idx);
    uint8_t& baload_ref(Ref arr, int32_t idx);
    int32_t baload(Ref arr, int32_t idx);
    void bastore(Ref arr, int32_t idx, int32_t val);

    // Invoke external DoJa / CLDC methods
    Slot invoke_external(const char* cls, const char* name, const char* desc, Slot* args, int num_args);

    // Poll SDL events, render frame interpolation / present screen
    void poll_events();
    void present_screen();
    int64_t current_time_millis();
    void sleep_ms(int64_t ms);
    void yield_spin_wait();

    // FPS & Interpolation settings
    void set_render_fps(int fps) { target_render_fps_ = fps; }
    int render_fps() const { return target_render_fps_; }
    void set_max_frames(int max_frames, const std::string& dump_ppm_path = "") {
        max_frames_ = max_frames;
        dump_ppm_path_ = dump_ppm_path;
    }
    void set_fast_forward(bool ff) { fast_forward_ = ff; }
    bool fast_forward() const { return fast_forward_; }
    void set_headless(bool h) { headless_ = h; }
    bool is_headless() const { return headless_; }
    bool should_bake_1x_text_into_gfx() const {
        return pvp_self_test_ || (headless_ && dump_ppm_path_.empty());
    }
    void set_config_path(const std::string& path);
    void set_touch_controls(bool enabled);
    void queue_game_touch_tap(int x, int y) {
        pending_tap_x_ = x;
        pending_tap_y_ = y;
        pending_tap_ttl_ = 2;
    }
    bool consume_game_touch_tap(int& out_x, int& out_y) {
        if (pending_tap_ttl_ <= 0) return false;
        out_x = pending_tap_x_;
        out_y = pending_tap_y_;
        pending_tap_ttl_ = 0;
        return true;
    }
    void attach_external_sdl(void* window, void* renderer) {
        sdl_window_ = window;
        sdl_renderer_ = renderer;
    }
    void set_data_dir(const std::string& dir) { data_dir_ = dir; }
    void set_asset_dir(const std::string& dir) { asset_dir_ = dir; }
    const std::string& data_dir() const { return data_dir_; }
    const std::string& asset_dir() const { return asset_dir_; }
    void set_game_module(const GameModule* mod) { game_module_ = mod; }
    const GameModule* game_module() const { return game_module_; }
    void dispatch_process_event(Ref canvas_ref, int32_t type, int32_t param) {
        if (game_module_ && game_module_->process_event) {
            game_module_->process_event(*this, canvas_ref, type, param);
        }
    }
    void inject_key_at_frame(int frame, int doja_key) {
        injected_keys_.push_back({frame, frame, doja_key});
    }
    void inject_key_span(int start_frame, int end_frame, int doja_key) {
        injected_keys_.push_back({start_frame, std::max(start_frame, end_frame), doja_key});
    }

    Ref current_app() const { return current_app_ref_; }
    void set_current_app(Ref app) { current_app_ref_ = app; }
    Ref current_canvas() const { return current_canvas_ref_; }
    int frame_count() const { return frame_count_; }
    uint32_t* framebuffer() { return framebuffer_; }

    // Online PvP & GGPO Rollback host hooks
    pvp::PvpClientSession& pvp_session();
    std::string player_nickname() const;
    int take_cli_pvp_action(uint32_t& room_id_out);
    void set_pvp_match_active(bool active) { pvp_match_active_ = active; }
    bool pvp_match_active() const { return pvp_match_active_; }
    void set_pvp_self_test(bool en) { pvp_self_test_ = en; }
    bool pvp_self_test() const { return pvp_self_test_; }
    void set_pvp_auto_bot(bool en) { pvp_auto_bot_ = en; }
    bool pvp_auto_bot() const { return pvp_auto_bot_; }
    void set_assist_self_test(bool en) { assist_self_test_ = en; }
    bool assist_self_test() const { return assist_self_test_; }
    void set_autosave_self_test(bool en) { autosave_self_test_ = en; }
    bool autosave_self_test() const { return autosave_self_test_; }
    bool auto_save_enabled() const;
    void set_auto_save_enabled(bool en);
    bool has_manual_save() const;
    bool has_auto_save() const;
    SaveSlotSummary get_manual_save_summary() const;
    SaveSlotSummary get_auto_save_summary() const;
    void set_location_name_tables(std::vector<std::string> map_names, std::vector<std::string> machi_names);
    std::string resolve_location_name(int32_t back_menu, int32_t map_no, int32_t machi_no) const;
    void begin_autosave_capture();
    bool commit_autosave_capture(const SaveSlotSummary& meta);
    void abort_autosave_capture();
    bool arm_oneshot_autosave_read();
    void disarm_oneshot_autosave_read();
    bool reload_autosave_from_disk();
    const std::string& autosave_path() const { return autosave_path_; }
    int run_save_slot_chooser_modal(const SaveSlotSummary& auto_sum, const SaveSlotSummary& manual_sum);
    int consume_pending_save_load_request();
    void request_save_load(int slot);
    void trigger_autosave_badge();
    const AssistConfig& assist_config() const;
    AssistConfig& mutable_assist_config();
    bool assist_hold_key_down() const;
    void set_assist_hud_status(const AssistHudStatus& st);
    std::string decode_game_bytes_to_utf8(const uint8_t* data, size_t len) const;
    void set_suppress_av(bool sup) { suppress_av_ = sup; }
    bool suppress_av() const { return suppress_av_; }
    void set_keypad_override(bool enabled, int32_t mask) {
        keypad_override_enabled_ = enabled;
        keypad_override_mask_ = mask;
    }
    int32_t poll_local_pvp_keypad();

    void save_full_snapshot(FullHeapSnapshot& out) const;
    void restore_full_snapshot(const FullHeapSnapshot& in);
    void save_combat_snapshot(FastCombatSnapshot& out, size_t min_ref = 1) const;
    void restore_combat_snapshot(const FastCombatSnapshot& in);
    size_t heap_size() const { return heap_.size(); }
    Ref audio_presenter_sound_ref(Ref ap_ref) const;
    int last_played_port_sound_id(int port) const;

    // Framebuffer HUD overlay drawing helpers (240x240 screen space)
    void reset_interpolator();
    uint32_t interp_test_img_gen(Ref img_ref) const;
    void interp_test_begin_frame();
    bool interp_test_end_and_rasterize(float alpha, uint32_t* out_fb_240x240);
    void fb_fill_rect_alpha(int rx, int ry, int rw, int rh, uint32_t argb);
    void fb_draw_border(int rx, int ry, int rw, int rh, uint32_t argb);
    int fb_measure_text(const std::string& utf8) const;
    void fb_draw_text(const std::string& utf8, int x, int baseline_y, uint32_t color, bool shadow = true);
    void show_ui_toast(const std::string& msg, int frames = 90);

private:
    struct AudioPresenterSlot {
        int32_t port = 0;
        Ref sound_ref = 0;
        int32_t volume = 100;
    };
    static constexpr Ref kAudioPresenterTag = 0x20000000;
    static constexpr size_t kPermanentAudioPresenters = 256;
    static constexpr size_t kDynamicAudioPresenterRing = 256;

    std::vector<HeapObject> heap_;
    Ref current_app_ref_ = 0;
    Ref current_canvas_ref_ = 0;
    Ref screen_graphics_ref_ = 0;
    Ref default_font_ref_ = 0;
    Ref sys_out_ref_ = 0;
    std::vector<AudioPresenterSlot> audio_presenters_;
    size_t dynamic_audio_presenter_cursor_ = 0;

    std::string jar_path_;
    std::string sp_path_;
    std::string sav_path_;
    std::string autosave_path_;
    std::string data_dir_;
    std::string asset_dir_;
    const GameModule* game_module_ = nullptr;
    std::vector<uint8_t> sp_data_;
    size_t sp_header_offset_ = 0;
    bool is_lon_ = false;
    size_t sp_save_offset_ = 300;
    size_t sp_save_payload_size_ = 437 * 4;
    bool sp_dirty_ = false;
    int64_t manual_save_timestamp_ = 0;
    bool has_autosave_ = false;
    int64_t autosave_timestamp_ = 0;
    uint32_t autosave_manual_crc_hint_ = 0;
    int64_t autosave_manual_ts_hint_ = 0;
    std::vector<uint8_t> autosave_payload_;
    bool autosave_capture_active_ = false;
    std::vector<uint8_t> autosave_capture_buf_;
    bool oneshot_autosave_read_ = false;
    int pending_save_load_request_ = 0;
    std::vector<std::string> map_name_table_;
    std::vector<std::string> machi_name_table_;

    // 240x240 screen framebuffer (ARGB8888) and dual premultiplied text overlays
    uint32_t framebuffer_[240 * 240] = {0};
    uint32_t fb_pix_text_[240 * 240] = {0};
    std::vector<uint32_t> fb_hd_text_;
    bool graphics_locked_ = false;
    int32_t keypad_state_ = 0;
    int target_render_fps_ = 60;
    int window_scale_ = 3;
    int hd_scale_ = 3;
    bool headless_ = false;
    bool running_ = true;
    int frame_count_ = 0;
    int max_frames_ = 0;
    bool fast_forward_ = false;
    int64_t fake_time_ms_ = 1000;
    std::string dump_ppm_path_;
    std::vector<InjectedKeySpan> injected_keys_;
    bool pvp_match_active_ = false;
    bool pvp_self_test_ = false;
    bool pvp_auto_bot_ = false;
    bool assist_self_test_ = false;
    bool autosave_self_test_ = false;
    bool suppress_av_ = false;
    bool keypad_override_enabled_ = false;
    int32_t keypad_override_mask_ = 0;
    int pending_tap_x_ = -1;
    int pending_tap_y_ = -1;
    int pending_tap_ttl_ = 0;

    // SDL handles (opaque pointers to avoid polluting generated code with SDL headers)
    void* sdl_window_ = nullptr;
    void* sdl_renderer_ = nullptr;
    void* sdl_texture_ = nullptr;
    void* sdl_texture_hd_ = nullptr;

    void load_sav_overlay();
    void save_sav_overlay();
    Ref alloc_raw_string(const std::string& utf8);
    std::vector<uint8_t> read_jar_Bytes(const std::string& entry_path);
    void draw_image_internal(HeapObject& gfx, Ref img_ref, int dx, int dy, int sx, int sy, int sw, int sh, int dw, int dh);
    void draw_text_internal(HeapObject& gfx, const std::string& text, int x, int y);
};

extern DojaRuntime* g_runtime;

void set_cli_pvp_bo3(bool v);

// Generated code entry points
void generated_static_init(DojaRuntime& rt);
Ref generated_create_app_main(DojaRuntime& rt);
void generated_start_app(DojaRuntime& rt, Ref app_ref);
void generated_process_event(DojaRuntime& rt, Ref canvas_ref, int32_t type, int32_t param);

} // namespace rmexe
