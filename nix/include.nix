# Installs the logos-liblogos headers
{ pkgs, common, src, logosSdk ? null }:

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
    if [ -f interface.h ]; then
      cp interface.h $out/include/
    fi
    
    # Also copy SDK headers if available (including logos_mode.h)
    if [ -n "${toString logosSdk}" ] && [ -d "${toString logosSdk}/include" ]; then
      cp -r ${toString logosSdk}/include/* $out/include/ 2>/dev/null || true
    fi
    
    runHook postInstall
  '';
}

