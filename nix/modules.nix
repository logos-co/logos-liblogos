# Bundles modules from external flake inputs into the logoscore modules directory.
# logoscore expects: modules/<name>/manifest.json + <name>_plugin.{so,dylib}
# When portableBuild is false (default/dev), manifest keys get a "-dev" suffix
# to match the dev variant lookup in platformVariantsToTry().
{ pkgs, common, capabilityModule, portableBuild ? false }:

pkgs.runCommand "${common.pname}-modules-${common.version}"
  {
    inherit (common) meta;
  }
  ''
    mkdir -p $out/modules/capability_module

    # Copy the plugin library
    if [ -d ${capabilityModule}/lib ]; then
      for lib in ${capabilityModule}/lib/*.dylib ${capabilityModule}/lib/*.so; do
        if [ -f "$lib" ]; then
          cp "$lib" $out/modules/capability_module/
        fi
      done
    fi

    # Determine the plugin filename that was copied
    pluginFile=""
    for f in $out/modules/capability_module/*; do
      if [ -f "$f" ]; then
        pluginFile="$(basename "$f")"
        break
      fi
    done

    if [ -z "$pluginFile" ]; then
      echo "Error: No capability_module library found"
      exit 1
    fi

    # Determine platform variant keys
    platform=""
    arch=""
    case "$(uname -s)" in
      Linux)  platform="linux" ;;
      Darwin) platform="darwin" ;;
    esac
    case "$(uname -m)" in
      x86_64)       arch="x86_64" ;;
      aarch64|arm64) arch="aarch64" ;;
    esac

    # Dev builds use "-dev" suffixed variant keys to match platformVariantsToTry()
    suffix="${if portableBuild then "" else "-dev"}"

    # Create manifest.json for plugin discovery
    cat > $out/modules/capability_module/manifest.json <<EOF
    {
      "name": "capability_module",
      "version": "1.0.0",
      "main": {
        "$platform-$arch$suffix": "$pluginFile",
        "$platform-amd64$suffix": "$pluginFile",
        "$platform-arm64$suffix": "$pluginFile",
        "$platform-x86_64$suffix": "$pluginFile",
        "$platform-aarch64$suffix": "$pluginFile"
      }
    }
    EOF

    echo "Modules directory contents:"
    ls -laR $out/modules/
  ''
