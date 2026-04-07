# Static archive for Boost.Process v2 symbols omitted from nixpkgs' trimmed Boost.
# Sources: https://github.com/boostorg/process (tag must match pkgs.boost.version).
{ lib
, stdenv
, boost
, fetchFromGitHub
}:

let
  tag = "boost-${boost.version}";
  # When nixpkgs bumps Boost, add the new tag here (nix build will suggest the correct hash).
  srcHash = {
    "boost-1.87.0" = "sha256-CM4tkQ+YOSf36ELec/ZZtMNUen6aA+lcuZy7lcdOSAo=";
  }.${tag} or null;
in

if srcHash == null then
  throw ''
    logos-liblogos: unsupported Boost ${boost.version} for Boost.Process v2 fetch.
    Update nix/boost-process-v2-impl.nix: add fetchFromGitHub hash for boostorg/process tag ${tag}.
  ''
else
stdenv.mkDerivation {
  pname = "logos-boost-process-v2-impl";
  version = boost.version;

  src = fetchFromGitHub {
    owner = "boostorg";
    repo = "process";
    rev = tag;
    hash = srcHash;
  };

  nativeBuildInputs = [ boost ];

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    mkdir -p objs
    inc="-isystem ${lib.getDev boost}/include"
    for rel in \
      src/error.cpp \
      src/detail/last_error.cpp \
      src/detail/throw_error.cpp \
      src/posix/close_handles.cpp
    do
      base="''${rel##*/}"
      base="''${base%.cpp}"
      $CXX -std=c++17 -O2 -fPIC $inc -c "$rel" -o "objs/$base.o"
    done
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/lib
    ''${AR:-ar} rcs $out/lib/libboost_process_v2_impl.a objs/*.o
    runHook postInstall
  '';

  meta = with lib; {
    description = "Boost.Process v2 compiled stubs for logos-liblogos (nixpkgs Boost has no libboost_process)";
    license = licenses.boost;
  };
}
