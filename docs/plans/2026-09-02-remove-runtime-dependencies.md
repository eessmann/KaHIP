# KaHIP dependency-removal implementation plan

## Goal

Remove KaHIP's production and installed-consumer dependencies on fmt, spdlog,
Boost.Hana, and Boost.MP11 while preserving the explicit MPI wire schema and
fatal-abort behavior. Replace vcpkg as the documented local development path
with a locked devenv environment. Keep CMake 4 as the project baseline and
verify the MPI build with the existing Cirrus toolchains without installing
anything on the cluster.

## Binding constraints

- Work in the existing `mpi-collective` checkout and preserve unrelated dirty
  changes; do not create a worktree or commit implicitly.
- Keep installed C interfaces and public headers unchanged.
- Use standard streams and small stream helpers; do not use `std::format`,
  `std::print`, or standard range formatting.
- Keep MPI metadata explicit and private. Do not vendor Cista or qlibs/reflect.
- The private fatal sink is process-global and non-atomic. It must flush before
  the existing abort path and must not turn formatting failures into a new
  failure path.
- Retain the existing fatal-path probes; do not add dedicated logging unit tests.
- Require CMake 4.0 or newer. Keep preset schema version 9.
- Local development uses devenv. Existing CI is out of scope and may retain its
  vcpkg bootstrap path for Catch2.
- On Cirrus, operate only below
  `/work/e609/e609/eriche609/KaHIP`, use existing modules, and install or
  download nothing.

## Task 1: Reconcile the in-flight implementation with the specification

Inspect the complete dirty diff and run source/build-manifest scans for the four
removed libraries. Verify that no production source, CMake target, package
lookup, installed link interface, or diagnostic test marker still depends on
them. Treat upstream text under vendored `extern/` directories separately from
KaHIP-owned dependency declarations.

## Task 2: Complete MPI metadata replacement and focused coverage

Keep the native MPI type map in a `std::tuple` with fold-based membership and a
local tuple-index trait. Keep `wire_members<T>` as explicit member-pointer tuples
iterated with `std::apply`. Verify native-handle alignment, member ordering,
unsupported member rejection, non-default-constructible records, actual-object
address calculation, and `extent == sizeof(T)`.

## Task 3: Complete fatal diagnostics and stream formatting replacement

Use the private header-only fatal sink and standard streams throughout. Preserve
the exact payload, one trailing newline, and fallback behavior; use the existing
MPI abort probes to verify the payload and flush ordering. Retain the PMPI
callback-safety scan for the new helper. Verify range joining, CLI and fixture
output, and `mpi_error::what()`.

## Task 4: Restore CMake 4 and add the local devenv environment

Restore all KaHIP-owned minimum-version declarations and documentation to CMake
4.0+, leaving preset schema version 9. Remove the vcpkg toolchain from ordinary
local presets. Add `devenv.yaml`, `devenv.nix`, and a committed lockfile that
provide CMake 4+, Ninja, pkg-config, Catch2 3, and MPI on supported local hosts.
Pin the local MPI version through nixpkgs-multiverse, with its input locked
alongside the base toolchain.
Expose small configure/build/test tasks without changing KaHIP's installed
interfaces. Keep CI-only vcpkg files unchanged.

## Task 5: Verify local dependency closure

From the devenv shell, configure and build serial and MPI release trees, run the
focused/unit tests available on the host, and stage an install. Build the
pkg-config/static consumer without manually linking any of the four removed
libraries. Run a final KaHIP-owned source and generated-build scan.

## Task 6: Verify Cirrus compiler compatibility

Without modifying the remote checkout until the local implementation is ready,
use CMake 4.1.2 and the Cray compiler wrappers with:

- `PrgEnv-gnu` / GCC 14.2 / Cray MPICH; and
- `PrgEnv-cray` / CCE (Cray Clang) 19.0 / Cray MPICH.

Keep configure, build, and test output under `out/build/cirrus-*` in the allowed
KaHIP directory. Do not install packages or write outside that directory. Record
any cluster-runtime limitation separately from source or compile failures.

## Completion evidence

- Clean KaHIP-owned scan for fmt, spdlog, Hana, and MP11 dependencies and old
  marker names.
- Successful CMake 4 serial and MPI compilation with the available local
  toolchains.
- Passing focused metadata, diagnostics, CLI/fixture, and install-consumer tests.
- Devenv evaluation plus locked package/version evidence.
- Successful Cirrus GCC 14 and Cray Clang 19 configure/build checks, or exact
  compiler diagnostics for any remaining source incompatibility.
