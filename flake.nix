{
  description = "Logos liblogos core library";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    logos-cpp-sdk.url = "github:logos-co/logos-cpp-sdk";
    logos-cpp-sdk.inputs.logos-protocol.follows = "logos-protocol";
    # Tracks master again. The rev pin here (c8bab12) existed only because
    # logos-qt-host's logos_api.cpp calls TokenManager::forIdentity and
    # ::isolateIdentity, and master's TokenManager had neither -- three hard
    # compile errors, not a subtle ABI skew. logos-protocol#59 ("per-client token
    # store, the host-services C ABI, and a container shape-check") MERGED and
    # closed that gap: master carries forIdentity/isolateIdentity in
    # cpp/token_manager.h and lp_grant_host_services/lp_token_keys in
    # cpp/logos_protocol.h. Note #59 was SQUASH-merged, so the old rev is not an
    # ancestor of master even though every line of it is in master -- check files,
    # not `git merge-base --is-ancestor`.
    #
    # What has NOT changed is why every in-process consumer must agree on ONE
    # logos-protocol: a single process holds liblogos_core plus the app image plus
    # every UI plugin, and they share TokenManager. Two revs across that boundary
    # give two TokenManager generations, hence two token stores, hence
    # "ModuleProxy: rejecting unauthorized call ... auth token not recognized".
    # That invariant is now maintained by the `follows` lines below rather than by
    # a rev, so keep them.
    logos-protocol.url = "github:logos-co/logos-protocol";
    logos-qt-sdk.url = "github:logos-co/logos-qt-sdk";
    logos-qt-sdk.inputs.logos-protocol.follows = "logos-protocol";
    logos-qt-sdk.inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    # The Qt HOST RUNTIME this library links: LogosAPI (the object handed to
    # initLogos), LogosAPIProvider, LogosProviderBase and the legacy
    # PluginInterface. It used to be compiled into logos-qt-sdk; the CODE now
    # lives in logos-plugin-qt, which owns the Qt plugin backend, and liblogos
    # takes it from there instead of through logos-qt-sdk's forwarding shim.
    #
    # logos-qt-sdk stays an input regardless: it still owns the DEVELOPER layer
    # this repo re-exports through nix/include.nix (logos_ui_plugin_context.h,
    # logos_qt_lp_bridge.h, logos_qt_wire.h) plus the Qt code generator. That
    # dependency is about headers, not about the host runtime.
    #
    # logos-protocol MUST follow. logos_qt_host and liblogos_core share
    # TokenManager and the transport ABI in ONE process, so two protocol revs
    # across that boundary is exactly the split-brain the Windows .def block in
    # src/CMakeLists.txt exists to prevent. logos-nix/nixpkgs follow so the Qt
    # the host runtime is compiled against is the Qt liblogos_core links.
    #
    # Tracks master again. This was pinned to cc24fa1c (a branch tip) for two
    # reasons, and logos-plugin-qt#19 ("the Qt host runtime and cdylib-glue
    # generator") MERGED as 9b2c64e retired both:
    #
    #   * logos-qt-host did not exist on master at all. It does now -- master's
    #     flake.nix publishes it (9 references) out of nix/qt-host.nix.
    #
    #   * master keyed `packages` off forAllSystems, so it had no x86_64-windows
    #     attribute, and this flake reaches for
    #     logos-plugin-qt.packages.x86_64-windows.logos-qt-host from
    #     forAllTargets -- an EVALUATION failure ("attribute 'x86_64-windows'
    #     missing"), not a link-time one. master now keys `packages` off
    #     forAllTargets, so that attribute resolves.
    #
    # The old note about matching whatever rev logos-qt-sdk pins is obsolete:
    # logos-qt-sdk's master (c6be61d0) has no logos-plugin-qt input at all, so
    # there is no second logos-qt-host to collide with. #19 was SQUASH-merged, so
    # cc24fa1c is not an ancestor of master; the files are what settle it.
    logos-plugin-qt.url = "github:logos-co/logos-plugin-qt";
    logos-plugin-qt.inputs.logos-nix.follows = "logos-nix";
    logos-plugin-qt.inputs.nixpkgs.follows = "nixpkgs";
    logos-plugin-qt.inputs.logos-protocol.follows = "logos-protocol";
    logos-capability-module.url = "github:logos-co/logos-capability-module";
    logos-module.url = "github:logos-co/logos-module";
    process-stats.url = "github:logos-co/process-stats";
    logos-container.url = "github:logos-co/logos-container";
    logos-module-loader.url = "github:logos-co/logos-module-loader";
    # The built-in default container + format-loader implementations. Named for
    # their ROLE rather than the backing repo, so `--override-input
    # default-container <other>` reads clearly. They point at the subprocess /
    # qt-plugin repos by default; swap the url (or override the input) to change
    # the default implementation.
    default-container.url = "github:logos-co/logos-container-subprocess";
    default-module-loader.url = "github:logos-co/logos-module-loader-qt";
    # The host transport (logos_host_qt) must be built against the SAME
    # logos-protocol as liblogos_core; otherwise the QtRO capability-token
    # handshake fails across the host<->plugin boundary. Pin it via follows so a
    # protocol bump here rebuilds the bundled host instead of leaving it on a
    # stale rev.
    default-module-loader.inputs.logos-protocol.follows = "logos-protocol";
    default-module-loader.inputs.logos-cpp-sdk.follows = "logos-cpp-sdk";
    default-module-loader.inputs.logos-qt-sdk.follows = "logos-qt-sdk";
    logos-package-manager.url = "github:logos-co/logos-package-manager";
  };

  outputs = { self, nixpkgs, logos-nix, logos-cpp-sdk, logos-protocol, logos-qt-sdk, logos-plugin-qt, logos-capability-module, logos-module, logos-package-manager, process-stats, logos-container, default-container, logos-module-loader, default-module-loader }:

    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        logosSdk = logos-cpp-sdk.packages.${system}.default;
        logosProtocolPkg = logos-protocol.packages.${system}.default;
        logosQtSdk = logos-qt-sdk.packages.${system}.default;
        logosQtHost = logos-plugin-qt.packages.${system}.logos-qt-host;
        capabilityModule = logos-capability-module.packages.${system}.default;
        logosModule = logos-module.packages.${system}.default;
        processStats = process-stats.packages.${system}.default;
        logosContainer = logos-container.packages.${system}.default;
        logosModuleLoader = logos-module-loader.packages.${system}.default;
        defaultContainer = default-container.packages.${system}.default;
        defaultModuleLoader = default-module-loader.packages.${system}.default;
        logosPackageManager = logos-package-manager.packages.${system}.lib;
        logosPackageManagerPortable = logos-package-manager.packages.${system}.lib-portable;
      });

      # Same as forAllSystems, plus the "x86_64-windows" pseudo-system. This
      # cannot just be logos-nix.lib.forAllTargets, because that only supplies
      # { system, pkgs } and this flake threads a dozen per-system dependencies
      # through.
      #
      # Every dependency below is a TARGET-side artifact (headers, archives, or
      # DLLs linked into logos_core, plus the host binary / plugin that are
      # merely re-exported). liblogos runs NO code generator at build time
      # (verified: no logos-cpp-generator / qt-generator anywhere in this repo),
      # so nothing here needs to come from the build platform's package set.
      #
      # Applied to `packages` ONLY: `checks` would have to execute PE test
      # binaries on the Linux builder, and a cross devShell offers no way to run
      # what it produces.
      windowsBuildSystem = "x86_64-linux";
      forAllTargets = f:
        nixpkgs.lib.genAttrs (systems ++ [ "x86_64-windows" ]) (system: f {
          inherit system;
          pkgs =
            if system == "x86_64-windows"
            then logos-nix.lib.mkWindowsPkgs { buildSystem = windowsBuildSystem; }
            else import nixpkgs { inherit system; };
          logosSdk = logos-cpp-sdk.packages.${system}.default;
          logosProtocolPkg = logos-protocol.packages.${system}.default;
          logosQtSdk = logos-qt-sdk.packages.${system}.default;
          logosQtHost = logos-plugin-qt.packages.${system}.logos-qt-host;
          capabilityModule = logos-capability-module.packages.${system}.default;
          logosModule = logos-module.packages.${system}.default;
          processStats = process-stats.packages.${system}.default;
          logosContainer = logos-container.packages.${system}.default;
          logosModuleLoader = logos-module-loader.packages.${system}.default;
          defaultContainer = default-container.packages.${system}.default;
          defaultModuleLoader = default-module-loader.packages.${system}.default;
          logosPackageManager = logos-package-manager.packages.${system}.lib;
          logosPackageManagerPortable = logos-package-manager.packages.${system}.lib-portable;
        });
    in
    {
      packages = forAllTargets ({ pkgs, system, logosSdk, logosProtocolPkg, logosQtSdk, logosQtHost, capabilityModule, logosModule, processStats, logosContainer, logosModuleLoader, defaultContainer, defaultModuleLoader, logosPackageManager, logosPackageManagerPortable }:
        let
          # The built-in default container + format-loader implementations — the
          # single place the default is chosen. Each is just the package; it
          # ships a generic CMake config (LogosContainerImpl / LogosFormatLoaderImpl)
          # that carries its library and deps, which liblogos find_package's. Swap
          # an entry to change the default — no C++, CMake, or nix-flag change.
          containerImpl = defaultContainer;
          formatLoaderImpl = defaultModuleLoader;

          # Common configuration (dev, default)
          common = import ./nix/default.nix {
            inherit pkgs logosSdk logosProtocolPkg logosQtSdk logosQtHost logosModule processStats logosContainer logosModuleLoader logosPackageManager containerImpl formatLoaderImpl;
          };
          # Common configuration (portable)
          commonPortable = import ./nix/default.nix {
            inherit pkgs logosSdk logosProtocolPkg logosQtSdk logosQtHost logosModule processStats logosContainer logosModuleLoader containerImpl formatLoaderImpl;
            logosPackageManager = logosPackageManagerPortable;
            portableBuild = true;
          };
          src = ./.;

          # Shared build that compiles everything (dev)
          build = import ./nix/build.nix { inherit pkgs common src; };

          # Shared build (portable)
          buildPortable = import ./nix/build.nix { inherit pkgs src; common = commonPortable; };

          # Individual package components (reference the shared build)
          lib = import ./nix/lib.nix { inherit pkgs common build; };
          modules = import ./nix/modules.nix { inherit pkgs common capabilityModule; };
          modulesPortable = import ./nix/modules.nix { inherit pkgs capabilityModule; common = commonPortable; portableBuild = true; };
          bin = import ./nix/bin.nix { inherit pkgs common build lib modules formatLoaderImpl; };
          include = import ./nix/include.nix { inherit pkgs common src logosSdk; inherit logosProtocolPkg logosQtSdk logosQtHost; };
          tests = import ./nix/tests.nix { inherit pkgs common build; };

          # Portable package components
          libPortable = import ./nix/lib.nix { inherit pkgs; common = commonPortable; build = buildPortable; };
          binPortable = import ./nix/bin.nix { inherit pkgs formatLoaderImpl; common = commonPortable; build = buildPortable; lib = libPortable; modules = modulesPortable; };
          includePortable = import ./nix/include.nix { inherit pkgs src logosSdk; inherit logosProtocolPkg logosQtSdk logosQtHost; common = commonPortable; };

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
          logos-liblogos-modules = modules;

          # Combined output
          logos-liblogos = liblogos;

          # Portable output (compiled with LOGOS_PORTABLE_BUILD)
          portable = liblogosPortable;

          # Default package (dev)
          default = liblogos;
        }
        # The test suite is POSIX-only (posix_spawn/waitpid/kill, /bin/sh) and
        # CMake gates it off for a Windows host, so `ninja logos_core_tests`
        # would have no such target. Not exposing the output at all beats
        # shipping one that cannot be built.
        // pkgs.lib.optionalAttrs (!pkgs.stdenv.hostPlatform.isWindows) {
          logos-liblogos-tests = tests;
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
