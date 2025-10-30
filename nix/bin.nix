# Extracts binaries from the shared build
{ pkgs, common, build, lib }:

pkgs.runCommand "${common.pname}-bin-${common.version}" 
  { 
    inherit (common) meta;
    nativeBuildInputs = pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ];
  } 
  ''
    # Copy binaries from the shared build
    mkdir -p $out/bin
    if [ -d ${build}/bin ]; then
      cp -r ${build}/bin/* $out/bin/
    fi
    
    # Also include libraries in lib path for runtime linking
    mkdir -p $out/lib
    if [ -d ${lib}/lib ]; then
      cp -r ${lib}/lib/* $out/lib/
    fi
    
    # Fix rpaths for all binaries to find the libraries
    for binary in $out/bin/*; do
      if [ -f "$binary" ] && [ -x "$binary" ]; then
        :
        ${pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
          # On macOS, use install_name_tool to fix the library paths
          for dylib in $out/lib/*.dylib; do
            if [ -f "$dylib" ]; then
              libname=$(basename $dylib)
              install_name_tool -change "@rpath/$libname" "$out/lib/$libname" "$binary" 2>/dev/null || true
            fi
          done
        ''}
      fi
    done
  ''

