# Task 7C1 report: lazy ghost topology and distributed consistency

## Scope and contract

Task 7C1 starts from reviewed Task 7B HEAD
`497f03e7a7225dd3b458b7021cf15e5b71e9a82b`. It adds only:

- a lazily cached, graph-generation-specific ghost exchange plan;
- nonmutating local and ghost lookup helpers plus a complete graph-generation
  reset path;
- migration of the two debug distributed-consistency checks from tag-17
  point-to-point traffic to the synchronous Task 7A neighborhood collective;
  and
- the object-target factoring needed to make the owned MPI implementation
  available exactly once to each final artifact that contains
  `parallel_graph_access`.

No production ghost label/weight update was migrated. The legacy incremental
ghost-generation queues and their epoch semantics are unchanged. In
particular, `update_ghost_node_data_global()` intentionally neither consumes
nor clears labels previously queued by `setNodeLabel()`. The lifecycle check
therefore permits an inactive queue in buffer A while rejecting active MPI
requests, a selected buffer B, packed peers, a live round, or inconsistent
counter/tag state. Only graph-generation reset or graph destruction discards
an inactive queued label.

The reviewed fail-fast HPC policy is preserved. Backend, lifecycle,
state-machine, allocation, or unilateral programming failures log and abort
the affected communicator. `MPI_ERRORS_RETURN` is diagnostic-only. Common
semantic validation failures may leave a safe temporary topology scope and
throw the same structured `mpi_error` on every rank so tests can prove
symmetry. A graph that still owns a cached topology after `MPI_Finalize` raw
aborts without attempting MPI cleanup.

## TDD and systematic-debugging evidence

Both required RED stages preceded production implementation.

1. The graph-plan tests first failed to compile with
   `fatal error: communication/ghost_exchange_plan.h: No such file or
   directory`. The initial GREEN supplied graph traits, nonmutating lookup,
   graph-generation reset, lazy topology caching, authoritative neighbor
   ordering, and lifecycle probes.
2. The distributed-consistency PMPI test then ran against the legacy code and
   exited through the forbidden-protocol interposer. Its collective-path
   assertion was RED at `REQUIRE(common == 1)` with `0 == 1`, proving that
   tag 17 still used the old send/probe/receive route. The final GREEN uses one
   typed neighborhood exchange for each check.

Testing and review also exposed and closed these defects:

1. A two-rank fixture accidentally treated the same previous/next peer as two
   distinct peers. It now uses one peer with two parallel edges, proving
   per-node/per-peer deduplication without overflowing reserved graph storage.
2. Early fatal Catch assertions could have unwound one rank while a topology
   communicator was live. Tests now converge results and release/reset the
   plan collectively before fatal assertions; PMPI activation is RAII-scoped.
3. Initial graph lifecycle validation rejected an inactive incremental label
   queue after a global ghost update. That changed upstream epoch semantics.
   The global update was restored untouched and the idle predicate was refined
   to distinguish safe queued labels from active communication.
4. Application graphs in DSPAC and graph utilities could outlive
   `MPI_Finalize`. Graph owners are now scoped before finalization; DSPAC also
   has an exception-to-`MPI_Abort` executable boundary.
5. Graph storage and ghost-plan construction had several local-only failure
   edges. Node-count arithmetic, `size_t` representability, metadata coverage,
   communicator compatibility, allocation, and post-topology construction are
   now checked while the correct communicator is live.

## Implementation

`ghost_exchange_plan` is noncopyable and nonmovable and owns a Task 7A
`distributed_graph`. Its factory:

- scans the completed graph without mutating its global-to-local map;
- validates edge ranges, ghost metadata, owner ranks, map round-trips, and
  complete edge reachability of every ghost slot;
- derives sorted unique outgoing ranks and per-destination local-node lists,
  deduplicating parallel edges from one local node to the same owner;
- constructs the topology only after collective preflight;
- honors the authoritative MPI source and destination order for all stored
  segments; and
- builds sorted expected global IDs per source, requiring the graph's ghost
  relation to be symmetric before committing the cache collectively.

All potentially throwing post-topology work remains inside a catch boundary
that can abort the live topology communicator. A common semantic failure
destroys the temporary topology on every rank before throwing. Cache presence
is collectively checked before either returning the cached plan or building a
new one, and the no-throw move commit occurs only after plan readiness agrees.

`parallel_graph_access` now has out-of-line construction/destruction around the
complete plan type, deleted copy/move operations, explicit construction state,
and a reset that first requires idle legacy generation, collectively releases
the cached plan while MPI is active, deletes balance management, and clears all
generation-owned storage and counters. `start_construction()` checks `n + 1`,
`size_t` representation, and allocation/resource failure on the graph
communicator. `find_local_id()` ignores the legacy inclusive `to` field and
validates against the local count; `find_ghost_local_id()` validates the map,
node/data bounds, metadata slot, global ID, and expected owner without
insertion.

The two consistency checks share a typed `node_value` wire record with Hana
metadata and the adapter's exact-extent scoped datatype. Each call validates
communicator identity/congruence, transforms local nodes into records in the
plan's destination order, and calls `neighbor_all_to_all_v`. It first validates
all receive structure collectively—segment count, source owner, known IDs,
duplicates, and exact completeness—and only then reads ghost values and
converges equality. The check mutates neither graph values nor lookup state.
Unknown ID, wrong value, duplicate/missing ID, and wrong-source-owner injection
all fail commonly and permit a subsequent clean call using the same plan.

CMake now places `mpi_adapter.cpp`, `mpi_neighbors.cpp`, `mpi_failure.cpp`, and
`ghost_exchange_plan.cpp` in `parhip_mpi_obj` with a private communication
header file set and target-based fmt, spdlog, Boost.Hana, Boost.MP11, and MPI
requirements. `parallel`, `libedgelist`, `graph2binary`,
`graph2binary_external`, and `readbgf` receive that object implementation once;
DSPAC, toolbox, ParHIP, and the installed interfaces obtain it transitively
from `parallel`.

## Verification

Every shell, configure, build, test, oracle, and Git command used the requested
`systemd-run --user --scope` limits (`MemoryHigh=28G`, `MemoryMax=30G`,
`MemorySwapMax=2G`). Builds used at most two jobs.

The verification host used GCC 16.2.1, CMake 4.3.0, and Open MPI 5.0.10
(advertising MPI 3.1).

- Final focused rank/lifecycle matrix: **14/14 passed** in 1.02 seconds. It
  covers plan/lookup/reset behavior at ranks 1 through 5, migrated consistency
  at ranks 1 through 5, no-plan post-finalize success, cached-plan
  post-finalize raw abort, active-destructor fail-fast, and active-reset
  fail-fast.
- Trace-ON local GCC debug configure and complete build: passed.
- Trace-ON local GCC debug full CTest: **129/129 passed** in 9.44 seconds.
- Default trace-OFF local GCC release configure and complete build: passed.
  The build included `parallel`, `libedgelist`, `graph2binary`,
  `graph2binary_external`, `readbgf`, `edge_list_to_metis_graph`, and `dspac`.
  Ninja reported a recoverable premature-EOF log warning and then completed
  successfully.
- Default trace-OFF local GCC release full CTest: **124/124 passed** in 6.21
  seconds.
- A two-rank debug DSPAC run on `examples/rgg_n_2_15_s0.bgf`, seed 1, k 2,
  `ultrafastsocial`, completed with edge cut 260, balance 1.01672, and vertex
  cut 124.
- Both generated capability headers report
  `KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C=0`,
  `KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV=1`,
  `KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C=0`,
  `KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT=1`, and
  `KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C=0`. The PMPI tests therefore prove
  the classic synchronous call on this MPI-3.1 host and contain guarded,
  independently counted `_c`, immediate, and persistent interposers for an
  MPI-4 build.
- The production audit of the migrated files found no tag 17 and no blocking,
  nonblocking, matched-probe, or send/receive point-to-point API. The only new
  production collective call site is the shared consistency helper used by
  `check_labels()` and `check()`.
- `clang-format --dry-run --Werror` passed for all six new C++ sources, and
  `git diff --check` passed.

### Exact deterministic oracle

The final trace-ON debug executable was run with exactly
`examples/rgg_n_2_15_s0.graph`, 2 ranks, k 2, `ultrafastmesh`, and seed 0.
The result retained edge cut 196 and balance 1.0094. The 32,768-line partition
SHA-256 is exactly:

```text
a600acd0029ee9342e4f7c5b041d224a308b874c85fd35bdbcd3a5a73d48cdd0
```

After removing the one v3 header from each rank and globally sorting complete
canonical records, the aggregate contains exactly 436,721 records and has
SHA-256:

```text
a179bb30213dbb26638657a1d611e951a8bf900b817647fc03d4c700a83f0a18
```

Both values exactly match the pinned pristine-upstream oracle.

## Independent review and deliberate deferrals

An independent final source and report audit after the queue-parity correction
returned **ACCEPT** with no Critical, Important, or Minor findings. It found
the prior plan/reset, consistency, DSPAC lifecycle, exception-boundary,
catch-allocation, PMPI breadth, CMake linkage, and inactive-queue concerns
closed, and independently checked the exact oracle evidence. The reviewer did
not rerun commands under its read-only remit.

Deliberately deferred:

- migration of production ghost label/weight exchanges;
- nonblocking or persistent production contexts for graph state;
- local execution of the MPI-4 `_c` path, whose bindings are absent from this
  host; and
- any rewrite of the semantically asynchronous evolutionary gossip paths.

The existing incremental ghost epoch behavior and the two evolutionary
`exchanger.cpp` point-to-point protocols remain unchanged.
