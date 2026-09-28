#pragma once

// Crash-loop guard (SPEC §7/§10, NOTES "Duplicate instance detection"'s
// cousin): a marker left at init, whose pid is no longer alive by the next
// load of the same build against the same running Hyprland, means the
// previous load died before reaching teardown() -- refuse to load rather
// than crash-loop silently. Hyprland-free (no Hyprland headers): parse/
// format/key-match/stale-pid logic is unit tested directly (make test-unit).
// main.cpp supplies the Hyprland-specific inputs (build revision, the
// running compositor's ABI hash, HYPRLAND_INSTANCE_SIGNATURE, pid) and wires
// the event-loop timer.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace hyprtail::crashguard {

    // Identifies "this build against this running Hyprland". Two markers
    // with equal fields describe the same pairing.
    struct SKey {
        std::string revision;     // HYPRTAIL_REV
        std::string hyprlandHash; // __hyprland_api_get_hash() of the running compositor
    };

    struct SMarker {
        std::string revision;
        std::string hyprlandHash;
        std::string instanceSignature; // HYPRLAND_INSTANCE_SIGNATURE, for the refusal notification only
        long long   pid = 0;
    };

    // $XDG_STATE_HOME/hyprtail/crash-guard.marker (or the ~/.local/state
    // fallback, hyprtail::stateDir()). Empty if neither env var resolves.
    std::filesystem::path markerPath();

    // Parses marker file text ("key=value\n" lines: rev, hyprland, instance,
    // pid). nullopt if any of the four fields is missing, or pid doesn't
    // parse as a positive integer.
    std::optional<SMarker> parse(std::string_view text);

    // Marker text for `key`, `instanceSignature` and `pid`. parse(format(k,
    // s, p)) round-trips to {k.revision, k.hyprlandHash, s, p}.
    std::string format(const SKey& key, std::string_view instanceSignature, long long pid);

    // True when `pid` no longer names a running process. Only a definite
    // "no such process" counts as dead; a live pid, or any other reason we
    // can't tell (e.g. no permission to signal it), is treated as alive --
    // a live-or-unknown pid must never be mistaken for a crash.
    bool pidIsDead(long long pid);

    // True when `marker` proves the run it names died before reaching
    // teardown(): same revision and Hyprland hash as `key`, and its pid is
    // dead. A live pid means some other instance (e.g. a nested one sharing
    // the same state directory) is still in its own run, not a crash.
    bool indicatesEarlyDeath(const SMarker& marker, const SKey& key);

    // Real filesystem glue around markerPath(). No-ops (nullopt/false) if
    // the state directory can't be resolved; readMarker() also returns
    // nullopt if the file doesn't exist or doesn't parse.
    std::optional<SMarker> readMarker();
    bool                   writeMarker(const SKey& key, std::string_view instanceSignature, long long pid);
    void                   removeMarker();
}
