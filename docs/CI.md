# CI

Three workflows, one question each. None of them runs Hyprland on the runner
(the smoke test runs it only inside a VM).

| Workflow | Question | Triggers | Required |
|---|---|---|---|
| `ci.yml` | Did this change break hyprtail? | `pull_request`, manual | `ci-gate` |
| `upstream.yml` | Did upstream Hyprland break hyprtail? | daily, manual | never |
| `smoke.yml` | Does the plugin survive load, hotplug, unload and reload? | weekly, manual | never |

`ci.yml` tests against the pinned Hyprland only (SPEC §2, the Makefile's
`HYPRLAND_PIN`), so its answer never moves under a PR. Upstream drift is
`upstream.yml`'s job: a PR can't fix it, so it must not block one.

## `ci.yml`: the PR gate

### `arch-pin`

Runs in an `archlinux` container with pacman pointed at an Arch Linux Archive
snapshot (`ARCH_SNAPSHOT`) whose repos carry the pinned Hyprland package. The
snapshot fixes every package, so the job is reproducible: Hyprland, GCC,
hyprutils, glslang and clang-format all stay where they are until a re-pin.

Steps:

1. `make check-ascii` (see "ASCII rule" below).
2. `scripts/ci/arch.sh`:
   - `make all` in the default pkg-config mode, exactly what hyprpm runs.
   - Checks that `hyprpm.toml`'s `output` exists.
   - `make test-unit SANITIZE=1`: the Hyprland-free tests under ASan and
     UBSan, plus the GLSL checks through `glslangValidator`.
   - `make test-compat`.
   - `scripts/ci/check-imports.sh` against the package's `/usr/bin/Hyprland`.
3. `clang-format --dry-run -Werror` over `src/` and `tests/`. Runs even when
   step 2 failed, so one run reports both kinds of problem.

### `nix-pin`

The two ways a Nix user installs hyprtail, both at the pin:

- `nix flake check`: evaluates the flake and builds `checks`, which is
  `packages`, so `packages.x86_64-linux.hyprtail`. This is what a user gets
  from `hyprtail.packages.${system}.hyprtail`.
- `nix build ./nix/consumer`: a downstream flake that applies
  `overlays.default` to its own pkgs, wired the way the README recommends
  (see "Overlay consumer" below).

Both use `NIX_PIN`, a fixed set of `--override-input` flags: the flake's
`hyprland` input at the pinned tag and the nixpkgs the glaze workaround needs
("Nix: pinned nixpkgs"). `flake.nix` names the tag too, and `flake.lock`
locks it, so a plain `nix build` gets the pinned Hyprland. Only the nixpkgs
override is CI-only: a lock can't carry it, so a plain build of v0.56.2 fails
on glaze. Users who set `inputs.hyprland.follows` use their own Hyprland and
are unaffected by either.

### `ci-gate`

The one required check. GitHub counts a **skipped** required job as passing,
and a job is skipped when a job it `needs` failed or was cancelled. `ci-gate`
runs `always()` and fails unless `arch-pin` and `nix-pin` both report
`success`. It also gives the ruleset one stable name to require. Gate a new
job by adding it to `ci-gate`'s `needs` and its loop. Do not require it on
its own.

No `paths:` filter on `ci.yml`. A filtered-out run reports no check at all,
and a required check that never reports blocks the merge.

## Ruleset settings for `master` (public repo)

- Require a pull request before merging; block direct pushes and force pushes.
- Require status checks: `ci-gate` only.
- Require branches to be up to date before merging.

## Re-pinning

The pin lives in five places. Move them together, in one PR:

1. `SPEC.md` §2 and the Makefile's `HYPRLAND_PIN`.
2. `flake.nix` `hyprland.url`: the tag. Then regenerate `flake.lock` with
   `nix flake lock` (no Nix on the host:
   `docker run --rm --network host -v "$PWD":/src -w /src nixos/nix nix --extra-experimental-features 'nix-command flakes' flake lock`,
   run on a copy of `flake.nix`, then copy the lock back) and check
   `jq .nodes.hyprland.locked.rev flake.lock` is the pinned commit.
3. `ci.yml` `ARCH_SNAPSHOT`: the first Arch Linux Archive date whose `extra`
   repo has the new Hyprland package. Check with
   `curl -s https://archive.archlinux.org/repos/YYYY/MM/DD/extra/os/x86_64/extra.db | tar -tz | grep '^hyprland-'`.
   Also confirm the snapshot's `clang` matches the local `clang-format`
   major version, or the format step disagrees with `make format`.
4. `ci.yml` and `smoke.yml` `NIX_PIN`: the tag. Drop the `hyprland/nixpkgs`
   override if the new tag's own lock builds (see next section).
5. `upstream.yml` `pin_tag`/`pin_nixpkgs`, same rule.

## Nix: pinned nixpkgs

Hyprland v0.56.2's `CMakeLists.txt:133` requires `glaze 7...<8`, but its
`flake.lock` pins a nixpkgs where glaze is 8.0.0. Its Nix build then falls
back to `FetchContent` (a `git clone` of glaze), which the Nix sandbox blocks:
`error: could not find git for clone of glaze`. Main has no upper bound
(`CMakeLists.txt:131`).

So the pin overrides `hyprland/nixpkgs` with
`github:NixOS/nixpkgs/61b7c44c4073f0b827768aff0049561b5110ea5a`, the nixpkgs
v0.56.1's `flake.lock` uses (glaze 7.9.0). v0.56.1's `nix/default.nix` and
`nix/overlays.nix` are identical to v0.56.2's. Nothing upstream built that
combination, so there is no Cachix hit: a cold cache builds Hyprland and its
libraries from source (about 15 minutes for Hyprland alone).

Do not patch the glaze bound out instead. A build that succeeds against an
unsupported glaze is worse than an honest failure. Drop the override when a
pinned tag builds without it.

## Overlay consumer

`nix/consumer/flake.nix` is a downstream flake: `hyprland` as its own input,
`nixpkgs` and hyprtail's `hyprland` both following it, as the README's Nix
section recommends. It applies Hyprland's `overlays.hyprland-packages`, then
hyprtail's `overlays.default`, which adds only the plugin. CI points its
`hyprtail` input at the checkout (`git+file://$GITHUB_WORKSPACE`), which is
why `nix-pin` checks out with `fetch-depth: 0`: Nix refuses `revCount` on a
shallow clone.

- **What it proves:** the overlay builds the plugin against the consumer's
  `hyprland`, and that `hyprland` is the same one hyprtail's own `packages`
  use (same `version` as `legacyPackages.<system>.hyprland`).
- **What it does not prove:** that the overlay builds against nixpkgs' own
  `hyprland`, without Hyprland's overlay. That works only when nixpkgs'
  Hyprland is 0.55.0 or newer; older ones fail in `make check-headers`.

## `upstream.yml`: drift watch

Builds unchanged `master` against refs that move:

| Row | Source | Build |
|---|---|---|
| `upstream-arch` | Arch's live repos | `scripts/ci/arch.sh`, as in `arch-pin` |
| `upstream-stable` | latest Hyprland release tag | Nix |
| `upstream-release-branch` | `vX.Y.Z-b`, only when ahead of the tag | Nix |
| `upstream-main` | Hyprland `main` | Nix |

`upstream-arch` is the most useful row: it is the binary Arch users will
build against next, LTO included, and needs no Hyprland build. The Nix rows
catch a release before Arch packages it. Each Nix row builds Hyprland, then
hyprtail, then runs `make test-unit test-compat SANITIZE=1` in `devShells.ci`
and the nm check against that row's non-LTO binary.

`resolve-hyprland-refs` builds the Nix matrix through read-only GitHub API
calls. While the latest tag is v0.56.2 it adds the pinned nixpkgs to the
stable row. The other rows are never pinned: a failure there is information.

A red row means one of three things:

- **Re-pin:** the host moved to the new release (see "Re-pinning").
- **Add a `hyprpm.toml` `commit_pins` entry:** hyprtail has to change for
  the new Hyprland, and users still on the old Hyprland should keep building
  a commit that works there (see "commit_pins").
- **Port:** for `main`, see "Planned for 1.1".

### Alerting

`alert` runs on scheduled runs only. It keeps one open issue titled
"Upstream Hyprland broke hyprtail" (label `upstream-regression`). It
comments only when the **set** of failing rows changes, so a row that stays
red does not comment every day. The comparison uses an HTML comment marker
at the end of each body. The body lists the failing rows and every row's
resolved ref and commit (or Arch package version). Close the issue once the
rows are green again.

`upstream-main` never alerts: it is known red until the 1.1 port. Remove it
from the `grep -Ev` filter in `alert` after the port.

## `smoke.yml`

Runs `tests/hyprtester/hyprtail_smoke.cpp` (load, duplicate refusal, monitor
hotplug, unload/reload, crash-loop guard, `errors.log` checked after each
step) in a NixOS VM (`nix/smoke.nix`), against the pin. It is the same
approach as Hyprland's own `nix-test.yml`.

- **When:** by hand before a release, and weekly so it doesn't rot unnoticed.
  Never required.
- **Cost:** a cold cache builds Hyprland more than once (NOTES "Smoke job: the
  triple Hyprland build"), well past an hour. The job gets 150 minutes and the
  build step 140, so the `Save Nix store` step (`if: always()`) still runs.
  GitHub evicts caches unused for 7 days, so the weekly run may often start
  cold. Run it by hand a day or two before a release to warm it.
- **KVM:** the first step makes `/dev/kvm` usable and fails if it is missing.
  Without KVM the VM would boot under software emulation and time out with no
  error.
- **Reading a red run:** `Check exit status` is hyprtester's own status: read
  `smoke-logs/testerlog`, `hyprlog` and `errors.log`. A failure inside
  `nix build` is the VM or the derivation: read the `-L` output.
- **Not covered:** it doesn't check that anything was drawn.

## How to read a red check

| Failure | Usual cause |
|---|---|
| `arch-pin` / install step | The archive was unreachable or the snapshot date is wrong. Re-run once, then check `ARCH_SNAPSHOT`. |
| `arch-pin` / `make check-ascii` | Non-ASCII byte in `.github/`, `flake.nix`, `nix/` or the Makefile (lines listed). |
| `arch-pin` / `make all` | hyprtail's source doesn't compile against the pinned headers. |
| `arch-pin` / hyprpm output | `hyprpm.toml` `output` no longer matches the Makefile's `OUTPUT`. |
| `arch-pin` / `make test-unit` | A regression in the Hyprland-free logic, a sanitizer finding, or a GLSL compile or link error. Same as a local `make test-unit SANITIZE=1`. |
| `arch-pin` / `make test-compat` | An assumption `src/compat.hpp` asserts no longer holds. |
| `arch-pin` / nm import check | The plugin needs a symbol the LTO binary doesn't export. It would build and then fail at `hyprctl plugin load`. The output lists the demangled names. |
| `arch-pin` / clang-format | Run `make format`. |
| `nix-pin` / `nix flake check` | `flake.nix` itself, or the plugin under `mkHyprlandPlugin`. If `arch-pin` is green, the source is fine and the packaging broke. |
| `nix-pin` / overlay consumer | An evaluation error `overlays.default left hyprland at ...` means the overlay no longer carries Hyprland's. A compile error matches the flake check's. |
| `upstream-*` | See "upstream.yml". |

## Arch jobs and the LTO gap

Arch builds packages with LTO (`makepkg.conf` `OPTIONS` has `lto`, and the
`hyprland` PKGBUILD doesn't turn it off). LTO can inline or dedupe a function
that a non-LTO build still exports, and a plugin that calls or hooks that
symbol then fails at load. So the nm check in `arch-pin` runs against the
kind of binary users run, which the Nix builds (`gcc16Stdenv`, no LTO) can't
give.

The host is CachyOS, a different LTO build from Arch's. After a host Hyprland
upgrade, check the real binary by hand:

```
make all
scripts/ci/check-imports.sh out/hyprtail.so
```

## ASCII rule

Workflow, YAML and Nix files must be pure ASCII: a stray byte (an em dash)
makes GitHub silently drop the workflow. `make check-ascii` covers `.github/`,
`flake.nix`, `nix/` and the Makefile. It runs in `arch-pin` and in the
`.githooks/pre-push` hook (`make install-hooks`). A broken `ci.yml` can't
report its own failure; it shows up as a `ci-gate` check that never arrives.

## Formatting and linting

`arch-pin` checks formatting with the snapshot's clang-format. `make lint`
(clang-tidy) is local only, by decision: run it before opening a PR.

| Target | What it does | Tools |
|---|---|---|
| `make format` | `clang-format -i` over `src/` and `tests/` (`.clang-format`, copied from Hyprland). | `clang-format` |
| `make lint` | `clang-tidy` (`.clang-tidy`) over every `src/*.cpp`, `tests/compat/compat.cpp` and `tests/unit/unit.cpp`. Exits non-zero if any check in `WarningsAsErrors` fires. | `compiledb`, `clang++`, `clang-tidy`, `run-clang-tidy` |

`compiledb` is dev-only: `pip install compiledb`, in a venv if you like, or
`make lint COMPILEDB=/path/to/venv/bin/compiledb`.

How `make lint` works:

- **Compile database from this Makefile.** `compiledb -n make -B CXX=clang++
  all test-compat` (a dry run) writes `$(OUT)/lint/compile_commands.json`, so
  clang-tidy sees the real flags of the selected build mode.
- **`clang++` whatever `CXX` is.** The Makefile adds `--no-gnu-unique` for
  GCC, which clang-tidy rejects.
- **Header filter.** `.clang-tidy`'s `HeaderFilterRegex` also matches
  Hyprland's headers; `make lint` overrides it to this repo's `src/` and
  `tests/`.
- **`tests/unit/unit.cpp` separately.** `test-unit` compiles it with six
  sources in one command, which compiledb files under the last source. It
  gets a direct `clang-tidy` run instead.

`.clang-tidy` is Hyprland's, plus four disabled checks explained above
`Checks:` in the file. Don't re-enable anything Hyprland's list disables.

## `commit_pins`

`hyprpm.toml`'s `commit_pins` lets hyprpm build an older hyprtail commit when
the running Hyprland is exactly some pinned commit, instead of `HEAD`. Add
`[hyprland_commit, hyprtail_commit]` when a hyprtail change needs a newer
Hyprland than some users run. Not needed while `master` supports the pin.
A red `upstream-*` row is what tells you that time is coming.

## Release procedure

1. `ci-gate` green on `master`. Run `smoke.yml` by hand and wait for green.
2. Re-pin if the host's Hyprland moved (see "Re-pinning"). Re-pinning is
   deliberate, never automatic (SPEC §2).
3. When a new Hyprland minor ships, update the tag in the README's Nix example
   (`?ref=v0.56.2`), and drop its `inputs.nixpkgs.url` pin once the glaze
   workaround no longer applies.
4. Tag.

## Planned for 1.1

- **Port to Hyprland main.** Building against main fails to compile by
  design at launch; NOTES "Main 579829f" has what changed and the fix shape.
  Then let `upstream-main` alert.
- **Pin the `flake.nix` input defaults to the tested release**, so a consumer
  who pins nothing gets a Hyprland that builds instead of `main`. Deferred
  because CI overrides the `hyprland` input everywhere, so a wrong default
  would pass CI and only fail for a user.

## Runner image

Every job runs on `ubuntu-24.04`, not `ubuntu-latest`, which moves to Ubuntu
26.04 between 2026-10-19 and 2026-11-19 (runner-images #14748). 26.04 makes
`/tmp` a RAM-backed tmpfs with a per-user quota near 6.3G (runner-images
#14777), which a Hyprland build or the VM test may exceed. When trying 26.04,
add a non-required copy of a Nix job first. `cache-nix-action` may include
the OS release in its cache key, so caches saved on 24.04 might not restore
on 26.04. The `arch-*` jobs run in a container and don't care.

## Nix store cache

`.github/actions/nix-setup` installs Nix, restores the job's store cache and
registers `hyprland.cachix.org` read-only. It does not save:
`cache-nix-action`'s post step only saves on success. Each job saves
explicitly with `nix-community/cache-nix-action/save@v7`: on success in
`nix-pin` and the upstream rows, always in the smoke job, because finished
derivations are in the store even after a timeout.

## Permissions

Every workflow sets `permissions: contents: read`. Only `upstream.yml`'s
`alert` job declares its own (`issues: write`, `actions: read` to list the
run's jobs). A job-level block replaces the workflow-level one, so `alert` has
no `contents` access; it doesn't check out the repo. Nothing needs a secret,
so fork PRs run `ci.yml` the same as same-repo PRs.

## Scheduled-run limitations

GitHub disables scheduled workflows after 60 days without repository
activity. If `upstream.yml` or `smoke.yml` stops firing after a quiet period,
re-enable it under Settings -> Actions.
