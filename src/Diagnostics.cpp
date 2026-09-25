#include "Diagnostics.hpp"

#include <any>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <managers/eventLoop/EventLoopManager.hpp>
#include <helpers/Color.hpp>
#include <debug/log/Logger.hpp>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include "rev.hpp"

namespace hyprtail::diag {
    namespace {
        // The error file holds the current session (started fresh in init; the
        // previous session's file is kept as errors.log.1) and stops growing
        // at this size.
        constexpr size_t   ERROR_FILE_CAP_BYTES = 256 * 1024;

        constexpr size_t   NOTIFY_MAX_LINES = 8;
        constexpr size_t   NOTIFY_MAX_CHARS = 600;
        constexpr uint64_t NOTIFY_ERR_MS    = 15000;
        constexpr uint64_t NOTIFY_WARN_MS   = 10000;

        struct SPending {
            eSeverity   severity;
            std::string text;
        };

        struct SState {
            HANDLE                          handle = nullptr;
            std::string                     filePath;
            size_t                          fileBytes  = 0;
            bool                            fileCapped = false;
            std::unordered_set<std::string> seen;
            std::vector<SPending>           pending;
            std::optional<uint64_t>         doLaterSeq;
        };

        SState& state() {
            static SState s;
            return s;
        }

        const char* severityName(eSeverity s) {
            return s == eSeverity::ERR ? "error" : "warning";
        }

        // $XDG_STATE_HOME/hyprtail/errors.log, else ~/.local/state/hyprtail/errors.log.
        std::filesystem::path resolveErrorFile() {
            const char* xdg = std::getenv("XDG_STATE_HOME");
            if (xdg && xdg[0] == '/')
                return std::filesystem::path{xdg} / "hyprtail" / "errors.log";

            const char* home = std::getenv("HOME");
            if (home && home[0] == '/')
                return std::filesystem::path{home} / ".local" / "state" / "hyprtail" / "errors.log";

            return {};
        }

        std::string utcNow() {
            return std::format("{:%F %T} UTC", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        }

        // Write text to path (appending, or replacing the file) and fsync it.
        // The file is for debugging after the fact, and a hang that ends in a
        // power-off loses whatever is still only in the page cache. Writes
        // are rare (one per distinct report key), so the sync is cheap.
        bool writeDurable(const std::string& path, std::string_view text, bool replace) {
            const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (replace ? O_TRUNC : O_APPEND), 0644);
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
                off += sc<size_t>(n);
            }

            if (ok)
                ::fsync(fd);
            ::close(fd);
            return ok;
        }

        // fsync a directory, so a rename in it survives a power-off.
        void syncDir(const std::filesystem::path& dir) {
            const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0)
                return;
            ::fsync(fd);
            ::close(fd);
        }

        void appendToFile(std::string_view text) {
            auto& st = state();
            if (st.filePath.empty() || st.fileCapped)
                return;

            if (st.fileBytes + text.size() > ERROR_FILE_CAP_BYTES) {
                writeDurable(st.filePath, "[hyprtail] error file size cap reached, further messages only go to the Hyprland log\n", false);
                st.fileCapped = true;
                return;
            }

            if (writeDurable(st.filePath, text, false))
                st.fileBytes += text.size();
        }

        std::string truncateForNotification(std::string_view msg) {
            std::string out;
            size_t      lines = 1;
            for (const char c : msg) {
                if (c == '\n' && ++lines > NOTIFY_MAX_LINES)
                    return out + "\n… (truncated)";
                if (out.size() >= NOTIFY_MAX_CHARS)
                    return out + "… (truncated)";
                out += c;
            }
            return out;
        }

        void notifyNow(const SPending& p) {
            auto&            st   = state();
            const bool       err  = p.severity == eSeverity::ERR;
            const CHyprColor col  = err ? CHyprColor{1.0F, 0.2F, 0.2F, 1.0F} : CHyprColor{1.0F, 0.6F, 0.1F, 1.0F};
            const uint64_t   time = err ? NOTIFY_ERR_MS : NOTIFY_WARN_MS;

            const bool       ok = HyprlandAPI::addNotificationV2(st.handle,
                                                                 {
                                                               {"text", p.text},
                                                               {"time", time},
                                                               {"color", col},
                                                               {"icon", err ? ICON_ERROR : ICON_WARNING},
                                                           });
            if (!ok)
                HyprlandAPI::addNotification(st.handle, p.text, col, sc<float>(time));
        }

        // Runs from the event loop, never inside a render.
        void flushPending() noexcept {
            auto& st = state();
            st.doLaterSeq.reset();

            auto pending = std::move(st.pending);
            st.pending.clear();

            for (const auto& p : pending) {
                try {
                    notifyNow(p);
                } catch (...) {
                    // Nothing sensible left to report to; the message is
                    // already in the Hyprland log and the error file.
                }
            }
        }

        void scheduleFlush() {
            auto& st = state();
            if (st.doLaterSeq || !g_pEventLoopManager)
                return;

            st.doLaterSeq = g_pEventLoopManager->doLater([] { flushPending(); });
        }
    }

    void init(HANDLE handle) noexcept {
        try {
            shutdown();

            auto& st  = state();
            st.handle = handle;

            const auto path = resolveErrorFile();
            if (path.empty())
                return;

            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) {
                Log::logger->log(Log::ERR, "[hyprtail] can't create {}: {}", path.parent_path().string(), ec.message());
                return;
            }

            // Keep the previous session's file as errors.log.1 instead of
            // truncating it: after a crash or hang, the next load (hyprpm at
            // login) would otherwise wipe the one file that matters. Replaces
            // an older errors.log.1. A duplicate instance is refused before
            // this runs (PLUGIN_INIT), so it can't rotate a live session's file.
            if (std::filesystem::exists(path, ec)) {
                auto rotated = path;
                rotated += ".1";
                std::filesystem::rename(path, rotated, ec);
                if (ec)
                    Log::logger->log(Log::ERR, "[hyprtail] can't rotate {} to {}: {}", path.string(), rotated.string(), ec.message());
                else
                    syncDir(path.parent_path());
            }

            const char* sig    = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
            const auto  header = std::format("hyprtail {} error log, session started {} (instance {})\n", HYPRTAIL_REV, utcNow(), sig ? sig : "unknown");
            if (!writeDurable(path.string(), header, true)) {
                Log::logger->log(Log::ERR, "[hyprtail] can't write error file {}", path.string());
                return;
            }

            st.filePath  = path.string();
            st.fileBytes = header.size();
        } catch (...) {
            // Reporting stays usable without the file.
        }
    }

    void shutdown() noexcept {
        try {
            auto& st = state();
            if (st.doLaterSeq && g_pEventLoopManager)
                g_pEventLoopManager->removeDoLater(*st.doLaterSeq);

            st.doLaterSeq.reset();
            st.pending.clear();
            st.seen.clear();
            st.handle = nullptr;
        } catch (...) {}
    }

    void report(eSeverity severity, std::string_view key, std::string_view message) noexcept {
        try {
            auto& st = state();

            Log::logger->log(severity == eSeverity::ERR ? Log::ERR : Log::WARN, "[hyprtail] {} [{}]: {}", severityName(severity), key, message);

            if (!st.seen.emplace(key).second)
                return;

            appendToFile(std::format("{} {} [{}]\n{}\n\n", utcNow(), severityName(severity), key, message));

            std::string text = "hyprtail: " + truncateForNotification(message);
            if (!st.filePath.empty())
                text += "\nFull message: " + st.filePath;

            st.pending.push_back({severity, std::move(text)});
            scheduleFlush();
        } catch (...) {}
    }

    void resetKey(std::string_view key) noexcept {
        try {
            state().seen.erase(std::string{key});
        } catch (...) {}
    }

    std::string errorFilePath() noexcept {
        try {
            return state().filePath;
        } catch (...) { return {}; }
    }

    void reportException(std::string_view where, std::exception_ptr ex) noexcept {
        try {
            std::string what = "unknown exception";
            try {
                if (ex)
                    std::rethrow_exception(ex);
            } catch (const std::exception& e) { what = e.what(); } catch (...) {
            }

            report(eSeverity::ERR, std::format("callback:{}", where), std::format("unexpected exception in {}: {}", where, what));
        } catch (...) {}
    }
}
