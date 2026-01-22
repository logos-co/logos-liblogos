# Builds tests
{ pkgs, common, build }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;
  
  inherit (build) src;
  inherit (common) buildInputs meta env;
  
  # Add platform-specific tools conditionally
  nativeBuildInputs = common.nativeBuildInputs 
    ++ pkgs.lib.optionals pkgs.stdenv.isDarwin [ pkgs.darwin.cctools ]
    ++ pkgs.lib.optionals pkgs.stdenv.isLinux [ pkgs.patchelf ];
  
  # Use the same CMake flags as the main build
  cmakeFlags = common.cmakeFlags;
  
  # Configure phase - reuse build outputs
  configurePhase = ''
    runHook preConfigure
    
    # Copy the built artifacts from the main build
    cp -r ${build}/* .
    chmod -R u+w .
    
    # Reconfigure to generate test targets
    cmake -B build -S ${build.src} \
      -GNinja \
      -DLOGOS_CPP_SDK_ROOT=${common.env.LOGOS_CPP_SDK_ROOT} \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$out
    
    runHook postConfigure
  '';
  
  # Build phase - only build the test executable
  buildPhase = ''
    runHook preBuild
    
    cd build
    ninja logos_core_tests
    
    runHook postBuild
  '';
  
  # Install phase - install the test executable
  installPhase = ''
    runHook preInstall
    
    mkdir -p $out/bin
    cp bin/logos_core_tests $out/bin/
    
    # Copy the libraries so tests can run
    mkdir -p $out/lib
    cp -r lib/* $out/lib/ || true
    
    ${pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
      # Fix RPATH to find libraries in $out/lib on macOS
      install_name_tool \
        -change @rpath/liblogos_core.dylib $out/lib/liblogos_core.dylib \
        $out/bin/logos_core_tests || true
    ''}
    
    ${pkgs.lib.optionalString pkgs.stdenv.isLinux ''
      # Fix RPATH on Linux to avoid /build/ references and include all dependencies
      patchelf --set-rpath "$out/lib:${pkgs.gtest}/lib:${pkgs.qt6.qtbase}/lib:${pkgs.qt6.qtremoteobjects}/lib:${pkgs.stdenv.cc.cc.lib}/lib" $out/bin/logos_core_tests || true
    ''}
    
    runHook postInstall
  '';
}
