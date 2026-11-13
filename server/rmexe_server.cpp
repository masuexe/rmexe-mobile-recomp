#include "../common/capcom_protocol.hpp"
#include "../common/pvp_protocol.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_running = 1;

void handle_sig(int) {
    g_running = 0;
}

struct TcpClientConn {
    rmexe::plat::sock_t fd = rmexe::plat::kInvalidSocket;
    uint64_t created_ms = 0;
    std::vector<uint8_t> in_buf;
    std::vector<uint8_t> out_buf;
    size_t out_sent = 0;
    bool response_ready = false;
};

std::vector<uint8_t> serialize_http_response(const rmexe::net::HttpResponse& resp) {
    const char* reason = (resp.status_code == 200) ? "OK" :
                         (resp.status_code == 404) ? "Not Found" : "Bad Request";
    std::ostringstream head;
    head << "HTTP/1.1 " << resp.status_code << " " << reason << "\r\n";
    bool has_len = false;
    for (const auto& [k, v] : resp.headers) {
        if (rmexe::net::iequals(k, "Content-Length")) has_len = true;
        head << k << ": " << v << "\r\n";
    }
    if (!has_len) {
        head << "Content-Length: " << resp.body.size() << "\r\n";
    }
    head << "Connection: close\r\n\r\n";
    std::string hstr = head.str();

    std::vector<uint8_t> out;
    out.reserve(hstr.size() + resp.body.size());
    out.insert(out.end(), hstr.begin(), hstr.end());
    out.insert(out.end(), resp.body.begin(), resp.body.end());
    return out;
}

bool try_parse_http_request(
    const std::vector<uint8_t>& raw,
    std::string& method_out,
    std::string& uri_out,
    std::unordered_map<std::string, std::string>& headers_out,
    std::vector<uint8_t>& body_out
) {
    std::string_view sv(reinterpret_cast<const char*>(raw.data()), raw.size());
    auto hdr_end = sv.find("\r\n\r\n");
    if (hdr_end == std::string_view::npos) return false;

    std::string hdr_block(sv.substr(0, hdr_end));
    std::istringstream iss(hdr_block);
    std::string req_line;
    if (!std::getline(iss, req_line)) return false;
    if (!req_line.empty() && req_line.back() == '\r') req_line.pop_back();

    std::istringstream rls(req_line);
    if (!(rls >> method_out >> uri_out)) return false;

    headers_out.clear();
    size_t content_len = 0;
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto col = line.find(':');
        if (col != std::string::npos) {
            std::string k = line.substr(0, col);
            std::string v = line.substr(col + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            headers_out[k] = v;
            if (rmexe::net::iequals(k, "Content-Length")) {
                content_len = static_cast<size_t>(std::max(0, std::atoi(v.c_str())));
            }
        }
    }

    size_t body_start = hdr_end + 4;
    if (raw.size() < body_start + content_len) {
        return false; // Wait for full POST payload
    }
    body_out.assign(raw.begin() + body_start, raw.begin() + body_start + content_len);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string bind_ip = "0.0.0.0";
    uint16_t port = 8765;
    std::string data_dir = "server_data";
    std::string pon_sp_path;
    std::string lon_sp_path;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::clamp(std::atoi(argv[++i]), 1, 65535));
        } else if (arg == "--bind" && i + 1 < argc) {
            bind_ip = argv[++i];
        } else if (arg == "--data-dir" && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (arg == "--pon-sp" && i + 1 < argc) {
            pon_sp_path = argv[++i];
        } else if (arg == "--lon-sp" && i + 1 < argc) {
            lon_sp_path = argv[++i];
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "  --port <port>       TCP+UDP listen port (default: 8765)\n"
                      << "  --bind <ip>         Bind address        (default: 0.0.0.0)\n"
                      << "  --data-dir <dir>    Persistence folder  (default: server_data)\n"
                      << "  --pon-sp <path>     Optional PoN.sp for dataget.php asset slices\n"
                      << "  --lon-sp <path>     Optional LoN.sp for dataget.php asset slices\n"
                      << "  --quiet             Suppress per-request HTTP access logs\n";
            return 0;
        }
    }

    std::signal(SIGINT, handle_sig);
    std::signal(SIGTERM, handle_sig);

    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);

    rmexe::net::CapcomServiceState capcom_service;
    capcom_service.set_db_path(data_dir + "/capcom_state.db");

    // Auto-detect ScratchPad images if available so dataget.php can also serve full data/mld slices
    const std::vector<std::string> pon_candidates = {
        pon_sp_path, "assets/PoN_jp/sp/PoN.sp", "assets/PoN/sp/PoN.sp"
    };
    for (const auto& p : pon_candidates) {
        if (!p.empty() && capcom_service.load_sp_file(false, p)) {
            if (!quiet) std::cout << "[rmexe_server] Loaded PoN ScratchPad assets from " << p << "\n";
            break;
        }
    }
    const std::vector<std::string> lon_candidates = {
        lon_sp_path, "assets/LoN_jp/sp/LoN.sp", "assets/LoN/sp/LoN.sp"
    };
    for (const auto& p : lon_candidates) {
        if (!p.empty() && capcom_service.load_sp_file(true, p)) {
            if (!quiet) std::cout << "[rmexe_server] Loaded LoN ScratchPad assets from " << p << "\n";
            break;
        }
    }

    rmexe::plat::ensure_sockets_init();
    rmexe::pvp::PvpServerEngine pvp_engine;

    // 1. Bind TCP Listening Socket (HTTP/1.1: Capcom i-mode + PvP Matchmaking REST API)
    rmexe::plat::sock_t tcp_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd == rmexe::plat::kInvalidSocket) {
        std::cerr << "[rmexe_server] Failed to create TCP socket\n";
        return 1;
    }
    rmexe::plat::set_socket_reuseaddr(tcp_fd);
    rmexe::plat::set_socket_nonblocking(tcp_fd, true);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) <= 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(tcp_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(tcp_fd, 64) != 0) {
        std::cerr << "[rmexe_server] Failed to bind/listen TCP on " << bind_ip << ":" << port << "\n";
        rmexe::plat::close_socket(tcp_fd);
        return 1;
    }

    // 2. Bind UDP Socket on the EXACT SAME PORT (Real-Time PvP NetBattle Frame-Sync & Relay)
    rmexe::plat::sock_t udp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd == rmexe::plat::kInvalidSocket) {
        std::cerr << "[rmexe_server] Failed to create UDP socket\n";
        rmexe::plat::close_socket(tcp_fd);
        return 1;
    }
    rmexe::plat::set_socket_reuseaddr(udp_fd);
    rmexe::plat::set_socket_nonblocking(udp_fd, true);
    rmexe::plat::disable_udp_connreset(udp_fd);

    if (bind(udp_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[rmexe_server] Failed to bind UDP on " << bind_ip << ":" << port << "\n";
        rmexe::plat::close_socket(udp_fd);
        rmexe::plat::close_socket(tcp_fd);
        return 1;
    }

    pvp_engine.set_udp_fd(udp_fd);
    std::cout << std::unitbuf
              << "[rmexe_server] Unified Capcom i-mode + PvP NetBattle Server listening on "
              << bind_ip << ":" << port << " (TCP HTTP/1.1 + UDP Relay)\n"
              << "[rmexe_server] Persistent database: " << data_dir << "/capcom_state.db\n";

    std::vector<TcpClientConn> clients;
    std::vector<pollfd> pfds;
    uint64_t last_maint_ms = rmexe::pvp::steady_millis();

    while (g_running) {
        pfds.clear();
        pfds.push_back(pollfd{tcp_fd, POLLIN, 0});
        pfds.push_back(pollfd{udp_fd, POLLIN, 0});
        for (const auto& c : clients) {
            short ev = c.response_ready ? POLLOUT : POLLIN;
            pfds.push_back(pollfd{c.fd, ev, 0});
        }

        int ready = rmexe::plat::poll_sockets(pfds.data(), pfds.size(), 100);
        if (ready < 0) {
            if (rmexe::plat::socket_interrupted()) continue;
            break;
        }

        // Priority 1: Process all incoming UDP PvP datagrams immediately for minimal jitter
        if (pfds[1].revents & POLLIN) {
            uint8_t ubuf[1500];
            while (true) {
                sockaddr_storage src{};
                socklen_t slen = sizeof(src);
                ptrdiff_t n = rmexe::plat::sock_recvfrom(udp_fd, ubuf, sizeof(ubuf), reinterpret_cast<sockaddr*>(&src), &slen);
                if (n <= 0) break;
                pvp_engine.handle_udp_datagram(udp_fd, ubuf, static_cast<size_t>(n), src, slen);
            }
        }

        // Priority 2: Accept new TCP HTTP connections
        if (pfds[0].revents & POLLIN) {
            while (true) {
                sockaddr_in caddr{};
                socklen_t clen = sizeof(caddr);
                rmexe::plat::sock_t cfd = ::accept(tcp_fd, reinterpret_cast<sockaddr*>(&caddr), &clen);
                if (cfd == rmexe::plat::kInvalidSocket) break;
                int nodelay = 1;
#ifdef _WIN32
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
#else
                ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
#endif
                rmexe::plat::set_socket_nonblocking(cfd, true);
                if (clients.size() < 256) {
                    TcpClientConn cc{};
                    cc.fd = cfd;
                    cc.created_ms = rmexe::pvp::steady_millis();
                    clients.push_back(std::move(cc));
                } else {
                    rmexe::plat::close_socket(cfd);
                }
            }
        }

        // Priority 3: Drive non-blocking TCP HTTP state machines
        uint64_t now_ms = rmexe::pvp::steady_millis();
        for (size_t i = 0; i < clients.size();) {
            TcpClientConn& c = clients[i];
            short rev = (i + 2 < pfds.size()) ? pfds[i + 2].revents : 0;
            bool drop = false;

            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                drop = true;
            } else if (!c.response_ready && (rev & POLLIN)) {
                uint8_t rbuf[4096];
                ptrdiff_t n = rmexe::plat::sock_recv(c.fd, rbuf, sizeof(rbuf));
                if (n <= 0) {
                    drop = true;
                } else {
                    c.in_buf.insert(c.in_buf.end(), rbuf, rbuf + n);
                    if (c.in_buf.size() > 262144) {
                        drop = true; // Guard against oversized payloads
                    } else {
                        std::string method, uri;
                        std::unordered_map<std::string, std::string> req_hdrs;
                        std::vector<uint8_t> req_body;
                        if (try_parse_http_request(c.in_buf, method, uri, req_hdrs, req_body)) {
                            rmexe::net::HttpResponse resp;
                            std::string path_only = rmexe::net::extract_path_only(uri);
                            if (path_only == "/" || path_only == "/health") {
                                resp.status_code = 200;
                                resp.headers["Content-Type"] = "application/json";
                                std::string js = "{\"ok\":true,\"service\":\"rmexe_server\",\"accounts\":" +
                                                 std::to_string(capcom_service.account_count()) + "}";
                                resp.body.assign(js.begin(), js.end());
                            } else if (pvp_engine.is_pvp_http_path(uri)) {
                                resp = pvp_engine.handle_http(method, uri, req_body);
                            } else {
                                resp = capcom_service.handle_request(method, uri, req_hdrs, req_body);
                            }

                            if (!quiet) {
                                std::string uid_hdr;
                                for (const auto& [hk, hv] : req_hdrs) {
                                    if (rmexe::net::iequals(hk, "X-RMEXE-UID")) uid_hdr = hv;
                                }
                                std::cout << "[HTTP] " << method << " " << uri
                                          << (uid_hdr.empty() ? "" : (" [uid=" + uid_hdr + "]"))
                                          << " -> " << resp.status_code << " (" << resp.body.size() << "B)\n";
                            }

                            c.out_buf = serialize_http_response(resp);
                            c.response_ready = true;
                        }
                    }
                }
            } else if (c.response_ready && (rev & POLLOUT)) {
                ptrdiff_t n = rmexe::plat::sock_send(c.fd, c.out_buf.data() + c.out_sent, c.out_buf.size() - c.out_sent);
                if (n <= 0) {
                    drop = true;
                } else {
                    c.out_sent += static_cast<size_t>(n);
                    if (c.out_sent >= c.out_buf.size()) {
                        drop = true; // Complete!
                    }
                }
            }

            if (!drop && now_ms > c.created_ms && (now_ms - c.created_ms) > 5000ULL) {
                drop = true; // 5s HTTP keepalive timeout
            }

            if (drop) {
                rmexe::plat::close_socket(c.fd);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }

        // Priority 4: Periodic PvP room liveness check (auto-dissolve abandoned/dead rooms)
        if (now_ms > last_maint_ms && (now_ms - last_maint_ms) >= 1000ULL) {
            last_maint_ms = now_ms;
            pvp_engine.periodic_maintenance();
        }
    }

    for (const auto& c : clients) {
        rmexe::plat::close_socket(c.fd);
    }
    rmexe::plat::close_socket(udp_fd);
    rmexe::plat::close_socket(tcp_fd);
    std::cout << "[rmexe_server] Shut down cleanly.\n";
    return 0;
}
