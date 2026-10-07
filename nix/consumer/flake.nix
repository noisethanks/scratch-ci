{
  description = "hyprtail overlay, consumed the way a downstream flake would";

  inputs = {
    hyprland.url = "github:hyprwm/Hyprland";
    nixpkgs.follows = "hyprland/nixpkgs";
    hyprtail = {
      url = "github:noisethanks/hyprtail";
      inputs.hyprland.follows = "hyprland";
    };
  };

  outputs =
    {
      nixpkgs,
      hyprland,
      hyprtail,
      ...
    }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        localSystem.system = system;
        overlays = [
          hyprland.overlays.hyprland-packages
          hyprtail.overlays.default
        ];
      };
      expected = hyprtail.legacyPackages.${system}.hyprland.version;
    in
    {
      packages.${system}.default =
        assert
          pkgs.hyprland.version == expected
          || throw "consumer hyprland is ${pkgs.hyprland.version}, hyprtail's is ${expected}";
        pkgs.hyprlandPlugins.hyprtail;
    };
}
