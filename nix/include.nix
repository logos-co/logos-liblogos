# Installs the logos-liblogos headers
{ pkgs, common, src }:

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
    
    runHook postInstall
  '';
}

