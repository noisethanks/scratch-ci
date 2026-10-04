{
  description = "Cursor trail plugin for Hyprland";

  # Mirrors hyprland-plugins/flake.nix: `inputs.hyprland.follows` lets a
  # downstream flake lock hyprtail to the exact same Hyprland revision it
  # already has, avoiding an ABI mismatch between two separately-resolved
  # Hyprland closures.
  inputs = {
    hyprland.url = "github:hyprwm/Hyprland";
    nixpkgs.follows = "hyprland/nixpkgs";
    systems.follows = "hyprland/systems";
  };

  outputs =
    {
      self,
      hyprland,
      nixpkgs,
      systems,
      ...
    }:
    let
      inherit (nixpkgs) lib;
      eachSystem = lib.genAttrs (import systems);

      pkgsFor = eachSystem (
        system:
        import nixpkgs {
          localSystem.system = system;
          # `overlays.default` carries Hyprland's overlay with it (see there),
          # so this pkgs instance and any consumer's get the same Hyprland.
          overlays = [ self.overlays.default ];
        }
      );
    in
    {
      # Self-contained: composes `hyprland.overlays.hyprland-packages`
      # (Hyprland's own nix/overlays.nix: its dependency overlays and
      # `hyprland-no-deps`, which sets `final.hyprland` via `callPackage
      # ./default.nix { stdenv = final.gcc16Stdenv; ... }`) with the plugin.
      #
      # `hyprlandPlugins.mkHyprlandPlugin` (nixpkgs:
      # applications/window-managers/hyprwm/hyprland-plugins/default.nix)
      # builds every plugin via `hyprland.stdenv.mkDerivation` -- whatever
      # `hyprland` resolves to in the same pkgs at build time. Overlays patch
      # one shared fixed point, so the plugin below sees this flake's
      # `hyprland` input, built with GCC 16 (required for `#embed`: GCC 15+;
      # Makefile, src/ShaderSource.cpp), and its hyprland.pc with
      # `-I<prefix>/hyprland/src` (Hyprland >= v0.55.0). Without that, a
      # consumer applying only the plugin overlay builds against their own
      # nixpkgs' `hyprland`, which may be older (a 0.52.1 hyprland.pc lacks
      # that flag, so `#include <plugins/PluginAPI.hpp>` fails).
      #
      # Consequence: applying this overlay REPLACES the consumer's `hyprland`
      # package (and Hyprland's dependency overlays: hyprutils, aquamarine,
      # ...) with the ones from this flake's `hyprland` input. The consumer's
      # nixpkgs must provide `gcc16Stdenv`, which Hyprland's overlay uses.
      overlays.default = lib.composeManyExtensions [
        hyprland.overlays.hyprland-packages
        (final: prev: {
          hyprlandPlugins = (prev.hyprlandPlugins or { }) // {
            hyprtail = final.hyprlandPlugins.mkHyprlandPlugin (finalAttrs: {
              pluginName = "hyprtail";
              version = "git";
              src = ./.;

              # hyprtail's own Makefile has no `install` target (SPEC section 2: it's
              # built via `make all`, the same entry point hyprpm uses); the
              # default stdenv buildPhase already runs `make` unmodified, so
              # only the install step needs to be supplied here.
              installPhase = ''
                mkdir -p $out/lib
                install -Dm755 out/hyprtail.so $out/lib/libhyprtail.so
              '';

              meta = {
                homepage = "https://github.com/noisethanks/hyprtail-dev";
                description = "Cursor trail with a user-programmable ribbon shader";
                platforms = lib.platforms.linux;
                # No LICENSE file in the repo yet -- add one and a `license`
                # field here together.
              };
            });
          };
        })
      ];

      packages = eachSystem (system: {
        inherit (pkgsFor.${system}.hyprlandPlugins) hyprtail;
      });

      checks = eachSystem (system: self.packages.${system});

      # The NixOS-VM smoke test (nix/smoke.nix), kept out of `checks` and
      # `packages` on purpose: `nix flake check` (the flake-check CI job)
      # would build it, and it needs KVM and takes minutes. CI builds it per
      # Hyprland row by overriding the `hyprland` input, see the smoke job.
      legacyPackages = eachSystem (system: {
        smoke = import ./nix/smoke.nix {
          pkgs = pkgsFor.${system};
          hyprland = hyprland.packages.${system}.hyprland-with-tests;
          hyprtail = self.packages.${system}.hyprtail;
        };

        # The exact `hyprland` derivation hyprtail is built against (same
        # pkgsFor), for CI's nm import check: building it here reuses the
        # plugin's closure instead of building a second Hyprland.
        inherit (pkgsFor.${system}) hyprland;
      });

      # CI shell (`make test-unit`, `make test-compat`): hyprtail's own build
      # inputs, which is Hyprland's GCC 16 stdenv (hyprland.stdenv, as in
      # Hyprland's own devShell), pkg-config resolving hyprland.pc and its
      # Requires chain, plus glslangValidator for test-unit's GLSL check
      # (which skips itself if the tool is missing).
      devShells = eachSystem (
        system:
        let
          pkgs = pkgsFor.${system};
        in
        {
          ci = pkgs.mkShell.override { inherit (pkgs.hyprland) stdenv; } {
            name = "hyprtail-ci";
            inputsFrom = [ pkgs.hyprlandPlugins.hyprtail ];
            packages = [
              pkgs.pkg-config
              pkgs.glslang
            ];
          };
        }
      );
    };
}
