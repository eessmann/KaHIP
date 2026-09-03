{ pkgs, multiverse, ... }:

{
  packages = [
    pkgs.cmake
    pkgs.ninja
    pkgs.pkg-config
    pkgs.catch2_3
    pkgs.llvmPackages.clang
    # Keep the MPI runtime stable independently of the rolling toolchain.
    multiverse.mpich."4.3.2"
  ];

  languages.cplusplus = {
    enable = true;
    lsp.enable = false;
  };

  tasks = {
    "kahip:configure".exec =
      "cmake --fresh --preset unix-clang-release -DNONATIVEOPTIMIZATIONS=ON";
    "kahip:build" = {
      exec = "cmake --build --preset build-unix-clang-release";
      after = [ "kahip:configure" ];
    };
    "kahip:test" = {
      exec = "ctest --preset test-unix-clang-release";
      after = [ "kahip:build" ];
    };
  };
}
