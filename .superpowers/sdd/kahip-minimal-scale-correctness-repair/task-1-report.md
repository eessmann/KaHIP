# Task 1 implementation report: authoritative imbalance normalization and propagation

## Implementation

Added `parallel/shared/imbalance.h`, a dependency-free normalizer for public
fractional imbalance inputs.  It rejects non-finite, negative, and
unrepresentable values; returns an unsigned effective whole percentage and a
normalization flag; preserves floor semantics for ordinary fractions; and
snaps only a uniquely identifiable widened binary32 value.  The raw double
must exactly equal `double(float(next_percent / 100))`, and that candidate
must differ from the binary32 representations of both adjacent valid integer
percentage targets.  IEEE conversion is monotone, so those checked adjacent
targets establish uniqueness across the unsigned percentage domain.  This
avoids a broad binary32 tolerance interval and preserves floor semantics for
neighbouring binary32 values, colliding large percentage origins, and genuine
binary64 inputs.

Both public boundaries now normalize exactly once:

- `ParHIPPartitionKWay` agrees on the raw double, normalizes it, computes the
  existing checked exact bound with the resulting integer percentage, and
  records that percentage in `PPartitionConfig`.
- The installed `kaffpaE` C entry point normalizes once before deriving its
  exact bound.

The private ParHIP-to-modified-KaHIP path now carries the authoritative
unsigned percentage and authoritative absolute bound directly.  It no longer
creates an `inbalance / 100.0` double only to multiply it back at the
modified-KaHIP side.  Installed C signatures remain unchanged.

Rank-zero partition-balance diagnostics now emit the raw input at
`max_digits10`, effective percentage, normalization status, total weight,
block count, configured bound, lowest-ID heaviest block, actual weight, and
excess.  The diagnostic failure probe asserts the complete deterministic
rank-zero line.

## Files changed

- `parallel/shared/imbalance.h`
- `parallel/parallel_src/interface/parhip_interface.cpp`
- `parallel/parallel_src/lib/distributed_partitioning/initial_partitioning/distributed_evolutionary_partitioning.cpp`
- `parallel/modified_kahip/interface/kaHIP_evolutionary_interface.cpp`
- `parallel/modified_kahip/interface/kaHIP_evolutionary_interface_internal.h`
- `parallel/parallel_src/tests/interface/parhip_partition_balance_test.cpp`
- `parallel/parallel_src/tests/interface/parhip_interface_mpi_test.cpp`
- `parallel/parallel_src/tests/evolutionary/evolutionary_mpi_test.cpp`
- `parallel/parallel_src/tests/evolutionary/evolutionary_failure_probe.cpp`
- `parallel/parallel_src/tests/interface/verify_parhip_interface_failure.cmake`
- `parallel/parallel_src/tests/CMakeLists.txt`
- `.superpowers/sdd/kahip-minimal-scale-correctness-repair/task-1-report.md`

## Verification record

All configure, build, and test commands were run inside the required user
systemd scope with the requested memory limits and `VCPKG_ROOT`.

### Earlier issue provenance

The following run predates the exact-origin review correction.  It documents
the original accidental-2% public API failure, but is not claimed as RED
evidence for the review correction below.

Command:

```sh
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G env VCPKG_ROOT=/var/home/erich/Projects/vcpkg ctest --test-dir out/build/ci-mpi-gcc --output-on-failure -R '^unit-parhip-interface-1-rank$'
```

Relevant output before production implementation:

```text
1/1 Test #428: unit-parhip-interface-1-rank .....***Failed
ParHIP partition balance failure: total weight=68, block count=2, raw imbalance=0.03, quantized imbalance=2%, configured bound=34, heaviest block=1, actual weight=35, excess=1
```

This records that the weighted two-vertex public fixture is
feasible at 3% (bound 35) but impossible at the accidental 2% (bound 34).

### Review-correction RED

After adding the exact-origin regression, before changing the normalizer,
this command failed as expected:

```sh
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G env VCPKG_ROOT=/var/home/erich/Projects/vcpkg ctest --test-dir out/build/ci-mpi-gcc -V -R '^unit-only the exact widened binary32 origin normalizes upward$'
```

Relevant output:

```text
CHECK( preceding_binary32->effective_percent == 2 ) with expansion: 3 == 2
CHECK( genuine_near_integer->effective_percent == 2 ) with expansion: 3 == 2
CHECK( large_percentage_ambiguity->effective_percent == 8388607 ) with expansion: 8388608 == 8388607
```

The same test also confirmed that `static_cast<double>(float{0.03F})` was
normalized to 3 before the correction, isolating the defect to the over-broad
admission interval.

### Review-correction GREEN focused checks

Command:

```sh
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G env VCPKG_ROOT=/var/home/erich/Projects/vcpkg ctest --test-dir out/build/ci-mpi-gcc --output-on-failure -R '^(unit-(only the exact widened binary32 origin normalizes upward|native three-percent imbalance keeps its whole percentage|genuine fractional percentages retain floor semantics|imbalance normalization rejects invalid and out-of-range inputs|normalized three percent keeps the checked 600-cubed bound)|unit-parhip-interface-[1-5]-rank|unit-parhip-interface-imbalanced-result-failure)$'
```

Relevant output:

```text
100% tests passed, 0 tests failed out of 11
```

The focused set covers native `0.03`, `static_cast<double>(float{0.03F})`,
the preceding binary32 value, genuine `0.029999998`, a large-percentage
half-percent ambiguity, genuine `0.025`, invalid/range and overflow boundaries, the
`600^3, k=2304, p=3` bound of `96562`, rank counts one through five, and
zero-work ranks.  The private modified-KaHIP handoff was also checked with:

```sh
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G env VCPKG_ROOT=/var/home/erich/Projects/vcpkg ctest --test-dir out/build/ci-mpi-gcc --output-on-failure -R '^(unit-evolutionary-lifetime-2-rank|unit-evolutionary-lifetime-upper-bound-narrowing-failure)$'
```

Output: `100% tests passed, 0 tests failed out of 2`.

### Review-correction 2 RED

The exact-candidate check alone did not establish a unique origin. After
adding the explicit `16000001`/`16000002` binary32-collision regression and
before adding the adjacent-target proof, this command failed as expected:

```sh
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G env VCPKG_ROOT=/var/home/erich/Projects/vcpkg ctest --test-dir out/build/ci-mpi-gcc -V -R '^unit-colliding widened binary32 origins retain historical floor semantics$'
```

Relevant output:

```text
CHECK( normalized->effective_percent == 16000001U ) with expansion: 16000002 == 16000001
CHECK_FALSE( normalized->was_normalized ) with expansion: !true
```

This exact widened candidate has two valid integer-percent origins, so it
must retain its historical floor of `16000001` rather than snap upward.

### Review-correction 2 GREEN focused checks

Command:

```sh
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G env VCPKG_ROOT=/var/home/erich/Projects/vcpkg ctest --test-dir out/build/ci-mpi-gcc --output-on-failure -R '^(unit-(colliding widened binary32 origins retain historical floor semantics|only the exact widened binary32 origin normalizes upward|native three-percent imbalance keeps its whole percentage|genuine fractional percentages retain floor semantics|imbalance normalization rejects invalid and out-of-range inputs|normalized three percent keeps the checked 600-cubed bound)|unit-parhip-interface-[1-5]-rank|unit-parhip-interface-imbalanced-result-failure)$'
```

Relevant output:

```text
100% tests passed, 0 tests failed out of 12
```

The collision regression also verifies the lowest valid percentage candidate
(`float{0.01F}`) still normalizes upward. Existing invalid/range tests cover
the upper unsigned percentage boundary.

### Final review-correction 2 non-large GCC MPI suite

The existing GCC MPI configuration was retained. Commands:

```sh
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G env VCPKG_ROOT=/var/home/erich/Projects/vcpkg cmake --build --preset build-ci-mpi-gcc
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G env VCPKG_ROOT=/var/home/erich/Projects/vcpkg ctest --preset test-ci-mpi-gcc
```

Final output:

```text
100% tests passed, 0 tests failed out of 473
Total Test time (real) = 29.30 sec
```

## Self-review

- The normalizer has no production dependency outside the standard library.
  It admits a widened binary32 candidate only if the exact candidate matches
  and neither checked adjacent valid integer percentage maps to it. Because
  the conversion is monotone, that is the required uniqueness proof; the
  checked unsigned-minimum/maximum guards avoid boundary overflow.
- The exact bound remains `floor((100 + p) * ceil(total_weight / blocks) / 100)`
  through the existing checked integer helper.
- No public C function signature changed, and no random state or draw order
  was modified.
- The new public fixture uses only the real API and real MPI collectives; its
  zero-work ranks retain a non-null optional-weight pointer so all ranks agree
  on optional input presence.
- The regressions cover the predecessor binary32 value, a genuine
  near-integer double, a large-percentage non-candidate, and the exact
  `16000001`/`16000002` colliding widened candidate. No remaining
  implementation concerns found.
