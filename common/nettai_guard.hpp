#pragma once

// ============================================================================
// common/nettai_guard.hpp
// Cross-platform startup guard that refuses launch when `nettai` is detected.
// Header-only, zero SDL2 or game-state dependency (safe for runtime & server).
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if !defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE
#include <mach-o/dyld.h>
#include <unistd.h>
#endif
#elif defined(__linux__) && !defined(__ANDROID__)
#include <unistd.h>
#endif

namespace rmexe::guard {

inline std::filesystem::path canonicalize_best_effort(const std::filesystem::path& p) {
    if (p.empty()) return {};
    std::error_code ec;
    auto canon = std::filesystem::canonical(p, ec);
    if (!ec) return canon.lexically_normal();
    ec.clear();
    auto weak = std::filesystem::weakly_canonical(p, ec);
    if (!ec) return weak.lexically_normal();
    return p.lexically_normal();
}

inline std::filesystem::path current_executable_path() {
#if defined(_WIN32)
    wchar_t buf[32768];
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len > 0 && len < std::size(buf)) {
        return canonicalize_best_effort(std::filesystem::path(std::wstring(buf, len)));
    }
    return {};
#elif defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size > 0) {
        std::string buf(size, '\0');
        if (_NSGetExecutablePath(buf.data(), &size) == 0) {
            return canonicalize_best_effort(std::filesystem::path(buf.c_str()));
        }
    }
    return {};
#elif defined(__linux__) && !defined(__ANDROID__)
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) {
        return canonicalize_best_effort(exe);
    }
    return {};
#else
    return {};
#endif
}

inline unsigned long current_pid() {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#elif (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE))
    return static_cast<unsigned long>(getpid());
#else
    return 0UL;
#endif
}

inline bool paths_equal_canonical(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) return false;
    auto ca = canonicalize_best_effort(a);
    auto cb = canonicalize_best_effort(b);
#if defined(_WIN32)
    auto sa = ca.wstring();
    auto sb = cb.wstring();
    if (sa.size() != sb.size()) return false;
    for (size_t i = 0; i < sa.size(); ++i) {
        if (std::towlower(sa[i]) != std::towlower(sb[i])) return false;
    }
    return true;
#else
    return ca == cb;
#endif
}

inline std::vector<std::string> collect_path_hits(
    const std::vector<std::filesystem::path>& candidates,
    const std::filesystem::path& self_exe
) {
    const auto self_canon = canonicalize_best_effort(self_exe);
    std::set<std::string> seen_keys;
    std::vector<std::string> hits;

    for (const auto& raw : candidates) {
        if (raw.empty()) continue;
        std::error_code ec;
        auto st = std::filesystem::symlink_status(raw, ec);
        if (ec || !std::filesystem::exists(st)) continue;

        auto canon = canonicalize_best_effort(raw);
        if (!self_canon.empty() && paths_equal_canonical(canon, self_canon)) {
            continue;
        }

        std::string key = canon.string();
#if defined(_WIN32)
        for (char& ch : key) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
#endif
        if (seen_keys.insert(key).second) {
            bool is_dir = std::filesystem::is_directory(canon, ec);
            hits.push_back(std::string(is_dir ? "dir: " : "file: ") + canon.string());
        }
    }
    return hits;
}

inline std::vector<std::string> deduplicate_hits(const std::vector<std::string>& raw_hits) {
    std::set<std::string> seen;
    std::vector<std::string> out;
    out.reserve(raw_hits.size());
    for (const auto& h : raw_hits) {
        if (!h.empty() && seen.insert(h).second) {
            out.push_back(h);
        }
    }
    return out;
}

inline std::string format_refusal_message(const std::vector<std::string>& hits) {
    std::ostringstream oss;
    oss << "Startup blocked: detected `nettai` on this device.\n"
        << "启动已阻止：检测到本机安装或运行了 `nettai`。\n\n"
        << "Detected item(s) / 命中项:\n";
    for (const auto& h : hits) {
        oss << "  - " << h << "\n";
    }
    oss << "\nPlease completely uninstall/remove `nettai` and try again.\n"
        << "请彻底卸载或移除 `nettai` 后重试。";
    return oss.str();
}

#if defined(_WIN32)
inline std::filesystem::path env_path_win(const wchar_t* name) {
    wchar_t buf[32768];
    DWORD len = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return {};
    return std::filesystem::path(std::wstring(buf, len));
}

inline bool iequals_wide(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        if (std::towlower(*a) != std::towlower(*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}
#endif

#if (defined(__linux__) && !defined(__ANDROID__)) || (defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE))
inline std::vector<std::filesystem::path> split_colon_paths(const char* val) {
    std::vector<std::filesystem::path> out;
    if (!val || !*val) return out;
    std::string s(val);
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(':', start);
        std::string part = (pos == std::string::npos) ? s.substr(start) : s.substr(start, pos - start);
        if (!part.empty()) out.emplace_back(part);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}
#endif

inline std::vector<std::string> detect_nettai(const std::filesystem::path& self_exe_override = {}) {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
    (void)self_exe_override;
    // iOS / Web(WASM) are exempted by OS sandbox; Android checks PackageManager in Java before native startup.
    return {};
#else
    const auto self_exe = self_exe_override.empty() ? current_executable_path()
                                                    : canonicalize_best_effort(self_exe_override);
    const unsigned long self_pid = current_pid();
    std::vector<std::filesystem::path> candidates;
    std::vector<std::string> extra_hits;

#if defined(_WIN32)
    if (auto p = env_path_win(L"LOCALAPPDATA"); !p.empty()) {
        candidates.push_back(p / L"nettai");
    }
    if (auto p = env_path_win(L"APPDATA"); !p.empty()) {
        candidates.push_back(p / L"nettai");
        candidates.push_back(p / L"Microsoft" / L"Windows" / L"Start Menu" / L"Programs" / L"nettai.lnk");
    }
    if (auto p = env_path_win(L"ProgramFiles"); !p.empty()) {
        candidates.push_back(p / L"nettai");
    }
    if (auto p = env_path_win(L"ProgramFiles(x86)"); !p.empty()) {
        candidates.push_back(p / L"nettai");
    }
    if (auto p = env_path_win(L"USERPROFILE"); !p.empty()) {
        candidates.push_back(p / L"Desktop" / L"nettai.lnk");
    }

    // Registry check (native Win32 API, no console window spawned)
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\nettai",
                      0, KEY_READ, &hkey) == ERROR_SUCCESS) {
        RegCloseKey(hkey);
        extra_hits.push_back("registry: HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\nettai");
    }

    // Process check (native Toolhelp32 API, no console window spawned)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == self_pid) continue;
                if (iequals_wide(pe.szExeFile, L"nettai.exe")) {
                    bool is_self_exe = false;
                    if (!self_exe.empty()) {
                        HANDLE hproc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                        if (hproc) {
                            wchar_t img[32768];
                            DWORD sz = static_cast<DWORD>(std::size(img));
                            if (QueryFullProcessImageNameW(hproc, 0, img, &sz)) {
                                if (paths_equal_canonical(std::filesystem::path(std::wstring(img, sz)), self_exe)) {
                                    is_self_exe = true;
                                }
                            }
                            CloseHandle(hproc);
                        }
                    }
                    if (!is_self_exe) {
                        extra_hits.push_back("process: nettai.exe (PID " + std::to_string(pe.th32ProcessID) + ")");
                    }
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

#elif defined(__linux__) && !defined(__ANDROID__)
    const char* home = std::getenv("HOME");
    const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
    const char* xdg_data_dirs = std::getenv("XDG_DATA_DIRS");
    const char* path_env = std::getenv("PATH");

    if (xdg_data_home && *xdg_data_home) {
        std::filesystem::path xdh(xdg_data_home);
        candidates.push_back(xdh / "nettai");
        candidates.push_back(xdh / "applications" / "nettai.desktop");
        candidates.push_back(xdh / "applications" / "net.n1gp.nettai.desktop");
    }
    if (home && *home) {
        std::filesystem::path h(home);
        candidates.push_back(h / ".local" / "share" / "nettai");
        candidates.push_back(h / ".local" / "bin" / "nettai");
        candidates.push_back(h / ".local" / "share" / "applications" / "nettai.desktop");
        candidates.push_back(h / ".local" / "share" / "applications" / "net.n1gp.nettai.desktop");
    }
    candidates.push_back("/opt/nettai");
    candidates.push_back("/usr/bin/nettai");
    candidates.push_back("/usr/local/bin/nettai");
    candidates.push_back("/usr/share/applications/nettai.desktop");
    candidates.push_back("/usr/share/applications/net.n1gp.nettai.desktop");
    candidates.push_back("/usr/local/share/applications/nettai.desktop");
    candidates.push_back("/usr/local/share/applications/net.n1gp.nettai.desktop");

    for (const auto& dir : split_colon_paths(xdg_data_dirs)) {
        candidates.push_back(dir / "applications" / "nettai.desktop");
        candidates.push_back(dir / "applications" / "net.n1gp.nettai.desktop");
    }
    for (const auto& dir : split_colon_paths(path_env)) {
        candidates.push_back(dir / "nettai");
    }

    // Scan /proc/<pid>/comm for running `nettai`
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        if (ec) break;
        std::string fname = entry.path().filename().string();
        if (fname.empty() || !std::all_of(fname.begin(), fname.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            continue;
        }
        unsigned long pid = std::strtoul(fname.c_str(), nullptr, 10);
        if (pid == 0 || pid == self_pid) continue;

        std::ifstream comm_in(entry.path() / "comm");
        std::string comm;
        if (std::getline(comm_in, comm)) {
            while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
            if (comm == "nettai") {
                std::error_code exe_ec;
                auto pexe = std::filesystem::read_symlink(entry.path() / "exe", exe_ec);
                if (!exe_ec && !self_exe.empty() && paths_equal_canonical(pexe, self_exe)) {
                    continue;
                }
                extra_hits.push_back("process: nettai (PID " + std::to_string(pid) + ")");
            }
        }
    }

#elif defined(__APPLE__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
    const char* home = std::getenv("HOME");
    candidates.push_back("/Applications/nettai.app");
    if (home && *home) {
        std::filesystem::path h(home);
        candidates.push_back(h / "Applications" / "nettai.app");
        candidates.push_back(h / "Library" / "Application Support" / "nettai");
    }

    // Bundle ID lookup via Spotlight mdfind
    if (FILE* fp = popen("mdfind \"kMDItemCFBundleIdentifier == 'net.n1gp.nettai'\" 2>/dev/null", "r")) {
        char line[4096];
        while (std::fgets(line, sizeof(line), fp)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) {
                std::filesystem::path p(s);
                if (self_exe.empty() || !paths_equal_canonical(p, self_exe)) {
                    extra_hits.push_back("bundle(net.n1gp.nettai): " + s);
                }
            }
        }
        pclose(fp);
    }

    // Process check via pgrep
    if (FILE* fp = popen("pgrep -x nettai 2>/dev/null", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), fp)) {
            unsigned long pid = std::strtoul(line, nullptr, 10);
            if (pid != 0 && pid != self_pid) {
                extra_hits.push_back("process: nettai (PID " + std::to_string(pid) + ")");
            }
        }
        pclose(fp);
    }
#endif

    auto hits = collect_path_hits(candidates, self_exe);
    hits.insert(hits.end(), extra_hits.begin(), extra_hits.end());
    return deduplicate_hits(hits);
#endif
}

inline void enforce_or_exit(
    const std::function<void(const std::string& title, const std::string& msg)>& gui_popup = nullptr
) {
    auto hits = detect_nettai();
    if (hits.empty()) return;

    const std::string msg = format_refusal_message(hits);
    std::cerr << msg << std::endl;
    if (gui_popup) {
        gui_popup("Startup Blocked / 启动已阻止", msg);
    }
    std::exit(1);
}

} // namespace rmexe::guard
