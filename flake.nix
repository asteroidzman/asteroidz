{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs = {
    self,
    flake-parts,
    ...
  } @ inputs:
    flake-parts.lib.mkFlake {inherit inputs;} {
      imports = [
        inputs.flake-parts.flakeModules.easyOverlay
      ];

      flake = {
        hmModules.asteroidz = import ./nix/hm-modules.nix self;
        nixosModules.asteroidz = import ./nix/nixos-modules.nix self;
      };

      perSystem = {
        config,
        pkgs,
        ...
      }: let
        inherit (pkgs) callPackage ;
        asteroidz = callPackage ./nix { };
        shellOverride = old: {
          nativeBuildInputs = old.nativeBuildInputs ++ [];
          buildInputs = old.buildInputs ++ [];
        };
      in {
        packages = {
          default = asteroidz;
          inherit asteroidz;
          hm-options-json = pkgs.callPackage (import ./nix/generate-options.nix self) {
            module = ./nix/hm-modules.nix;
            optionPrefix = "wayland.windowManager.asteroidz.";
          };
          nixos-options-json = pkgs.callPackage (import ./nix/generate-options.nix self) {
            module = ./nix/nixos-modules.nix;
            optionPrefix = "programs.asteroidz.";
          };
        };
        overlayAttrs = {
          inherit (config.packages) asteroidz;
        };
        devShells.default = asteroidz.overrideAttrs shellOverride;
        formatter = pkgs.alejandra;
      };
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
    };
}
