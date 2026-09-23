#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

struct wl_event_source;

namespace hyprtail {
    // Watches a set of files for changes and calls back on Hyprland's event
    // loop (main thread). Watches the files' parent directories, not the
    // files: editors commonly save by writing a new file and renaming it over
    // the old one, which a watch on the old inode would miss.
    class CFileWatch {
      public:
        ~CFileWatch();

        // Registers with g_pCompositor->m_wlEventLoop. False (with a report)
        // if inotify or the event loop isn't available; the plugin then just
        // doesn't hot-reload on file edits.
        bool init(std::function<void()> onChange);

        // Replace the watched set. Cheap when unchanged.
        void setFiles(const std::vector<std::filesystem::path>& files);

        // Remove the event source and close the fd. Must run before the
        // plugin is unloaded.
        void shutdown();

      private:
        static int                               onReadable(int fd, uint32_t mask, void* data);
        void                                     drain();

        int                                      m_fd     = -1;
        wl_event_source*                         m_source = nullptr;
        std::function<void()>                    m_onChange;
        std::map<int, std::string>               m_wdToDir;    // watch descriptor -> directory
        std::map<std::string, std::set<std::string>> m_names;  // directory -> watched file names
    };
}
