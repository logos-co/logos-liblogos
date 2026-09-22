# Common build configuration shared across all packages
{ pkgs, logosProtocolPkg, processStats
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

  # The parent library is deliberately Qt-free. Qt appears only in the
  # separately packaged compatibility host selected by bin.nix.
  nativeBuildInputs = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
  ];

  buildInputs = [
    logosProtocolPkg
    pkgs.boost
    pkgs.nlohmann_json
    pkgs.gtest
    pkgs.spdlog
    processStats
    logosContainer
    containerImpl
    logosModuleLoader
    formatLoaderImpl
    logosPackageManager
  ];

  cmakeFlags = [
    "-GNinja"
    "-DLOGOS_PROTOCOL_ROOT=${logosProtocolPkg}"
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
    LOGOS_PROTOCOL_ROOT = "${logosProtocolPkg}";
    PROCESS_STATS_ROOT = "${processStats}";
    LOGOS_CONTAINER_ROOT = "${logosContainer}";
    LOGOS_MODULE_LOADER_ROOT = "${logosModuleLoader}";
    LOGOS_PACKAGE_MANAGER_ROOT = "${logosPackageManager}";
  };

  # Metadata
  meta = with pkgs.lib; {
    description = "Logos liblogos core library";
    platforms = platforms.unix ++ platforms.windows;
  };
}
