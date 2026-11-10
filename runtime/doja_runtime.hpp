#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <iostream>

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
    explicit JavaException(Ref r = 1) : exc_ref(r) {}
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

// Runtime host interface implemented in doja_runtime.cpp
class DojaRuntime {
public:
    DojaRuntime();
    ~DojaRuntime();

    bool init(const std::string& jar_path, const std::string& sp_path, const std::string& jam_path, int scale = 3);
    void shutdown();

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
            throw JavaException(1);
        }
        return heap_[r];
    }

    int32_t array_length(Ref r);

    // Field & array access helpers
    int32_t& field_i32(Ref obj, size_t idx) { return get(obj).fields_i32[idx]; }
    int64_t& field_i64(Ref obj, size_t idx) { return get(obj).fields_i64[idx]; }
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

    // FPS & Interpolation settings
    void set_render_fps(int fps) { target_render_fps_ = fps; }
    int render_fps() const { return target_render_fps_; }
    void set_max_frames(int max_frames, const std::string& dump_ppm_path = "") {
        max_frames_ = max_frames;
        dump_ppm_path_ = dump_ppm_path;
    }
    void set_fast_forward(bool ff) { fast_forward_ = ff; }
    void inject_key_at_frame(int frame, int doja_key) {
        injected_keys_.push_back({frame, doja_key});
    }

    Ref current_app() const { return current_app_ref_; }
    void set_current_app(Ref app) { current_app_ref_ = app; }
    Ref current_canvas() const { return current_canvas_ref_; }

private:
    std::vector<HeapObject> heap_;
    Ref current_app_ref_ = 0;
    Ref current_canvas_ref_ = 0;
    Ref screen_graphics_ref_ = 0;
    Ref default_font_ref_ = 0;
    Ref sys_out_ref_ = 0;

    std::string jar_path_;
    std::string sp_path_;
    std::string sav_path_;
    std::vector<uint8_t> sp_data_;
    size_t sp_header_offset_ = 0;

    // 240x240 screen framebuffer (ARGB8888)
    uint32_t framebuffer_[240 * 240] = {0};
    bool graphics_locked_ = false;
    int32_t keypad_state_ = 0;
    int target_render_fps_ = 60;
    int window_scale_ = 3;
    bool running_ = true;
    int frame_count_ = 0;
    int max_frames_ = 0;
    bool fast_forward_ = false;
    int64_t fake_time_ms_ = 1000;
    std::string dump_ppm_path_;
    std::vector<std::pair<int, int>> injected_keys_;

    // SDL handles (opaque pointers to avoid polluting generated code with SDL headers)
    void* sdl_window_ = nullptr;
    void* sdl_renderer_ = nullptr;
    void* sdl_texture_ = nullptr;

    void load_sav_overlay();
    void save_sav_overlay();
    std::vector<uint8_t> read_jar_Bytes(const std::string& entry_path);
    void draw_image_internal(HeapObject& gfx, Ref img_ref, int dx, int dy, int sx, int sy, int sw, int sh, int dw, int dh);
    void draw_text_internal(HeapObject& gfx, const std::string& text, int x, int y);
};

extern DojaRuntime* g_runtime;

// Generated code entry points
void generated_static_init(DojaRuntime& rt);
Ref generated_create_app_main(DojaRuntime& rt);
void generated_start_app(DojaRuntime& rt, Ref app_ref);
void generated_process_event(DojaRuntime& rt, Ref canvas_ref, int32_t type, int32_t param);

} // namespace rmexe
