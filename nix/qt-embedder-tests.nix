# tests/qt_embedder, built against the installed package as an app would be.
{ pkgs, src, liblogos, logosProtocolQt, logosQtHost }:

pkgs.stdenv.mkDerivation {
  pname = "logos-liblogos-qt-embedder-tests";
  version = "0.1.0";
  inherit src;

  nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
  buildInputs = [
    pkgs.qt6.qtbase
    pkgs.qt6.qtremoteobjects
    pkgs.gtest
    pkgs.boost
    pkgs.openssl
    pkgs.nlohmann_json
    logosProtocolQt
    logosQtHost
  ];
  dontWrapQtApps = true;

  cmakeFlags = [
    "-DLOGOS_LIBLOGOS_ROOT=${liblogos}"
    "-DLOGOS_PROTOCOL_ROOT=${logosProtocolQt}"
    "-DLOGOS_QT_HOST_ROOT=${logosQtHost}"
  ];

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp qt_embedder_tests $out/bin/
    runHook postInstall
  '';
}
