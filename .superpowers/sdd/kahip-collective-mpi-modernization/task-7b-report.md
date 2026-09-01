# Task 7B report: owned asynchronous neighborhood exchanges

## Scope and contract

Task 7B starts from reviewed Task 7A HEAD
`87018e238616be329942fba5cfc17f475ee68610`. It adds only:

- an owning, move-only one-shot neighborhood request;
- a fixed-layout, nonmovable reusable neighborhood context;
- a default-disabled, explicitly selected persistent-collective path; and
- independent configure/link capability probes for the MPI-3.1 immediate and
  MPI-4 persistent and large-count neighborhood APIs.

No production graph-partitioning call site was migrated. Synchronous Task 7A
transport remains unchanged, and nonblocking chunking or production ghost-state
migration remains later work.

The implementation retains the reviewed fail-fast HPC policy. Internal
communicators use `MPI_ERRORS_RETURN` only to preserve the exact diagnostic.
Backend, lifecycle, state-machine, or unilateral programming failures log and
terminate the affected communicator when MPI is active. An object that still
owns MPI resources after finalization raw-aborts without calling MPI cleanup.
Only collectively converged preflight failures leave an internal communicator
scope normally and throw a common structured error.

## TDD and systematic-debugging evidence

The focused tests were written before the implementation. The genuine RED
build failed because `context_options`, `neighbor_exchange_request`, and
`neighbor_all_to_all_v_context` did not exist. A later targeted RED test forced
the MPI-3 policy and observed one persistent-init call when zero was required;
the backend selection had disabled only large-count persistence. The corrected
selection disables every MPI-4 persistent API when `force_mpi3` is active.

Testing and review also exposed and closed these lifecycle defects:

1. A completed one-shot request could enter `wait()` after `MPI_Finalize` and
   silently release owned MPI resources. Every operation now checks runtime
   state before either completion or release.
2. The already-complete `test()` branch likewise bypassed the runtime check;
   it now raw-aborts instead of silently succeeding post-finalize.
3. An arbitrary injected backend integer caused Open MPI's own error-string
   conversion to fail before the adapter could preserve the diagnostic. The
   death probe now injects the valid `MPI_ERR_OTHER` code and still verifies
   exact raw-code retention.
4. Early cleanup probes used a native `int` datatype and therefore could not
   observe forbidden `MPI_Type_free`. They now use an owned structured wire
   datatype and separately track the operation communicator, datatype, and
   request.
5. The PMPI lifecycle recorder originally allocated from an `extern "C"`
   interposer. It now uses fixed allocation-free storage, so the failure and
   teardown probes cannot throw across the MPI ABI.

The final focused matrix is **25/25 passed**: the Task 7B test executable at
ranks 1, 2, 3, 4, and 5, plus twenty independent fail-fast/death cases. The
rank matrix covers zero-degree and explicit empty-edge segments, self traffic,
both asymmetric star directions, source-only, destination-only, and isolated
ranks, uneven typed payloads, stable send and receive addresses across three
generations, move ownership, `test(false)`, `test(true)`, consuming wait,
active-destruction wait ordering, common option and persistence-policy
agreement, rank-local malformed layouts, bounded-layout rejection, synchronous
fallback, forced-MPI-3 backend selection, and persistent init/start/wait/free
ordering.

The PMPI interposers prove that initiation returns an active operation without
hidden completion, use no point-to-point initiation or probe route, preserve
operation buffer addresses, and free an inactive persistent request before its
datatype and communicator. They distinguish graph/validation resources from
the operation communicator, datatype, and request to avoid false cleanup
positives.

The failure matrix covers immediate and persistent init/start/test/wait,
active-destructor wait, inactive persistent request-free, active restart,
inactive test, send/receive access while active, five post-finalize ownership
states, and both `MPI_Initialized` and `MPI_Finalized` query failures. Backend
failures retain the injected MPI code and abort immediately. Post-finalize
cases prove that no wait, request-free, datatype-free, or communicator-free is
attempted.

## Implementation

`neighbor_exchange_request<T>`:

- owns a duplicated distributed-graph communicator, a scoped datatype,
  contiguous send/receive storage, fixed count/displacement metadata, and its
  `MPI_Request`;
- is move-constructible but neither copyable nor move-assignable;
- keeps explicit active and receive-ready state because a successful one-shot
  `MPI_Test` nulls the request while a persistent request remains non-null and
  inactive;
- supports non-consuming `test()` and consuming `wait()` without exposing
  receives before completion; and
- completes an active request before ordinary destruction, while any backend
  failure immediately aborts.

`neighbor_all_to_all_v_context<T>`:

- is statically noncopyable and nonmovable so MPI-observed addresses cannot
  change;
- performs all validation, allocation, copying, layout construction,
  communicator duplication, and datatype construction before request
  initiation;
- defaults to ordinary `MPI_Ineighbor_alltoallv`, with persistence available
  only through explicit `prefer` or `required` policy and a globally agreed
  backend decision;
- makes persistent init the final potentially successful constructor action;
- exposes mutable sends only while inactive and receives only after a completed
  generation; and
- reuses invariant send/receive addresses and layout across generations.

The common preflight collectively validates canonical degree-sized segments,
checked offsets and byte products, policy/options agreement, representability,
and backend selection before payload initiation. Direct asynchronous transport
rejects layouts needing the deterministic bounded MPI-3 rounds; callers may
use the existing synchronous Task 7A fallback. There is no background progress
thread.

CMake now requires MPI 3.1 explicitly. The generated capability header records
four independent compile-and-link results, all linked against `MPI::MPI_CXX`:

- `MPI_Ineighbor_alltoallv` (required);
- `MPI_Ineighbor_alltoallv_c`;
- `MPI_Neighbor_alltoallv_init`; and
- `MPI_Neighbor_alltoallv_init_c`.

## Verification

Every shell, configure, build, test, and Git command used the requested
`systemd-run --user --scope` limits (`MemoryHigh=28G`, `MemoryMax=30G`,
`MemorySwapMax=2G`). Builds used at most two jobs.

The verification host used GCC 16.2.1, CMake 4.3.0, and Open MPI 5.0.10
(advertising MPI 3.1).

- Focused Task 7B ranks and fail-fast cases: **25/25 passed**.
- Existing Task 7A adapter, neighborhood, and lifecycle regression set:
  **12/12 passed**.
- Trace-ON local GCC debug configure and full build: passed.
- Trace-ON local GCC debug full CTest: **115/115 passed** in 8.52 seconds.
- Default trace-OFF local GCC release configure and full build: passed. Ninja
  reported a recoverable premature-EOF `.ninja_log` warning, then completed all
  222 build steps.
- Default trace-OFF local GCC release full CTest: **110/110 passed** in 5.18
  seconds.
- Both generated capability headers report
  `KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV=1`,
  `KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C=0`,
  `KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT=1`, and
  `KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C=0`.
- `clang-format --dry-run --Werror` passed for all three new C++ sources.
- The production API audit found no one-shot or reusable-context call site
  outside the adapter and tests. Therefore the exact partition/trace oracle
  was not rerun, as permitted by the Task 7B brief.

## Independent review and deliberate deferrals

An independent final source review after the complete focused and full
verification matrices returned **ACCEPT** with no Critical or Important
findings. It independently confirmed the force-MPI-3 backend selection,
collective agreement, heap-stable ownership, persistent init/cleanup ordering,
post-finalize raw-abort behavior, representative failure coverage, capability
readback, and absence of a production call site or point-to-point transport
route.

Deliberately deferred:

- production ghost and distributed-consistency migration;
- asynchronous deterministic chunking for MPI-3 layouts exceeding `int`;
- MPI-4 `_c` execution on this host, whose installed bindings do not expose the
  functions; and
- persistent selection by performance measurement at production call sites.

Persistence remains disabled by default and no capability is emulated.
