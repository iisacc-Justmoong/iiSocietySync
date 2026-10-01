#include "WatchPaths.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>

namespace fs = std::filesystem;
static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    const auto root = fs::path(SYNC_TEST_DIRECTORY) / ("watch-paths-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    try {
        fs::create_directories(root / "visible");
        fs::create_directories(root / ".society-private");
        fs::create_directories(root / ".iiserverhost-private");
        std::ofstream(root / "visible" / "file") << "payload";
        std::ofstream(root / ".society-private" / "secret") << "internal";
        std::ofstream(root / ".hidden") << "visible hidden file";
        fs::create_directory_symlink(root / "visible", root / "link");
        const auto all = iiSocietySync::detail::collectWatchPaths({root}, 64, [] { return false; });
        require(!all.cancelled && all.paths.size() == 4, "exclude internal trees and symlinks, retain hidden files");
        for (int i = 0; i < 100; ++i) fs::create_directory(root / ("directory-" + std::to_string(i)));
        const auto bounded = iiSocietySync::detail::collectWatchPaths({root}, 3, [] { return false; });
        require(bounded.paths.size() == 3 && bounded.visitedEntries <= 24, "stop enumeration at bounded work budget");
        require(iiSocietySync::detail::collectWatchPaths({root}, 0, [] { return false; }).paths.empty(), "zero budget");
        const auto cancelled = iiSocietySync::detail::collectWatchPaths({root}, 64, [] { return true; });
        require(cancelled.cancelled && cancelled.paths.empty() && cancelled.visitedEntries == 0, "cancel before filesystem access");
        unsigned checks = 0;
        const auto interrupted = iiSocietySync::detail::collectWatchPaths({root}, 64, [&] { return ++checks >= 5; });
        require(interrupted.cancelled && interrupted.visitedEntries < 10, "check cancellation during a directory");
        const auto symlink = iiSocietySync::detail::collectWatchPaths({root / "link"}, 64, [] { return false; });
        require(symlink.paths.empty(), "do not follow a symlink root");
        const auto sections = iiSocietySync::detail::collectWatchPaths({root, root / "visible"}, 2, [] { return false; });
        require(sections.paths.size() == 2 && sections.visitedEntries == 0, "prioritize section roots over descendants");
        fs::create_directory(root / "excluded-only");
        for (int i = 0; i < 20; ++i) std::ofstream(root / "excluded-only" / (".society-" + std::to_string(i))) << "internal";
        const auto excluded = iiSocietySync::detail::collectWatchPaths({root / "excluded-only"}, 2, [] { return false; });
        require(excluded.paths.size() == 1 && excluded.visitedEntries == 16, "bound inspection even when no entries are eligible");
        std::cout << "bounded watch traversal contracts passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
