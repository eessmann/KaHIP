# Task 7A report: distributed-graph neighborhood foundation

## Scope and contract

Task 7A starts from reviewed Task 6 HEAD
`9564c410336cbb665aa84a34e42532ef4e121218`. It adds only:

- an explicitly owning `distributed_graph` communicator;
- synchronous typed `neighbor_all_to_all_v` transport;
- a configure/link capability probe for
  `MPI_Neighbor_alltoallv_c`.

No production ghost, consistency, DSPAC, root-gather, or evolutionary-gossip
call site was migrated. Nonblocking and persistent neighborhood contexts remain
Task 7B/7C work.

The implementation retains the reviewed fail-fast HPC policy. Internal
communicators install `MPI_ERRORS_RETURN` for diagnostics only. MPI/backend,
resource, or unilateral local failures log and call `MPI_Abort` while the
affected communicator is valid. Only collectively completed semantic
validation may leave its internal communicator scope normally and then throw a
common structured error; production boundaries still terminate rather than
returning partial state.

## TDD and debugging evidence

The focused tests were written before the implementation. The genuine RED
build failed because `parhip::mpi::distributed_graph` and
`neighbor_all_to_all_v` did not exist. After the first implementation compiled,
runtime testing found three substantive defects:

1. Open MPI 5.0.10 rejected a null destination-array pointer for a valid
   zero-degree `MPI_Dist_graph_create` call. The constructor now supplies a
   valid ignored pointer when the total local degree is zero.
2. Receive storage size was computed in the same function call that moved the
   count and offset vectors. Unspecified argument-evaluation order allowed the
   vectors to move first, allocating zero bytes for nonempty metadata. The
   storage size is now computed in a named value before either move.
3. A reviewer-driven product-only overflow regression was made RED by
   temporarily disabling the `element_count * sizeof(T)` representability
   predicate: the rank-one test reached the impossible allocation and aborted.
   Restoring the collective pre-allocation predicate made the complete rank
   matrix green.

The final focused rank matrix is **5/5 passed** at ranks 1, 2, 3, 4, and 5.
It covers zero-degree, self, directed ring, both star directions, isolated and
zero-work ranks, sorted/unique normalization, authoritative MPI query order,
degree-sized rank lookup, move-only handle transfer state, typed and uneven
payloads, forced MPI-3 ceiling-two rounds, malformed layouts, option
disagreement, zero ceiling,
receive-offset and receive-byte-product overflow, and capability readback.

The PMPI protocol probe requires exactly one neighbor-count exchange, no P2P
send/receive/probe/wait route, and for bounded rounds: identical exact payload
call counts on every rank, at most one active outgoing and incoming segment,
counts no larger than two, and zero displacements. The injected receive-count
overflow cases are collectively rejected after the count exchange and before
allocation or any payload call. The product-only overflow is injected on rank
zero alone, so ranks 2--5 also prove common error convergence.

The PMPI interposition is deliberately payload-focused. Sorted/unique topology
normalization, `reorder=false`, queried topology order, and move transfer are
verified through topology state and API readback rather than claimed as PMPI
argument or exact successful-free counters. Backend create/query/free behavior
is covered separately by the fail-fast death probes below.

Representative injected backend failures in graph creation, neighbor query,
and graph free are **3/3 passed**. The probes require the critical diagnostic
and observe `MPI_Abort` on the live internal/graph communicator, or on
`MPI_COMM_WORLD` after a failed free where the original handle may be stale.

## Implementation

`distributed_graph`:

- collectively validates outgoing ranks and checked `int` degree
  representability on an internal duplicate;
- sorts and deduplicates the local destination list;
- calls `MPI_Dist_graph_create` with `reorder=false` from an
  `MPI_ERRORS_RETURN` duplicate and adopts the returned communicator;
- verifies `MPI_DIST_GRAPH`, queries source/destination order from MPI, and
  preserves that order exactly;
- maintains sorted degree-sized `{rank,index}` lookups rather than an O(P)
  rank table;
- preserves the caller communicator's error handler and owns no process-lifetime
  handle.

`neighbor_all_to_all_v`:

- validates canonical send layout and common options before the count
  collective;
- exchanges `uint64_t` counts in authoritative neighbor order with
  `MPI_Neighbor_alltoall`, validates checked canonical receive offsets
  collectively, then allocates contiguous typed storage;
- keeps the scoped MP11/Hana-derived datatype alive through the payload;
- uses `MPI_Count`/`MPI_Aint` and `MPI_Neighbor_alltoallv_c` only when the real
  generated capability is present;
- otherwise narrows only checked-representable MPI-3 layouts;
- uses deterministic global cyclic rank phases for oversized MPI-3 layouts.
  Every rank enters the same phase count and globally reduced round count,
  activates at most one queried outgoing and incoming edge, advances base
  pointers with checked arithmetic, and uses zero displacements.

Self, asymmetric, source-only, destination-only, isolated, and zero-degree
topologies use the same collective path without sentinels.

## Verification

Every shell, configure, build, test, and Git command used the required
`systemd-run --user --scope` limits (`MemoryHigh=28G`, `MemoryMax=30G`,
`MemorySwapMax=2G`). Builds used at most two jobs.

- Trace-ON GCC debug configure and full build: passed.
- Trace-ON GCC debug full CTest: **90/90 passed**.
- Default trace-OFF GCC release configure and full build: passed. Ninja emitted
  its known recoverable premature-EOF regeneration warning; compilation and
  linking completed.
- Default trace-OFF GCC release full CTest: **85/85 passed**.
- Focused adapter ranks 1--5: **5/5 passed**.
- Focused create/query/free fail-fast probes: **3/3 passed**.
- Both generated capability headers report:
  `KAHIP_HAVE_MPI_ALLTOALLV_C 0` and
  `KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C 0` for local Open MPI 5.0.10. The
  configure check is compiled and linked with `MPI::MPI_CXX`; guarded tests
  verify generated readback. A real MPI-4 binding remains required in CI to
  execute the `_c` branch.
- The debug compile command confirms `mpi_neighbors.cpp` is built with
  `-std=c++23`.
- The production P2P audit finds no `MPI_Isend`, `MPI_Send`, `MPI_Irecv`,
  `MPI_Recv`, probe, send-receive, or wait call in the neighborhood adapter.
- The production API search finds `distributed_graph` and
  `neighbor_all_to_all_v` only in `mpi_neighbors.h/.cpp`; no algorithm call
  site changed. Therefore the exact Task 6 partition/trace oracle was not
  rerun, as permitted by the Task 7A brief.
- `git diff --check`: passed before commit.

An independent algorithm/lifecycle review found no remaining correctness issue
in topology ownership, authoritative ordering, collective preflight,
zero-degree handling, or the cyclic MPI-3 fallback. Its header-hygiene finding
was closed by directly including `<utility>` for `std::in_range`.

## Deliberate deferrals

- Production ghost and distributed-consistency migrations.
- Nonblocking neighborhood operations.
- Persistent neighborhood contexts and their invariant-layout policy.
- MPI-4 `_c` runtime execution on this host, whose installed headers do not
  expose the function.

These are later slices; no workaround or emulated capability was added.
