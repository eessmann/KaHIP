# Task 3 implementation report

## Delivered

- Added Unix target `parhip_cube_scale_probe` at
  `out/build/<preset>/parallel/parallel_src/tests/parhip_cube_scale_probe`, with
  CLI `--side <n> [--expected-ranks <ranks>]`. It links only
  `parhip_interface`, calls unchanged public `ParHIPPartitionKWay` with `k=P`,
  seed 2022, `PARHIP_FASTSOCIAL`, suppressed output, null unit weights, and
  widened `float{0.03F}`.
- Added checked cube counts, independent exact 3% bound, overflow-safe
  `floor(i*N/P)` boundaries, sorted allocation-free neighbors, and local-only
  two-pass CSR generation. Tests include overflow, `N<P`, all requested scale
  literals, side-3 corner/edge/face/interior neighbors, and exact side-4 CSR
  count/slices.
- Added a lockstep 65,536-source-window cut protocol through the existing
  `all_to_all_v` adapter. It agrees inputs/boundaries before rounds, validates
  full `(source,target)` request/reply keys collectively before indexing,
  handles empty/self/zero-work ranks, proves `3W`/`3*local_nodes` and all
  scalar/offset/byte/vector capacities, and accumulates a checked `uint64_t`
  cut before comparison with the C `int` result.
- Captures exactly two ordered, full 17-field gather-boundary profiles through
  the private observer in the same library image. The ordered JSON record
  reports explicit pass/provenance/recipes, raw bits
  `3f9eb851e0000000`, raw `0.029999999329447746`, effective 3%, both profiles,
  selected maximum-memory profile, counts/bound/cuts/digests/timings, and
  checked maximum RSS. Record construction/write is collective-failure-safe.
- Exact scale count/bound tuples are: `600/2304 ->
  216000000,646920000,1293840000,93750,96562`; `755/4608 ->
  430368875,1289396550,2578793100,93397,96198`; `900/7776 ->
  729000000,2184570000,4369140000,93750,96562`; `1008/10944 ->
  1024192512,3069529344,6139058688,93585,96392` (nodes, undirected edges,
  directed edges, maximum local nodes, exact bound).

## Digest v1 provenance

`semantic-splitmix64-xor-v1` uses version 1; golden
`9e3779b97f4a7c15`; seeds `243f6a8885a308d3`, `13198a2e03707344`,
`a4093822299f31d0`, `082efa98ec4e6c89`; domains vertex
`637562655f767478`, arc `637562655f617263`, partition
`706172745f6d6170`, profile `70726f66696c655f`; and SplitMix multipliers
`bf58476d1ce4e5b9`, `94d049bb133111eb`. For one-based fields, each lane starts
`SM(seed xor domain xor golden*version)` and folds
`SM(previous xor SM(field + golden*ordinal))`, modulo `2^64`. Records are
vertex `(side,gid,degree,node_weight=1)`, arc
`(side,source,sorted_ordinal,target,edge_weight=1)`, partition
`(side,gid,label,blocks)`, and profile `(sequence_index,17 fields)`. Record
lanes combine by checked `MPI_BXOR`; digests are provenance, not proof.

Independent Python derivation (temporary script SHA-256
`ec4d9ef21f9a1509b45f84fa5e4fcc13ae791be78bfced3755c196a08f7e8308`) ran
under the required wrapper as scope `run-p369148-i408907.scope`:

```text
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G python3 /tmp/kahip-task3-digest-v1.py
```

It derived graph side-2
`ef5146193e8c0fef,27c7c004c7c7a159,2ec68fd76213931e,97e4152d3243b55e`,
side-4 `f12b9a02fe2a75b7,e6a832aefcaa2a99,dd15134b3a2a0e43,30789a9a367d34f9`,
side-10 `73e09509b201c17a,402fd2d71ea65a5b,d002e5024a1072db,bcc02b4ee29b27bd`,
and side-2 partition `00001111`
`cfab14550a55a03f,6c9ae38680f7894a,509848c142fe96db,ec6f053ec2141019`.
C++ tests prove exact literals and decomposition equivalence.

## RED/GREEN evidence

- RED core build `run-p342869-i379680.scope`: missing
  `cube_scale_probe_core.h`. RED protocol build
  `run-p344367-i386672.scope`: missing `cube_scale_probe_protocol.h`. RED probe
  configure `run-p346457-i380000.scope`: missing probe source/unknown linker
  language. Implementation followed those failures.
- GCC configure `run-p358855-i408048.scope` and full build
  `run-p359117-i372666.scope` passed. Core unit
  `run-p359254-i408074.scope` passed 129 assertions/7 cases. Focused CTest
  `run-p359322-i399695.scope` passed 5/5: one-/three-rank protocol, CLI, and
  side-4/two-rank plus side-10/five-rank public-call probes.
- Full GCC non-large/non-performance suite
  `run-p363863-i382815.scope` ran
  `ctest --test-dir out/build/ci-mpi-gcc --quiet --output-on-failure -LE
  'large|performance'` and exited 0 in 30.895 s; quiet mode emitted no result
  lines. Matching scoped enumeration `run-p368085-i414802.scope` counted 515
  tests.
- Clang configure `run-p368176-i414815.scope` and focused build
  `run-p368346-i414834.scope` passed. Core
  `run-p368628-i395362.scope` passed 129 assertions/7 cases; focused CTest
  `run-p368692-i410082.scope` passed 5/5 in 0.81 s.
- Independent final static review found no critical, important, or minor
  findings. All configure/build/test/generator commands used the mandated
  `systemd-run` memory scope and vcpkg root where applicable.

## Limitation

The four requested 600/755/900/1008 tests are registered only as
`large;scale;mpi`, with `PROCESSORS`, `RUN_SERIAL`, and 7,200-second timeouts.
They were enumerated but intentionally not executed locally. Actual site-scale
runtime/RSS and partition digests remain Task 4 acceptance evidence. The probe
creates no graph, partition, or other output files.
