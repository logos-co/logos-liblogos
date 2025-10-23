# Builds the logos-liblogos binary
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-bin";
  version = common.version;
  
  inherit src;
  inherit (common) nativeBuildInputs buildInputs cmakeFlags meta env;
  
  installPhase = ''
    runHook preInstall
    
    # Install binaries
    mkdir -p $out/bin
    if [ -d bin ]; then
      cp -r bin/* $out/bin/ || true
    fi
    
    runHook postInstall
  '';
}

