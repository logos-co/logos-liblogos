# Extracts binaries from the shared build
{ pkgs, common, build, lib, modules }:

pkgs.stdenvNoCC.mkDerivation {
  pname = "${common.pname}-bin";
  version = common.version;
  
  # No source to unpack - we're copying from another derivation
  dontUnpack = true;
  
  nativeBuildInputs = 
    [ pkgs.qt6.wrapQtAppsNoGuiHook ] ++  # Wrap CLI apps to be immune to LD_LIBRARY_PATH pollution
    pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ] ++
    pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.autoPatchelfHook ];
  
  # Clear LD_LIBRARY_PATH to prevent external Qt installations from interfering
  qtWrapperArgs = [ "--unset LD_LIBRARY_PATH" ];
  
  # Required for autoPatchelfHook (Linux) and wrapQtAppsNoGuiHook (both platforms)
  buildInputs = common.buildInputs;
  
  installPhase = ''
    runHook preInstall
    
    # Copy binaries from the shared build
    mkdir -p $out/bin
    if [ -d ${build}/bin ]; then
      cp -r ${build}/bin/* $out/bin/
      chmod -R +w $out/bin  # Make writable so autoPatchelf can modify
    fi
    
    # Also include libraries in lib path for runtime linking
    mkdir -p $out/lib
    if [ -d ${lib}/lib ]; then
      cp -r ${lib}/lib/* $out/lib/
      chmod -R +w $out/lib  # Make writable if needed
    fi
    
    # Copy modules from the modules derivation
    mkdir -p $out/modules
    if [ -d ${modules}/modules ]; then
      cp -r ${modules}/modules/* $out/modules/
    fi
    
    runHook postInstall
  '';
  
  # macOS-specific fixup (autoPatchelfHook handles Linux automatically)
  postFixup = pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
    # On macOS, use install_name_tool to fix the library paths
    for binary in $out/bin/*; do
      if [ -f "$binary" ] && [ -x "$binary" ]; then
        for dylib in $out/lib/*.dylib; do
          if [ -f "$dylib" ]; then
            libname=$(basename $dylib)
            install_name_tool -change "@rpath/$libname" "$out/lib/$libname" "$binary" 2>/dev/null || true
          fi
        done
      fi
    done
  '';
  
  inherit (common) meta;
}
