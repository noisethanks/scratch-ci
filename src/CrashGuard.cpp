#include "CrashGuard.hpp"
#include "StatePath.hpp"

#include <cerrno>
#include <charconv>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <unordered_map>

namespace hyprtail::crashguard {
    namespace {
        constexpr const char* MARKER_FILENAME = "crash-guard.marker";

        // Same durable-write idiom as Diagnostics.cpp's writeDurable
        // (open/write/fsync the file, then fsync the directory so the write
        // survives a power-off) but kept separate: this file has no
        // Hyprland dependency and the marker is always a full replace,
        // never an append.
        bool writeDurable(const std::filesystem::path& path, std::string_view text) {
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0)
                return false;

            bool   ok  = true;
            size_t off = 0;
            while (off < text.size()) {
                const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    ok = false;
                    break;
                }
                off += static_cast<size_t>(n);
            }
            if (ok)
                ::fsync(fd);
            ::close(fd);
            if (!ok)
                return false;

            if (const int dfd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); dfd >= 0) {
                ::fsync(dfd);
                ::close(dfd);
            }
            return true;
        }
    }

    std::filesystem::path markerPath() {
        const auto dir = hyprtail::stateDir();
        return dir.empty() ? std::filesystem::path{} : dir / MARKER_FILENAME;
    }

    std::optional<SMarker> parse(std::string_view text) {
        std::unordered_map<std::string, std::string> fields;

        std::istringstream in{std::string{text}};
        std::string        line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            fields.emplace(line.substr(0, eq), line.substr(eq + 1));
        }

        const auto rev = fields.find("rev");
        const auto hy  = fields.find("hyprland");
        const auto ins = fields.find("instance");
        const auto pidF = fields.find("pid");
        if (rev == fields.end() || hy == fields.end() || ins == fields.end() || pidF == fields.end())
            return std::nullopt;

        long long  pid = 0;
        const auto res = std::from_chars(pidF->second.data(), pidF->second.data() + pidF->second.size(), pid);
        if (res.ec != std::errc{} || res.ptr != pidF->second.data() + pidF->second.size() || pid <= 0)
            return std::nullopt;

        return SMarker{rev->second, hy->second, ins->second, pid};
    }

    std::string format(const SKey& key, std::string_view instanceSignature, long long pid) {
        std::ostringstream out;
        out << "rev=" << key.revision << '\n'
            << "hyprland=" << key.hyprlandHash << '\n'
            << "instance=" << instanceSignature << '\n'
            << "pid=" << pid << '\n';
        return out.str();
    }

    bool pidIsDead(long long pid) {
        if (pid <= 0)
            return false; // malformed; never treat as dead
        errno = 0;
        if (::kill(static_cast<pid_t>(pid), 0) == 0)
            return false; // alive
        return errno == ESRCH; // only a definite "no such process" counts as dead
    }

    bool indicatesEarlyDeath(const SMarker& marker, const SKey& key) {
        return marker.revision == key.revision && marker.hyprlandHash == key.hyprlandHash && pidIsDead(marker.pid);
    }

    std::optional<SMarker> readMarker() {
        const auto path = markerPath();
        if (path.empty())
            return std::nullopt;

        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::nullopt;

        std::ostringstream ss;
        ss << in.rdbuf();
        return parse(ss.str());
    }

    bool writeMarker(const SKey& key, std::string_view instanceSignature, long long pid) {
        const auto path = markerPath();
        if (path.empty())
            return false;

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
            return false;

        return writeDurable(path, format(key, instanceSignature, pid));
    }

    void removeMarker() {
        const auto path = markerPath();
        if (path.empty())
            return;
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
}
