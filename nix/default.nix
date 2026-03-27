# Common build configuration shared across all packages
{ pkgs, logosSdk, logosModule, processStats, logosPackageManager, portableBuild ? false }:

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

  # Common runtime dependencies
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.zstd
    pkgs.gtest
    pkgs.nlohmann_json
    logosModule
    processStats
    logosPackageManager
  ];

  # Common CMake flags
  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
    "-DPROCESS_STATS_ROOT=${processStats}"
    "-DLOGOS_PACKAGE_MANAGER_ROOT=${logosPackageManager}"
  ] ++ pkgs.lib.optionals portableBuild [
    "-DLOGOS_PORTABLE_BUILD=ON"
  ];

  # Environment variables
  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_MODULE_ROOT = "${logosModule}";
    PROCESS_STATS_ROOT = "${processStats}";
    LOGOS_PACKAGE_MANAGER_ROOT = "${logosPackageManager}";
  };

  # Metadata
  meta = with pkgs.lib; {
    description = "Logos liblogos core library";
    platforms = platforms.unix;
  };
}
