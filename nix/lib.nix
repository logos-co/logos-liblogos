# Builds the logos-liblogos libraries
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-lib";
  version = common.version;
  
  inherit src;
  inherit (common) nativeBuildInputs buildInputs cmakeFlags meta env;
  
  installPhase = ''
    runHook preInstall
    
    # Install libraries
    mkdir -p $out/lib
    if [ -d lib ]; then
      cp -r lib/* $out/lib/ || true
    fi
    
    runHook postInstall
  '';
}

