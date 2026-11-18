#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef WC_NO_BEST_FIT_CHARS
#define WC_NO_BEST_FIT_CHARS 0x00000400
#endif
#else
#include <cerrno>
#include <iconv.h>
#endif

namespace rmexe::loc {

inline uint64_t fnv1a_64(const uint8_t* data, size_t len) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 0x100000001B3ULL;
    }
    return h;
}

inline void append_cp_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

inline std::vector<uint32_t> utf8_to_codepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        if (c < 0x80u) {
            cps.push_back(c);
            i += 1;
        } else if ((c & 0xE0u) == 0xC0u && i + 1 < s.size()) {
            uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3Fu);
            cps.push_back(cp);
            i += 2;
        } else if ((c & 0xF0u) == 0xE0u && i + 2 < s.size()) {
            uint32_t cp = ((c & 0x0Fu) << 12) |
                          ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 6) |
                          (static_cast<uint8_t>(s[i + 2]) & 0x3Fu);
            cps.push_back(cp);
            i += 3;
        } else if ((c & 0xF8u) == 0xF0u && i + 3 < s.size()) {
            uint32_t cp = ((c & 0x07u) << 18) |
                          ((static_cast<uint8_t>(s[i + 1]) & 0x3Fu) << 12) |
                          ((static_cast<uint8_t>(s[i + 2]) & 0x3Fu) << 6) |
                          (static_cast<uint8_t>(s[i + 3]) & 0x3Fu);
            cps.push_back(cp);
            i += 4;
        } else {
            cps.push_back(c);
            i += 1;
        }
    }
    return cps;
}

// ============================================================================
// Self-Contained RFC 8259 JSON Parser
// ============================================================================
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    const JsonValue* get(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj_val) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }

    std::string get_str(const std::string& key, const std::string& def = "") const {
        const JsonValue* v = get(key);
        return (v && v->type == Type::String) ? v->str_val : def;
    }

    int64_t get_int(const std::string& key, int64_t def = 0) const {
        const JsonValue* v = get(key);
        return (v && v->type == Type::Number) ? static_cast<int64_t>(v->num_val) : def;
    }

    bool get_bool(const std::string& key, bool def = false) const {
        const JsonValue* v = get(key);
        return (v && v->type == Type::Bool) ? v->bool_val : def;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src), pos_(0) {
        if (src_.size() >= 3 &&
            static_cast<uint8_t>(src_[0]) == 0xEF &&
            static_cast<uint8_t>(src_[1]) == 0xBB &&
            static_cast<uint8_t>(src_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    bool parse(JsonValue& out) {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        return pos_ == src_.size();
    }

private:
    const std::string& src_;
    size_t pos_;

    void skip_ws() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos_;
            else break;
        }
    }

    bool parse_hex4(uint32_t& out) {
        if (pos_ + 4 > src_.size()) return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = src_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(10 + (c - 'a'));
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(10 + (c - 'A'));
            else return false;
        }
        out = v;
        return true;
    }

    bool parse_string(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= src_.size()) return false;
                char esc = src_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!parse_hex4(cp)) return false;
                        if (cp >= 0xD800u && cp <= 0xDBFFu &&
                            pos_ + 6 <= src_.size() &&
                            src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            size_t saved = pos_;
                            pos_ += 2;
                            uint32_t low = 0;
                            if (parse_hex4(low) && low >= 0xDC00u && low <= 0xDFFFu) {
                                cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                            } else {
                                pos_ = saved;
                            }
                        }
                        append_cp_utf8(out, cp);
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_number(JsonValue& out) {
        size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        if (pos_ >= src_.size()) return false;
        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        } else {
            return false;
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return false;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return false;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        out.type = JsonValue::Type::Number;
        out.num_val = std::strtod(src_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }

    bool parse_value(JsonValue& out) {
        skip_ws();
        if (pos_ >= src_.size()) return false;
        char c = src_[pos_];
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parse_string(out.str_val);
        }
        if (c == '{') {
            ++pos_;
            out.type = JsonValue::Type::Object;
            out.obj_val.clear();
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                skip_ws();
                std::string k;
                if (!parse_string(k)) return false;
                skip_ws();
                if (pos_ >= src_.size() || src_[pos_] != ':') return false;
                ++pos_;
                JsonValue v;
                if (!parse_value(v)) return false;
                out.obj_val.emplace_back(std::move(k), std::move(v));
                skip_ws();
                if (pos_ >= src_.size()) return false;
                if (src_[pos_] == '}') { ++pos_; return true; }
                if (src_[pos_] != ',') return false;
                ++pos_;
            }
        }
        if (c == '[') {
            ++pos_;
            out.type = JsonValue::Type::Array;
            out.arr_val.clear();
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                JsonValue elem;
                if (!parse_value(elem)) return false;
                out.arr_val.push_back(std::move(elem));
                skip_ws();
                if (pos_ >= src_.size()) return false;
                if (src_[pos_] == ']') { ++pos_; return true; }
                if (src_[pos_] != ',') return false;
                ++pos_;
            }
        }
        if (src_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            out.type = JsonValue::Type::Bool;
            out.bool_val = true;
            return true;
        }
        if (src_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            out.type = JsonValue::Type::Bool;
            out.bool_val = false;
            return true;
        }
        if (src_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            out.type = JsonValue::Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(out);
        }
        return false;
    }
};

inline bool parse_json(const std::string& src, JsonValue& out) {
    JsonParser p(src);
    return p.parse(out);
}

// ============================================================================
// Non-Destructive Chinese Translation Overlay & 2-Byte CP932-Superset Codec
// ============================================================================
enum class LangMode : int {
    Auto = 0,
    Zh = 1,
    Orig = 2
};

class LocalizationManager {
public:
    void set_lang_mode(LangMode mode) { lang_mode_ = mode; }
    LangMode lang_mode() const { return lang_mode_; }

    bool has_loaded_pack() const { return pack_loaded_; }
    size_t translated_count() const { return translated_count_; }
    const std::string& loaded_path() const { return loaded_path_; }
    const std::string& configured_path() const { return configured_path_; }

    void set_configured_path(const std::string& path) {
        configured_path_ = path;
    }

    bool is_enabled() const {
        if (lang_mode_ == LangMode::Orig) return false;
        if (lang_mode_ == LangMode::Zh) return pack_loaded_ && translated_count_ > 0;
        return pack_loaded_ && translated_count_ > 0;
    }

    bool reload() {
        if (configured_path_.empty()) return false;
        return load_json_file(configured_path_);
    }

    bool load_json_file(const std::string& path) {
        configured_path_ = path;
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            pack_loaded_ = false;
            translated_count_ = 0;
            class_map_.clear();
            stream_overlays_.clear();
            loaded_path_.clear();
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        JsonValue root;
        if (!parse_json(content, root) || root.type != JsonValue::Type::Object) {
            pack_loaded_ = false;
            translated_count_ = 0;
            class_map_.clear();
            stream_overlays_.clear();
            loaded_path_.clear();
            return false;
        }

        // Preserve ext_cp_to_pair_ / ext_pair_to_cp_ across hot-reloads so existing heap byte[] stay valid.
        class_map_.clear();
        stream_overlays_.clear();
        translated_count_ = 0;
        loaded_path_ = path;

        if (const JsonValue* cls_arr = root.get("class_strings");
            cls_arr && cls_arr->type == JsonValue::Type::Array) {
            for (const auto& item : cls_arr->arr_val) {
                if (item.type != JsonValue::Type::Object) continue;
                std::string jp = item.get_str("jp");
                std::string en = item.get_str("en");
                std::string zh = item.get_str("zh");
                if (zh.empty()) continue;
                if (!jp.empty()) {
                    class_map_[jp] = zh;
                }
                if (is_safe_en_fallback(en) && class_map_.find(en) == class_map_.end()) {
                    class_map_[en] = zh;
                }
                ++translated_count_;
            }
        }

        if (const JsonValue* streams_arr = root.get("streams");
            streams_arr && streams_arr->type == JsonValue::Type::Array) {
            for (const auto& st : streams_arr->arr_val) {
                if (st.type != JsonValue::Type::Object) continue;
                compile_stream_overlay(st);
            }
        }

        pack_loaded_ = true;
        return true;
    }

    const std::string& translate_class_string(const std::string& src) const {
        auto it = class_map_.find(src);
        return (it != class_map_.end()) ? it->second : src;
    }

    const std::vector<uint8_t>* find_stream_overlay(uint64_t hash) const {
        auto it = stream_overlays_.find(hash);
        return (it != stream_overlays_.end()) ? &it->second : nullptr;
    }

    std::vector<uint8_t> encode_text_to_bytes(const std::string& utf8, size_t max_bytes = SIZE_MAX, bool* truncated = nullptr) {
        if (truncated) *truncated = false;
        std::vector<uint8_t> out;
        auto cps = utf8_to_codepoints(utf8);
        for (uint32_t cp : cps) {
            if (cp == 0) continue;
            uint8_t b0 = 0, b1 = 0;
            int clen = 0;
            if (!encode_codepoint_superset(cp, b0, b1, clen)) {
                b0 = '?';
                clen = 1;
            }
            if (out.size() + static_cast<size_t>(clen) > max_bytes) {
                if (truncated) *truncated = true;
                break;
            }
            out.push_back(b0);
            if (clen == 2) out.push_back(b1);
        }
        return out;
    }

    template <typename RawCp932Fn>
    std::string decode_bytes_to_utf8(const uint8_t* data, size_t len, RawCp932Fn raw_cp932_fn) const {
        if (!data || len == 0) return "";
        size_t actual_len = 0;
        while (actual_len < len && data[actual_len] != 0) {
            ++actual_len;
        }
        if (actual_len == 0) return "";

        std::string out;
        out.reserve(actual_len * 2);
        size_t i = 0;
        while (i < actual_len) {
            uint8_t b0 = data[i];
            if (is_ext_lead(b0)) {
                if (i + 1 >= actual_len) break;
                uint8_t b1 = data[i + 1];
                uint16_t key = (static_cast<uint16_t>(b0) << 8) | static_cast<uint16_t>(b1);
                auto it = ext_pair_to_cp_.find(key);
                if (it != ext_pair_to_cp_.end()) {
                    append_cp_utf8(out, it->second);
                } else {
                    out += raw_cp932_fn(data + i, 2);
                }
                i += 2;
                continue;
            }

            size_t run_start = i;
            while (i < actual_len) {
                uint8_t c = data[i];
                if (is_ext_lead(c)) break;
                if (is_sjis_lead(c)) {
                    if (i + 1 >= actual_len) {
                        if (i > run_start) {
                            out += raw_cp932_fn(data + run_start, i - run_start);
                        }
                        return out;
                    }
                    i += 2;
                } else {
                    i += 1;
                }
            }
            if (i > run_start) {
                out += raw_cp932_fn(data + run_start, i - run_start);
            }
        }
        return out;
    }

private:
    static constexpr uint8_t kExtLeadBytes[18] = {
        0x80, 0xA0, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
        0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
    };

    LangMode lang_mode_ = LangMode::Auto;
    bool pack_loaded_ = false;
    std::string configured_path_;
    std::string loaded_path_;
    size_t translated_count_ = 0;

    std::unordered_map<std::string, std::string> class_map_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> stream_overlays_;

    std::unordered_map<uint32_t, uint32_t> cp932_cache_;
    std::unordered_map<uint32_t, uint16_t> ext_cp_to_pair_;
    std::unordered_map<uint16_t, uint32_t> ext_pair_to_cp_;
    uint32_t next_ext_slot_ = 0;

    static bool is_ext_lead(uint8_t b) {
        return b == 0x80u || b == 0xA0u || b >= 0xF0u;
    }

    static bool is_sjis_lead(uint8_t b) {
        return (b >= 0x81u && b <= 0x9Fu) || (b >= 0xE0u && b <= 0xEFu);
    }

    static bool is_safe_en_fallback(const std::string& en) {
        if (en.empty()) return false;
        bool non_ascii = false;
        for (unsigned char c : en) {
            if (c > 0x7Fu) { non_ascii = true; break; }
        }
        if (!non_ascii && en.size() < 3) return false;
        if (en.rfind("resource:", 0) == 0 ||
            en.rfind("scratchpad:", 0) == 0 ||
            en.rfind("http:", 0) == 0 ||
            en.rfind("https:", 0) == 0 ||
            en[0] == '/' || en[0] == '.' || en[0] == '%') {
            return false;
        }
        return true;
    }

    static uint64_t parse_hex_u64(const std::string& s) {
        if (s.empty()) return 0;
        return static_cast<uint64_t>(std::strtoull(s.c_str(), nullptr, 16));
    }

    static bool entry_has_zh(const JsonValue& zh_val) {
        if (zh_val.type == JsonValue::Type::String) {
            return !zh_val.str_val.empty();
        }
        if (zh_val.type == JsonValue::Type::Array) {
            for (const auto& el : zh_val.arr_val) {
                if (el.type == JsonValue::Type::String && !el.str_val.empty()) return true;
            }
        }
        return false;
    }

    void compile_stream_overlay(const JsonValue& st) {
        const JsonValue* entries_val = st.get("entries");
        if (!entries_val || entries_val->type != JsonValue::Type::Array || entries_val->arr_val.empty()) {
            return;
        }

        size_t st_zh_count = 0;
        for (const auto& e : entries_val->arr_val) {
            if (e.type != JsonValue::Type::Object) continue;
            if (const JsonValue* zv = e.get("zh"); zv && entry_has_zh(*zv)) {
                ++st_zh_count;
            }
        }
        if (st_zh_count == 0) return;

        std::string fmt = st.get_str("format", "fixed");
        uint64_t jp_hash = parse_hex_u64(st.get_str("jp_fnv1a"));
        uint64_t en_hash = parse_hex_u64(st.get_str("en_fnv1a"));
        size_t orig_len = static_cast<size_t>(std::max<int64_t>(0, st.get_int("orig_len", 0)));
        size_t en_orig_len = static_cast<size_t>(std::max<int64_t>(0, st.get_int("en_orig_len", 0)));

        std::vector<uint8_t> compiled;
        if (fmt == "fixed") {
            size_t line_bytes = static_cast<size_t>(std::max<int64_t>(1, st.get_int("line_bytes", 30)));
            size_t lpr = static_cast<size_t>(std::max<int64_t>(1, st.get_int("lines_per_record", 1)));

            size_t total_lines = 0;
            for (const auto& e : entries_val->arr_val) {
                if (e.type != JsonValue::Type::Object) continue;
                const JsonValue* jv = e.get("jp");
                if (lpr == 1) {
                    total_lines += 1;
                } else if (jv && jv->type == JsonValue::Type::Array && !jv->arr_val.empty()) {
                    total_lines += jv->arr_val.size();
                } else {
                    total_lines += lpr;
                }
            }

            size_t buf_len = std::max(orig_len, total_lines * line_bytes);
            compiled.assign(buf_len, 0);

            size_t line_slot = 0;
            for (const auto& e : entries_val->arr_val) {
                if (e.type != JsonValue::Type::Object) continue;
                const JsonValue* jv = e.get("jp");
                const JsonValue* zv = e.get("zh");
                bool use_zh = (zv && entry_has_zh(*zv));

                if (lpr == 1) {
                    std::string line_str = use_zh
                        ? ((zv->type == JsonValue::Type::String) ? zv->str_val : "")
                        : ((jv && jv->type == JsonValue::Type::String) ? jv->str_val : "");
                    auto enc = encode_text_to_bytes(line_str, line_bytes);
                    if (!enc.empty() && line_slot * line_bytes + enc.size() <= compiled.size()) {
                        std::memcpy(compiled.data() + line_slot * line_bytes, enc.data(), enc.size());
                    }
                    ++line_slot;
                } else {
                    size_t rec_lines = (jv && jv->type == JsonValue::Type::Array && !jv->arr_val.empty())
                        ? jv->arr_val.size()
                        : lpr;
                    for (size_t l = 0; l < rec_lines; ++l) {
                        std::string line_str;
                        if (use_zh) {
                            if (zv->type == JsonValue::Type::Array && l < zv->arr_val.size() &&
                                zv->arr_val[l].type == JsonValue::Type::String) {
                                line_str = zv->arr_val[l].str_val;
                            }
                        } else {
                            if (jv && jv->type == JsonValue::Type::Array && l < jv->arr_val.size() &&
                                jv->arr_val[l].type == JsonValue::Type::String) {
                                line_str = jv->arr_val[l].str_val;
                            }
                        }
                        auto enc = encode_text_to_bytes(line_str, line_bytes);
                        if (!enc.empty() && line_slot * line_bytes + enc.size() <= compiled.size()) {
                            std::memcpy(compiled.data() + line_slot * line_bytes, enc.data(), enc.size());
                        }
                        ++line_slot;
                    }
                }
            }
        } else if (fmt == "crlf") {
            bool trailing_crlf = st.get_bool("trailing_crlf", false);
            for (size_t i = 0; i < entries_val->arr_val.size(); ++i) {
                const auto& e = entries_val->arr_val[i];
                if (e.type != JsonValue::Type::Object) continue;
                std::string txt = e.get_str("zh");
                if (txt.empty()) txt = e.get_str("jp");
                auto enc = encode_text_to_bytes(txt);
                compiled.insert(compiled.end(), enc.begin(), enc.end());
                if (i + 1 < entries_val->arr_val.size() || trailing_crlf) {
                    compiled.push_back('\r');
                    compiled.push_back('\n');
                }
            }
        } else {
            return;
        }

        translated_count_ += st_zh_count;
        if (jp_hash != 0) {
            stream_overlays_[jp_hash] = compiled;
        }
        if (en_hash != 0 && en_hash != jp_hash) {
            if (fmt == "fixed" && en_orig_len > compiled.size()) {
                std::vector<uint8_t> en_buf = compiled;
                en_buf.resize(en_orig_len, 0);
                stream_overlays_[en_hash] = std::move(en_buf);
            } else {
                stream_overlays_[en_hash] = std::move(compiled);
            }
        }
    }

    bool try_cp_to_cp932(uint32_t cp, uint8_t& b0, uint8_t& b1, int& out_len) {
        if (cp < 0x80u) {
            b0 = static_cast<uint8_t>(cp);
            b1 = 0;
            out_len = 1;
            return true;
        }
        if (cp >= 0xFF61u && cp <= 0xFF9Fu) {
            b0 = static_cast<uint8_t>(0xA1u + (cp - 0xFF61u));
            b1 = 0;
            out_len = 1;
            return true;
        }

        auto cit = cp932_cache_.find(cp);
        if (cit != cp932_cache_.end()) {
            uint32_t packed = cit->second;
            if (packed == 0xFFFFFFFFu) return false;
            b0 = static_cast<uint8_t>((packed >> 8) & 0xFFu);
            b1 = static_cast<uint8_t>(packed & 0xFFu);
            out_len = 2;
            return true;
        }

        uint8_t tmp[4] = {0, 0, 0, 0};
        int conv_len = 0;
#ifdef _WIN32
        std::string u8;
        append_cp_utf8(u8, cp);
        wchar_t wbuf[4] = {0};
        int wlen = MultiByteToWideChar(CP_UTF8, 0, u8.data(), static_cast<int>(u8.size()), wbuf, 4);
        if (wlen > 0) {
            BOOL used_def = FALSE;
            int clen = WideCharToMultiByte(
                932, WC_NO_BEST_FIT_CHARS, wbuf, wlen,
                reinterpret_cast<char*>(tmp), 4, nullptr, &used_def);
            if (clen == 2 && !used_def) {
                conv_len = 2;
            }
        }
#else
        std::string u8;
        append_cp_utf8(u8, cp);
        iconv_t cd = iconv_open("CP932", "UTF-8");
        if (cd != reinterpret_cast<iconv_t>(-1)) {
            char* inbuf = const_cast<char*>(u8.data());
            size_t inbytes = u8.size();
            char* outbuf = reinterpret_cast<char*>(tmp);
            size_t outbytes = sizeof(tmp);
            size_t rc = iconv(cd, &inbuf, &inbytes, &outbuf, &outbytes);
            iconv_close(cd);
            if (rc != static_cast<size_t>(-1) && inbytes == 0) {
                size_t written = sizeof(tmp) - outbytes;
                if (written == 2) {
                    conv_len = 2;
                }
            }
        }
#endif
        if (conv_len == 2 &&
            is_sjis_lead(tmp[0]) &&
            tmp[1] >= 0x40u && tmp[1] != 0x7Fu && tmp[1] <= 0xFCu) {
            b0 = tmp[0];
            b1 = tmp[1];
            out_len = 2;
            cp932_cache_[cp] = (static_cast<uint32_t>(b0) << 8) | static_cast<uint32_t>(b1);
            return true;
        }
        cp932_cache_[cp] = 0xFFFFFFFFu;
        return false;
    }

    bool encode_codepoint_superset(uint32_t cp, uint8_t& b0, uint8_t& b1, int& out_len) {
        if (try_cp_to_cp932(cp, b0, b1, out_len)) {
            return true;
        }
        auto it = ext_cp_to_pair_.find(cp);
        if (it != ext_cp_to_pair_.end()) {
            b0 = static_cast<uint8_t>((it->second >> 8) & 0xFFu);
            b1 = static_cast<uint8_t>(it->second & 0xFFu);
            out_len = 2;
            return true;
        }

        // Allocate deterministic slot from 18 extended lead bytes x 190 trail bytes (0x40..0x7E, 0x80..0xFE) = 3,420 slots
        static constexpr uint32_t kNumTrails = 190;
        static constexpr uint32_t kMaxSlots = 18u * kNumTrails;
        if (next_ext_slot_ >= kMaxSlots) return false;

        uint32_t slot = next_ext_slot_++;
        uint8_t lb = kExtLeadBytes[slot / kNumTrails];
        uint32_t t_idx = slot % kNumTrails;
        uint8_t tb = static_cast<uint8_t>((t_idx < 63u) ? (0x40u + t_idx) : (0x80u + (t_idx - 63u)));
        uint16_t pair = (static_cast<uint16_t>(lb) << 8) | static_cast<uint16_t>(tb);
        ext_cp_to_pair_[cp] = pair;
        ext_pair_to_cp_[pair] = cp;
        b0 = lb;
        b1 = tb;
        out_len = 2;
        return true;
    }
};

} // namespace rmexe::loc
