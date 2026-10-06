#pragma once
#include <filesystem>
#include <limits>
#include <set>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace iiSocietySync::detail {
struct WatchPaths {
    std::vector<std::filesystem::path> paths;
    std::size_t visitedEntries = 0;
    bool cancelled = false;
};

// Streaming, bounded discovery: do not materialize/sort a whole directory to
// fill a small native watcher budget. No Qt or payload reads in this helper.
template<class Cancelled>
WatchPaths collectWatchPaths(const std::vector<std::filesystem::path> &roots,
                            std::size_t limit, Cancelled cancelled) {
    namespace fs = std::filesystem;
    WatchPaths result;
    if (cancelled()) { result.cancelled = true; return result; }
    if (!limit) return result;
    const auto inspectionLimit = limit > std::numeric_limits<std::size_t>::max() / 8
        ? std::numeric_limits<std::size_t>::max() : limit * 8;
    std::set<fs::path> seen;
    std::vector<fs::path> directories;
    const auto societyPrefix = fs::path(".society-").native();
    const auto hostPrefix = fs::path(".iiserverhost-").native();
    auto include = [&](const fs::path &path, bool directory) {
        if (!seen.insert(path).second) return;
        result.paths.push_back(path);
        if (directory) directories.push_back(path);
    };
    const auto reparse = [](const fs::path &path) {
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(path.c_str());
        return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
        (void)path;
        return false;
#endif
    };
    // Give every section root priority over any one section's descendants.
    for (const auto &root : roots) {
        if (cancelled()) { result.cancelled = true; return result; }
        if (result.paths.size() >= limit) break;
        std::error_code error;
        if (!reparse(root) && fs::is_directory(fs::symlink_status(root, error)) && !error) include(root, true);
    }
    for (std::size_t next = 0; next < directories.size() && result.paths.size() < limit
         && result.visitedEntries < inspectionLimit; ++next) {
        if (cancelled()) { result.cancelled = true; return result; }
        std::error_code error;
        fs::directory_iterator entry(directories[next], error), end;
        while (!error && entry != end && result.paths.size() < limit
               && result.visitedEntries < inspectionLimit) {
            if (cancelled()) { result.cancelled = true; return result; }
            ++result.visitedEntries;
            const auto name = entry->path().filename().native();
            if (!name.starts_with(societyPrefix) && !name.starts_with(hostPrefix)) {
                std::error_code statusError;
                const auto status = entry->symlink_status(statusError);
                if (!statusError && !reparse(entry->path()) && (fs::is_directory(status) || fs::is_regular_file(status)))
                    include(entry->path(), fs::is_directory(status));
            }
            // Do not fetch another directory block once the budget is full.
            if (result.paths.size() >= limit || result.visitedEntries >= inspectionLimit) break;
            if (cancelled()) { result.cancelled = true; return result; }
            entry.increment(error);
        }
    }
    result.cancelled = cancelled();
    return result;
}
}
