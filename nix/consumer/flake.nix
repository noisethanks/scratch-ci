{
  description = "hyprtail overlay, consumed the way a downstream flake would";

  # The wiring README.md ("Installation: Nix") recommends, and what CI's
  # `overlay-consumer` job builds (docs/CI.md). CI points `hyprtail` at the
  # checkout and `hyprland` at the row's ref with `--override-input`.
  inputs = {
    hyprland.url = "github:hyprwm/Hyprland";
    # Hyprland's own nixpkgs: the overlay builds Hyprland from this nixpkgs
    # (its dependencies, e.g. glaze, come from here), so this is the set
    # Hyprland is tested with.
    nixpkgs.follows = "hyprland/nixpkgs";
    hyprtail = {
      url = "github:noisethanks/hyprtail";
      inputs.hyprland.follows = "hyprland";
    };
  };

  outputs =
    {
      nixpkgs,
      hyprtail,
      ...
    }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        localSystem.system = system;
        overlays = [ hyprtail.overlays.default ];
      };
      # The Hyprland hyprtail's own `packages` are built against. Overlay
      # consumers must end up with the very same one, not nixpkgs' `hyprland`.
      expected = hyprtail.legacyPackages.${system}.hyprland.version;
    in
    {
      packages.${system}.default =
        assert
          pkgs.hyprland.version == expected
          || throw "overlays.default left hyprland at ${pkgs.hyprland.version}, expected ${expected}";
        pkgs.hyprlandPlugins.hyprtail;
    };
}
