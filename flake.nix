{
  description = "Logos liblogos core library";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-cpp-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-protocol.url = "github:logos-co/logos-protocol";
    logos-qt-sdk.url = "github:logos-co/logos-qt-sdk";
    logos-qt-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-qt-sdk.inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-module.url = "github:logos-co/logos-module";
    process-stats.url = "github:logos-co/process-stats";
    logos-container.url = "github:logos-co/logos-container/abstract_container";
    logos-container-subprocess.url = "github:logos-co/logos-container-subprocess/abstract_container";
    logos-container-subprocess.inputs.logos-container.follows = "logos-container";
    logos-module-loader.url = "github:logos-co/logos-module-loader/abstract_module_loader";
    logos-module-loader.inputs.logos-container.follows = "logos-container";
    logos-module-loader-qt.url = "github:logos-co/logos-module-loader-qt/abstract_module_loader";
    logos-module-loader-qt.inputs.logos-container.follows = "logos-container";
    logos-module-loader-qt.inputs.logos-module-loader.follows = "logos-module-loader";
    logos-module-loader-qt.inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    logos-module-loader-qt.inputs.logos-protocol.follows = "logos-protocol";
    logos-module-loader-qt.inputs.logos-qt-sdk.follows = "logos-qt-sdk";
    logos-module-loader-qt.inputs.logos-module.follows = "logos-module";
    logos-package-manager.url = "github:logos-co/logos-package-manager";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-qt-sdk, logos-capability-module, logos-module, logos-package-manager, process-stats, logos-container, logos-container-subprocess, logos-module-loader, logos-module-loader-qt }:

    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosProtocolPkg = logos-protocol.packages.${system}.default;
        logosQtSdk = logos-qt-sdk.packages.${system}.default;
        capabilityModule = logos-capability-module.packages.${system}.default;
        logosModule = logos-module.packages.${system}.default;
        processStats = process-stats.packages.${system}.default;
        logosContainer = logos-container.packages.${system}.default;
        logosContainerSubprocess = logos-container-subprocess.packages.${system}.default;
        logosModuleLoader = logos-module-loader.packages.${system}.default;
        logosModuleLoaderQt = logos-module-loader-qt.packages.${system}.default;
        logosPackageManager = logos-package-manager.packages.${system}.lib;
        logosPackageManagerPortable = logos-package-manager.packages.${system}.lib-portable;
      });
    in
    {
      packages = forAllSystems ({ pkgs, system, logosSdk, logosProtocolPkg, logosQtSdk, capabilityModule, logosModule, processStats, logosContainer, logosContainerSubprocess, logosModuleLoader, logosModuleLoaderQt, logosPackageManager, logosPackageManagerPortable }:
        let
          # Common configuration (dev, default)
          common = import ./nix/default.nix { inherit pkgs logosSdk logosProtocolPkg logosQtSdk logosModule processStats logosContainer logosContainerSubprocess logosModuleLoader logosModuleLoaderQt logosPackageManager; };
          # Common configuration (portable)
          commonPortable = import ./nix/default.nix { inherit pkgs logosSdk logosProtocolPkg logosQtSdk logosModule processStats logosContainer logosContainerSubprocess logosModuleLoader logosModuleLoaderQt; logosPackageManager = logosPackageManagerPortable; portableBuild = true; };
          src = ./.;

          # Shared build that compiles everything (dev)
          build = import ./nix/build.nix { inherit pkgs common src; };

          # Shared build (portable)
          buildPortable = import ./nix/build.nix { inherit pkgs src; common = commonPortable; };

          # Individual package components (reference the shared build)
          lib = import ./nix/lib.nix { inherit pkgs common build; };
          modules = import ./nix/modules.nix { inherit pkgs common capabilityModule; };
          modulesPortable = import ./nix/modules.nix { inherit pkgs capabilityModule; common = commonPortable; portableBuild = true; };
          bin = import ./nix/bin.nix { inherit pkgs common build lib modules logosModuleLoaderQt; };
          include = import ./nix/include.nix { inherit pkgs common src logosSdk; inherit logosProtocolPkg logosQtSdk; };
          tests = import ./nix/tests.nix { inherit pkgs common build; };

          # Portable package components
          libPortable = import ./nix/lib.nix { inherit pkgs; common = commonPortable; build = buildPortable; };
          binPortable = import ./nix/bin.nix { inherit pkgs; common = commonPortable; build = buildPortable; lib = libPortable; modules = modulesPortable; logosModuleLoaderQt = logosModuleLoaderQt; };
          includePortable = import ./nix/include.nix { inherit pkgs src logosSdk; inherit logosProtocolPkg logosQtSdk; common = commonPortable; };

          # Combined package (dev)
          liblogos = pkgs.symlinkJoin {
            name = "logos-liblogos";
            paths = [ bin lib include ];
          };

          # Combined package (portable)
          liblogosPortable = pkgs.symlinkJoin {
            name = "logos-liblogos-portable";
            paths = [ binPortable libPortable includePortable ];
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

          # Portable output (compiled with LOGOS_PORTABLE_BUILD)
          portable = liblogosPortable;

          # Default package (dev)
          default = liblogos;
        }
      );

      checks = forAllSystems ({ pkgs, system, ... }:
        let
          testsPkg = self.packages.${system}.logos-liblogos-tests;
          # Real Qt plugin used by RealPluginRegistryTest (TEST_PLUGIN env var).
          # capability_module is already a flake input and builds a real plugin.
          capabilityModulePkg = logos-capability-module.packages.${system}.default;
          pluginExt = if pkgs.stdenv.isDarwin then "dylib" else "so";
        in {
          tests = pkgs.runCommand "logos-liblogos-tests" {
            nativeBuildInputs = [ testsPkg ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.qt6.qtbase ];
          } ''
            export QT_QPA_PLATFORM=offscreen
            ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/${pkgs.qt6.qtbase.qtPluginPrefix}"
            ''}
            export TEST_PLUGIN="${capabilityModulePkg}/lib/capability_module_plugin.${pluginExt}"
            mkdir -p $out
            echo "Running logos-liblogos tests..."
            echo "TEST_PLUGIN=$TEST_PLUGIN"
            ${testsPkg}/bin/logos_core_tests --gtest_output=xml:$out/test-results.xml
          '';
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
            pkgs.spdlog
          ];
        };
      });
    };
}
