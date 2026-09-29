# CI

`.github/workflows/ci.yml` builds hyprtail against three moving targets —
the latest stable Hyprland release, `main`, and (when it carries unreleased
backports) the current release branch head — the same way `hyprpm` builds
it: default pkg-config mode, never `make DEV=1`. It never starts a Hyprland
instance, nested or otherwise.

## Required check names

Configure these as required status checks on `master` (Settings → Rules →
Rulesets, or the legacy branch protection UI):

- `hyprland-stable`
- `hyprland-main`
- `flake-check`

Do **not** require `hyprland-release-branch`: it only exists in the matrix
(and therefore only reports a check) on runs where the release branch is
actually ahead of the latest tag. A required check that sometimes never
runs blocks every merge — see "how to read a red row" below for what
"required but skipped" looks like and why the workflow has no `paths:`
filter for exactly this reason.

`hyprland-smoke-*` runs only while the repository variable
`SMOKE_ENABLED` is `'true'` (see "Smoke job" below); don't require it until
it has run cleanly a few times.

## Ruleset settings for `master`

- Require a pull request before merging.
- Require status checks to pass: the three above.
- Require branches to be up to date before merging. Trade-off: every PR
  re-runs CI after a rebase onto a moved `master`, which is what you want
  for a required check, but means a stack of PRs re-verifies at each merge.
- Block force pushes.

## How to read a red row

| Failure | Usual cause |
|---|---|
| `nix build ...#hyprland-with-tests` fails | Upstream Hyprland/its deps don't build at that ref right now — not a hyprtail problem, but worth a comment on the PR either way. |
| `make all` fails (compile error) | hyprtail's own source references something the pinned headers changed. Update `SPEC.md` §2's pin, fix the break, re-run. |
| `nm import check` fails | The build succeeded but a symbol hyprtail calls into (or hooks by address) is no longer exported the same way by the built Hyprland binary — a silent ABI break that would only otherwise show up when a user runs `hyprpm update` and loads the plugin. The failure output lists the missing (demangled) symbol names. |
| `make test-unit` fails | A real regression in the Hyprland-free logic (params, shader preprocessing, the node ring) — same as a local `make test-unit` failure. |
| `flake-check` fails | `flake.nix` itself, independent of the matrix — check it still matches `hyprlandPlugins.mkHyprlandPlugin`'s current shape in nixpkgs. |

The job summary on every run (pass or fail) lists files changed in
`src/render/`, `src/pointer/`, `Monitor.cpp`, or anything matching `damage`
between that row's last recorded green SHA and the current one — a quick
"what actually moved upstream" view before diving into a failure.

## Smoke job

`hyprland-smoke-<row>` runs hyprtail's lifecycle smoke test
(`tests/hyprtester/hyprtail_smoke.cpp`: load, duplicate refusal, hotplug,
unload/reload, crash-loop guard, with `errors.log` checked after each step)
in a NixOS VM, one per matrix row, the way Hyprland runs its own hyprtester
suite (`nix/tests/default.nix` and `.github/workflows/nix-test.yml`
upstream). It never runs a Hyprland on the runner itself, only inside the VM.

- **Enable:** set the repository variable `SMOKE_ENABLED` to `true`
  (Settings -> Secrets and variables -> Actions -> Variables). Unset or
  anything else and the job is skipped.
- **What runs:** `nix build .#legacyPackages.x86_64-linux.smoke` with the
  `hyprland` flake input overridden to the row's ref. `nix/smoke.nix` builds
  hyprtester with the smoke test compiled in (only the `hyprtester` target,
  not Hyprland), builds hyprtail against the same Hyprland (`make all`, the
  pkg-config path, so `src/compat.hpp` is exercised per row), and runs
  `hyprtester ... hyprtailLifecycle` in the VM. The output is deliberately not
  under `checks`/`packages`, so `flake-check` doesn't build a VM.
- **KVM:** the first step makes `/dev/kvm` usable (udev rule) and fails the
  job if it's missing. Without KVM the VM would still boot, in software
  emulation, and just be very slow.
- **Reading a red row:** the `Check exit status` step is hyprtester's own
  status (test failure: read `smoke-logs-<row>/testerlog`, `hyprlog` and
  `errors.log`); a failure in the `nix build` step is the VM or the
  derivation (no `result`, read the `-L` output; a Hyprland row whose
  hyprtester no longer builds with the test looks like this). Timeouts are
  60 minutes.
- **Promoting it to a required check:** let it run several times on the
  scratch repo first, across pushes and at least one scheduled run, and
  watch for flakiness (the test uses fixed sleeps), the `stable` row (the
  test has only been compiled against the pin and main) and run time. Then
  add `hyprland-smoke-stable` and `hyprland-smoke-main` to the required
  checks. `hyprland-smoke-release-branch` exists only when that row does, so
  it can't be required (same reason as `hyprland-release-branch`).
- **Not covered yet:** the test does not check that anything was drawn; a
  plugin that loads and draws nothing passes.

## Manual ground-truth check: the LTO gap

`scripts/ci/check-imports.sh` accepts an optional second argument
(`check-imports.sh out/hyprtail.so [hyprland-binary]`, default
`/usr/bin/Hyprland`). CI always passes both explicitly, pointed at the
row's own Nix-built `result/bin/Hyprland` (`nix/default.nix`:
`gcc16Stdenv`, no LTO — `BUILT_WITH_NIX = true` build type
`RelWithDebInfo`, not the host's build). The host's installed Hyprland
(CachyOS package) is built with LTO (SPEC §2 pin note).

**Run it manually against `/usr/bin/Hyprland` after every host Hyprland
package upgrade:**

```
make all   # or make DEV=1, whichever headers you're validating against
scripts/ci/check-imports.sh out/hyprtail.so
```

Why CI passing isn't enough on its own: LTO can inline or dedupe a symbol
across translation units in ways a non-LTO build never does, which can
turn a previously-exported dynamic symbol into one that no longer appears
in the LTO binary's dynamic symbol table — while the exact same source, in
CI's non-LTO Nix build, still exports it fine. A green CI run proves the
*source* still matches Hyprland's headers and that a non-LTO build of that
Hyprland commit still exports what hyprtail needs; it does not prove the
*specific LTO'd binary a user actually runs* does. Only running the script
against `/usr/bin/Hyprland` proves that.

## Re-pinning: `hyprpm.toml` `commit_pins`

`hyprpm.toml`'s `commit_pins` field (see the comment already in that file,
and SPEC §2) lets hyprpm build an older hyprtail commit when the *running*
Hyprland is exactly some older pinned commit, instead of always building
`HEAD`. Add an entry as `[hyprland_commit, hyprtail_commit]` when a hyprtail
change requires a Hyprland version newer than what some users are still
running and you want `hyprpm update` to keep working for them instead of
just breaking. Not needed while `main` supports the currently pinned
Hyprland (SPEC §2) — CI's `hyprland-main` row is exactly what tells you
when that stops being true.

## Release procedure

1. Confirm `hyprland-stable` and `hyprland-main` are both green on `master`.
2. Advance the pin in `SPEC.md` §2 and the Makefile's `HYPRLAND_PIN` if the
   host's installed Hyprland version has moved (deliberate re-pin, not
   automatic — SPEC §2's own reasoning).
3. Tag.

(Nothing about tagging or publishing is automated by this workflow — it's a
test gate, not a release pipeline.)

## Scheduled-run limitations

- **GitHub disables scheduled workflows after 60 days of no repository
  activity** (no pushes at all, not related to whether the workflow itself
  has been running). Not worked around here: this repo has regular commit
  activity, so the window is unlikely to be hit in practice. If a schedule
  silently stops firing after a long quiet period, check
  Settings → Actions and re-enable it there.
- **Fork PRs never see the `alert` job**: it's gated on
  `github.event_name == 'schedule'`, which fork pull requests never are.
  Nothing in the `test`/`flake-check` jobs needs a secret either, so fork
  PRs run identically to same-repo PRs otherwise.
- **The default `GITHUB_TOKEN` can't write repository variables**, which is
  why the last-known-good Hyprland SHA per row is stored in a GitHub Actions
  cache (`last-good-hyprland-<row>`) instead. **Cache keys are
  content-addressed and immutable**: `actions/cache` can never overwrite an
  existing key, only create a new one or restore an existing one. The
  workflow therefore *saves* under a key that includes `github.run_id`
  (guaranteed unique, so the save always succeeds as a fresh key) and
  *restores* by the shared prefix `last-good-hyprland-<row>-` via
  `restore-keys` (GitHub resolves a `restore-keys` prefix to the most
  recently created matching key) — never the same literal key for both
  operations. Entries can still be evicted (GitHub's LRU, currently ~7
  days/10 GB repo-wide); on a miss, the job summary and the tracking-issue
  compare link both just say "no last-good SHA on record" rather than
  failing. If you want that history to survive evictions, the
  straightforward upgrade is a committed `last-good.json` on a dedicated
  orphan branch (e.g. `ci-state`), written by the workflow with
  `contents: write` — not built by default so this workflow doesn't push
  to the repo unasked.
- **The job-summary diff is a blobless `git clone` + `git diff --name-only`,
  not the GitHub compare API.** The compare API silently truncates its
  `files` list past 300 changed files, which a stale/evicted last-good SHA
  (previous bullet) makes easy to hit across a large upstream gap; a real
  `git diff --name-only` between two commits has no such limit.
  `--filter=blob:none --no-checkout` keeps this cheap: no blob content is
  ever fetched, since a name-only, no-rename diff only walks tree objects.
  Repeated per matrix row, every run — a deliberate correctness-over-speed
  trade-off; if clone time becomes a real cost, caching the clone across
  runs (another `actions/cache` entry, keyed by row) is the next step, not
  built here.

## Alerting

Only on `schedule` runs, only on failure: opens or updates a single issue
labeled `ci-hyprland-regression` (created automatically, idempotent — the
`alert` job runs `gh label create ... || true` before filing/commenting, so
a missing label never fails the job) instead of a new issue per failed
schedule. The issue body links the run and, for every row that has both a
current SHA and a recorded last-good SHA, a `hyprwm/Hyprland` compare link
between them.

## Permissions (least privilege)

The workflow sets `permissions: contents: read` once, at the top level.
`resolve`, `test`, `flake-check` and `smoke` all inherit that and need
nothing more (checkout, plus read-only GitHub API calls against
`hyprwm/Hyprland`, a *different* repo — reading it needs a valid token, not
any scope granted on *this* repo). `actions/cache` and
`actions/upload-artifact`/`download-artifact` use their own dedicated,
separately-scoped tokens internally and aren't affected by this
`permissions:` block either way.

`alert` is the only job that declares its own `permissions:` block
(`issues: write`, `actions: read` — the latter to list this run's jobs and
find which ones failed). A job-level `permissions:` block *replaces* the
workflow-level one for that job rather than adding to it, so `alert`
implicitly has `contents: none`: it doesn't check out the repo and doesn't
need to, which is more restrictive than simply inheriting `contents: read`
would have been.

## `flake.nix`'s compiler

`hyprlandPlugins.mkHyprlandPlugin`'s derivation constructor is
`hyprland.stdenv.mkDerivation` (nixpkgs:
`pkgs/applications/window-managers/hyprwm/hyprland-plugins/default.nix`) —
whatever `stdenv` the `hyprland` package in scope at build time was itself
built with. `flake.nix`'s overlay list is
`[ self.overlays.default hyprland.overlays.hyprland-packages ]`: the second
overlay is Hyprland's own (`nix/overlays.nix`'s `hyprland-no-deps`, which
calls `final.callPackage ./default.nix { stdenv = final.gcc16Stdenv; ...
}`), applied into the *same* `pkgsFor.${system}` fixed point our own overlay
also patches — Nix overlays are fixed-point patches over one shared
attribute set, so `final.hyprland` (and therefore the `hyprland` that
`mkHyprlandPlugin` captures) resolves to that GCC 16 build regardless of
which of the two overlays is listed first. GCC 15+ is required for
`#embed` (Makefile comment, `src/ShaderSource.cpp`); GCC 16 satisfies it.
Not evaluated with an actual `nix` binary in this session — this is read
from the source of both files, not executed.
