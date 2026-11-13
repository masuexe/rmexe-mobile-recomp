#pragma once

#include "capcom_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace rmexe::pvp {

enum PvpPktType : uint8_t {
    PKT_BIND          = 1,
    PKT_BIND_ACK      = 2,
    PKT_MATCH_START   = 3,
    PKT_PING          = 4,
    PKT_PONG          = 5,
    PKT_FRAME_INPUT   = 6,
    PKT_BATTLE_STATE  = 7,
    PKT_DISCONNECT    = 8,
    PKT_BUILD_SYNC    = 9,
    PKT_BUILD_ACK     = 10,
    PKT_CUSTOM_COMMIT = 11,
    PKT_CUSTOM_ACK    = 12,
    PKT_INPUT_BUNDLE  = 13,
    PKT_INPUT_REQ     = 14,
    PKT_MATCH_END     = 15,
};

constexpr uint8_t PVP_FORMAT_SINGLE = 0;
constexpr uint8_t PVP_FORMAT_TRIPLE = 1;
constexpr uint8_t PVP_HDR_FLAG_TRIPLE = 0x01;
constexpr uint8_t PVP_HDR_FLAG_FMT_KNOWN = 0x02;

// Player's .sav Fighter Build (HP, Buster levels, Element, NaviCust skills, 30-chip Folder + Regular)
struct PvpFighterBuild {
    int32_t max_hp = 100;
    int32_t ata_lv = 1;       // Buster Attack 1..5
    int32_t cha_lv = 1;       // Buster Charge 1..5
    int32_t r_zoku = 0;       // 0=Null, 1=Heat, 2=Aqua, 3=Elec, 4=Wood
    int32_t regu_flg = 0;     // 1 if Regular Chip active
    int32_t regu_id = 0;      // Regular slot index 0..29 in folder
    int32_t skill_kouka[29]{};
    int32_t folder_packed[30]{}; // (tip_id << 8) | code_id
    std::string player_name;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out;
        out.reserve(276);
        net::push_be32(out, max_hp);
        net::push_be32(out, ata_lv);
        net::push_be32(out, cha_lv);
        net::push_be32(out, r_zoku);
        net::push_be32(out, regu_flg);
        net::push_be32(out, regu_id);
        for (int i = 0; i < 29; ++i) net::push_be32(out, skill_kouka[i]);
        for (int i = 0; i < 30; ++i) net::push_be32(out, folder_packed[i]);
        std::string nm = player_name.substr(0, 24);
        out.push_back(static_cast<uint8_t>(nm.size()));
        out.insert(out.end(), nm.begin(), nm.end());
        return out;
    }

    static bool deserialize(const uint8_t* d, size_t len, PvpFighterBuild& b) {
        if (!d || len < 261) return false;
        auto rd32 = [&](size_t off) -> int32_t {
            return static_cast<int32_t>(
                (static_cast<uint32_t>(d[off]) << 24) |
                (static_cast<uint32_t>(d[off + 1]) << 16) |
                (static_cast<uint32_t>(d[off + 2]) << 8) |
                static_cast<uint32_t>(d[off + 3]));
        };
        size_t p = 0;
        b.max_hp   = rd32(p); p += 4;
        b.ata_lv   = rd32(p); p += 4;
        b.cha_lv   = rd32(p); p += 4;
        b.r_zoku   = rd32(p); p += 4;
        b.regu_flg = rd32(p); p += 4;
        b.regu_id  = rd32(p); p += 4;
        for (int i = 0; i < 29; ++i) { b.skill_kouka[i] = rd32(p); p += 4; }
        for (int i = 0; i < 30; ++i) { b.folder_packed[i] = rd32(p); p += 4; }
        uint8_t nlen = d[p++];
        if (p + nlen > len) return false;
        b.player_name.assign(reinterpret_cast<const char*>(d + p), nlen);
        return true;
    }
};

// Custom Screen (BattleTip) locked-in chip queue for a given combat turn
struct PvpCustomCommit {
    uint16_t turn_no = 0;
    int8_t   tip_no = -1;       // -1 = empty/ADD, 0..8 = top index in rock.tip[]
    uint8_t  max_sel = 5;       // Updated draw size 5..10 (including ADD bonus)
    uint8_t  regu_flg = 0;      // Updated regular chip availability
    uint8_t  sec_cha = 0;       // Updated shuffle availability
    int16_t  pa_no = -1;        // >= 0 if Program Advance formed
    uint16_t rock_tip_ids[9]{}; // tip_id (0..255) in rock.tip[0..8]
    uint32_t fol_used_mask = 0; // Bitmask of used folder slots [0..29]

    static constexpr size_t kWireSize = 30;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out;
        out.reserve(kWireSize);
        net::push_be16(out, turn_no);
        out.push_back(static_cast<uint8_t>(tip_no));
        out.push_back(max_sel);
        out.push_back(regu_flg);
        out.push_back(sec_cha);
        net::push_be16(out, static_cast<uint16_t>(pa_no));
        for (int i = 0; i < 9; ++i) net::push_be16(out, rock_tip_ids[i]);
        net::push_be32(out, static_cast<int32_t>(fol_used_mask));
        return out;
    }

    static bool deserialize(const uint8_t* d, size_t len, PvpCustomCommit& c) {
        if (!d || len < kWireSize) return false;
        auto rd16 = [&](size_t off) -> uint16_t {
            return static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
        };
        auto rd32 = [&](size_t off) -> uint32_t {
            return (static_cast<uint32_t>(d[off]) << 24) |
                   (static_cast<uint32_t>(d[off + 1]) << 16) |
                   (static_cast<uint32_t>(d[off + 2]) << 8) |
                   static_cast<uint32_t>(d[off + 3]);
        };
        c.turn_no  = rd16(0);
        c.tip_no   = static_cast<int8_t>(d[2]);
        c.max_sel  = d[3];
        c.regu_flg = d[4];
        c.sec_cha  = d[5];
        c.pa_no    = static_cast<int16_t>(rd16(6));
        for (int i = 0; i < 9; ++i) c.rock_tip_ids[i] = rd16(8 + i * 2);
        c.fol_used_mask = rd32(26);
        return true;
    }
};

struct PvpRoomInfo {
    uint32_t room_id = 0;
    std::string game;
    std::string host;
    uint8_t match_format = PVP_FORMAT_SINGLE;
};

// 28-byte wire-safe network-order UDP packet header
struct PvpPacketHeader {
    uint8_t  version = 1;
    uint8_t  pkt_type = 0;
    uint8_t  slot = 0;         // 1 = Host (P1), 2 = Challenger (P2)
    uint8_t  flags = 0;
    uint32_t room_id = 0;
    uint32_t token = 0;
    uint32_t frame_or_seq = 0; // Frame index, RNG seed (MATCH_START), or ping timestamp
    int32_t  keypad_mask = 0;  // DoJa 32-bit keypad state
    uint16_t payload_len = 0;
    uint16_t checksum16 = 0;

    static constexpr size_t kWireSize = 28;

    std::vector<uint8_t> serialize(const uint8_t* payload = nullptr, size_t plen = 0) const {
        uint16_t capped = static_cast<uint16_t>(std::min<size_t>(plen, 1200));
        std::vector<uint8_t> buf;
        buf.reserve(kWireSize + capped);
        buf.push_back('R');
        buf.push_back('M');
        buf.push_back('X');
        buf.push_back('P');
        buf.push_back(version);
        buf.push_back(pkt_type);
        buf.push_back(slot);
        buf.push_back(flags);
        net::push_be32(buf, static_cast<int32_t>(room_id));
        net::push_be32(buf, static_cast<int32_t>(token));
        net::push_be32(buf, static_cast<int32_t>(frame_or_seq));
        net::push_be32(buf, keypad_mask);
        net::push_be16(buf, capped);
        net::push_be16(buf, checksum16);
        if (payload && capped > 0) {
            buf.insert(buf.end(), payload, payload + capped);
        }
        return buf;
    }

    static bool deserialize(const uint8_t* data, size_t len, PvpPacketHeader& out, const uint8_t*& payload_out) {
        if (!data || len < kWireSize) return false;
        if (data[0] != 'R' || data[1] != 'M' || data[2] != 'X' || data[3] != 'P') return false;
        auto rd32 = [&](size_t off) -> uint32_t {
            return (static_cast<uint32_t>(data[off]) << 24) |
                   (static_cast<uint32_t>(data[off + 1]) << 16) |
                   (static_cast<uint32_t>(data[off + 2]) << 8) |
                   static_cast<uint32_t>(data[off + 3]);
        };
        auto rd16 = [&](size_t off) -> uint16_t {
            return static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
        };
        out.version      = data[4];
        out.pkt_type     = data[5];
        out.slot         = data[6];
        out.flags        = data[7];
        out.room_id      = rd32(8);
        out.token        = rd32(12);
        out.frame_or_seq = rd32(16);
        out.keypad_mask  = static_cast<int32_t>(rd32(20));
        out.payload_len  = rd16(24);
        out.checksum16   = rd16(26);
        if (kWireSize + out.payload_len > len) return false;
        payload_out = (out.payload_len > 0) ? (data + kWireSize) : nullptr;
        return true;
    }
};

inline uint64_t steady_millis() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

inline std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
    }
    return out;
}

// Simple JSON field extractor for lightweight client parsing
inline std::string json_extract_str(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\":";
    auto p = json.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < json.size() && json[p] == ' ') ++p;
    if (p < json.size() && json[p] == '"') {
        ++p;
        std::string val;
        while (p < json.size() && json[p] != '"') {
            if (json[p] == '\\' && p + 1 < json.size()) ++p;
            val.push_back(json[p++]);
        }
        return val;
    }
    std::string num;
    while (p < json.size() && (std::isalnum(static_cast<unsigned char>(json[p])) || json[p] == '-')) {
        num.push_back(json[p++]);
    }
    return num;
}

// ============================================================================
// Server-Side PvP Matchmaking Lobby + UDP Real-Time Relay Engine
// ============================================================================
class PvpServerEngine {
public:
    struct PeerSlot {
        bool occupied = false;
        bool udp_bound = false;
        std::string uid;
        std::string name;
        uint32_t token = 0;
        sockaddr_storage udp_addr{};
        socklen_t udp_addr_len = 0;
        uint64_t last_seen_ms = 0;
        std::map<uint32_t, int32_t> recent_inputs; // Legacy frame inputs
        std::map<uint64_t, int32_t> turn_inputs;   // ((uint64_t)turn << 32) | frame -> mask
        bool has_build = false;
        std::vector<uint8_t> build_wire;
        bool has_commit = false;
        uint16_t commit_turn = 0;
        std::vector<uint8_t> commit_wire;
    };

    struct Room {
        uint32_t room_id = 0;
        std::string game = "pon"; // "pon" or "lon"
        uint8_t match_format = PVP_FORMAT_SINGLE;
        uint32_t rng_seed = 0;
        bool match_started = false;
        uint64_t created_ms = 0;
        uint64_t match_ended_ms = 0;
        PeerSlot peers[2]; // [0] = P1, [1] = P2
    };

    PvpServerEngine() : rng_(std::random_device{}()) {}

    void set_udp_fd(plat::sock_t fd) { udp_fd_ = fd; }
    void set_legacy_wire_compat_for_test(bool v) { legacy_wire_test_ = v; }
    void set_drop_commit_to_slot_for_test(uint8_t target_slot, int drop_count) {
        std::lock_guard<std::mutex> lk(mtx_);
        drop_commit_target_slot_ = target_slot;
        drop_commit_remaining_ = drop_count;
    }

    void periodic_maintenance() {
        std::lock_guard<std::mutex> lk(mtx_);
        prune_stale_rooms_unlocked();
    }

    bool is_pvp_http_path(const std::string& url) const {
        std::string path = net::extract_path_only(url);
        return path.rfind("/pvp/", 0) == 0;
    }

    net::HttpResponse handle_http(const std::string& method, const std::string& url, const std::vector<uint8_t>& body = {}) {
        std::lock_guard<std::mutex> lk(mtx_);
        prune_stale_rooms_unlocked();

        std::string path = net::extract_path_only(url);
        auto params = net::parse_query_params(url);
        auto format_str = [](uint8_t fmt) -> const char* {
            return (fmt == PVP_FORMAT_TRIPLE) ? "bo3" : "single";
        };

        net::HttpResponse resp;
        resp.status_code = 200;
        resp.headers["Content-Type"] = "application/json";

        std::ostringstream js;
        if (path == "/pvp/status") {
            size_t open_cnt = 0, active_cnt = 0, players = 0;
            for (const auto& [id, rm] : rooms_) {
                if (rm.peers[0].occupied) ++players;
                if (rm.peers[1].occupied) ++players;
                if (rm.peers[0].occupied && !rm.peers[1].occupied) ++open_cnt;
                if (rm.peers[0].occupied && rm.peers[1].occupied) ++active_cnt;
            }
            js << "{\"ok\":true,\"server\":\"rmexe_server\",\"version\":1"
               << ",\"open_rooms\":" << open_cnt
               << ",\"active_matches\":" << active_cnt
               << ",\"online_players\":" << players << "}";
        } else if (path == "/pvp/rooms") {
            std::string filter_game = params.count("game") ? params["game"] : "";
            js << "{\"ok\":true,\"rooms\":[";
            bool first = true;
            for (const auto& [id, rm] : rooms_) {
                if (!rm.peers[0].occupied || rm.peers[1].occupied) continue;
                if (!filter_game.empty() && filter_game != "all" && rm.game != filter_game) continue;
                if (!first) js << ",";
                first = false;
                js << "{\"room_id\":" << rm.room_id
                   << ",\"game\":\"" << json_escape(rm.game) << "\""
                   << ",\"host\":\"" << json_escape(rm.peers[0].name) << "\"";
                if (!legacy_wire_test_) {
                    js << ",\"format\":\"" << format_str(rm.match_format) << "\"";
                }
                js << "}";
            }
            js << "]}";
        } else if (path == "/pvp/create" && net::iequals(method, "POST")) {
            std::string req_uid = params.count("uid") ? params["uid"] : "P1";
            // Dissolve any previous room owned/joined by this same uid so ghost rooms never stack up
            if (!req_uid.empty()) {
                for (auto it = rooms_.begin(); it != rooms_.end();) {
                    Room& old_rm = it->second;
                    bool match0 = old_rm.peers[0].occupied && old_rm.peers[0].uid == req_uid;
                    bool match1 = old_rm.peers[1].occupied && old_rm.peers[1].uid == req_uid;
                    if (match0 || match1) {
                        int surv = match0 ? 1 : 0;
                        notify_peer_disconnect_unlocked(old_rm, surv);
                        it = rooms_.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (rooms_.empty()) next_room_id_ = 1001;
            }

            std::string req_fmt = params.count("format") ? params["format"] : "";
            if (req_fmt.empty() && !body.empty()) {
                std::string bstr(reinterpret_cast<const char*>(body.data()), body.size());
                req_fmt = json_extract_str(bstr, "format");
            }

            uint32_t rid = next_room_id_++;
            Room rm{};
            rm.room_id = rid;
            rm.game = params.count("game") && !params["game"].empty() ? params["game"] : "pon";
            rm.match_format = (req_fmt == "bo3" || req_fmt == "triple") ? PVP_FORMAT_TRIPLE : PVP_FORMAT_SINGLE;
            rm.rng_seed = rng_() | 1u;
            rm.created_ms = steady_millis();
            rm.peers[0].occupied = true;
            rm.peers[0].uid = req_uid;
            rm.peers[0].name = params.count("name") && !params["name"].empty() ? params["name"] : "P1_NAVI";
            rm.peers[0].token = (rng_() & 0x7FFFFFFFu) | 0x10000u;
            rm.peers[0].last_seen_ms = rm.created_ms;

            uint32_t tok = rm.peers[0].token;
            uint8_t fmt = rm.match_format;
            rooms_[rid] = std::move(rm);
            js << "{\"ok\":true,\"room_id\":" << rid
               << ",\"slot\":1,\"token\":" << tok;
            if (!legacy_wire_test_) {
                js << ",\"format\":\"" << format_str(fmt) << "\"";
            }
            js << "}";
        } else if (path == "/pvp/join" && net::iequals(method, "POST")) {
            uint32_t rid = params.count("room") ? static_cast<uint32_t>(std::strtoul(params["room"].c_str(), nullptr, 10)) : 0;
            Room* target = nullptr;
            if (rid != 0) {
                auto it = rooms_.find(rid);
                if (it != rooms_.end() && it->second.peers[0].occupied && !it->second.peers[1].occupied) {
                    target = &it->second;
                }
            } else {
                std::string want_game = params.count("game") ? params["game"] : "";
                for (auto& [id, rm] : rooms_) {
                    if (rm.peers[0].occupied && !rm.peers[1].occupied &&
                        (want_game.empty() || rm.game == want_game)) {
                        target = &rm;
                        break;
                    }
                }
            }
            if (!target) {
                resp.status_code = 404;
                js << "{\"ok\":false,\"error\":\"no_open_room\"}";
            } else {
                target->peers[1].occupied = true;
                target->peers[1].uid = params.count("uid") ? params["uid"] : "P2";
                target->peers[1].name = params.count("name") && !params["name"].empty() ? params["name"] : "P2_NAVI";
                target->peers[1].token = (rng_() & 0x7FFFFFFFu) | 0x20000u;
                target->peers[1].last_seen_ms = steady_millis();
                js << "{\"ok\":true,\"room_id\":" << target->room_id
                   << ",\"slot\":2,\"token\":" << target->peers[1].token
                   << ",\"host\":\"" << json_escape(target->peers[0].name) << "\"";
                if (!legacy_wire_test_) {
                    js << ",\"format\":\"" << format_str(target->match_format) << "\"";
                }
                js << "}";
            }
        } else if (path == "/pvp/leave") {
            uint32_t rid = params.count("room") ? static_cast<uint32_t>(std::strtoul(params["room"].c_str(), nullptr, 10)) : 0;
            uint32_t tok = params.count("token") ? static_cast<uint32_t>(std::strtoul(params["token"].c_str(), nullptr, 10)) : 0;
            auto it = rooms_.find(rid);
            if (it != rooms_.end()) {
                Room& rm = it->second;
                if (rm.peers[0].token == tok || rm.peers[1].token == tok) {
                    int surv = (rm.peers[0].token == tok) ? 1 : 0;
                    notify_peer_disconnect_unlocked(rm, surv);
                    rooms_.erase(it);
                    if (rooms_.empty()) next_room_id_ = 1001;
                }
            }
            js << "{\"ok\":true}";
        } else {
            resp.status_code = 404;
            js << "{\"ok\":false,\"error\":\"unknown_endpoint\"}";
        }

        std::string s = js.str();
        resp.body.assign(s.begin(), s.end());
        resp.headers["Content-Length"] = std::to_string(resp.body.size());
        return resp;
    }

    void handle_udp_datagram(plat::sock_t udp_fd, const uint8_t* data, size_t len, const sockaddr_storage& src_addr, socklen_t src_len) {
        PvpPacketHeader hdr{};
        const uint8_t* payload = nullptr;
        if (!PvpPacketHeader::deserialize(data, len, hdr, payload)) return;

        std::lock_guard<std::mutex> lk(mtx_);
        uint64_t now = steady_millis();

        // Stateless PING support (room_id == 0 allowed for server latency probe)
        if (hdr.pkt_type == PKT_PING && hdr.room_id == 0) {
            PvpPacketHeader pong = hdr;
            pong.pkt_type = PKT_PONG;
            auto wire = pong.serialize();
            plat::sock_sendto(udp_fd, wire.data(), wire.size(), reinterpret_cast<const sockaddr*>(&src_addr), src_len);
            return;
        }

        auto it = rooms_.find(hdr.room_id);
        if (it == rooms_.end()) return;
        Room& rm = it->second;

        if (hdr.slot != 1 && hdr.slot != 2) return;
        int idx = hdr.slot - 1;
        int opp_idx = 1 - idx;
        PeerSlot& me = rm.peers[idx];
        PeerSlot& opp = rm.peers[opp_idx];
        if (!me.occupied || me.token != hdr.token) return;

        me.udp_addr = src_addr;
        me.udp_addr_len = src_len;
        me.udp_bound = true;
        me.last_seen_ms = now;

        switch (hdr.pkt_type) {
            case PKT_BIND: {
                const uint8_t fmt_flag = legacy_wire_test_
                    ? 0
                    : static_cast<uint8_t>(PVP_HDR_FLAG_FMT_KNOWN | ((rm.match_format == PVP_FORMAT_TRIPLE) ? PVP_HDR_FLAG_TRIPLE : 0));
                PvpPacketHeader ack{};
                ack.pkt_type = PKT_BIND_ACK;
                ack.slot = hdr.slot;
                ack.flags = fmt_flag;
                ack.room_id = rm.room_id;
                ack.token = me.token;
                ack.frame_or_seq = rm.rng_seed;
                auto wire = ack.serialize();
                plat::sock_sendto(udp_fd, wire.data(), wire.size(), reinterpret_cast<const sockaddr*>(&src_addr), src_len);

                // If both P1 and P2 have bound UDP endpoints, broadcast PKT_MATCH_START to both!
                if (rm.peers[0].udp_bound && rm.peers[1].udp_bound) {
                    rm.match_started = true;
                    for (int p = 0; p < 2; ++p) {
                        PvpPacketHeader st{};
                        st.pkt_type = PKT_MATCH_START;
                        st.slot = static_cast<uint8_t>(p + 1);
                        st.flags = fmt_flag;
                        st.room_id = rm.room_id;
                        st.token = rm.peers[p].token;
                        st.frame_or_seq = rm.rng_seed;
                        const std::string& opp_name = rm.peers[1 - p].name;
                        auto swire = st.serialize(reinterpret_cast<const uint8_t*>(opp_name.data()), opp_name.size());
                        plat::sock_sendto(udp_fd, swire.data(), swire.size(),
                                          reinterpret_cast<const sockaddr*>(&rm.peers[p].udp_addr), rm.peers[p].udp_addr_len);
                    }
                    if (opp.has_build && !opp.build_wire.empty()) {
                        plat::sock_sendto(udp_fd, opp.build_wire.data(), opp.build_wire.size(),
                                          reinterpret_cast<const sockaddr*>(&src_addr), src_len);
                    }
                }
                break;
            }
            case PKT_PING: {
                PvpPacketHeader pong = hdr;
                pong.pkt_type = PKT_PONG;
                auto wire = pong.serialize();
                plat::sock_sendto(udp_fd, wire.data(), wire.size(), reinterpret_cast<const sockaddr*>(&src_addr), src_len);
                break;
            }
            case PKT_BUILD_SYNC: {
                me.has_build = true;
                me.build_wire.assign(data, data + len);
                // Server ACK back so sender knows server cached it, AND relay to opponent
                PvpPacketHeader ack{};
                ack.pkt_type = PKT_BUILD_ACK;
                ack.slot = hdr.slot;
                ack.room_id = rm.room_id;
                ack.token = me.token;
                auto awire = ack.serialize();
                plat::sock_sendto(udp_fd, awire.data(), awire.size(), reinterpret_cast<const sockaddr*>(&src_addr), src_len);
                if (opp.occupied && opp.udp_bound) {
                    plat::sock_sendto(udp_fd, data, len,
                                      reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                if (opp.has_build && !opp.build_wire.empty()) {
                    plat::sock_sendto(udp_fd, opp.build_wire.data(), opp.build_wire.size(),
                                      reinterpret_cast<const sockaddr*>(&src_addr), src_len);
                }
                break;
            }
            case PKT_BUILD_ACK:
            case PKT_CUSTOM_ACK:
            case PKT_BATTLE_STATE: {
                if (opp.occupied && opp.udp_bound) {
                    plat::sock_sendto(udp_fd, data, len,
                                      reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                break;
            }
            case PKT_MATCH_END: {
                if (rm.match_ended_ms == 0) rm.match_ended_ms = now;
                if (opp.occupied && opp.udp_bound) {
                    plat::sock_sendto(udp_fd, data, len,
                                      reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                break;
            }
            case PKT_CUSTOM_COMMIT: {
                if (payload && hdr.payload_len >= PvpCustomCommit::kWireSize) {
                    uint16_t cturn = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
                    me.has_commit = true;
                    me.commit_turn = cturn;
                    me.commit_wire.assign(data, data + len);
                    // Send ACK and relay to opponent; also push opponent's cached commit if same turn
                    PvpPacketHeader ack{};
                    ack.pkt_type = PKT_CUSTOM_ACK;
                    ack.slot = hdr.slot;
                    ack.room_id = rm.room_id;
                    ack.token = me.token;
                    ack.frame_or_seq = cturn;
                    auto awire = ack.serialize();
                    plat::sock_sendto(udp_fd, awire.data(), awire.size(), reinterpret_cast<const sockaddr*>(&src_addr), src_len);
                    if (opp.occupied && opp.udp_bound) {
                        if (drop_commit_target_slot_ != 0 &&
                            (opp_idx + 1) == drop_commit_target_slot_ &&
                            drop_commit_remaining_ > 0) {
                            --drop_commit_remaining_;
                        } else {
                            plat::sock_sendto(udp_fd, data, len,
                                              reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                        }
                    }
                    if (opp.has_commit && opp.commit_turn == cturn && !opp.commit_wire.empty()) {
                        if (drop_commit_target_slot_ != 0 &&
                            hdr.slot == drop_commit_target_slot_ &&
                            drop_commit_remaining_ > 0) {
                            --drop_commit_remaining_;
                        } else {
                            plat::sock_sendto(udp_fd, opp.commit_wire.data(), opp.commit_wire.size(),
                                              reinterpret_cast<const sockaddr*>(&src_addr), src_len);
                        }
                    }
                }
                break;
            }
            case PKT_INPUT_BUNDLE: {
                // Payload format: [turn_no:u16][count:u8][reserved:u8][inputs[0..count-1]:be32]
                if (payload && hdr.payload_len >= 4) {
                    uint16_t turn = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
                    uint8_t cnt = payload[2];
                    if (cnt > 32) cnt = 32;
                    if (hdr.payload_len >= static_cast<uint16_t>(4 + cnt * 4)) {
                        for (uint8_t i = 0; i < cnt; ++i) {
                            if (hdr.frame_or_seq < i) break;
                            uint32_t f = hdr.frame_or_seq - i;
                            size_t off = 4 + i * 4;
                            int32_t mask = static_cast<int32_t>(
                                (static_cast<uint32_t>(payload[off]) << 24) |
                                (static_cast<uint32_t>(payload[off + 1]) << 16) |
                                (static_cast<uint32_t>(payload[off + 2]) << 8) |
                                static_cast<uint32_t>(payload[off + 3]));
                            uint64_t key = (static_cast<uint64_t>(turn) << 32) | f;
                            me.turn_inputs[key] = mask;
                        }
                        while (me.turn_inputs.size() > 4096) {
                            me.turn_inputs.erase(me.turn_inputs.begin());
                        }
                    }
                }
                if (opp.occupied && opp.udp_bound) {
                    plat::sock_sendto(udp_fd, data, len,
                                      reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                break;
            }
            case PKT_INPUT_REQ: {
                uint16_t turn = static_cast<uint16_t>(hdr.keypad_mask & 0xFFFF);
                uint32_t want_f = hdr.frame_or_seq;
                bool any_replayed = false;
                // Replay up to 32 cached inputs from opp starting at want_f in 8-frame wire-compatible chunks!
                for (uint32_t chunk = 0; chunk < 4; ++chunk) {
                    uint32_t base_f = want_f + chunk * 8;
                    std::vector<int32_t> found_masks;
                    uint32_t top_f = base_f;
                    for (uint32_t f = base_f; f < base_f + 8; ++f) {
                        uint64_t key = (static_cast<uint64_t>(turn) << 32) | f;
                        auto fit = opp.turn_inputs.find(key);
                        if (fit == opp.turn_inputs.end()) break;
                        found_masks.push_back(fit->second);
                        top_f = f;
                    }
                    if (found_masks.empty()) break;
                    any_replayed = true;
                    std::vector<uint8_t> pl;
                    net::push_be16(pl, turn);
                    pl.push_back(static_cast<uint8_t>(found_masks.size()));
                    pl.push_back(0);
                    // Pack newest first: top_f down to base_f
                    for (int i = static_cast<int>(found_masks.size()) - 1; i >= 0; --i) {
                        net::push_be32(pl, found_masks[i]);
                    }
                    PvpPacketHeader rep{};
                    rep.pkt_type = PKT_INPUT_BUNDLE;
                    rep.slot = static_cast<uint8_t>(opp_idx + 1);
                    rep.room_id = rm.room_id;
                    rep.token = opp.token;
                    rep.frame_or_seq = top_f;
                    auto rwire = rep.serialize(pl.data(), pl.size());
                    plat::sock_sendto(udp_fd, rwire.data(), rwire.size(), reinterpret_cast<const sockaddr*>(&src_addr), src_len);
                    if (found_masks.size() < 8) break;
                }
                if (!any_replayed && me.has_commit && me.commit_turn == turn &&
                    !me.commit_wire.empty() && opp.occupied && opp.udp_bound) {
                    plat::sock_sendto(udp_fd, me.commit_wire.data(), me.commit_wire.size(),
                                      reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                if (opp.occupied && opp.udp_bound) {
                    plat::sock_sendto(udp_fd, data, len,
                                      reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                break;
            }
            case PKT_FRAME_INPUT: {
                me.recent_inputs[hdr.frame_or_seq] = hdr.keypad_mask;
                while (me.recent_inputs.size() > 64) {
                    me.recent_inputs.erase(me.recent_inputs.begin());
                }
                if (opp.occupied && opp.udp_bound) {
                    plat::sock_sendto(udp_fd, data, len,
                                      reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                break;
            }
            case PKT_DISCONNECT: {
                if (opp.occupied && opp.udp_bound) {
                    plat::sock_sendto(udp_fd, data, len,
                                      reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                rooms_.erase(it);
                if (rooms_.empty()) next_room_id_ = 1001;
                break;
            }
            default:
                break;
        }
    }

private:
    std::mutex mtx_;
    std::mt19937 rng_;
    plat::sock_t udp_fd_ = plat::kInvalidSocket;
    bool legacy_wire_test_ = false;
    uint8_t drop_commit_target_slot_ = 0;
    int drop_commit_remaining_ = 0;
    uint32_t next_room_id_ = 1001;
    std::unordered_map<uint32_t, Room> rooms_;

    void notify_peer_disconnect_unlocked(const Room& rm, int surv_idx) {
        if (udp_fd_ == plat::kInvalidSocket || surv_idx < 0 || surv_idx > 1) return;
        const PeerSlot& surv = rm.peers[surv_idx];
        if (!surv.occupied || !surv.udp_bound) return;
        PvpPacketHeader d{};
        d.pkt_type = PKT_DISCONNECT;
        d.slot = static_cast<uint8_t>((1 - surv_idx) + 1);
        d.room_id = rm.room_id;
        d.token = rm.peers[1 - surv_idx].token;
        auto wire = d.serialize();
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&surv.udp_addr), surv.udp_addr_len);
    }

    void prune_stale_rooms_unlocked() {
        uint64_t now = steady_millis();
        for (auto it = rooms_.begin(); it != rooms_.end();) {
            const Room& rm = it->second;
            bool dissolve = false;
            int notify_surv = -1;

            if (rm.match_ended_ms > 0 && now > rm.match_ended_ms && (now - rm.match_ended_ms) > 6000ULL) {
                dissolve = true;
            } else if (!rm.match_started && !rm.peers[1].occupied) {
                // Open waiting room: live hosts send PKT_BIND every 500ms (even inside F1 menu).
                // Dissolve abandoned rooms after 8 seconds of silence!
                uint64_t seen0 = rm.peers[0].last_seen_ms;
                if (now > seen0 && (now - seen0) > 8000ULL) {
                    dissolve = true;
                }
            } else {
                // Paired / active match: live clients send PKT_PING every 2s + 16Hz combat inputs.
                // If either player vanishes for >12 seconds, notify survivor & dissolve room!
                bool dead0 = !rm.peers[0].occupied || (now > rm.peers[0].last_seen_ms && (now - rm.peers[0].last_seen_ms) > 12000ULL);
                bool dead1 = !rm.peers[1].occupied || (now > rm.peers[1].last_seen_ms && (now - rm.peers[1].last_seen_ms) > 12000ULL);
                if (dead0 || dead1) {
                    dissolve = true;
                    if (dead0 && !dead1) notify_surv = 1;
                    else if (dead1 && !dead0) notify_surv = 0;
                }
            }

            if (dissolve) {
                if (notify_surv >= 0) {
                    notify_peer_disconnect_unlocked(rm, notify_surv);
                }
                it = rooms_.erase(it);
            } else {
                ++it;
            }
        }
        if (rooms_.empty()) {
            next_room_id_ = 1001;
        }
    }
};

// ============================================================================
// Client-Side Non-Blocking PvP Lobby & UDP Lockstep/Rollback Session Helper
// ============================================================================
class PvpClientSession {
public:
    ~PvpClientSession() {
        close_udp();
    }

    bool active() const { return udp_fd_ != plat::kInvalidSocket && room_id_ != 0; }
    bool match_started() const { return match_started_; }
    bool peer_disconnected() const { return peer_disconnected_; }
    bool remote_match_ended() const { return remote_match_ended_; }
    int32_t remote_winner_slot() const { return remote_winner_slot_; }
    uint32_t room_id() const { return room_id_; }
    uint8_t slot() const { return slot_; }
    bool is_host() const { return slot_ == 1; }
    uint32_t shared_rng_seed() const { return rng_seed_; }
    uint32_t rng_seed() const { return rng_seed_; }
    int last_rtt_ms() const { return last_rtt_ms_; }
    int rtt_ms() const { return last_rtt_ms_; }
    const std::string& opponent_name() const { return opponent_name_; }

    bool has_remote_build() const { return has_remote_build_; }
    bool local_build_acked() const { return local_build_acked_; }
    const PvpFighterBuild& remote_build() const { return remote_build_; }

    void set_preferred_format(uint8_t fmt) {
        preferred_format_ = (fmt == PVP_FORMAT_TRIPLE) ? PVP_FORMAT_TRIPLE : PVP_FORMAT_SINGLE;
    }
    uint8_t preferred_format() const { return preferred_format_; }
    bool is_triple_battle() const { return active_match_format_ == PVP_FORMAT_TRIPLE; }
    bool peer_supports_bo3() const { return peer_fmt_capable_; }
    void set_send_legacy_build_for_test(bool v) { send_legacy_build_test_ = v; }

    static bool check_server_status(const std::string& server_host_port, std::string& summary_out) {
        net::HttpResponse resp;
        if (!net::http_request_tcp(server_host_port, "GET", "/pvp/status", {}, {}, resp, 1500) ||
            resp.status_code != 200) {
            return false;
        }
        std::string js(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
        std::string open_r = json_extract_str(js, "open_rooms");
        std::string act_m  = json_extract_str(js, "active_matches");
        summary_out = "房间:" + (open_r.empty() ? "0" : open_r) + " 对战中:" + (act_m.empty() ? "0" : act_m);
        return true;
    }

    static bool list_open_rooms(const std::string& server_host_port, const std::string& game, std::vector<PvpRoomInfo>& out_rooms) {
        out_rooms.clear();
        std::string uri = "/pvp/rooms?game=" + net::url_encode(game);
        net::HttpResponse resp;
        if (!net::http_request_tcp(server_host_port, "GET", uri, {}, {}, resp, 1500) ||
            resp.status_code != 200) {
            return false;
        }
        std::string js(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
        size_t pos = js.find("\"rooms\":");
        if (pos == std::string::npos) return true;
        pos = js.find('[', pos);
        if (pos == std::string::npos) return true;
        while (pos < js.size()) {
            size_t lb = js.find('{', pos);
            if (lb == std::string::npos) break;
            size_t rb = js.find('}', lb);
            if (rb == std::string::npos) break;
            std::string item = js.substr(lb, rb - lb + 1);
            PvpRoomInfo ri{};
            ri.room_id = static_cast<uint32_t>(std::strtoul(json_extract_str(item, "room_id").c_str(), nullptr, 10));
            ri.game = json_extract_str(item, "game");
            ri.host = json_extract_str(item, "host");
            std::string fmt = json_extract_str(item, "format");
            ri.match_format = (fmt == "bo3" || fmt == "triple") ? PVP_FORMAT_TRIPLE : PVP_FORMAT_SINGLE;
            if (ri.room_id != 0) out_rooms.push_back(std::move(ri));
            pos = rb + 1;
        }
        return true;
    }

    bool create_room(const std::string& server_host_port, const std::string& uid, const std::string& name, const std::string& game = "pon") {
        close_udp();
        const char* fmt_s = (preferred_format_ == PVP_FORMAT_TRIPLE) ? "bo3" : "single";
        std::string uri = "/pvp/create?uid=" + net::url_encode(uid) +
                          "&name=" + net::url_encode(name) +
                          "&game=" + net::url_encode(game) +
                          "&format=" + fmt_s;
        std::string body_str = std::string("{\"format\":\"") + fmt_s + "\"}";
        std::vector<uint8_t> req_body(body_str.begin(), body_str.end());
        std::unordered_map<std::string, std::string> req_hdrs{{"Content-Type", "application/json"}};
        net::HttpResponse resp;
        if (!net::http_request_tcp(server_host_port, "POST", uri, req_hdrs, req_body, resp, 2000) || resp.status_code != 200) {
            return false;
        }
        std::string js(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
        if (json_extract_str(js, "ok") != "true") return false;

        reset_match_buffers();
        room_id_ = static_cast<uint32_t>(std::strtoul(json_extract_str(js, "room_id").c_str(), nullptr, 10));
        token_   = static_cast<uint32_t>(std::strtoul(json_extract_str(js, "token").c_str(), nullptr, 10));
        slot_    = 1;
        std::string rfmt = json_extract_str(js, "format");
        if (rfmt == "bo3" || rfmt == "triple") active_match_format_ = PVP_FORMAT_TRIPLE;
        else if (rfmt == "single") active_match_format_ = PVP_FORMAT_SINGLE;
        else active_match_format_ = preferred_format_;
        opponent_name_.clear();
        server_addr_str_ = server_host_port;
        return open_and_bind_udp(server_host_port);
    }

    bool join_room(const std::string& server_host_port, uint32_t room_id_or_zero, const std::string& uid, const std::string& name, const std::string& game = "pon") {
        close_udp();
        std::string uri = "/pvp/join?uid=" + net::url_encode(uid) +
                          "&name=" + net::url_encode(name) +
                          "&game=" + net::url_encode(game);
        if (room_id_or_zero > 0) {
            uri += "&room=" + std::to_string(room_id_or_zero);
        }
        net::HttpResponse resp;
        if (!net::http_request_tcp(server_host_port, "POST", uri, {}, {}, resp, 2000) || resp.status_code != 200) {
            return false;
        }
        std::string js(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
        if (json_extract_str(js, "ok") != "true") return false;

        reset_match_buffers();
        room_id_       = static_cast<uint32_t>(std::strtoul(json_extract_str(js, "room_id").c_str(), nullptr, 10));
        token_         = static_cast<uint32_t>(std::strtoul(json_extract_str(js, "token").c_str(), nullptr, 10));
        opponent_name_ = json_extract_str(js, "host");
        std::string rfmt = json_extract_str(js, "format");
        active_match_format_ = (rfmt == "bo3" || rfmt == "triple") ? PVP_FORMAT_TRIPLE : PVP_FORMAT_SINGLE;
        slot_          = 2;
        server_addr_str_ = server_host_port;
        return open_and_bind_udp(server_host_port);
    }

    void send_build_sync(const PvpFighterBuild& b) {
        if (udp_fd_ == plat::kInvalidSocket) return;
        auto pl = b.serialize();
        PvpPacketHeader h{};
        h.pkt_type = PKT_BUILD_SYNC;
        h.slot = slot_;
        if (!send_legacy_build_test_) {
            uint8_t my_fmt = (slot_ == 1) ? active_match_format_ : preferred_format_;
            h.flags = static_cast<uint8_t>(PVP_HDR_FLAG_FMT_KNOWN |
                      ((my_fmt == PVP_FORMAT_TRIPLE) ? PVP_HDR_FLAG_TRIPLE : 0));
        }
        h.room_id = room_id_;
        h.token = token_;
        auto wire = h.serialize(pl.data(), pl.size());
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    void send_custom_commit(const PvpCustomCommit& c) {
        if (udp_fd_ == plat::kInvalidSocket) return;
        auto pl = c.serialize();
        PvpPacketHeader h{};
        h.pkt_type = PKT_CUSTOM_COMMIT;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = c.turn_no;
        auto wire = h.serialize(pl.data(), pl.size());
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    bool has_remote_commit(uint16_t turn, PvpCustomCommit& out) const {
        auto it = remote_commits_.find(turn);
        if (it == remote_commits_.end()) return false;
        out = it->second;
        return true;
    }

    bool is_custom_acked(uint16_t turn) const {
        return acked_custom_turns_.count(turn) > 0;
    }

    // Sends redundant window of up to 8 recent local inputs for (turn, latest_frame).
    // Optional local_tick_advantage (signed tick lead = local_frontier - remote_frontier, following
    // HCTOrganization/trill's getgud::Session) is encoded in keypad_mask with bit 16 set so
    // PvpRelayServer forwards it verbatim without breaking wire compatibility.
    void send_input_bundle(uint16_t turn, uint32_t latest_frame, const int32_t* newest_first_masks,
                           uint8_t count, uint16_t state_hash16 = 0,
                           int16_t local_tick_advantage = INT16_MIN) {
        if (udp_fd_ == plat::kInvalidSocket || count == 0) return;
        if (count > 8) count = 8;
        // Cache locally too so we can answer PKT_INPUT_REQ directly via relay
        for (uint8_t i = 0; i < count; ++i) {
            if (latest_frame >= i) {
                uint64_t key = (static_cast<uint64_t>(turn) << 32) | (latest_frame - i);
                local_turn_inputs_[key] = newest_first_masks[i];
            }
        }
        while (local_turn_inputs_.size() > 4096) {
            local_turn_inputs_.erase(local_turn_inputs_.begin());
        }

        std::vector<uint8_t> pl;
        pl.reserve(4 + count * 4);
        net::push_be16(pl, turn);
        pl.push_back(count);
        pl.push_back(0);
        for (uint8_t i = 0; i < count; ++i) {
            net::push_be32(pl, newest_first_masks[i]);
        }
        PvpPacketHeader h{};
        h.pkt_type = PKT_INPUT_BUNDLE;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = latest_frame;
        if (local_tick_advantage != INT16_MIN) {
            h.keypad_mask = static_cast<int32_t>((1u << 16) | static_cast<uint16_t>(local_tick_advantage));
        }
        h.checksum16 = state_hash16;
        auto wire = h.serialize(pl.data(), pl.size());
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    bool get_remote_turn_input(uint16_t turn, uint32_t frame, int32_t& mask_out, uint16_t* hash16_out = nullptr) const {
        uint64_t key = (static_cast<uint64_t>(turn) << 32) | frame;
        auto it = remote_turn_inputs_.find(key);
        if (it == remote_turn_inputs_.end()) return false;
        mask_out = it->second;
        if (hash16_out) {
            auto hit = remote_turn_hashes_.find(key);
            *hash16_out = (hit != remote_turn_hashes_.end()) ? hit->second : 0;
        }
        return true;
    }

    bool has_any_remote_turn_input_after(uint16_t turn, uint32_t frame) const {
        uint64_t key = (static_cast<uint64_t>(turn) << 32) | frame;
        auto it = remote_turn_inputs_.upper_bound(key);
        if (it == remote_turn_inputs_.end()) return false;
        return static_cast<uint16_t>(it->first >> 32) == turn;
    }

    bool get_highest_remote_turn_frame(uint16_t turn, uint32_t& max_frame_out) const {
        uint64_t upper_key = (static_cast<uint64_t>(turn) << 32) | 0xFFFFFFFFULL;
        auto it = remote_turn_inputs_.upper_bound(upper_key);
        if (it == remote_turn_inputs_.begin()) return false;
        --it;
        if (static_cast<uint16_t>(it->first >> 32) != turn) return false;
        max_frame_out = static_cast<uint32_t>(it->first & 0xFFFFFFFFULL);
        return true;
    }

    bool get_remote_tick_advantage(uint16_t turn, int16_t& adv_out) const {
        if (!has_remote_tick_adv_ || remote_adv_turn_ != turn) return false;
        adv_out = remote_tick_adv_;
        return true;
    }

    void request_missing_input(uint16_t turn, uint32_t missing_frame) {
        if (udp_fd_ == plat::kInvalidSocket) return;
        uint64_t now = steady_millis();
        uint64_t req_key = (static_cast<uint64_t>(turn) << 32) | missing_frame;
        uint64_t min_interval = (req_key != last_input_req_key_) ? 20 : 50;
        if (now - last_input_req_ms_ < min_interval) return;
        last_input_req_ms_ = now;
        last_input_req_key_ = req_key;
        PvpPacketHeader h{};
        h.pkt_type = PKT_INPUT_REQ;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = missing_frame;
        h.keypad_mask = static_cast<int32_t>(turn);
        auto wire = h.serialize();
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    void send_match_end(int32_t winner_slot) {
        if (udp_fd_ == plat::kInvalidSocket || room_id_ == 0) return;
        PvpPacketHeader h{};
        h.pkt_type = PKT_MATCH_END;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.keypad_mask = winner_slot;
        auto wire = h.serialize();
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    void send_frame_input(uint32_t frame, int32_t keypad_mask) {
        if (udp_fd_ == plat::kInvalidSocket) return;
        PvpPacketHeader h{};
        h.pkt_type = PKT_FRAME_INPUT;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = frame;
        h.keypad_mask = keypad_mask;
        auto wire = h.serialize();
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    void send_battle_state(uint32_t seq, const uint8_t* payload, size_t len) {
        if (udp_fd_ == plat::kInvalidSocket) return;
        PvpPacketHeader h{};
        h.pkt_type = PKT_BATTLE_STATE;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = seq;
        auto wire = h.serialize(payload, len);
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    void send_ping() {
        if (udp_fd_ == plat::kInvalidSocket) return;
        PvpPacketHeader h{};
        h.pkt_type = PKT_PING;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = static_cast<uint32_t>(steady_millis() & 0xFFFFFFFFu);
        auto wire = h.serialize();
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    // Polls all pending UDP packets without blocking; returns true if PKT_MATCH_START newly arrived
    bool poll_udp() {
        if (udp_fd_ == plat::kInvalidSocket) return false;
        uint64_t now = steady_millis();
        // Periodically refresh BIND until match_started_ so NAT mappings stay alive & dropped BINDs recover
        if (room_id_ != 0 && !match_started_ && (now - last_bind_ms_) >= 500) {
            last_bind_ms_ = now;
            PvpPacketHeader b{};
            b.pkt_type = PKT_BIND;
            b.slot = slot_;
            b.room_id = room_id_;
            b.token = token_;
            auto wire = b.serialize();
            plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                              reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
        }
        // Periodic ping during match for live HUD RTT display & time-sync fallback
        if (room_id_ != 0 && match_started_ && (now - last_ping_ms_) >= 1000) {
            last_ping_ms_ = now;
            send_ping();
        }

        bool newly_started = false;
        uint8_t buf[1500];
        while (true) {
            ptrdiff_t n = plat::sock_recvfrom(udp_fd_, buf, sizeof(buf), nullptr, nullptr);
            if (n <= 0) break;
            PvpPacketHeader h{};
            const uint8_t* payload = nullptr;
            if (!PvpPacketHeader::deserialize(buf, static_cast<size_t>(n), h, payload)) continue;

            if (h.pkt_type == PKT_BIND_ACK) {
                udp_bound_ = true;
                rng_seed_ = h.frame_or_seq;
                if (!has_host_build_fmt_) {
                    if (h.flags & PVP_HDR_FLAG_FMT_KNOWN) {
                        active_match_format_ = (h.flags & PVP_HDR_FLAG_TRIPLE) ? PVP_FORMAT_TRIPLE : PVP_FORMAT_SINGLE;
                    } else if (h.flags & PVP_HDR_FLAG_TRIPLE) {
                        active_match_format_ = PVP_FORMAT_TRIPLE;
                    }
                }
            } else if (h.pkt_type == PKT_MATCH_START) {
                rng_seed_ = h.frame_or_seq;
                if (!has_host_build_fmt_) {
                    if (h.flags & PVP_HDR_FLAG_FMT_KNOWN) {
                        active_match_format_ = (h.flags & PVP_HDR_FLAG_TRIPLE) ? PVP_FORMAT_TRIPLE : PVP_FORMAT_SINGLE;
                    } else if (h.flags & PVP_HDR_FLAG_TRIPLE) {
                        active_match_format_ = PVP_FORMAT_TRIPLE;
                    }
                }
                if (payload && h.payload_len > 0) {
                    opponent_name_.assign(reinterpret_cast<const char*>(payload), h.payload_len);
                }
                if (!match_started_) newly_started = true;
                match_started_ = true;
            } else if (h.pkt_type == PKT_PONG) {
                uint32_t now32 = static_cast<uint32_t>(steady_millis() & 0xFFFFFFFFu);
                last_rtt_ms_ = static_cast<int>(now32 - h.frame_or_seq);
            } else if (h.pkt_type == PKT_BUILD_SYNC) {
                PvpFighterBuild fb{};
                if (PvpFighterBuild::deserialize(payload, h.payload_len, fb)) {
                    remote_build_ = std::move(fb);
                    if (!remote_build_.player_name.empty()) {
                        opponent_name_ = remote_build_.player_name;
                    }
                    has_remote_build_ = true;
                    peer_fmt_capable_ = (h.flags & PVP_HDR_FLAG_FMT_KNOWN) != 0;
                    if (!peer_fmt_capable_) {
                        has_host_build_fmt_ = true;
                        active_match_format_ = PVP_FORMAT_SINGLE;
                    } else if (h.slot == 1) {
                        has_host_build_fmt_ = true;
                        active_match_format_ = (h.flags & PVP_HDR_FLAG_TRIPLE) ? PVP_FORMAT_TRIPLE : PVP_FORMAT_SINGLE;
                    } else if (slot_ == 1) {
                        has_host_build_fmt_ = true;
                    }
                    PvpPacketHeader ack{};
                    ack.pkt_type = PKT_BUILD_ACK;
                    ack.slot = slot_;
                    ack.room_id = room_id_;
                    ack.token = token_;
                    auto awire = ack.serialize();
                    plat::sock_sendto(udp_fd_, awire.data(), awire.size(),
                                      reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
                }
            } else if (h.pkt_type == PKT_BUILD_ACK) {
                local_build_acked_ = true;
            } else if (h.pkt_type == PKT_CUSTOM_COMMIT) {
                PvpCustomCommit cc{};
                if (PvpCustomCommit::deserialize(payload, h.payload_len, cc)) {
                    remote_commits_[cc.turn_no] = cc;
                    PvpPacketHeader ack{};
                    ack.pkt_type = PKT_CUSTOM_ACK;
                    ack.slot = slot_;
                    ack.room_id = room_id_;
                    ack.token = token_;
                    ack.frame_or_seq = cc.turn_no;
                    auto awire = ack.serialize();
                    plat::sock_sendto(udp_fd_, awire.data(), awire.size(),
                                      reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
                }
            } else if (h.pkt_type == PKT_CUSTOM_ACK) {
                acked_custom_turns_[static_cast<uint16_t>(h.frame_or_seq & 0xFFFF)] = true;
            } else if (h.pkt_type == PKT_INPUT_BUNDLE) {
                if (payload && h.payload_len >= 4) {
                    uint16_t turn = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
                    uint8_t cnt = payload[2];
                    if (cnt > 32) cnt = 32;
                    if (h.payload_len >= static_cast<uint16_t>(4 + cnt * 4)) {
                        if ((static_cast<uint32_t>(h.keypad_mask) & (1u << 16)) != 0) {
                            if (!has_remote_tick_adv_ || turn > remote_adv_turn_ ||
                                (turn == remote_adv_turn_ && h.frame_or_seq >= remote_adv_frame_)) {
                                has_remote_tick_adv_ = true;
                                remote_adv_turn_ = turn;
                                remote_adv_frame_ = h.frame_or_seq;
                                remote_tick_adv_ = static_cast<int16_t>(static_cast<uint16_t>(h.keypad_mask & 0xFFFF));
                            }
                        }
                        for (uint8_t i = 0; i < cnt; ++i) {
                            if (h.frame_or_seq < i) break;
                            uint32_t f = h.frame_or_seq - i;
                            size_t off = 4 + i * 4;
                            int32_t mask = static_cast<int32_t>(
                                (static_cast<uint32_t>(payload[off]) << 24) |
                                (static_cast<uint32_t>(payload[off + 1]) << 16) |
                                (static_cast<uint32_t>(payload[off + 2]) << 8) |
                                static_cast<uint32_t>(payload[off + 3]));
                            uint64_t key = (static_cast<uint64_t>(turn) << 32) | f;
                            remote_turn_inputs_[key] = mask;
                            if (i == 0 && h.checksum16 != 0) {
                                remote_turn_hashes_[key] = h.checksum16;
                            }
                        }
                        while (remote_turn_inputs_.size() > 4096) {
                            remote_turn_inputs_.erase(remote_turn_inputs_.begin());
                        }
                        while (remote_turn_hashes_.size() > 4096) {
                            remote_turn_hashes_.erase(remote_turn_hashes_.begin());
                        }
                    }
                }
            } else if (h.pkt_type == PKT_INPUT_REQ) {
                uint16_t turn = static_cast<uint16_t>(h.keypad_mask & 0xFFFF);
                uint32_t want_f = h.frame_or_seq;
                for (uint32_t chunk = 0; chunk < 4; ++chunk) {
                    uint32_t base_f = want_f + chunk * 8;
                    std::vector<int32_t> masks;
                    uint32_t top_f = base_f;
                    for (uint32_t f = base_f; f < base_f + 8; ++f) {
                        uint64_t key = (static_cast<uint64_t>(turn) << 32) | f;
                        auto it = local_turn_inputs_.find(key);
                        if (it == local_turn_inputs_.end()) break;
                        masks.push_back(it->second);
                        top_f = f;
                    }
                    if (masks.empty()) break;
                    int32_t rev[8]{};
                    uint8_t cnt = static_cast<uint8_t>(masks.size());
                    for (uint8_t i = 0; i < cnt; ++i) rev[i] = masks[cnt - 1 - i];
                    send_input_bundle(turn, top_f, rev, cnt, 0);
                    if (cnt < 8) break;
                }
            } else if (h.pkt_type == PKT_MATCH_END) {
                remote_match_ended_ = true;
                remote_winner_slot_ = h.keypad_mask;
            } else if (h.pkt_type == PKT_FRAME_INPUT) {
                remote_inputs_[h.frame_or_seq] = h.keypad_mask;
                while (remote_inputs_.size() > 128) {
                    remote_inputs_.erase(remote_inputs_.begin());
                }
            } else if (h.pkt_type == PKT_BATTLE_STATE) {
                if (payload && h.payload_len > 0) {
                    remote_states_.emplace_back(payload, payload + h.payload_len);
                    while (remote_states_.size() > 32) remote_states_.pop_front();
                }
            } else if (h.pkt_type == PKT_DISCONNECT) {
                peer_disconnected_ = true;
                match_started_ = false;
            }
        }
        return newly_started;
    }

    bool pop_remote_input(uint32_t frame, int32_t& mask_out) {
        auto it = remote_inputs_.find(frame);
        if (it == remote_inputs_.end()) return false;
        mask_out = it->second;
        return true;
    }

    bool pop_remote_battle_state(std::vector<uint8_t>& state_out) {
        if (remote_states_.empty()) return false;
        state_out = std::move(remote_states_.front());
        remote_states_.pop_front();
        return true;
    }

    void close_udp() {
        uint32_t old_room = room_id_;
        uint32_t old_token = token_;
        std::string old_srv = server_addr_str_;
        if (udp_fd_ != plat::kInvalidSocket) {
            if (old_room != 0) {
                PvpPacketHeader d{};
                d.pkt_type = PKT_DISCONNECT;
                d.slot = slot_;
                d.room_id = old_room;
                d.token = old_token;
                auto wire = d.serialize();
                // Send twice over UDP + reliable TCP /pvp/leave so rooms dissolve immediately
                plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                                  reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
                plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                                  reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
            }
            plat::close_socket(udp_fd_);
            udp_fd_ = plat::kInvalidSocket;
        }
        reset_match_buffers();
        room_id_ = 0;
        token_ = 0;
        udp_bound_ = false;
        if (old_room != 0 && !old_srv.empty()) {
            net::HttpResponse dummy{};
            std::string uri = "/pvp/leave?room=" + std::to_string(old_room) +
                              "&token=" + std::to_string(old_token);
            net::http_request_tcp(old_srv, "POST", uri, {}, {}, dummy, 350);
        }
    }

private:
    plat::sock_t udp_fd_ = plat::kInvalidSocket;
    sockaddr_storage srv_addr_{};
    socklen_t srv_addr_len_ = 0;
    std::string server_addr_str_;
    uint32_t room_id_ = 0;
    uint32_t token_ = 0;
    uint8_t slot_ = 0;
    uint8_t preferred_format_ = PVP_FORMAT_SINGLE;
    uint8_t active_match_format_ = PVP_FORMAT_SINGLE;
    bool peer_fmt_capable_ = false;
    bool has_host_build_fmt_ = false;
    bool send_legacy_build_test_ = false;
    uint32_t rng_seed_ = 0;
    bool udp_bound_ = false;
    bool match_started_ = false;
    bool peer_disconnected_ = false;
    bool remote_match_ended_ = false;
    int32_t remote_winner_slot_ = -1;
    bool has_remote_build_ = false;
    bool local_build_acked_ = false;
    PvpFighterBuild remote_build_{};
    uint64_t last_bind_ms_ = 0;
    uint64_t last_ping_ms_ = 0;
    uint64_t last_input_req_ms_ = 0;
    uint64_t last_input_req_key_ = UINT64_MAX;
    int last_rtt_ms_ = -1;
    bool has_remote_tick_adv_ = false;
    uint16_t remote_adv_turn_ = 0;
    uint32_t remote_adv_frame_ = 0;
    int16_t remote_tick_adv_ = 0;
    std::string opponent_name_;
    std::map<uint32_t, int32_t> remote_inputs_;
    std::map<uint64_t, int32_t> remote_turn_inputs_;
    std::map<uint64_t, uint16_t> remote_turn_hashes_;
    std::map<uint64_t, int32_t> local_turn_inputs_;
    std::map<uint16_t, PvpCustomCommit> remote_commits_;
    std::unordered_map<uint16_t, bool> acked_custom_turns_;
    std::deque<std::vector<uint8_t>> remote_states_;

    void reset_match_buffers() {
        active_match_format_ = PVP_FORMAT_SINGLE;
        peer_fmt_capable_ = false;
        has_host_build_fmt_ = false;
        match_started_ = false;
        peer_disconnected_ = false;
        remote_match_ended_ = false;
        remote_winner_slot_ = -1;
        has_remote_build_ = false;
        local_build_acked_ = false;
        remote_build_ = PvpFighterBuild{};
        last_bind_ms_ = 0;
        last_ping_ms_ = 0;
        last_input_req_ms_ = 0;
        last_input_req_key_ = UINT64_MAX;
        has_remote_tick_adv_ = false;
        remote_adv_turn_ = 0;
        remote_adv_frame_ = 0;
        remote_tick_adv_ = 0;
        remote_inputs_.clear();
        remote_turn_inputs_.clear();
        remote_turn_hashes_.clear();
        local_turn_inputs_.clear();
        remote_commits_.clear();
        acked_custom_turns_.clear();
        remote_states_.clear();
    }

    bool open_and_bind_udp(const std::string& server_host_port) {
        plat::ensure_sockets_init();
        std::string host;
        uint16_t port = 8765;
        if (!net::parse_host_port(server_host_port, host, port)) return false;

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo* res = nullptr;
        std::string pstr = std::to_string(port);
        if (getaddrinfo(host.c_str(), pstr.c_str(), &hints, &res) != 0 || !res) return false;

        udp_fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (udp_fd_ == plat::kInvalidSocket) {
            freeaddrinfo(res);
            return false;
        }
        plat::set_socket_nonblocking(udp_fd_, true);
        plat::disable_udp_connreset(udp_fd_);

        std::memcpy(&srv_addr_, res->ai_addr, res->ai_addrlen);
        srv_addr_len_ = static_cast<socklen_t>(res->ai_addrlen);
        freeaddrinfo(res);

        last_bind_ms_ = steady_millis();
        PvpPacketHeader b{};
        b.pkt_type = PKT_BIND;
        b.slot = slot_;
        b.room_id = room_id_;
        b.token = token_;
        auto wire = b.serialize();
        plat::sock_sendto(udp_fd_, wire.data(), wire.size(),
                          reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
        return true;
    }
};

} // namespace rmexe::pvp
