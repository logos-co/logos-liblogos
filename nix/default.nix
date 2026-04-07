# Common build configuration shared across all packages
{ pkgs, logosSdk, logosModule, logosPackageManager, processStats, portableBuild ? false }:

{
  pname = "logos-liblogos";
  version = "0.1.0";

  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.qt6.wrapQtAppsNoGuiHook
  ];

  buildInputs = [
    pkgs.boost
    pkgs.spdlog
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.zstd
    pkgs.gtest
    pkgs.nlohmann_json
    pkgs.cli11
    logosModule
    logosPackageManager
    processStats
  ];

  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_CPP_SDK_ROOT=${logosSdk}"
    "-DLOGOS_MODULE_ROOT=${logosModule}"
    "-DLOGOS_PACKAGE_MANAGER_ROOT=${logosPackageManager}"
    "-DPROCESS_STATS_ROOT=${processStats}"
  ] ++ pkgs.lib.optionals portableBuild [
    "-DLOGOS_PORTABLE_BUILD=ON"
  ];

  env = {
    LOGOS_CPP_SDK_ROOT = "${logosSdk}";
    LOGOS_MODULE_ROOT = "${logosModule}";
    LOGOS_PACKAGE_MANAGER_ROOT = "${logosPackageManager}";
    PROCESS_STATS_ROOT = "${processStats}";
  };

  meta = with pkgs.lib; {
    description = "Logos liblogos core library";
    platforms = platforms.unix;
  };
}
