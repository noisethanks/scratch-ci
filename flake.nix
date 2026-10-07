{
  description = "Cursor trail plugin for Hyprland";

  inputs = {
    hyprland.url = "github:hyprwm/Hyprland/v0.56.2";
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
          hyprtail = final.hyprlandPlugins.mkHyprlandPlugin {
            pluginName = "hyprtail";
            version = "git";
            src = ./.;

            installPhase = ''
              runHook preInstall
              install -Dm755 out/hyprtail.so $out/lib/libhyprtail.so
              runHook postInstall
            '';

            meta = {
              homepage = "https://github.com/noisethanks/hyprtail";
              description = "Cursor trail with a user-programmable ribbon shader";
              license = lib.licenses.bsd3;
              platforms = lib.platforms.linux;
            };
          };
        };
      };

      packages = eachSystem (system: {
        inherit (pkgsFor.${system}.hyprlandPlugins) hyprtail;
      });

      checks = eachSystem (system: self.packages.${system});

      # CI only: the VM smoke test (needs KVM, kept out of `checks`) and the
      # Hyprland the plugin is built against.
      legacyPackages = eachSystem (system: {
        smoke = import ./nix/smoke.nix {
          pkgs = pkgsFor.${system};
          hyprland = hyprland.packages.${system}.hyprland-with-tests;
          hyprtail = self.packages.${system}.hyprtail;
        };
        inherit (pkgsFor.${system}) hyprland;
      });

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
