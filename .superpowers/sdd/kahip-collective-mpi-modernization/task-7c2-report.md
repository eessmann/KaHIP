# Task 7C2 report: transactional contraction ghost CNodes

## Result and scope

Task 7C2 is complete against base
`53d63985ab514533953b3b3302b64a22543d63b9`. The tag-6 contraction ghost
CNode request/reply protocol now uses the cached graph-generation topology and
one synchronous neighborhood exchange. The local CNode table, every ghost
CNode, the sentinel slot, and contraction-label trace emission are one
validate-then-commit transaction.

This change deliberately does not migrate tag 9, alter `m_send_buffers`, add a
new asynchronous context, or change any other production ghost exchange.

## Production design

### Explicit wire record and authoritative topology order

`contraction::ghost_cnode_assignment` is a standard-layout,
trivially-copyable two-field record:

- the fine global vertex ID; and
- its coarse global vertex ID.

Its Boost.Hana member metadata feeds the adapter's scoped resized MPI datatype.
The test suite requires an extent of exactly `sizeof(ghost_cnode_assignment)`.

The migrated helper validates that its caller communicator is `MPI_IDENT` or
`MPI_CONGRUENT` with the graph communicator. It collectively agrees the fine
global count and coarse domain before acquiring the cached ghost plan. Send
segments are built in the MPI-queried destination order from
`outgoing_local_nodes()`, without sorting or deduplicating coarse IDs. Receive
segments are interpreted in the MPI-queried source order.

The payload path performs exactly one `neighbor_all_to_all_v`. On this MPI-3.1
host the independently linked MPI-4 `_c` probe is false, so the classic
`MPI_Neighbor_alltoallv` path is selected. The adapter retains the guarded
MPI-4 `_c` implementation and the test interposer counts it independently.

### Whole-table transaction

The caller no longer allocates or mutates `m_nodes_to_cnode` and no longer
emits local contraction-label traces before the exchange. The helper instead:

1. allocates an exact-size staged table and assignment bitmap;
2. stages local values using `label_mapping.find()` and validates every local
   label, fine ID, round trip, and coarse-domain value;
3. stages outgoing records from already validated local values;
4. validates receive segment count before indexing segments;
5. validates source ownership, fine/coarse domains, duplicate/missing/extra
   IDs, and every real local/ghost slot exactly once;
6. explicitly excludes the `local_n` sentinel from completeness; and
7. swaps the staged vector into the graph with a checked `noexcept` graph API,
   then emits local trace records in original local-node order.

The graph API exposes only the exact CNode storage size and checked replacement.
A replacement with any other size is a programming error and fail-fast aborts
the graph communicator. Zero local/ghost work retains exactly the sentinel
slot.

### Failure semantics

Communicator/domain/mapping/outgoing/receive/completeness failures are
collectively converged. Common semantic failures escape only after temporary
payload/datatype state has been destroyed, leaving the old complete CNode
vector and trace snapshot unchanged so a caller can retry with the same cached
plan.

All allocation, validation staging, exception construction, trace work, and
other potentially throwing operations after topology acquisition are inside a
catch boundary while the topology communicator is live. Unexpected local or
resource failures call `abort_on_exception`; MPI backend failures follow the
adapter's immediate fail-fast path. No rank-local unwind is allowed to free a
collective communicator after such a failure. `MPI_ERRORS_RETURN` remains a
diagnostic mechanism, not a recovery policy.

## TDD evidence

### RED 1: genuine legacy protocol observation

Before production changes, the new PMPI protocol test ran the existing tag-6
implementation. Value checks completed, but the common protocol assertion
failed because it observed zero blocking neighborhood payloads where one was
required (`0 == 1`). The interposer simultaneously observed the legacy
point-to-point protocol, proving the test was connected to the real production
path rather than a test surrogate.

### RED 2: missing transactional surface

After switching test access to the specified production entry point, the test
target failed to compile because the four-argument helper,
`ghost_cnode_assignment`, `node_to_cnode_storage_size()`, and
`replace_node_to_cnode()` did not exist. Those APIs were then added only as
needed by the production transaction.

## GREEN coverage

The final MPI test covers ranks 1 through 5 with:

- a rank-1 empty graph and zero-degree topology;
- a rank-2 graph with parallel cross-rank edges;
- a rank-3 graph with a trailing zero-local-work rank;
- a rank-4 ring; and
- a rank-5 path with an isolated rank.

It verifies repeated coarse IDs are retained, queried neighbor order is used,
and the successful phase performs one count exchange and one blocking payload.
PMPI interposers require the exact classic-versus-`_c` branch and reject all
tag-6 broad P2P, immediate-neighborhood, persistent-neighborhood, hidden
completion, cancellation/request-free, and barrier calls. Interposers are
allocation-free, `noexcept`, and mutate a received payload only after PMPI
success.

Five post-receive corruptions are exercised on a fixture with the necessary
multiple sources/expected IDs:

- unknown fine global ID;
- correct ID attributed to the wrong source;
- duplicate replacing an expected ID;
- missing/extra expected ID; and
- coarse ID exactly equal to the exclusive domain bound.

Each corruption must fire exactly once, converge to the same semantic error on
all ranks, preserve the complete CNode vector including its sentinel and the
trace snapshot, then succeed on a clean retry with the same cached topology.

Additional tests cover present-but-out-of-domain local mapping with `N=0` and
`id=0`, missing local mapping, skewed coarse and fine global counts, congruent
communicator acceptance, similar communicator rejection, asymmetric topology
rejection before payload, exact wire extent, exact graph replacement, and the
wrong-size fail-fast death probe. A consistency-first test prewarms the Task
7C1 cached plan through `distributed_partitioner::check_labels()`, resets only
PMPI counters, then proves tag 6 creates no additional topology while still
performing exactly one count exchange and one payload.

The apparently separate case “receive a nonempty payload when `N=0`” is
deliberately not manufactured: correct common sender preflight makes it
unreachable. The production `N=0,id=0` predicate is proved before payload, and
the receive-side exclusive bound is independently proved by mutating a real
nonempty payload to `coarse_id == N` for `N>0`. Weakening preflight or adding a
synthetic production hook would test a state the algorithm cannot enter.

## Verification

Every shell, configure, build, test, oracle, formatting, audit, and Git command
used:

```text
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G
```

Builds used at most two jobs. The verified environment was CMake 4.3.0, GCC
16.2.1, and Open MPI 5.0.10. Both generated capability headers contain
`KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C=0`; the PMPI tests therefore prove the
classic MPI-3.1 payload selection on this host while keeping independent
guarded MPI-4 coverage.

Final build and test evidence:

- trace-ON debug build completed at `-j2`;
- trace-OFF release build completed at `-j2` (Ninja recovered its existing
  truncated-log warning and exited successfully);
- focused debug rank/corruption/lifecycle matrix: 9/9 passed;
- focused release rank/corruption/lifecycle matrix: 9/9 passed;
- full trace-ON debug CTest: 133/133 passed in 9.63 seconds; and
- full trace-OFF release CTest: 128/128 passed in 6.45 seconds.

`git clang-format --diff` reports that it would modify no touched C++ file, and
`git diff --check` passes. The production source audit finds the new helper's
single `neighbor_all_to_all_v` call and no tag-6 send/receive/probe operation.
The legacy `m_send_buffers` and `peID + 9 * size` point-to-point protocol remain
unchanged for the deliberately deferred tag-9 path.

## Exact deterministic oracle

The definitive final trace-ON debug executable was run on:

```text
graph=examples/rgg_n_2_15_s0.graph
ranks=2
k=2
preconfiguration=ultrafastmesh
seed=0
```

It retained edge cut 196 and balance 1.0094. The 32,768-line partition SHA-256
is exactly:

```text
a600acd0029ee9342e4f7c5b041d224a308b874c85fd35bdbcd3a5a73d48cdd0
```

After removing each rank-local v3 header and globally sorting complete records,
the aggregate contains exactly 436,721 records and has SHA-256:

```text
a179bb30213dbb26638657a1d611e951a8bf900b817647fc03d4c700a83f0a18
```

The authoritative contraction-label multiplicity remains exactly 32,768.
All three oracle values match the pinned pristine-upstream manifest.

## Review and deliberate deferrals

Self-review found the final production transaction preserves queried MPI order,
strong preflight, sentinel semantics, no-throw commit, trace ordering, cached
plan ownership, and fail-fast boundaries. The independent final source, test,
lifecycle, CMake, and report review returned **ACCEPT** with no Critical,
Important, or Minor findings. It independently confirmed that the earlier
receive-layout and exception-construction blockers were closed and that the
root-approved `N=0` coverage does not introduce a synthetic unreachable seam.

Deliberately deferred from Task 7C2:

- tag-9 communication and `m_send_buffers`;
- all other production ghost label/weight exchanges;
- nonblocking and persistent production migrations; and
- local execution of the MPI-4 `_c` branch, which this host does not provide.
