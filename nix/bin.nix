# Assembles the liblogos `bin` output: logos_runtime, the module hosts
# re-exported from logos-module-loader-qt (already wrapped), the runtime libs
# and the built-in modules, in the shape frontends expect.
{ pkgs, common, build, lib, modules, moduleHosts }:

pkgs.stdenvNoCC.mkDerivation {
  pname = "${common.pname}-bin";
  version = common.version;

  # No source to unpack - we're copying from other derivations
  dontUnpack = true;

  installPhase = ''
    runHook preInstall

    # Re-export the host binary (logos_host_qt + logos_host symlink), already
    # Qt-wrapped and patched, from the format-loader implementation package.
    mkdir -p $out/bin
    if [ -d ${moduleHosts}/bin ]; then
      cp -a ${moduleHosts}/bin/. $out/bin/
      chmod u+w $out/bin
    fi

    # The runtime in a process of its own, which apps spawn beside the hosts.
    runtime=""
    for cand in ${build}/bin/logos_runtime ${build}/bin/logos_runtime.exe; do
      [ -f "$cand" ] && runtime="$cand"
    done
    if [ -z "$runtime" ]; then
      echo "Error: no logos_runtime in ${build}/bin" >&2
      exit 1
    fi
    cp "$runtime" $out/bin/

    # Runtime libraries for downstream linking (liblogos_core etc.)
    mkdir -p $out/lib
    if [ -d ${lib}/lib ]; then
      cp -r ${lib}/lib/* $out/lib/
    fi

    # Built-in modules
    mkdir -p $out/modules
    if [ -d ${modules}/modules ]; then
      cp -r ${modules}/modules/* $out/modules/
    fi

    runHook postInstall
  '';

  inherit (common) meta;
}
