# Extracts modules from external flake inputs
{ pkgs, common, capabilityModule }:

pkgs.runCommand "${common.pname}-modules-${common.version}" 
  { 
    inherit (common) meta;
  } 
  ''
    mkdir -p $out/modules
    
    if [ -d ${capabilityModule}/lib ]; then
      for lib in ${capabilityModule}/lib/*.dylib ${capabilityModule}/lib/*.so; do
        if [ -f "$lib" ]; then
          cp "$lib" $out/modules/
        fi
      done
    fi
    
    echo "Modules directory contents:"
    ls -la $out/modules/ || echo "No modules found"
  ''
