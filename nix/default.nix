# Common build configuration shared across all packages
{ pkgs, logosSdk, logosProtocolPkg, logosQtSdk, logosModule, processStats
, logosContainer       # container contract (headers: ModuleContainer + makeContainer seam)
, logosModuleLoader    # format-loader contract (headers: ModuleFormatLoader + makeFormatLoader seam)
, logosPackageManager

  # Built-in (default) loader implementations — just the packages. Each ships a
  # generic CMake config (LogosContainerImpl / LogosFormatLoaderImpl) that
  # find_package picks up from CMAKE_PREFIX_PATH and which carries the impl's
  # library + its own deps, so nothing else (lib name, deps, root flags) needs to
  # be passed here. Swapping an entry in flake.nix changes the default — no C++,
  # CMake, or nix-flag edit.
, containerImpl       # package providing makeContainer() (+ LogosContainerImpl config)
, formatLoaderImpl    # package providing makeFormatLoader() (+ LogosFormatLoaderImpl config)

, portableBuild ? false }:

{
  pname = "logos-liblogos";
  version = "0.1.0";

  # Common native build inputs
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsNoGuiHook
  ];

  # Common runtime dependencies. Boost, OpenSSL, and nlohmann_json
  # come in transitively via logosSdk's `propagatedBuildInputs`
  # (declared on the SDK's symlinkJoin in logos-cpp-sdk/flake.nix);
  # listing them here would be redundant. Qt6 is intentionally NOT
  # propagated by the SDK (qtbase's setup-hook fires `qtPreHook`
  # which errors unless `wrapQtAppsHook` was sourced first, and that
  # ordering can't be guaranteed through propagation), so we list it
  # explicitly. The SDK's CMake Config re-runs find_dependency(...)
  # against the propagated non-Qt entries + the Qt entries we list
  # here at configure time, so logoscore + downstream consumers get
  # the imported targets wired up automatically.
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    logosSdk
    logosProtocolPkg
    logosQtSdk
    pkgs.zstd
    pkgs.gtest
    pkgs.spdlog
    logosModule
    processStats
    logosContainer
    containerImpl
    logosModuleLoader
    formatLoaderImpl
    logosPackageManager
  ];

  # Common CMake flags
  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_PROTOCOL_ROOT=${logosProtocolPkg}"
    "-DLOGOS_QT_SDK_ROOT=${logosQtSdk}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
    "-DPROCESS_STATS_ROOT=${processStats}"
    "-DLOGOS_CONTAINER_ROOT=${logosContainer}"
    "-DLOGOS_MODULE_LOADER_ROOT=${logosModuleLoader}"
    # The container + format-loader implementations are discovered via
    # find_package (LogosContainerImpl / LogosFormatLoaderImpl) — their packages
    # are in buildInputs, so they're on CMAKE_PREFIX_PATH. No impl flags needed.
    "-DLOGOS_PACKAGE_MANAGER_ROOT=${logosPackageManager}"
  ] ++ pkgs.lib.optionals portableBuild [
    "-DLOGOS_PORTABLE_BUILD=ON"
  ];

  # Environment variables
  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_PROTOCOL_ROOT = "${logosProtocolPkg}";
    LOGOS_QT_SDK_ROOT = "${logosQtSdk}";
    LOGOS_MODULE_ROOT = "${logosModule}";
    PROCESS_STATS_ROOT = "${processStats}";
    LOGOS_CONTAINER_ROOT = "${logosContainer}";
    LOGOS_MODULE_LOADER_ROOT = "${logosModuleLoader}";
    LOGOS_PACKAGE_MANAGER_ROOT = "${logosPackageManager}";
  };

  # Metadata
  meta = with pkgs.lib; {
    description = "Logos liblogos core library";
    platforms = platforms.unix;
  };
}
