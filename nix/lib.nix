# Extracts libraries from the shared build
{ pkgs, common, build }:

let
  # Extract the package-manager root from CMake flags
  logosPackageManagerRoot = common.env.LOGOS_PACKAGE_MANAGER_ROOT;
in
pkgs.runCommand "${common.pname}-lib-${common.version}"
  {
    inherit (common) meta;
  }
  ''
    # Copy libraries from the shared build
    mkdir -p $out/lib
    if [ -d ${build}/lib ]; then
      cp -r ${build}/lib/* $out/lib/
    fi

    # Bundle package_manager_lib alongside logos_core (logos_core links against it)
    for f in ${logosPackageManagerRoot}/lib/libpackage_manager_lib*; do
      if [ -f "$f" ]; then
        cp -L "$f" $out/lib/
      fi
    done
  ''
