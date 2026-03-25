# Common build configuration shared across all packages
{ pkgs, logosSdk, moduleClient, logosModule, processStats, portableBuild ? false }:

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
    moduleClient
    logosModule
    processStats
  ];

  # Common CMake flags
  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_MODULE_CLIENT_ROOT=${moduleClient}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
    "-DPROCESS_STATS_ROOT=${processStats}"
  ] ++ pkgs.lib.optionals portableBuild [
    "-DLOGOS_PORTABLE_BUILD=ON"
  ];

  # Environment variables
  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_MODULE_CLIENT_ROOT = "${moduleClient}";
    LOGOS_MODULE_ROOT = "${logosModule}";
    PROCESS_STATS_ROOT = "${processStats}";
  };

  # Metadata
  meta = with pkgs.lib; {
    description = "Logos liblogos core library";
    platforms = platforms.unix;
  };
}

