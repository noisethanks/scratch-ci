# hyprtail lifecycle smoke test in a NixOS VM (SPEC section 10).
#
# Adapted from Hyprland's own hyprtester VM test (nix/tests/default.nix
# upstream, run by .github/workflows/nix-test.yml): same machine shape and
# environment, same `hyprtester -b ... -c ... -p ...` invocation as an unprivileged
# user on a virtio-gpu DRM backend. Differences: no gtests and no client
# programs, hyprtester carries tests/hyprtester/hyprtail_smoke.cpp, and
# hyprtail is loaded by that test through HYPRTAIL_SO.
#
# Built from flake.nix as legacyPackages.<system>.smoke, for whichever
# Hyprland the `hyprland` flake input is overridden to (CI's matrix rows):
#
#   nix build .#legacyPackages.x86_64-linux.smoke -L \
#     --override-input hyprland "github:hyprwm/Hyprland?ref=main" --no-write-lock-file
#
# Needs KVM to be usable; without it the VM falls back to software emulation
# and is very slow.
{
  pkgs,
  # hyprland-with-tests of the Hyprland under test.
  hyprland,
  # hyprtail built against that same Hyprland.
  hyprtail,
  memorySize ? 4096,
  cores ? 2,
}:
let
  # hyprtester with the smoke test compiled in. hyprtester globs
  # hyprtester/src/**/*.cpp (hyprtester/CMakeLists.txt) and is a
  # subdirectory of Hyprland's top-level build, so a test can only be added by
  # building it with the rest of the tree. Only the two targets it needs are
  # built, not Hyprland itself: `hyprtester` (its POST_BUILD step also builds
  # hyprtestplugin.so against the source tree's headers, which is why the
  # generated protocol headers come first). The config is test.lua with
  # smoke.lua appended, next to lua-require/: test.lua requires
  # config_dir() .. "/lua-require/...", so both have to sit in one directory.
  #
  # If building just these targets ever breaks, dropping the buildPhase and
  # installPhase overrides gives back the complete (slow) build.
  hyprtester = hyprland.overrideAttrs (old: {
    pname = "hyprtester-hyprtail";
    outputs = [ "out" ];

    postPatch = (old.postPatch or "") + ''
      cp ${../tests/hyprtester/hyprtail_smoke.cpp} hyprtester/src/tests/main/hyprtail_smoke.cpp
    '';

    buildPhase = ''
      runHook preBuild
      cmake --build . --target generate-protocol-headers hyprtester -j$NIX_BUILD_CORES
      runHook postBuild
    '';

    installPhase = ''
      runHook preInstall
      install -Dm755 hyprtester/hyprtester -t $out/bin
      install -Dm755 ../hyprtester/plugin/hyprtestplugin.so -t $out/lib
      mkdir -p $out/share/hypr
      cat ../hyprtester/test.lua ${../tests/hyprtester/smoke.lua} > $out/share/hypr/hyprtail_smoke.lua
      cp -r ../hyprtester/lua-require $out/share/hypr/
      runHook postInstall
    '';

    # The base derivation wraps Hyprland and installs the test clients here.
    postInstall = "";
  });
in
pkgs.testers.runNixOSTest {
  name = "hyprtail-smoke";

  nodes.machine =
    { pkgs, ... }:
    {
      environment.systemPackages = [ hyprtester ];

      # Enabled by default for some reason
      services.speechd.enable = false;

      environment.variables = {
        "AQ_TRACE" = "1";
        "HYPRLAND_TRACE" = "1";
        "XDG_RUNTIME_DIR" = "/tmp";
        "XDG_CACHE_HOME" = "/tmp";
        # Read by hyprtail_smoke.cpp: the plugin to test, and a scratch state
        # directory (errors.log and the crash-guard marker go there).
        "HYPRTAIL_SO" = "${hyprtail}/lib/libhyprtail.so";
        "XDG_STATE_HOME" = "/tmp/state";
        # Where the test writes the user preset it stacks layers with
        # (presets/ under hypr/hyprtail/).
        "XDG_CONFIG_HOME" = "/tmp/config";
      };

      programs.hyprland = {
        enable = true;
        package = hyprland;
        # We don't need portals in this test, so we don't set portalPackage
      };

      # Disable portals
      xdg.portal.enable = pkgs.lib.mkForce false;

      # Autologin into tty
      services.getty.autologinUser = "alice";

      system.stateVersion = "24.11";

      users.users.alice = {
        isNormalUser = true;
      };

      virtualisation = {
        inherit cores memorySize;
        resolution = {
          x = 1920;
          y = 1080;
        };

        qemu.options = [ "-vga none -device virtio-gpu-pci" ];
      };
    };

  testScript = ''
    # Wait for tty to be up
    machine.wait_for_unit("multi-user.target")

    # Hang or crash? Samples the Hyprland process (pid, state, kernel wait
    # channel) four times a second into /tmp/hyprproc, and marks the first
    # sample taken after hyprtester logs an IPC timeout. The process still
    # there (R busy loop, S/D blocked) means a hang, gone means a crash.
    watcher = """
    seen=0
    while :; do
      s="$(ps -eo pid=,stat=,wchan:20=,comm= | grep -i hyprland | tr -s ' ' | tr '\\n' ';')"
      [ -n "$s" ] || s="no Hyprland process"
      echo "$(date +%s.%N) $s"
      if [ $seen = 0 ] && grep -q "respond in time" /tmp/testerlog 2>/dev/null; then
        seen=1
        echo "$(date +%s.%N) IPC TIMEOUT first seen, Hyprland: $s"
      fi
      sleep 0.25
    done
    """
    machine.execute(f"cat > /tmp/hyprwatch.sh <<'EOF'\n{watcher}\nEOF")
    machine.execute("nohup sh /tmp/hyprwatch.sh > /tmp/hyprproc 2>&1 < /dev/null &")

    # Run only the hyprtail test (hyprtester takes test names as arguments)
    print("Running hyprtail smoke test")
    exit_status, _out = machine.execute("su - alice -c 'hyprtester -b ${hyprland}/bin/Hyprland -c ${hyprtester}/share/hypr/hyprtail_smoke.lua -p ${hyprtester}/lib/hyprtestplugin.so hyprtailLifecycle 2>&1 | tee /tmp/testerlog; exit ''${PIPESTATUS[0]}'")
    print(f"Hyprtester exited with {exit_status}")

    machine.execute("pkill -f hyprwatch.sh")

    # Print logs for visibility in CI
    _, out = machine.execute("cat /tmp/testerlog")
    print(f"Hyprtester log:\n{out}")

    # Only the samples where the process state changed (uniq skips the
    # timestamp field), so a healthy run is a few lines.
    _, out = machine.execute("uniq -f1 /tmp/hyprproc")
    print(f"Hyprland process timeline (epoch seconds):\n{out}")

    # Copy logs to host. The build succeeds whatever the test did, so the logs
    # survive; the workflow reads exit_status.
    machine.execute('cp "$(find /tmp/hypr -name *.log | head -1)" /tmp/hyprlog')
    machine.execute(f'echo {exit_status} > /tmp/exit_status')
    machine.copy_from_machine("/tmp/testerlog")
    machine.copy_from_machine("/tmp/hyprlog")
    machine.copy_from_machine("/tmp/exit_status")
    machine.copy_from_machine("/tmp/hyprproc")
    for optional in ["/tmp/state/hyprtail/errors.log", "/tmp/state/hyprtail/errors.log.1"]:
        # Not there when the plugin never got as far as writing them.
        if machine.execute(f"test -f {optional}")[0] == 0:
            machine.copy_from_machine(optional)

    # Finally - shutdown
    machine.shutdown()
  '';
}
