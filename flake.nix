{
  description = "Logos liblogos core library";

  inputs = {
    # Follow the same nixpkgs as logos-cpp-sdk to ensure compatibility
    nixpkgs.follows = "logos-cpp-sdk/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk?ref=feat/logos-instance-id";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-module.url = "github:logos-co/logos-module";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-cpp-sdk, logos-capability-module, logos-module, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        capabilityModule = logos-capability-module.packages.${system}.default;
        logosModule = logos-module.packages.${system}.default;
        dirBundler = nix-bundle-dir.bundlers.${system}.qtApp;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, logosSdk, capabilityModule, logosModule, dirBundler }:
        let
          # Common configuration
          common = import ./nix/default.nix { inherit pkgs logosSdk logosModule; };
          src = ./.;
          
          # Shared build that compiles everything
          build = import ./nix/build.nix { inherit pkgs common src; };
          
          # Individual package components (reference the shared build)
          lib = import ./nix/lib.nix { inherit pkgs common build; };
          modules = import ./nix/modules.nix { inherit pkgs common capabilityModule; };
          bin = import ./nix/bin.nix { inherit pkgs common build lib modules; };
          include = import ./nix/include.nix { inherit pkgs common src logosSdk; };
          tests = import ./nix/tests.nix { inherit pkgs common build; };
          
          # Combined package
          liblogos = pkgs.symlinkJoin {
            name = "logos-liblogos";
            paths = [ bin lib include ];
          };
        in
        {
          # Individual outputs
          logos-liblogos-bin = bin;
          logos-liblogos-lib = lib;
          logos-liblogos-include = include;
          logos-liblogos-tests = tests;
          logos-liblogos-modules = modules;
          
          # Combined output
          logos-liblogos = liblogos;
          
          # Bundle outputs
          cli-bundle-dir = dirBundler bin;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          cli-appimage = nix-bundle-appimage.lib.${system}.mkAppImage {
            drv = bin;
            name = "logoscore";
            bundle = dirBundler bin;
            desktopFile = ./assets/logoscore.desktop;
            icon = ./assets/logoscore.png;
          };
        } // {
          # Default package
          default = liblogos;
        }
      );

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.zstd
          ];
        };
      });
    };
}
