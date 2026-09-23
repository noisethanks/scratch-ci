#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <plugins/PluginAPI.hpp>
#include <plugins/HookSystem.hpp>
#include <render/Renderer.hpp>
#include <pointer/PointerManager.hpp>
#include <helpers/memory/Memory.hpp>
#include <helpers/time/Time.hpp>
#include <helpers/Color.hpp>
#include <debug/log/Logger.hpp>

#include "DotPassElement.hpp"

static HANDLE             s_handle = nullptr;
static CFunctionHook*     g_hook   = nullptr;

typedef void (*origRenderSoftwareCursorsFor)(
    void*, PHLMONITOR, const Time::steady_tp&, CRegion&,
    std::optional<Vector2D>, bool, bool);

struct SMonitorDotState {
    Vector2D lastCursorPos{-1e9, -1e9};
    CBox     lastDotBox{};
};

// Logical box -> pixel damage box covering every pixel the draw can touch,
// including partially covered edge pixels: scale, then floor left/top and
// ceil right/bottom. CBox::round() rounds x/y to nearest and derives w/h from
// the rounded origin, so it can drop a partially covered edge column/row.
static CBox outwardPixelBox(const CBox& logical, double scale) {
    const double x1 = std::floor(logical.x * scale);
    const double y1 = std::floor(logical.y * scale);
    const double x2 = std::ceil((logical.x + logical.w) * scale);
    const double y2 = std::ceil((logical.y + logical.h) * scale);
    return CBox{x1, y1, x2 - x1, y2 - y1};
}

static uint64_t                                   s_hookFires = 0;
static std::unordered_map<Monitor::CMonitor*, SMonitorDotState> s_monState;

void hkRenderSoftwareCursorsFor(void* thisptr, PHLMONITOR pMonitor,
                                 const Time::steady_tp& now, CRegion& damage,
                                 std::optional<Vector2D> overridePos,
                                 bool screencopy, bool forceRender) {
    (*(origRenderSoftwareCursorsFor)g_hook->m_original)(
        thisptr, pMonitor, now, damage, overridePos, screencopy, forceRender);

    if (!g_pHyprRenderer)
        return;

    ++s_hookFires;
    if (s_hookFires <= 3 || s_hookFires % 60 == 0)
        LOG(Log::INFO, "[hyprtail-s2] hook fire #{} monitor={}", s_hookFires, pMonitor->m_name);

    auto&          state     = s_monState[pMonitor.get()];
    const Vector2D cursorPos = Pointer::mgr()->position();
    if (cursorPos != state.lastCursorPos) {
        const Vector2D localPos = cursorPos - pMonitor->m_position;
        const CBox     newBox{localPos.x - 10.0, localPos.y - 10.0, 20.0, 20.0};

        const CBox prevScaled = outwardPixelBox(state.lastDotBox, pMonitor->m_scale);
        const CBox newScaled  = outwardPixelBox(newBox, pMonitor->m_scale);

        // Current frame: beginRender already captured the damage ring, so
        // this frame only sees damage added to the render region directly.
        damage.add(prevScaled);
        damage.add(newScaled);

        // Damage ring: lands in m_current, captured by the next frame's
        // transaction and rotated into history, so older swapchain buffers
        // (age > 1) also repaint these boxes. Also schedules that frame.
        pMonitor->addDamage(prevScaled);
        pMonitor->addDamage(newScaled);

        state.lastCursorPos = cursorPos;
        state.lastDotBox    = newBox;
    }

    g_pHyprRenderer->m_renderPass.add(makeUnique<CDotPassElement>());
}

// Extract function address from a non-virtual member function pointer.
// Uses the Itanium C++ ABI layout: {ptr, adj}; virtual bit is the LSB of ptr.
template <typename T>
static void* pmf_address(T pmf) {
    struct PMF { uintptr_t ptr; ptrdiff_t adj; };
    static_assert(sizeof(T) == sizeof(PMF), "unexpected PMF size");
    auto rep = std::bit_cast<PMF>(pmf);
    if (rep.ptr & 0x01)
        throw std::runtime_error("[hyprtail-s2] unexpected virtual function pointer");
    return reinterpret_cast<void*>(rep.ptr);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    s_handle = handle;

    // ABI version check — must match the compositor we were compiled against.
    const std::string compositorHash = __hyprland_api_get_hash();
    const std::string clientHash     = __hyprland_api_get_client_hash();
    if (compositorHash != clientHash) {
        HyprlandAPI::addNotification(handle,
            "[hyprtail-s2] version mismatch — recompile against running Hyprland",
            CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 8000);
        throw std::runtime_error(
            std::format("[hyprtail-s2] version mismatch: built={} running={}",
                        clientHash, compositorHash));
    }

    void* target = pmf_address(&Pointer::CPointerManager::renderSoftwareCursorsFor);
    g_hook = HyprlandAPI::createFunctionHook(handle, target,
                                              reinterpret_cast<void*>(&hkRenderSoftwareCursorsFor));
    if (!g_hook || !g_hook->hook()) {
        HyprlandAPI::addNotification(handle,
            "[hyprtail-s2] hook failed",
            CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 5000);
        throw std::runtime_error("[hyprtail-s2] could not hook renderSoftwareCursorsFor");
    }

    LOG(Log::INFO, "[hyprtail-s2] loaded — stage 1 dot render");
    HyprlandAPI::addNotification(handle,
        "[hyprtail-s2] loaded — red dot should follow cursor",
        CHyprColor{0.2f, 1.0f, 0.2f, 1.0f}, 5000);

    // Damage the primary monitor to schedule an immediate first frame.
    if (g_pHyprRenderer) {
        if (auto m = g_pHyprRenderer->m_mostHzMonitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    return {"hyprtail-stage2", "Stage 2: cursor-tracked dot — Pointer::mgr() uniform plumbing proof", "dev", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    LOG(Log::INFO, "[hyprtail-s2] unloading after {} hook fires", s_hookFires);
    s_hookFires = 0;
    s_monState.clear();

    if (g_hook) {
        g_hook->unhook();
        g_hook = nullptr;
    }

    // Remove any queued elements before the shader is destroyed so draw()
    // can't be called with a dangling shader pointer.
    if (g_pHyprRenderer)
        g_pHyprRenderer->m_renderPass.removeAllOfType("CDotPassElement");

    dotPassCleanup();

    LOG(Log::INFO, "[hyprtail-s2] unloaded");
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}
