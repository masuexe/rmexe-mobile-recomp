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
    PKT_BIND         = 1,
    PKT_BIND_ACK     = 2,
    PKT_MATCH_START  = 3,
    PKT_PING         = 4,
    PKT_PONG         = 5,
    PKT_FRAME_INPUT  = 6,
    PKT_BATTLE_STATE = 7,
    PKT_DISCONNECT   = 8,
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
        std::map<uint32_t, int32_t> recent_inputs; // Last 64 frames for loss recovery
    };

    struct Room {
        uint32_t room_id = 0;
        std::string game = "pon"; // "pon" or "lon"
        uint32_t rng_seed = 0;
        bool match_started = false;
        uint64_t created_ms = 0;
        PeerSlot peers[2]; // [0] = P1, [1] = P2
    };

    PvpServerEngine() : rng_(std::random_device{}()) {}

    bool is_pvp_http_path(const std::string& url) const {
        std::string path = net::extract_path_only(url);
        return path.rfind("/pvp/", 0) == 0;
    }

    net::HttpResponse handle_http(const std::string& method, const std::string& url) {
        std::lock_guard<std::mutex> lk(mtx_);
        prune_stale_rooms_unlocked();

        std::string path = net::extract_path_only(url);
        auto params = net::parse_query_params(url);

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
                   << ",\"host\":\"" << json_escape(rm.peers[0].name) << "\"}";
            }
            js << "]}";
        } else if (path == "/pvp/create" && net::iequals(method, "POST")) {
            uint32_t rid = next_room_id_++;
            Room rm{};
            rm.room_id = rid;
            rm.game = params.count("game") && !params["game"].empty() ? params["game"] : "pon";
            rm.rng_seed = rng_() | 1u;
            rm.created_ms = steady_millis();
            rm.peers[0].occupied = true;
            rm.peers[0].uid = params.count("uid") ? params["uid"] : "P1";
            rm.peers[0].name = params.count("name") && !params["name"].empty() ? params["name"] : "P1_NAVI";
            rm.peers[0].token = (rng_() & 0x7FFFFFFFu) | 0x10000u;
            rm.peers[0].last_seen_ms = rm.created_ms;

            uint32_t tok = rm.peers[0].token;
            rooms_[rid] = std::move(rm);
            js << "{\"ok\":true,\"room_id\":" << rid
               << ",\"slot\":1,\"token\":" << tok << "}";
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
                   << ",\"host\":\"" << json_escape(target->peers[0].name) << "\"}";
            }
        } else if (path == "/pvp/leave") {
            uint32_t rid = params.count("room") ? static_cast<uint32_t>(std::strtoul(params["room"].c_str(), nullptr, 10)) : 0;
            uint32_t tok = params.count("token") ? static_cast<uint32_t>(std::strtoul(params["token"].c_str(), nullptr, 10)) : 0;
            auto it = rooms_.find(rid);
            if (it != rooms_.end() &&
                (it->second.peers[0].token == tok || it->second.peers[1].token == tok)) {
                rooms_.erase(it);
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

    void handle_udp_datagram(int udp_fd, const uint8_t* data, size_t len, const sockaddr_storage& src_addr, socklen_t src_len) {
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
            ::sendto(udp_fd, wire.data(), wire.size(), 0, reinterpret_cast<const sockaddr*>(&src_addr), src_len);
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
                PvpPacketHeader ack{};
                ack.pkt_type = PKT_BIND_ACK;
                ack.slot = hdr.slot;
                ack.room_id = rm.room_id;
                ack.token = me.token;
                ack.frame_or_seq = rm.rng_seed;
                auto wire = ack.serialize();
                ::sendto(udp_fd, wire.data(), wire.size(), 0, reinterpret_cast<const sockaddr*>(&src_addr), src_len);

                // If both P1 and P2 have bound UDP endpoints, broadcast PKT_MATCH_START to both!
                if (rm.peers[0].udp_bound && rm.peers[1].udp_bound) {
                    rm.match_started = true;
                    for (int p = 0; p < 2; ++p) {
                        PvpPacketHeader st{};
                        st.pkt_type = PKT_MATCH_START;
                        st.slot = static_cast<uint8_t>(p + 1);
                        st.room_id = rm.room_id;
                        st.token = rm.peers[p].token;
                        st.frame_or_seq = rm.rng_seed;
                        const std::string& opp_name = rm.peers[1 - p].name;
                        auto swire = st.serialize(reinterpret_cast<const uint8_t*>(opp_name.data()), opp_name.size());
                        ::sendto(udp_fd, swire.data(), swire.size(), 0,
                                 reinterpret_cast<const sockaddr*>(&rm.peers[p].udp_addr), rm.peers[p].udp_addr_len);
                    }
                }
                break;
            }
            case PKT_PING: {
                PvpPacketHeader pong = hdr;
                pong.pkt_type = PKT_PONG;
                auto wire = pong.serialize();
                ::sendto(udp_fd, wire.data(), wire.size(), 0, reinterpret_cast<const sockaddr*>(&src_addr), src_len);
                break;
            }
            case PKT_FRAME_INPUT: {
                me.recent_inputs[hdr.frame_or_seq] = hdr.keypad_mask;
                while (me.recent_inputs.size() > 64) {
                    me.recent_inputs.erase(me.recent_inputs.begin());
                }
                if (opp.occupied && opp.udp_bound) {
                    ::sendto(udp_fd, data, len, 0,
                             reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                break;
            }
            case PKT_BATTLE_STATE: {
                if (opp.occupied && opp.udp_bound) {
                    ::sendto(udp_fd, data, len, 0,
                             reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                break;
            }
            case PKT_DISCONNECT: {
                if (opp.occupied && opp.udp_bound) {
                    ::sendto(udp_fd, data, len, 0,
                             reinterpret_cast<const sockaddr*>(&opp.udp_addr), opp.udp_addr_len);
                }
                rooms_.erase(it);
                break;
            }
            default:
                break;
        }
    }

private:
    std::mutex mtx_;
    std::mt19937 rng_;
    uint32_t next_room_id_ = 1001;
    std::unordered_map<uint32_t, Room> rooms_;

    void prune_stale_rooms_unlocked() {
        uint64_t now = steady_millis();
        for (auto it = rooms_.begin(); it != rooms_.end();) {
            uint64_t latest = std::max(it->second.peers[0].last_seen_ms, it->second.peers[1].last_seen_ms);
            if (now > latest && (now - latest) > 600000ULL) { // 10 min idle timeout
                it = rooms_.erase(it);
            } else {
                ++it;
            }
        }
    }
};

// ============================================================================
// Client-Side Non-Blocking PvP Lobby & UDP Lockstep Session Helper
// ============================================================================
class PvpClientSession {
public:
    ~PvpClientSession() {
        close_udp();
    }

    bool active() const { return udp_fd_ >= 0 && room_id_ != 0; }
    bool match_started() const { return match_started_; }
    uint32_t room_id() const { return room_id_; }
    uint8_t slot() const { return slot_; }
    uint32_t shared_rng_seed() const { return rng_seed_; }
    int last_rtt_ms() const { return last_rtt_ms_; }
    const std::string& opponent_name() const { return opponent_name_; }

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

    bool create_room(const std::string& server_host_port, const std::string& uid, const std::string& name, const std::string& game = "pon") {
        close_udp();
        std::string uri = "/pvp/create?uid=" + net::url_encode(uid) +
                          "&name=" + net::url_encode(name) +
                          "&game=" + net::url_encode(game);
        net::HttpResponse resp;
        if (!net::http_request_tcp(server_host_port, "POST", uri, {}, {}, resp, 2000) || resp.status_code != 200) {
            return false;
        }
        std::string js(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
        if (json_extract_str(js, "ok") != "true") return false;

        room_id_ = static_cast<uint32_t>(std::strtoul(json_extract_str(js, "room_id").c_str(), nullptr, 10));
        token_   = static_cast<uint32_t>(std::strtoul(json_extract_str(js, "token").c_str(), nullptr, 10));
        slot_    = 1;
        match_started_ = false;
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

        room_id_       = static_cast<uint32_t>(std::strtoul(json_extract_str(js, "room_id").c_str(), nullptr, 10));
        token_         = static_cast<uint32_t>(std::strtoul(json_extract_str(js, "token").c_str(), nullptr, 10));
        opponent_name_ = json_extract_str(js, "host");
        slot_          = 2;
        match_started_ = false;
        server_addr_str_ = server_host_port;
        return open_and_bind_udp(server_host_port);
    }

    void send_frame_input(uint32_t frame, int32_t keypad_mask) {
        if (udp_fd_ < 0) return;
        PvpPacketHeader h{};
        h.pkt_type = PKT_FRAME_INPUT;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = frame;
        h.keypad_mask = keypad_mask;
        auto wire = h.serialize();
        ::sendto(udp_fd_, wire.data(), wire.size(), 0,
                 reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    void send_battle_state(uint32_t seq, const uint8_t* payload, size_t len) {
        if (udp_fd_ < 0) return;
        PvpPacketHeader h{};
        h.pkt_type = PKT_BATTLE_STATE;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = seq;
        auto wire = h.serialize(payload, len);
        ::sendto(udp_fd_, wire.data(), wire.size(), 0,
                 reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    void send_ping() {
        if (udp_fd_ < 0) return;
        PvpPacketHeader h{};
        h.pkt_type = PKT_PING;
        h.slot = slot_;
        h.room_id = room_id_;
        h.token = token_;
        h.frame_or_seq = static_cast<uint32_t>(steady_millis() & 0xFFFFFFFFu);
        auto wire = h.serialize();
        ::sendto(udp_fd_, wire.data(), wire.size(), 0,
                 reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
    }

    // Polls all pending UDP packets without blocking; returns true if PKT_MATCH_START newly arrived
    bool poll_udp() {
        if (udp_fd_ < 0) return false;
        bool newly_started = false;
        uint8_t buf[1500];
        while (true) {
            ssize_t n = ::recvfrom(udp_fd_, buf, sizeof(buf), 0, nullptr, nullptr);
            if (n <= 0) break;
            PvpPacketHeader h{};
            const uint8_t* payload = nullptr;
            if (!PvpPacketHeader::deserialize(buf, static_cast<size_t>(n), h, payload)) continue;

            if (h.pkt_type == PKT_BIND_ACK) {
                udp_bound_ = true;
                rng_seed_ = h.frame_or_seq;
            } else if (h.pkt_type == PKT_MATCH_START) {
                rng_seed_ = h.frame_or_seq;
                if (payload && h.payload_len > 0) {
                    opponent_name_.assign(reinterpret_cast<const char*>(payload), h.payload_len);
                }
                if (!match_started_) newly_started = true;
                match_started_ = true;
            } else if (h.pkt_type == PKT_PONG) {
                uint32_t now32 = static_cast<uint32_t>(steady_millis() & 0xFFFFFFFFu);
                last_rtt_ms_ = static_cast<int>(now32 - h.frame_or_seq);
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
                match_started_ = false;
                room_id_ = 0;
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
        if (udp_fd_ >= 0) {
            if (room_id_ != 0) {
                PvpPacketHeader d{};
                d.pkt_type = PKT_DISCONNECT;
                d.slot = slot_;
                d.room_id = room_id_;
                d.token = token_;
                auto wire = d.serialize();
                ::sendto(udp_fd_, wire.data(), wire.size(), 0,
                         reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
            }
            ::close(udp_fd_);
            udp_fd_ = -1;
        }
        room_id_ = 0;
        token_ = 0;
        udp_bound_ = false;
        match_started_ = false;
        remote_inputs_.clear();
        remote_states_.clear();
    }

private:
    int udp_fd_ = -1;
    sockaddr_storage srv_addr_{};
    socklen_t srv_addr_len_ = 0;
    std::string server_addr_str_;
    uint32_t room_id_ = 0;
    uint32_t token_ = 0;
    uint8_t slot_ = 0;
    uint32_t rng_seed_ = 0;
    bool udp_bound_ = false;
    bool match_started_ = false;
    int last_rtt_ms_ = -1;
    std::string opponent_name_;
    std::map<uint32_t, int32_t> remote_inputs_;
    std::deque<std::vector<uint8_t>> remote_states_;

    bool open_and_bind_udp(const std::string& server_host_port) {
        std::string host;
        uint16_t port = 8765;
        if (!net::parse_host_port(server_host_port, host, port)) return false;

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo* res = nullptr;
        std::string pstr = std::to_string(port);
        if (getaddrinfo(host.c_str(), pstr.c_str(), &hints, &res) != 0 || !res) return false;

        udp_fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (udp_fd_ < 0) {
            freeaddrinfo(res);
            return false;
        }
        int flags = fcntl(udp_fd_, F_GETFL, 0);
        fcntl(udp_fd_, F_SETFL, flags | O_NONBLOCK);

        std::memcpy(&srv_addr_, res->ai_addr, res->ai_addrlen);
        srv_addr_len_ = res->ai_addrlen;
        freeaddrinfo(res);

        PvpPacketHeader b{};
        b.pkt_type = PKT_BIND;
        b.slot = slot_;
        b.room_id = room_id_;
        b.token = token_;
        auto wire = b.serialize();
        ::sendto(udp_fd_, wire.data(), wire.size(), 0,
                 reinterpret_cast<const sockaddr*>(&srv_addr_), srv_addr_len_);
        return true;
    }
};

} // namespace rmexe::pvp
