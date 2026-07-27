{
  description = "Scenefx development environment";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixpkgs-unstable";

    systems.url = "github:nix-systems/default-linux";

	flake-utils = {
	  url = "github:numtide/flake-utils";
	  inputs.systems.follows = "systems";
	};
  };

  outputs = { self, nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
	    pkgs = import nixpkgs { inherit system; };

      in {
        packages = rec {
          scenefx = pkgs.stdenv.mkDerivation {
            pname = "asteroidz-scenefx";
            version = "0.5.0";
            src = ./.;
            outputs = [
              "out"
              "lib"
            ];

            mesonFlags = [
              "-Drenderers=gles2,vulkan"
              "-Dexamples=false"
            ];

            nativeBuildInputs = with pkgs; [
              pkg-config
              meson
              cmake
              ninja
              scdoc
              wayland-scanner
              glslang # compile the fx_vk shaders to SPIR-V
            ];

            buildInputs = with pkgs; [
              libdrm
              libxkbcommon
              pixman
              libGL # egl
              mesa # gbm
              wayland # wayland-server
              wayland-protocols
              wlroots_0_20
              libgbm
              libxcb
              libxcb-wm
              lcms2
              vulkan-loader # fx_vk renderer
              vulkan-headers
            ];

            meta = with pkgs.lib; {
              description = "scenefx fork for asteroidz — wlroots scene API with GLES2 and Vulkan (fx_vk) effects";
              homepage = "https://github.com/asteroidzman/asteroidz-scenefx";
              license = licenses.mit;
              platforms = platforms.linux;
            };
          };

          # Aliases: asteroidz imports `.scenefx`; keep the old name working too.
          scenefx-git = scenefx;
          default = scenefx;
		};

        devShells.default = pkgs.mkShell {
          name = "scenefx-shell";
	      inputsFrom = [ self.packages.${system}.scenefx-git ];
		  hardeningDisable = [ "fortify" ];
	    };

        formatter = pkgs.nixfmt;
      }
    );
}

