# Assembles the liblogos `bin` output. liblogos itself no longer builds a
# binary (logos_host_qt moved to logos-module-loader-qt), so this re-exports the
# already-wrapped host binary from that package and bundles the runtime libs +
# built-in modules, keeping the output shape frontends expect.
{ pkgs, common, build, lib, modules, formatLoaderImpl }:

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
    if [ -d ${formatLoaderImpl}/bin ]; then
      cp -a ${formatLoaderImpl}/bin/. $out/bin/
    fi

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
