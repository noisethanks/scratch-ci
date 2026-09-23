#include "FileWatch.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <sys/inotify.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <Compositor.hpp>

#include "Diagnostics.hpp"

using hyprtail::diag::eSeverity;

namespace hyprtail {
    namespace {
        // Close-after-write covers in-place saves, moved-to and create cover
        // write-then-rename saves.
        constexpr uint32_t WATCH_MASK = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;
    }

    CFileWatch::~CFileWatch() {
        shutdown();
    }

    bool CFileWatch::init(std::function<void()> onChange) {
        shutdown();
        m_onChange = std::move(onChange);

        if (!g_pCompositor || !g_pCompositor->m_wlEventLoop) {
            diag::report(eSeverity::WARN, "filewatch", "no Wayland event loop; shader files won't hot-reload on save (a Hyprland config reload still reloads them)");
            return false;
        }

        m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_fd < 0) {
            diag::report(eSeverity::WARN, "filewatch",
                         std::format("inotify_init1 failed: {}; shader files won't hot-reload on save (a Hyprland config reload still reloads them)", std::strerror(errno)));
            return false;
        }

        m_source = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, m_fd, WL_EVENT_READABLE, &CFileWatch::onReadable, this);
        if (!m_source) {
            diag::report(eSeverity::WARN, "filewatch", "could not add the inotify fd to the event loop; shader files won't hot-reload on save");
            close(m_fd);
            m_fd = -1;
            return false;
        }

        return true;
    }

    void CFileWatch::setFiles(const std::vector<std::filesystem::path>& files) {
        if (m_fd < 0)
            return;

        std::map<std::string, std::set<std::string>> wanted;
        for (const auto& f : files) {
            if (f.has_parent_path() && f.has_filename())
                wanted[f.parent_path().string()].insert(f.filename().string());
        }

        if (wanted == m_names)
            return;

        // Drop directories no longer needed.
        for (auto it = m_wdToDir.begin(); it != m_wdToDir.end();) {
            if (!wanted.contains(it->second)) {
                inotify_rm_watch(m_fd, it->first);
                it = m_wdToDir.erase(it);
            } else
                ++it;
        }

        // Add new ones (inotify_add_watch returns the existing wd for a
        // directory already watched).
        for (const auto& [dir, names] : wanted) {
            const int wd = inotify_add_watch(m_fd, dir.c_str(), WATCH_MASK);
            if (wd < 0) {
                diag::report(eSeverity::WARN, std::format("filewatch:{}", dir), std::format("can't watch {}: {}; edits there won't hot-reload", dir, std::strerror(errno)));
                continue;
            }
            m_wdToDir[wd] = dir;
        }

        m_names = std::move(wanted);
    }

    void CFileWatch::shutdown() {
        if (m_source) {
            wl_event_source_remove(m_source);
            m_source = nullptr;
        }
        if (m_fd >= 0) {
            close(m_fd); // also drops all watches
            m_fd = -1;
        }
        m_wdToDir.clear();
        m_names.clear();
    }

    int CFileWatch::onReadable(int /*fd*/, uint32_t /*mask*/, void* data) {
        // Runs on the event loop: nothing may escape.
        diag::guard("filewatch", [data] { static_cast<CFileWatch*>(data)->drain(); });
        return 0;
    }

    void CFileWatch::drain() {
        alignas(inotify_event) std::array<char, 4096> buf;
        bool                                          relevant = false;

        while (true) {
            const ssize_t n = read(m_fd, buf.data(), buf.size());
            if (n <= 0)
                break; // EAGAIN: drained

            for (ssize_t off = 0; off < n;) {
                const auto* ev = reinterpret_cast<const inotify_event*>(buf.data() + off);
                off += sizeof(inotify_event) + ev->len;

                if (ev->len == 0)
                    continue;
                const auto dir = m_wdToDir.find(ev->wd);
                if (dir == m_wdToDir.end())
                    continue;
                const auto names = m_names.find(dir->second);
                if (names != m_names.end() && names->second.contains(ev->name))
                    relevant = true;
            }
        }

        // One callback per batch, however many events an editor's save made.
        if (relevant && m_onChange)
            m_onChange();
    }
}
