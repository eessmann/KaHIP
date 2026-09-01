# Task 7C3 report: contraction ghost weights and checked quotient arithmetic

## Scope and base

Task 7C3 was implemented from clean base
`c798af35185d51af13b6497d2c1af725112e8cd9`. The production change is
deliberately limited to:

- replacing contraction tag 9 ghost-weight point-to-point traffic with one
  synchronous neighborhood exchange on the Task 7C1 cached ghost plan;
- removing the contraction-only `m_send_buffers` member made obsolete by that
  migration; and
- making the touched quotient node-weight, edge-weight, local edge-count, and
  global edge-count arithmetic checked and transactional.

Tag 11 block-down propagation, all other production exchanges, asynchronous
evolutionary gossip, and persistent/nonblocking production migration remain
outside this task.

## TDD evidence

### Genuine communication RED

Before changing production tag 9, the three-rank test filtered as
`ghost node weights use one blocking neighborhood exchange` failed its common
protocol predicate. The interposer observed the legacy point-to-point path and
no blocking neighborhood payload. This was a genuine PMPI-observed RED, not a
source-text assertion.

The GREEN test now proves, in MPI's queried source/destination order:

- exactly one neighbor count exchange and one blocking neighbor payload;
- exact `ghost_node_weight` datatype extent;
- the classic MPI-3.1 payload on this host, with separately guarded MPI-4
  `_c` interposition;
- no point-to-point, immediate, persistent, completion, cancellation, probe,
  barrier, or hidden request operation; and
- no extra topology when a prior consistency check has warmed the cached plan.

### Genuine numerical RED

Before the checked/staged implementation, the three-rank arithmetic test
showed that neither a local coarse-node `max + 1` nor a local coarse-edge
`max + 1` was reported commonly: `caught_by_all` was `0/3` in both cases.

The GREEN coverage exercises guard-before-evaluation addition for
`NodeWeight`, `EdgeWeight`, local/global `EdgeID`, and sender sequence values.
It covers `max + 0`, `max + 1`, received-edge aggregation overflow,
owner-weight reduction overflow, missing owner coverage, local edge-count
overflow, and the production deterministic global fold on `{max, 0}` and
`{max, 1}`. The production local-edge increment helper is exercised at the
exact boundary and one past it for both its `+1` remote-target and `+2`
local-target branches. The generic fold requires exact unsigned element type,
so future callers cannot silently narrow or terminate through an unconditional
`noexcept` conversion.

## Production design

### Checked quotient construction

Local quotient construction stages node and edge aggregation in temporary
maps. Every addition tests `rhs > max - lhs` before forming the sum. Allocation
or unexpected local failure aborts the live graph communicator; a collectively
agreed arithmetic or semantic failure remains a symmetric, catchable
`mpi_error` for tests.

Redistributed quotient records are validated structurally before indexing.
Edge weights and local directed-edge counts are accumulated in checked staging
storage and validated collectively before commit. Each rank then allgathers
its exact `EdgeID` count and performs the same deterministic checked fold;
`MPI_SUM` is not used. The quotient graph is allocated only after that fold is
valid.

Owner node-weight records are mapped through checked local IDs into temporary
values and a seen vector. Exact owner coverage, record structure, and checked
reductions converge before any `setNodeWeight` call. Thus a numerical or
semantic failure cannot expose a partially assigned quotient.

### Tag 9 neighborhood transaction

The ghost-weight wire type is an explicit standard-layout, trivially-copyable
`{global_id, weight}` record with Boost.Hana metadata. The exchange:

1. accepts only IDENT/CONGRUENT caller and graph communicators, agrees global
   graph size, and rejects skew before acquiring topology;
2. reuses `parallel_graph_access::ghost_plan()` and constructs outgoing
   segments in the topology's authoritative destination order;
3. validates that every planned local ID is bounded, ordered, unique,
   round-trips to its global ID, belongs to the expected owner domain, and is
   marked as an interface vertex;
4. materializes all potentially allocating storage under a live-topology
   fail-fast boundary, then performs one `neighbor_all_to_all_v` call;
5. validates receive segment count before segment access, then checks each
   record against the exact expected per-source IDs and multiplicity;
6. converges unknown, wrong-source, duplicate, missing, extra, and domain
   failures before dereferencing staged ghost IDs; and
7. sorts pending assignments by `(global_id, source)` and commits ghost
   weights only after final common validation.

A common semantic exception is fully materialized while the topology
communicator is live and rethrown only after protected scopes have ended.
Backend failures and unexpected local exceptions log and abort the affected
communicator. `MPI_ERRORS_RETURN` remains diagnostic-only; production does not
continue from a failed MPI operation.

The transaction does not reset graph generation, invalidate the cached plan,
touch the incremental label queues, or emit replica trace records. The
owner-authoritative quotient trace remains after successful construction in
the original order.

## Test coverage

The focused MPI matrix covers ranks 1 through 5, including zero work, self,
asymmetric and isolated layouts, parallel edges to a single peer, and a real
three-rank multi-source/two-ID receive fixture. Valid weight traffic includes
an owner sending exact `NodeWeight{0}` to overwrite a nonzero ghost as well as
`NodeWeight::max()`.

PMPI corruption injects unknown ID, wrong source, duplicate replacing missing,
and missing/extra records after a successful payload. Each injection fires
exactly once, produces the same semantic failure on every rank, preserves all
local/sentinel/ghost weights and the trace snapshot, then succeeds on a clean
retry with the same cached topology. Additional cases prove congruent
communicator acceptance; similar communicator, global-count skew, and
asymmetric-plan rejection before payload; exact classic-versus-`_c` selection;
and exact record extent.

Four short-timeout focused CTest registrations make the transaction,
asymmetric, plan-reuse, and quotient-arithmetic cases independently visible.

## Verification

Every shell, configure, build, test, oracle, formatting, audit, and Git command
used:

```text
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G
```

Builds used at most two jobs. The verified environment was CMake 4.3.0, GCC
16.2.1, and Open MPI 5.0.10. Both debug and release generated capability
headers contain:

```text
KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C=0
KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C=0
KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT=1
KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C=0
```

The host therefore executes and proves the MPI-3.1 blocking path; guarded PMPI
coverage keeps the independently probed MPI-4 branch testable on capable MPI
implementations.

Final evidence after formatting and header self-containment fixes:

- trace-ON debug build completed at `-j2`;
- trace-OFF release build completed at `-j2`;
- focused contraction rank matrix: 5/5 passed;
- focused ghost-weight and arithmetic registrations: 4/4 passed;
- full trace-ON debug CTest: 137/137 passed in 9.94 seconds; and
- full trace-OFF release CTest: 132/132 passed in 6.81 seconds.

`git clang-format --diff` reports no touched C++ changes. The final production
protocol audit finds no contraction `m_send_buffers`, tag-9 send/receive/probe,
or other point-to-point operation. `git diff --check` passes.

## Exact deterministic oracle

The definitive final trace-ON debug executable was run on:

```text
graph=examples/rgg_n_2_15_s0.graph
ranks=2
k=2
preconfiguration=ultrafastmesh
seed=0
```

It retained edge cut 196 and balance 1.0094. The exact 32,768-line partition
SHA-256 is:

```text
a600acd0029ee9342e4f7c5b041d224a308b874c85fd35bdbcd3a5a73d48cdd0
```

After removing each rank-local v3 header and globally sorting complete records,
the aggregate contains exactly 436,721 records and has SHA-256:

```text
a179bb30213dbb26638657a1d611e951a8bf900b817647fc03d4c700a83f0a18
```

The touched stage multiplicities remain exactly 32,768 contraction-label
records and 2,898 quotient-node-weight records. Partition, trace, stage counts,
cut, and balance all match the pinned pristine-upstream oracle.

## Review and residuals

Self-review verifies checked-before-evaluation arithmetic, validate-then-commit
phase ordering, exact owner coverage, no partial graph mutation, queried MPI
neighbor order, zero-weight preservation, cached-plan reuse, trace parity, and
production fail-fast boundaries. Independent final review returned **ACCEPT**
with no Critical, Important, or Minor issue; its generic checked-fold,
direct-include hygiene, and local edge-count boundary findings were closed
before the final verification above.

Deliberately deferred from Task 7C3:

- tag 11 block-down propagation and other production ghost updates;
- nonblocking or persistent production exchange migration;
- runtime execution of MPI-4 `_c` collectives on this MPI-3.1-capability host;
  and
- the million-cell cube and performance gates, which belong to the later
  integration and benchmark tasks.
