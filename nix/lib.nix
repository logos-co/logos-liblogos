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

    # Fail loudly rather than shipping a lib output with no RUNTIME artifact.
    # Verified failure this guards against: drop `RUNTIME DESTINATION lib` from
    # install(TARGETS logos_core) and the Windows build still exits 0 while
    # liblogos_core.dll appears nowhere in the package -- only the
    # liblogos_core.dll.a import library does. Match the loadable image
    # explicitly; a `liblogos_core.*` glob is satisfied by the import library
    # and catches nothing.
    #
    # Tested explicitly with `for`/`-f` rather than a glob array: bash's
    # nullglob only drops patterns that CONTAIN a wildcard, so a literal
    # "$out/lib/liblogos_core.dll" survives into the array even when the file
    # does not exist and the check passes vacuously. (Observed: the first
    # version of this guard did exactly that and let the broken build through.)
    found=""
    for cand in $out/lib/liblogos_core.so $out/lib/liblogos_core.dylib $out/lib/liblogos_core.dll; do
      [ -f "$cand" ] && found="$cand"
    done
    if [ -z "$found" ]; then
      echo "Error: no loadable logos_core library in ${build}/lib" >&2
      ls -la ${build}/lib ${build}/bin >&2 || true
      exit 1
    fi

    # Bundle package_manager_lib alongside logos_core (logos_core links against it)
    for f in ${logosPackageManagerRoot}/lib/libpackage_manager_lib*; do
      if [ -f "$f" ]; then
        cp -L "$f" $out/lib/
      fi
    done
  ''
