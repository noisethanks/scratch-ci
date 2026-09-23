#include <bit>
#include <chrono>
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

#include "TrailPassElement.hpp"

static HANDLE             s_handle = nullptr;
static CFunctionHook*     g_hook   = nullptr;

typedef void (*origRenderSoftwareCursorsFor)(
    void*, PHLMONITOR, const Time::steady_tp&, CRegion&,
    std::optional<Vector2D>, bool, bool);

// Placeholder, SPEC §3 buffer size is still TBD.
static constexpr size_t TRAIL_CAPACITY = 64;

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

static uint64_t                s_hookFires = 0;
static Time::steady_tp         s_epoch;
static UP<STrailInstance>      s_motionTrail;

// Damage prev and current trail box on this monitor when the trail changed
// since this monitor last looked (SPEC §6 union).
static void damageTrail(STrailInstance& inst, const PHLMONITOR& pMonitor, CRegion& damage) {
    auto& ms = inst.monState[pMonitor.get()];
    if (ms.seenGeneration == inst.ring.generation())
        return;

    const CBox cur = inst.ring.empty() ? CBox{} : trailBoxLocal(inst, pMonitor->m_position);

    for (const auto& box : {ms.prevBoxLocal, cur}) {
        if (box.empty())
            continue;

        const CBox px = outwardPixelBox(box, pMonitor->m_scale);

        // Current frame: beginRender already captured the damage ring, so
        // this frame only sees damage added to the render region directly.
        damage.add(px);

        // Damage ring: lands in m_current, captured by the next frame's
        // transaction and rotated into history, so older swapchain buffers
        // (age > 1) also repaint this box. Also schedules that frame.
        pMonitor->addDamage(px);
    }

    ms.prevBoxLocal   = cur;
    ms.seenGeneration = inst.ring.generation();
}

void hkRenderSoftwareCursorsFor(void* thisptr, PHLMONITOR pMonitor,
                                 const Time::steady_tp& now, CRegion& damage,
                                 std::optional<Vector2D> overridePos,
                                 bool screencopy, bool forceRender) {
    (*(origRenderSoftwareCursorsFor)g_hook->m_original)(
        thisptr, pMonitor, now, damage, overridePos, screencopy, forceRender);

    // Screenshare calls this with fake damage (ScreenshareFrame.cpp:316, :359).
    // Whether the trail shows up in recordings is undecided, stay out for now.
    if (!g_pHyprRenderer || !s_motionTrail || screencopy)
        return;

    ++s_hookFires;
    if (s_hookFires <= 3 || s_hookFires % 60 == 0)
        LOG(Log::INFO, "[hyprtail-s3] hook fire #{} monitor={}", s_hookFires, pMonitor->m_name);

    auto& inst = *s_motionTrail;

    // Sample once per frame, insert only on real movement. With several
    // monitors the first hook of the frame inserts, the rest see no change.
    const Vector2D cursorPos = Pointer::mgr()->position();
    const SVec2f   pos{sc<float>(cursorPos.x), sc<float>(cursorPos.y)};
    if (inst.ring.empty() || inst.ring.newest().posPx != pos) {
        const float nowMs = std::chrono::duration<float, std::milli>(now - s_epoch).count();
        inst.ring.insert(pos, nowMs);
    }

    damageTrail(inst, pMonitor, damage);

    // Still added every frame the trail is on this monitor (known issue,
    // deferred to stage 4); simplify() drops it when outside the damage.
    if (inst.ring.empty())
        return;

    const CBox boxLocal = trailBoxLocal(inst, pMonitor->m_position);
    if (!boxLocal.overlaps(CBox{{}, pMonitor->m_size}))
        return;

    g_pHyprRenderer->m_renderPass.add(makeUnique<CTrailPassElement>(&inst, boxLocal));
}

// Extract function address from a non-virtual member function pointer.
// Uses the Itanium C++ ABI layout: {ptr, adj}; virtual bit is the LSB of ptr.
template <typename T>
static void* pmf_address(T pmf) {
    struct PMF { uintptr_t ptr; ptrdiff_t adj; };
    static_assert(sizeof(T) == sizeof(PMF), "unexpected PMF size");
    auto rep = std::bit_cast<PMF>(pmf);
    if (rep.ptr & 0x01)
        throw std::runtime_error("[hyprtail-s3] unexpected virtual function pointer");
    return reinterpret_cast<void*>(rep.ptr);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    s_handle = handle;

    // ABI version check — must match the compositor we were compiled against.
    const std::string compositorHash = __hyprland_api_get_hash();
    const std::string clientHash     = __hyprland_api_get_client_hash();
    if (compositorHash != clientHash) {
        HyprlandAPI::addNotification(handle,
            "[hyprtail-s3] version mismatch — recompile against running Hyprland",
            CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 8000);
        throw std::runtime_error(
            std::format("[hyprtail-s3] version mismatch: built={} running={}",
                        clientHash, compositorHash));
    }

    void* target = pmf_address(&Pointer::CPointerManager::renderSoftwareCursorsFor);
    g_hook = HyprlandAPI::createFunctionHook(handle, target,
                                              reinterpret_cast<void*>(&hkRenderSoftwareCursorsFor));
    if (!g_hook || !g_hook->hook()) {
        HyprlandAPI::addNotification(handle,
            "[hyprtail-s3] hook failed",
            CHyprColor{1.0f, 0.2f, 0.2f, 1.0f}, 5000);
        throw std::runtime_error("[hyprtail-s3] could not hook renderSoftwareCursorsFor");
    }

    s_epoch       = Time::steadyNow();
    s_motionTrail = makeUnique<STrailInstance>(TRAIL_CAPACITY);

    LOG(Log::INFO, "[hyprtail-s3] loaded — stage 3 row of dots");
    HyprlandAPI::addNotification(handle,
        "[hyprtail-s3] loaded — row of dots should follow cursor",
        CHyprColor{0.2f, 1.0f, 0.2f, 1.0f}, 5000);

    // Damage the primary monitor to schedule an immediate first frame.
    if (g_pHyprRenderer) {
        if (auto m = g_pHyprRenderer->m_mostHzMonitor.lock())
            g_pHyprRenderer->damageMonitor(m);
    }

    return {"hyprtail-stage3", "Stage 3: row of dots from the point buffer as instanced vertex data", "dev", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    LOG(Log::INFO, "[hyprtail-s3] unloading after {} hook fires", s_hookFires);
    s_hookFires = 0;

    if (g_hook) {
        g_hook->unhook();
        g_hook = nullptr;
    }

    // Remove any queued elements before GPU resources go away so draw()
    // can't run against a deleted VAO/program.
    if (g_pHyprRenderer)
        g_pHyprRenderer->m_renderPass.removeAllOfType("CTrailPassElement");

    if (s_motionTrail) {
        trailInstanceCleanup(*s_motionTrail);
        s_motionTrail.reset();
    }

    LOG(Log::INFO, "[hyprtail-s3] unloaded");
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}
