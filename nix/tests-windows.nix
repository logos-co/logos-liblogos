# Cross-builds logos_core_tests for Windows, with everything the flake check
# hands the suite on Unix: the real host, the bundled modules and the fixtures.
# Windows CI runs it from the manifest installed beside it.
{ pkgs, common, src, bin }:

let
  host = "{target}/host/logos_host_qt.exe";
  manifest = builtins.toFile "liblogos-tests.json" (builtins.toJSON {
    suites = [{
      name = "logos_core";
      exe = "bin/logos_core_tests.exe";
      timeout = 120;
      env = {
        TEST_PLUGIN = "{target}/modules/capability_module/capability_module_plugin.dll";
        TEST_PLUGIN_DEP_RANGE = "{target}/lib/dep_range_fixture_plugin.fixture";
        TEST_PLUGIN_DEP_MALFORMED = "{target}/lib/dep_malformed_fixture_plugin.fixture";
        TEST_BUNDLED_MODULES_DIR = "{target}/modules";
        TEST_REAL_HOST = host;
        LOGOS_HOST_PATH = host;
        LOGOS_REQUIRE_TEST_FIXTURES = "1";
      };
      # Copies of the hosts that some tests make elsewhere find their DLLs here.
      path = [ "{exe_dir}" "{target}/host" ];
    }];
  });
in
pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit src;
  inherit (common) nativeBuildInputs buildInputs meta env;

  cmakeFlags = common.cmakeFlags ++ [ "-DLOGOS_BUILD_TESTS=ON" ];
  ninjaFlags = [ "logos_core_tests" "logos_fake_module_host" ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/lib $out/host $out/modules $out/share/logos-tests
    # Their DLLs are linked in beside them by the mingw fixup hook.
    cp bin/logos_core_tests.exe bin/logos_fake_module_host.exe $out/bin/
    cp lib/*_fixture_plugin.fixture lib/*_fixture_plugin.metadata.json $out/lib/
    # The host and bundled modules as the package ships them, DLLs included.
    cp -rL ${bin}/bin/. $out/host/
    cp -rL ${bin}/modules/. $out/modules/
    chmod -R u+w $out/host $out/modules
    cp ${manifest} $out/share/logos-tests/liblogos.json
    runHook postInstall
  '';
}
