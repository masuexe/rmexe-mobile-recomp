#include "nettai_guard.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << "\n";
        ++g_failures;
    } else {
        std::cout << "[PASS] " << msg << "\n";
    }
}

void write_dummy(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p);
    ofs << "dummy\n";
}

} // namespace

int main() {
    std::error_code ec;
    fs::path tmp_root = fs::temp_directory_path(ec) / ("rmexe_nettai_guard_test_" + std::to_string(rmexe::guard::current_pid()));
    fs::remove_all(tmp_root, ec);
    fs::create_directories(tmp_root, ec);

    // 1. Clean environment passes (non-existent candidates produce zero hits)
    {
        std::vector<fs::path> clean_candidates = {
            tmp_root / "does_not_exist_dir",
            tmp_root / "bin" / "nettai",
        };
        auto hits = rmexe::guard::collect_path_hits(clean_candidates, tmp_root / "self_bin");
        check(hits.empty(), "clean environment yields zero hits");
    }

    // 2. Hit file/dir detection & deduplication (including symlink / unnormalized path)
    {
        fs::path nettai_dir = tmp_root / "share" / "nettai";
        fs::path nettai_bin = tmp_root / "bin" / "nettai";
        fs::create_directories(nettai_dir, ec);
        write_dummy(nettai_bin);

        fs::path dup_dir = tmp_root / "share" / "." / "nettai";
        fs::path sym_bin = tmp_root / "bin" / "nettai_link";
        fs::create_symlink(nettai_bin, sym_bin, ec);

        std::vector<fs::path> candidates = {
            nettai_dir,
            dup_dir,
            nettai_bin,
            nettai_bin,
            sym_bin,
        };
        auto hits = rmexe::guard::collect_path_hits(candidates, tmp_root / "other_self");
        check(hits.size() == 2, "duplicate files/dirs and symlinks deduplicate to 2 unique hits");

        // 4. Refusal message lists every hit item and instructs removing nettai
        std::string msg = rmexe::guard::format_refusal_message(hits);
        check(msg.find("nettai") != std::string::npos, "refusal message mentions nettai");
        bool all_listed = true;
        for (const auto& h : hits) {
            if (msg.find(h) == std::string::npos) all_listed = false;
        }
        check(all_listed, "refusal message contains all hit items");
    }

    // 3. Strictly exclude current executable path (canonicalized comparison)
    {
        fs::path self_real = tmp_root / "app" / "nettai";
        write_dummy(self_real);
        fs::path self_alias = tmp_root / "app" / "." / "nettai";
        fs::path self_link = tmp_root / "app" / "self_symlink";
        fs::create_symlink(self_real, self_link, ec);

        std::vector<fs::path> candidates = {self_real, self_alias, self_link};
        auto hits = rmexe::guard::collect_path_hits(candidates, self_real);
        check(hits.empty(), "self executable path (and symlink/alias) is excluded");
    }

    fs::remove_all(tmp_root, ec);
    if (g_failures > 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All nettai_guard unit tests passed.\n";
    return 0;
}
