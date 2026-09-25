# Installs the logos-liblogos headers
{ pkgs, common, src, logosProtocolPkg ? null }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-headers";
  version = common.version;
  
  inherit src;
  inherit (common) meta;

  # The plain protocol C API includes nlohmann in its C++ implementation
  # headers, so propagate it for consumers that use this prefix directly.
  propagatedBuildInputs = [ pkgs.nlohmann_json ];
  
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
    
    if [ -n "${toString logosProtocolPkg}" ] && [ -d "${toString logosProtocolPkg}/include" ]; then
      cp -r ${toString logosProtocolPkg}/include/* $out/include/ 2>/dev/null || true
    fi

    runHook postInstall
  '';
}
