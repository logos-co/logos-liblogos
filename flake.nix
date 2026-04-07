{
  description = "Logos liblogos core library";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-module.url = "github:logos-co/logos-module";
    logos-package-manager.url = "github:logos-co/logos-package-manager";
    process-stats.url = "github:logos-co/process-stats/replace_qt";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-capability-module, logos-module, logos-package-manager, process-stats }:

    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        capabilityModule = logos-capability-module.packages.${system}.default;
        logosModule = logos-module.packages.${system}.default;
        logosPackageManager = logos-package-manager.packages.${system}.lib;
        logosPackageManagerPortable = logos-package-manager.packages.${system}.lib-portable;
        processStatsPkg = process-stats.packages.${system}.default;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, logosSdk, capabilityModule, logosModule, logosPackageManager, logosPackageManagerPortable, processStatsPkg }:
        let
          common = import ./nix/default.nix {
            inherit pkgs logosSdk logosModule logosPackageManager;
            processStats = processStatsPkg;
            portableBuild = false;
          };
          commonPortable = import ./nix/default.nix {
            inherit pkgs logosSdk logosModule;
            logosPackageManager = logosPackageManagerPortable;
            processStats = processStatsPkg;
            portableBuild = true;
          };
          src = ./.;

          build = import ./nix/build.nix { inherit pkgs common src; };
          buildPortable = import ./nix/build.nix { inherit pkgs src; common = commonPortable; };

          lib = import ./nix/lib.nix { inherit pkgs common build; };
          modules = import ./nix/modules.nix { inherit pkgs common capabilityModule; };
          modulesPortable = import ./nix/modules.nix { inherit pkgs capabilityModule; common = commonPortable; portableBuild = true; };
          bin = import ./nix/bin.nix { inherit pkgs common build lib modules; };
          include = import ./nix/include.nix { inherit pkgs common src logosSdk; };
          tests = import ./nix/tests.nix { inherit pkgs common build; };

          libPortable = import ./nix/lib.nix { inherit pkgs; common = commonPortable; build = buildPortable; };
          binPortable = import ./nix/bin.nix { inherit pkgs; common = commonPortable; build = buildPortable; lib = libPortable; modules = modulesPortable; };
          includePortable = import ./nix/include.nix { inherit pkgs src logosSdk; common = commonPortable; };

          liblogos = pkgs.symlinkJoin {
            name = "logos-liblogos";
            paths = [ bin lib include ];
          };

          liblogosPortable = pkgs.symlinkJoin {
            name = "logos-liblogos-portable";
            paths = [ binPortable libPortable includePortable ];
          };
        in
        {
          logos-liblogos-bin = bin;
          logos-liblogos-lib = lib;
          logos-liblogos-include = include;
          logos-liblogos-tests = tests;
          logos-liblogos-modules = modules;

          logos-liblogos = liblogos;
          portable = liblogosPortable;
          default = liblogos;
        }
      );

      checks = forAllSystems ({ pkgs, system, ... }:
        let
          testsPkg = self.packages.${system}.logos-liblogos-tests;
        in {
          tests = pkgs.runCommand "logos-liblogos-tests" {
            nativeBuildInputs = [ testsPkg ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtbase ];
          } ''
            export QT_QPA_PLATFORM=offscreen
            ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
            ''}
            mkdir -p $out
            echo "Running logos-liblogos tests..."
            ${testsPkg}/bin/logos_core_tests --gtest_output=xml:$out/test-results.xml
          '';
        }
      );

      devShells = forAllSystems ({ pkgs, system, ... }:
        let
          boostProcessV2Impl = pkgs.callPackage ./nix/boost-process-v2-impl.nix { };
        in
        {
          default = pkgs.mkShell {
            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.pkg-config
              boostProcessV2Impl
            ];
            buildInputs = [
              pkgs.boost
              pkgs.spdlog
              pkgs.qt6.qtbase
              pkgs.qt6.qtremoteobjects
              pkgs.zstd
              process-stats.packages.${system}.default
            ];
            shellHook = ''
              export BOOST_PROCESS_V2_IMPL_LIBRARY="${boostProcessV2Impl}/lib/libboost_process_v2_impl.a"
            '';
          };
        });
    };
}
