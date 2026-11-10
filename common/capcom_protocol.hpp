#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rmexe::net {

inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_nibble(s[i + 1]);
            int lo = hex_nibble(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(s[i]);
            }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

inline std::string bytes_to_hex(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    if (len == 0) return "-";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[(data[i] >> 4) & 0xF]);
        out.push_back(hex[data[i] & 0xF]);
    }
    return out;
}

inline std::vector<uint8_t> hex_to_bytes(const std::string& s) {
    if (s.empty() || s == "-") return {};
    std::vector<uint8_t> out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = hex_nibble(s[i]);
        int lo = hex_nibble(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

// Extracts path + query (starting with '/') from a full http(s)://host/path?query URL or relative path
inline std::string extract_target_uri(const std::string& url) {
    if (url.empty()) return "/";
    if (url[0] == '/') return url;
    auto scheme = url.find("://");
    size_t host_start = (scheme == std::string::npos) ? 0 : scheme + 3;
    auto slash = url.find('/', host_start);
    if (slash == std::string::npos) {
        auto q = url.find('?', host_start);
        return (q == std::string::npos) ? "/" : ("/" + url.substr(q));
    }
    return url.substr(slash);
}

inline std::string extract_path_only(const std::string& url) {
    std::string uri = extract_target_uri(url);
    auto q = uri.find('?');
    return (q == std::string::npos) ? uri : uri.substr(0, q);
}

inline std::unordered_map<std::string, std::string> parse_query_params(const std::string& url) {
    std::unordered_map<std::string, std::string> params;
    auto q = url.find('?');
    if (q == std::string::npos) return params;
    std::string qs = url.substr(q + 1);
    size_t pos = 0;
    while (pos <= qs.size()) {
        auto amp = qs.find('&', pos);
        std::string pair = (amp == std::string::npos) ? qs.substr(pos) : qs.substr(pos, amp - pos);
        if (!pair.empty()) {
            auto eq = pair.find('=');
            if (eq == std::string::npos) {
                params[url_decode(pair)] = "";
            } else {
                params[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return params;
}

inline void push_be32(std::vector<uint8_t>& buf, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    buf.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(u & 0xFF));
}

inline void push_be16(std::vector<uint8_t>& buf, uint16_t u) {
    buf.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(u & 0xFF));
}

inline void push_le16(std::vector<uint8_t>& buf, uint16_t u) {
    buf.push_back(static_cast<uint8_t>(u & 0xFF));
    buf.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
}

inline int32_t current_yyyymmdd() {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
}

// ============================================================================
// Authentic 240-byte Seasonal Virus Busting Ranking Course Generator (Months 1..12)
// ============================================================================
inline std::vector<uint8_t> build_rank_course(int month) {
    struct WaveSpec {
        uint32_t ene[3];
        uint32_t map[3];
    };
    // 10 verified PoN Virus Busting wave & panel-layout definitions extracted from PoN.sp
    static constexpr WaveSpec kBaseWaves[10] = {
        {{0x00003200u, 0x00004100u, 0x00005000u}, {0x00222222u, 0x00222222u, 0x00222222u}},
        {{0x00003100u, 0x00004000u, 0x00005104u}, {0x00322232u, 0x00222222u, 0x00232223u}},
        {{0x0000310cu, 0x00004000u, 0x0000520cu}, {0x00220222u, 0x00222222u, 0x00222022u}},
        {{0x00003201u, 0x0000512cu, 0x00004001u}, {0x00022220u, 0x00222222u, 0x00022220u}},
        {{0x00004008u, 0x00004208u, 0x00000000u}, {0x00266662u, 0x00266662u, 0x00266662u}},
        {{0x00003101u, 0x00004001u, 0x00005240u}, {0x00222222u, 0x00222222u, 0x00222222u}},
        {{0x00004009u, 0x00004209u, 0x00000000u}, {0x00252252u, 0x00555555u, 0x00252252u}},
        {{0x00005020u, 0x00004220u, 0x00003001u}, {0x00223322u, 0x00222222u, 0x00223322u}},
        {{0x0000401eu, 0x00003102u, 0x00004215u}, {0x00222a22u, 0x00222222u, 0x00229222u}},
        {{0x00003043u, 0x00004116u, 0x0000521eu}, {0x00222222u, 0x00222222u, 0x00222222u}},
    };

    int m = std::clamp(month, 1, 12) - 1;
    // Rotate early/mid/late tiers by month while keeping progressive difficulty (boss/elite wave at 10)
    int perm[10];
    for (int i = 0; i < 4; ++i) perm[i] = (i + m) % 4;
    for (int i = 0; i < 4; ++i) perm[4 + i] = 4 + ((i + m) % 4);
    perm[8] = (m % 2 == 0) ? 8 : 7;
    perm[9] = (m % 3 == 2) ? 8 : 9;

    std::vector<uint8_t> out;
    out.reserve(240);
    // First 120 bytes: r_ene[0..9][0..2]
    for (int w = 0; w < 10; ++w) {
        for (int s = 0; s < 3; ++s) {
            push_be32(out, static_cast<int32_t>(kBaseWaves[perm[w]].ene[s]));
        }
    }
    // Next 120 bytes: r_map[0..9][0..2]
    for (int w = 0; w < 10; ++w) {
        for (int r = 0; r < 3; ++r) {
            push_be32(out, static_cast<int32_t>(kBaseWaves[perm[w]].map[r]));
        }
    }
    return out;
}

inline std::array<uint16_t, 30> default_starter_folder(int variant = 0) {
    static constexpr std::array<uint16_t, 30> kFolderA = {
        0x0003, 0x0002, 0x002c, 0x0004, 0x002c, 0x002c, 0x009c, 0x0098, 0x00a0, 0x009c,
        0x00cc, 0x00cc, 0x00d7, 0x00d7, 0x0145, 0x0145, 0x0145, 0x0145, 0x0152, 0x0152,
        0x015f, 0x015e, 0x0181, 0x0161, 0x01b8, 0x01b8, 0x01d1, 0x01b8, 0x01dd, 0x01d5
    };
    static constexpr std::array<uint16_t, 30> kFolderB = {
        0x0002, 0x0002, 0x0003, 0x0003, 0x001c, 0x001c, 0x001e, 0x001c, 0x001e, 0x001e,
        0x0024, 0x0024, 0x0098, 0x0098, 0x009c, 0x0098, 0x005e, 0x009c, 0x005f, 0x005e,
        0x0152, 0x005f, 0x0181, 0x0152, 0x015b, 0x015b, 0x015c, 0x015c, 0x01b8, 0x01b8
    };
    return (variant % 2 == 0) ? kFolderA : kFolderB;
}

struct HttpResponse {
    int status_code = 200;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    std::string get_header(const std::string& key) const {
        for (const auto& kv : headers) {
            if (iequals(kv.first, key)) return kv.second;
        }
        return "";
    }
};

// ============================================================================
// Persistent Capcom i-mode Server & Offline Fallback Simulation Engine
// ============================================================================
class CapcomServiceState {
public:
    struct PlayerAccount {
        std::string uid;
        std::string name = "ROCKMAN";
        uint16_t site_points = 5000;
        std::vector<uint8_t> save_pon; // 1748 bytes
        std::vector<uint8_t> save_lon; // 2192 bytes
    };

    struct LeaderboardEntry {
        int32_t player_id = 0;
        int32_t best_score = 0;
        std::string uid;
        std::string name; // ASCII / CP932-compatible (up to 12 bytes)
        std::array<uint16_t, 30> folder{};
    };

    CapcomServiceState() {
        seed_default_leaderboard();
    }

    void set_db_path(const std::string& path) {
        std::lock_guard<std::mutex> lk(mtx_);
        db_path_ = path;
        load_unlocked();
    }

    // Register raw ScratchPad bytes (excluding 64-byte SDK header) for dataget.php asset slicing
    void register_sp_image(bool is_lon, const uint8_t* raw_sp_no_hdr, size_t len) {
        if (!raw_sp_no_hdr || len == 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (is_lon) {
            lon_sp_.assign(raw_sp_no_hdr, raw_sp_no_hdr + len);
        } else {
            pon_sp_.assign(raw_sp_no_hdr, raw_sp_no_hdr + len);
        }
    }

    bool load_sp_file(bool is_lon, const std::string& sp_file_path) {
        std::ifstream f(sp_file_path, std::ios::binary);
        if (!f) return false;
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t hdr = (raw.size() % 100 == 64) ? 64 : 0;
        if (raw.size() <= hdr) return false;
        register_sp_image(is_lon, raw.data() + hdr, raw.size() - hdr);
        return true;
    }

    HttpResponse handle_request(
        const std::string& method,
        const std::string& url,
        const std::unordered_map<std::string, std::string>& req_headers,
        const std::vector<uint8_t>& req_body
    ) {
        std::lock_guard<std::mutex> lk(mtx_);

        std::string path = extract_path_only(url);
        auto params = parse_query_params(url);

        auto find_hdr = [&](const std::string& k) -> std::string {
            for (const auto& kv : req_headers) {
                if (iequals(kv.first, k)) return kv.second;
            }
            return "";
        };

        HttpResponse resp;
        resp.status_code = 200;
        resp.headers["Content-Type"] = "application/octet-stream";
        resp.headers["X-CAPCOM-STATUS"] = "OK";

        // 1. CpDataDownload.getDataEx -> dataget.php
        if (path.find("dataget.php") != std::string::npos) {
            std::string asset_path = params.count("path") ? params["path"] : "";
            size_t offset = params.count("offset") ? static_cast<size_t>(std::max(0, std::atoi(params["offset"].c_str()))) : 0;
            size_t length = params.count("length") ? static_cast<size_t>(std::max(1, std::atoi(params["length"].c_str()))) : 128000;

            std::vector<uint8_t> full_asset;
            if (asset_path.find("data/rank/") != std::string::npos) {
                int month = 1;
                auto rpos = asset_path.rfind("data/rank/");
                if (rpos != std::string::npos) {
                    month = std::atoi(asset_path.substr(rpos + 10).c_str());
                }
                if (month < 1 || month > 12) {
                    month = (current_yyyymmdd() / 100) % 100;
                }
                full_asset = build_rank_course(month);
            } else if (asset_path.find("data/mld/mld") != std::string::npos) {
                bool is_lon = (asset_path.find("exe2") != std::string::npos ||
                               asset_path.find("exelon") != std::string::npos);
                const auto& sp = is_lon ? lon_sp_ : pon_sp_;
                size_t mld_off = 420624;
                size_t mld_len = is_lon ? 22474 : 19841;
                if (sp.size() >= mld_off + mld_len) {
                    full_asset.assign(sp.begin() + mld_off, sp.begin() + mld_off + mld_len);
                }
            } else if (asset_path.find("data/data") != std::string::npos) {
                bool is_lon = (asset_path.find("exe2") != std::string::npos ||
                               asset_path.find("exelon") != std::string::npos);
                const auto& sp = is_lon ? lon_sp_ : pon_sp_;
                size_t dat_off = 3396;
                size_t dat_len = is_lon ? 364096 : 365281;
                if (sp.size() >= dat_off + dat_len) {
                    full_asset.assign(sp.begin() + dat_off, sp.begin() + dat_off + dat_len);
                }
            }

            if (offset < full_asset.size()) {
                size_t n = std::min(length, full_asset.size() - offset);
                resp.body.assign(full_asset.begin() + offset, full_asset.begin() + offset + n);
            }
            resp.headers["Content-Length"] = std::to_string(resp.body.size());
            return resp;
        }

        // 2. Capcom i-mode CGI / Cloud Save / Leaderboard endpoints (isr.php & iexe_score.php)
        std::string uid = find_hdr("X-RMEXE-UID");
        if (uid.empty() && params.count("uid") && params["uid"] != "NULLGWDOCOMO" && !params["uid"].empty()) {
            uid = params["uid"];
        }
        if (uid.empty()) uid = "LOCAL_PLAYER";

        std::string pname = url_decode(find_hdr("X-RMEXE-NAME"));
        PlayerAccount& acct = get_or_create_account(uid, pname);

        std::string app_k = params.count("k") ? params["k"] : "";
        if (app_k.empty()) {
            if (path.find("exelon") != std::string::npos || path.find("exe2") != std::string::npos ||
                (params.count("b0") && params["b0"] == "2192")) {
                app_k = "4421";
            } else {
                app_k = "4420";
            }
        }
        bool is_lon = (app_k == "4421");
        std::string ty = params.count("ty") ? params["ty"] : "";

        if (ty.empty()) {
            // CGI(0, 0) & CGI(5, 0): Server Date YYYYMMDD
            resp.body.push_back(0x01);
            push_be32(resp.body, current_yyyymmdd());
        } else if (ty == "buyreg") {
            // CGI(4, 0): Game registration / unlock verification
            resp.body = {0x01, 0x00};
        } else if (ty == "scck") {
            // CGI(1, sc): Episodic scenario availability check (1 = unlocked immediately)
            resp.body = {0x01, 0x00, 0x65};
        } else if (ty == "pts") {
            // CGI(2, qt): Site point balance / consumption
            int qt = params.count("qt") ? std::max(0, std::atoi(params["qt"].c_str())) : 0;
            if (acct.site_points < static_cast<uint16_t>(qt) || acct.site_points < 500) {
                acct.site_points = 5000; // Auto-replenish Capcom portal points
            }
            acct.site_points = static_cast<uint16_t>(acct.site_points - qt);
            save_unlocked();
            resp.body.push_back(0x01);
            push_be16(resp.body, acct.site_points);
        } else if (ty == "fg" || ty == "exefg") {
            // CGI(3, idx) & CGI(6, idx): Rival 30-chip folder download (1 status byte + 60 LE bytes)
            int32_t target_id = params.count("x0") ? std::atoi(params["x0"].c_str()) : 0;
            std::array<uint16_t, 30> folder = default_starter_folder(0);
            for (const auto& e : leaderboard_) {
                if (e.player_id == target_id) {
                    folder = e.folder;
                    break;
                }
            }
            resp.body.push_back(0x01);
            for (uint16_t chip : folder) {
                push_le16(resp.body, chip);
            }
        } else if (ty == "save" || (iequals(method, "POST") && req_body.size() >= 1700)) {
            // DataSave: Cloud Save Upload (1748 bytes for PoN, 2192 bytes for LoN)
            if (!req_body.empty()) {
                if (is_lon || req_body.size() == 2192) {
                    acct.save_lon = req_body;
                } else {
                    acct.save_pon = req_body;
                }
                save_unlocked();
            }
            // MUST return http_error == 3 and zan == 3!
            resp.body = {0x03, 0x03};
        } else if (ty == "load") {
            // Load(1) / Load(2): Cloud Save Download
            size_t expected_b0 = params.count("b0")
                ? static_cast<size_t>(std::max(0, std::atoi(params["b0"].c_str())))
                : (is_lon ? 2192u : 1748u);
            if (expected_b0 == 0) expected_b0 = is_lon ? 2192u : 1748u;

            const auto& saved = (is_lon || expected_b0 == 2192u) ? acct.save_lon : acct.save_pon;
            if (saved.empty()) {
                // 140 (0x8C) = "No save data stored on server"
                resp.body = {0x8C, 0x00};
            } else {
                resp.body.reserve(2 + expected_b0);
                resp.body.push_back(0x03); // http_error == 3 (success)
                resp.body.push_back(0x03); // zan == 3
                for (size_t i = 0; i < expected_b0; ++i) {
                    resp.body.push_back(i < saved.size() ? saved[i] : 0);
                }
            }
        } else if (ty == "exesr") {
            // RankUp(): PoN Virus Busting Score Upload & 269-byte Top-10 Leaderboard Response
            int32_t score = params.count("s") ? std::max(0, std::atoi(params["s"].c_str())) : 0;
            std::array<uint16_t, 30> submitted_folder = default_starter_folder(0);
            if (req_body.size() >= 60) {
                for (size_t i = 0; i < 30; ++i) {
                    submitted_folder[i] = static_cast<uint16_t>(req_body[i * 2]) |
                                          (static_cast<uint16_t>(req_body[i * 2 + 1]) << 8);
                }
            }

            LeaderboardEntry* player_entry = nullptr;
            for (auto& e : leaderboard_) {
                if (e.uid == uid) {
                    player_entry = &e;
                    break;
                }
            }
            if (!player_entry) {
                LeaderboardEntry ne{};
                ne.player_id = next_player_id_++;
                ne.best_score = score;
                ne.uid = uid;
                ne.name = acct.name.empty() ? "ROCKMAN" : acct.name;
                ne.folder = submitted_folder;
                leaderboard_.push_back(ne);
            } else {
                if (!acct.name.empty()) player_entry->name = acct.name;
                if (score >= player_entry->best_score) {
                    player_entry->best_score = score;
                    player_entry->folder = submitted_folder;
                }
            }

            sort_leaderboard();
            save_unlocked();

            int32_t player_rank = 1;
            int32_t player_best = score;
            for (size_t i = 0; i < leaderboard_.size(); ++i) {
                if (leaderboard_[i].uid == uid) {
                    player_rank = static_cast<int32_t>(i + 1);
                    player_best = leaderboard_[i].best_score;
                    break;
                }
            }

            resp.body.reserve(269);
            resp.body.push_back(0x01); // http_error == 1
            push_be32(resp.body, player_rank);
            push_be32(resp.body, player_best);

            // Top 10 scores (40 bytes)
            for (size_t i = 0; i < 10; ++i) {
                int32_t sc = (i < leaderboard_.size()) ? leaderboard_[i].best_score : 0;
                push_be32(resp.body, sc);
            }
            // Top 10 names (10 * 12 = 120 bytes, ASCII/CP932 zero-padded)
            for (size_t i = 0; i < 10; ++i) {
                std::string nm = (i < leaderboard_.size()) ? leaderboard_[i].name : "---";
                for (size_t c = 0; c < 12; ++c) {
                    resp.body.push_back(c < nm.size() ? static_cast<uint8_t>(nm[c]) : 0);
                }
            }
            // Top 10 player IDs (40 bytes)
            for (size_t i = 0; i < 10; ++i) {
                int32_t pid = (i < leaderboard_.size()) ? leaderboard_[i].player_id : 0;
                push_be32(resp.body, pid);
            }
            // Rival / Reward Folder fol[3] (60 bytes LE)
            size_t rival_idx = (leaderboard_.size() > 1 && leaderboard_[0].uid == uid) ? 1 : 0;
            const auto& reward_fol = (rival_idx < leaderboard_.size())
                ? leaderboard_[rival_idx].folder
                : submitted_folder;
            for (uint16_t chip : reward_fol) {
                push_le16(resp.body, chip);
            }
        } else {
            // Generic OK fallback
            resp.body = {0x01, 0x00};
        }

        resp.headers["Content-Length"] = std::to_string(resp.body.size());
        return resp;
    }

    size_t account_count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return accounts_.size();
    }

private:
    mutable std::mutex mtx_;
    std::string db_path_;
    std::unordered_map<std::string, PlayerAccount> accounts_;
    std::vector<LeaderboardEntry> leaderboard_;
    int32_t next_player_id_ = 2001;
    std::vector<uint8_t> pon_sp_;
    std::vector<uint8_t> lon_sp_;

    PlayerAccount& get_or_create_account(const std::string& uid, const std::string& name) {
        auto it = accounts_.find(uid);
        if (it == accounts_.end()) {
            PlayerAccount a;
            a.uid = uid;
            if (!name.empty()) a.name = name.substr(0, 12);
            it = accounts_.emplace(uid, std::move(a)).first;
        } else if (!name.empty()) {
            it->second.name = name.substr(0, 12);
        }
        return it->second;
    }

    void sort_leaderboard() {
        std::stable_sort(leaderboard_.begin(), leaderboard_.end(),
            [](const LeaderboardEntry& a, const LeaderboardEntry& b) {
                return a.best_score > b.best_score;
            });
    }

    void seed_default_leaderboard() {
        struct SeedNpc {
            int32_t id;
            int32_t score;
            const char* name;
            int fol_var;
        };
        static constexpr SeedNpc kNpcs[10] = {
            {1001, 9200, "PROTOMAN", 0},
            {1002, 8500, "COLONEL",  1},
            {1003, 7800, "SEARCHMN", 0},
            {1004, 7100, "CHAUD",    1},
            {1005, 6400, "ROLL",     0},
            {1006, 5600, "GUTSMAN",  1},
            {1007, 4800, "FIREMAN",  0},
            {1008, 3900, "NUMBERMN", 1},
            {1009, 3100, "GLYDE",    0},
            {1010, 2200, "MAYL",     1},
        };
        leaderboard_.clear();
        for (const auto& npc : kNpcs) {
            LeaderboardEntry e{};
            e.player_id = npc.id;
            e.best_score = npc.score;
            e.uid = "NPC_" + std::to_string(npc.id);
            e.name = npc.name;
            e.folder = default_starter_folder(npc.fol_var);
            leaderboard_.push_back(e);
        }
    }

    void load_unlocked() {
        if (db_path_.empty()) return;
        std::ifstream f(db_path_);
        if (!f) return;

        std::vector<LeaderboardEntry> loaded_lb;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream iss(line);
            std::string tag;
            iss >> tag;
            if (tag == "NEXT_ID") {
                iss >> next_player_id_;
            } else if (tag == "ACCOUNT") {
                std::string uid, name_hex, pon_hex, lon_hex;
                uint16_t pts = 5000;
                if (iss >> uid >> pts >> name_hex >> pon_hex >> lon_hex) {
                    PlayerAccount a;
                    a.uid = uid;
                    a.site_points = pts;
                    auto nb = hex_to_bytes(name_hex);
                    a.name.assign(reinterpret_cast<const char*>(nb.data()), nb.size());
                    a.save_pon = hex_to_bytes(pon_hex);
                    a.save_lon = hex_to_bytes(lon_hex);
                    accounts_[uid] = std::move(a);
                }
            } else if (tag == "RANK") {
                LeaderboardEntry e{};
                std::string name_hex, fol_hex;
                if (iss >> e.player_id >> e.best_score >> e.uid >> name_hex >> fol_hex) {
                    auto nb = hex_to_bytes(name_hex);
                    e.name.assign(reinterpret_cast<const char*>(nb.data()), nb.size());
                    auto fb = hex_to_bytes(fol_hex);
                    e.folder = default_starter_folder(0);
                    if (fb.size() >= 60) {
                        for (size_t i = 0; i < 30; ++i) {
                            e.folder[i] = static_cast<uint16_t>(fb[i * 2]) |
                                          (static_cast<uint16_t>(fb[i * 2 + 1]) << 8);
                        }
                    }
                    loaded_lb.push_back(e);
                    if (e.player_id >= next_player_id_) next_player_id_ = e.player_id + 1;
                }
            }
        }
        if (!loaded_lb.empty()) {
            leaderboard_ = std::move(loaded_lb);
            sort_leaderboard();
        }
    }

    void save_unlocked() const {
        if (db_path_.empty()) return;
        std::ofstream f(db_path_, std::ios::trunc);
        if (!f) return;
        f << "# rmexe Capcom i-mode persistent service state\n";
        f << "NEXT_ID " << next_player_id_ << "\n";
        for (const auto& [uid, a] : accounts_) {
            f << "ACCOUNT " << uid << " " << a.site_points << " "
              << bytes_to_hex(reinterpret_cast<const uint8_t*>(a.name.data()), a.name.size()) << " "
              << bytes_to_hex(a.save_pon.data(), a.save_pon.size()) << " "
              << bytes_to_hex(a.save_lon.data(), a.save_lon.size()) << "\n";
        }
        for (const auto& e : leaderboard_) {
            std::vector<uint8_t> fb;
            fb.reserve(60);
            for (uint16_t c : e.folder) push_le16(fb, c);
            f << "RANK " << e.player_id << " " << e.best_score << " " << e.uid << " "
              << bytes_to_hex(reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size()) << " "
              << bytes_to_hex(fb.data(), fb.size()) << "\n";
        }
    }
};

// ============================================================================
// Lightweight POSIX TCP HTTP/1.1 Client (Zero External Dependencies)
// ============================================================================
inline bool parse_host_port(const std::string& input, std::string& host_out, uint16_t& port_out, uint16_t default_port = 8765) {
    std::string s = input;
    auto scheme = s.find("://");
    if (scheme != std::string::npos) {
        s = s.substr(scheme + 3);
    }
    auto slash = s.find('/');
    if (slash != std::string::npos) {
        s = s.substr(0, slash);
    }
    if (s.empty()) return false;

    auto colon = s.rfind(':');
    if (colon != std::string::npos) {
        host_out = s.substr(0, colon);
        int p = std::atoi(s.substr(colon + 1).c_str());
        port_out = (p > 0 && p <= 65535) ? static_cast<uint16_t>(p) : default_port;
    } else {
        host_out = s;
        port_out = default_port;
    }
    return !host_out.empty();
}

inline bool http_request_tcp(
    const std::string& server_host_port,
    const std::string& method,
    const std::string& target_url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::vector<uint8_t>& body,
    HttpResponse& resp_out,
    int timeout_ms = 2500
) {
    std::string host;
    uint16_t port = 8765;
    if (!parse_host_port(server_host_port, host, port)) return false;

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }

    int fd = -1;
    for (struct addrinfo* rp = res; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = ::connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, timeout_ms) > 0) {
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen);
                if (so_err == 0) {
                    fcntl(fd, F_SETFL, flags);
                    break;
                }
            }
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;

    std::string uri = extract_target_uri(target_url);
    std::ostringstream req;
    req << method << " " << uri << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    for (const auto& [k, v] : headers) {
        if (!iequals(k, "Host") && !iequals(k, "Connection") && !iequals(k, "Content-Length")) {
            req << k << ": " << v << "\r\n";
        }
    }
    req << "\r\n";
    std::string head_str = req.str();

    auto write_all = [&](const uint8_t* ptr, size_t len) -> bool {
        size_t sent = 0;
        while (sent < len) {
            struct pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, timeout_ms) <= 0) return false;
            ssize_t n = ::send(fd, ptr + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    };

    if (!write_all(reinterpret_cast<const uint8_t*>(head_str.data()), head_str.size()) ||
        (!body.empty() && !write_all(body.data(), body.size()))) {
        ::close(fd);
        return false;
    }

    std::vector<uint8_t> raw;
    raw.reserve(4096);
    uint8_t buf[4096];
    while (true) {
        struct pollfd pfd{fd, POLLIN, 0};
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) break;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.insert(raw.end(), buf, buf + n);
    }
    ::close(fd);

    if (raw.size() < 12) return false;
    std::string_view sv(reinterpret_cast<const char*>(raw.data()), raw.size());
    auto hdr_end = sv.find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) return false;

    resp_out = HttpResponse{};
    std::string hdr_block(sv.substr(0, hdr_end));
    std::istringstream hss(hdr_block);
    std::string status_line;
    std::getline(hss, status_line);
    auto sp1 = status_line.find(' ');
    if (sp1 != std::string::npos) {
        resp_out.status_code = std::atoi(status_line.substr(sp1 + 1).c_str());
    }

    std::string hline;
    while (std::getline(hss, hline)) {
        if (!hline.empty() && hline.back() == '\r') hline.pop_back();
        auto col = hline.find(':');
        if (col != std::string::npos) {
            std::string hk = hline.substr(0, col);
            std::string hv = hline.substr(col + 1);
            while (!hv.empty() && hv.front() == ' ') hv.erase(hv.begin());
            resp_out.headers[hk] = hv;
        }
    }

    size_t body_start = hdr_end + 4;
    resp_out.body.assign(raw.begin() + body_start, raw.end());
    return resp_out.status_code > 0;
}

} // namespace rmexe::net
