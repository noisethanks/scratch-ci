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
          # `hyprlandPlugins.mkHyprlandPlugin` (nixpkgs:
          # applications/window-managers/hyprwm/hyprland-plugins/default.nix)
          # builds every plugin via `hyprland.stdenv.mkDerivation` -- whatever
          # stdenv the `hyprland` package resolves to in this same pkgs
          # instance at build time. `hyprland.overlays.hyprland-packages`
          # (Hyprland's own nix/overlays.nix, `hyprland-no-deps`) sets
          # `final.hyprland` via `callPackage ./default.nix { stdenv =
          # final.gcc16Stdenv; ... }`. Overlays patch one shared fixed point,
          # so both of these resolve against the same `final.hyprland`
          # regardless of list order below: hyprtail is built with GCC 16,
          # required for `#embed` (GCC 15+; Makefile, src/ShaderSource.cpp),
          # not nixpkgs' own separately-pinned `hyprland` package.
          overlays = [
            self.overlays.default
            hyprland.overlays.hyprland-packages
          ];
        }
      );
    in
    {
      overlays.default = final: prev: {
        hyprlandPlugins = (prev.hyprlandPlugins or { }) // {
          hyprtail = final.hyprlandPlugins.mkHyprlandPlugin (finalAttrs: {
            pluginName = "hyprtail";
            version = "git";
            src = ./.;

            # hyprtail's own Makefile has no `install` target (SPEC §2: it's
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
      };

      packages = eachSystem (system: {
        inherit (pkgsFor.${system}.hyprlandPlugins) hyprtail;
      });

      checks = eachSystem (system: self.packages.${system});
    };
}
