{
  description = "Logos liblogos core library";

  inputs = {
    # Follow the same nixpkgs as logos-cpp-sdk to ensure compatibility
    nixpkgs.follows = "logos-cpp-sdk/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
  };

  outputs = { self, nixpkgs, logos-cpp-sdk }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
      });
    in
    {
      packages = forAllSystems ({ pkgs, logosSdk }: 
        let
          # Common configuration
          common = import ./nix/default.nix { inherit pkgs logosSdk; };
          src = ./.;
          
          # Shared build that compiles everything
          build = import ./nix/build.nix { inherit pkgs common src; };
          
          # Individual package components (reference the shared build)
          lib = import ./nix/lib.nix { inherit pkgs common build; };
          bin = import ./nix/bin.nix { inherit pkgs common build lib; };
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
          
          # Combined output
          logos-liblogos = liblogos;
          
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
