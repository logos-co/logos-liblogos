# Installs the logos-liblogos headers
{ pkgs, common, src, logosSdk ? null, logosProtocolPkg ? null, logosQtSdk ? null
, logosQtHost ? null }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-headers";
  version = common.version;
  
  inherit src;
  inherit (common) meta;
  
  # No build phase needed, just install headers
  dontBuild = true;
  dontConfigure = true;
  
  installPhase = ''
    runHook preInstall
    
    # Install headers
    mkdir -p $out/include
    
    # Install logos_core.h (main C API header)
    if [ -f src/logos_core/logos_core.h ]; then
      cp src/logos_core/logos_core.h $out/include/
    fi
    
    # Also copy SDK headers if available (including logos_mode.h)
    if [ -n "${toString logosSdk}" ] && [ -d "${toString logosSdk}/include" ]; then
      cp -r ${toString logosSdk}/include/* $out/include/ 2>/dev/null || true
    fi
    if [ -n "${toString logosProtocolPkg}" ] && [ -d "${toString logosProtocolPkg}/include" ]; then
      cp -r ${toString logosProtocolPkg}/include/* $out/include/ 2>/dev/null || true
    fi
    if [ -n "${toString logosQtSdk}" ] && [ -d "${toString logosQtSdk}/include" ]; then
      cp -r ${toString logosQtSdk}/include/* $out/include/ 2>/dev/null || true
    fi
    # logos-qt-host LAST, and deliberately so. It and logos-qt-sdk both ship a
    # logos_api.h (and the rest of the host-runtime headers) while the B2b
    # forwarders are still in place, so this copy decides which declaration a
    # downstream consumer of THIS prefix compiles against. It has to be the one
    # whose code liblogos_core actually links -- logos-qt-host's. They differ:
    # qt-host's LogosAPI carries LOGOS_SHARED_API, which is the __declspec
    # (dllimport) that makes an in-process Windows consumer import the host's
    # TokenManager instead of linking a second one.
    #
    # Overwriting is the point, so this cp must stay after the qt-sdk one; the
    # qt-sdk copy above still supplies the headers qt-host does not ship at all
    # (logos_ui_plugin_context.h, logos_qt_lp_bridge.h, logos_qt_wire.h).
    #
    # `cp -rf` plus the chmod are load-bearing, not tidiness: everything copied
    # above came out of the nix store mode 0444, so a plain `cp -r` over it
    # fails with EACCES -- and the `|| true` these lines all carry would swallow
    # that and silently leave logos-qt-sdk's header winning. The assertion below
    # is what actually proves the overwrite happened.
    if [ -n "${toString logosQtHost}" ] && [ -d "${toString logosQtHost}/include" ]; then
      chmod -R u+w $out/include
      cp -rf ${toString logosQtHost}/include/* $out/include/

      # Fail closed if the header that ends up installed is not the one whose
      # code liblogos_core links. LOGOS_SHARED_API is present in qt-host's
      # logos_api.h and absent from qt-sdk's, so it is the discriminator.
      if ! grep -q 'LOGOS_SHARED_API' $out/include/logos_api.h; then
        echo "ERROR: $out/include/logos_api.h is not logos-qt-host's copy." >&2
        echo "       The qt-host headers did not overwrite the qt-sdk ones, so" >&2
        echo "       consumers of this prefix would compile against a different" >&2
        echo "       LogosAPI than liblogos_core links." >&2
        exit 1
      fi
    fi

    runHook postInstall
  '';
}

