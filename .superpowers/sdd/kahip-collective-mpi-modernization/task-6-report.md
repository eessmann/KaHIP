# Task 6 report: dense owner exchanges

## Contract and scope

Task 6 was implemented on full/base HEAD
`365f7afbbd2433060c632ae1a6bc81058f82dd56`. The change is limited to the
four reviewed dense owner phases:

1. `parallel_contraction::compute_label_mapping` request and reply exchanges;
2. quotient-edge redistribution inside
   `redistribute_hased_graph_and_build_graph_locally`;
3. quotient node-weight redistribution in the same function; and
4. the dense owner phase of
   `parallel_block_down_propagation::propagate_block_down`.

The implementation uses the Task 4 segmented `all_to_all_v` adapter. Every
wire type is Hana-described, standard-layout, and trivially copyable. Empty,
self, uneven, and zero-local-work segments use the same collective path and do
not use sentinels.

## Sequential TDD evidence

Each path was tested and migrated before work began on the next path.

### 1. Label mapping

RED: the new three-rank regression already proved the exact globally
contiguous mapping `{0 -> 0, 1 -> 1, 3 -> 2}`, but the protocol probe observed
two one-field payloads (`{8, 8}` byte extents) instead of an explicit one-field
request followed by a two-field keyed reply (`{8, 16}`). This isolated the old
positional reply protocol even though its mathematical result happened to be
correct on the fixture.

GREEN: requests are now `{old_label}` records and replies are
`{old_label, coarse_global_id}` records. Per-owner request and reply segments
are stably sorted by semantic keys, replies are applied by `old_label`, and
unknown, conflicting, or missing replies fail closed. The globally sorted
owner labels and prefix assignment are unchanged, preserving the upstream
contiguous mapping order and label count. Ranks 1--5 passed with exactly two
dense payload collectives, extents `{8, 16}`, and zero probed P2P calls.

The explicit-ID design intentionally removes positional correlation to
container iteration or arrival order. There was no semantic mapping mismatch:
the old implementation's positions encoded the same mapping only because its
request and reply vectors remained in lockstep.

The exact v3 oracle after this migration matched the golden partition and
aggregate hashes.

### 2. Quotient-edge redistribution

RED: the cross-rank fixture's exact directed edge multiset and weights passed
against the old implementation, including duplicate contributions, while the
probe reported no dense edge payload and observed the superseded tag-7
`MPI_Isend`/`MPI_Probe`/`MPI_Recv` protocol.

The first keyed collective candidate stably sorted semantic wire records and
passed the exact edge invariant, but the v3 oracle caught a parity regression:

- partition SHA:
  `1ff1496c2d26603ddf1958a5028cd691496e511f02a042541cb2fb544b4ae211`;
- aggregate SHA:
  `5e7e033fde94f9126c787957766c2d7a7ddbf914d4b6d8e92025a9f5c9dff6ca`.

Systematic isolation showed that all `quotient-edge` and
`quotient-node-weight` records still had the same combined SHA
`08f00ab0b3ffa0019deb884b999baf2d75ca668d87e1bd4df18ccc7e38a0712a`.
Only three final-partition records changed (six `comm -3` lines). Removing the
semantic wire sort as a control restored the exact oracle. The owner divisor,
self segment placement, duplicate aggregation, `/4` and `/2` arithmetic, and
source-canonical processing were therefore not the cause. The cause was the
receiver inserting semantic wire order into `local_graph` instead of the
pinned upstream per-source order. That changed unordered `local_graph`
iteration, quotient adjacency materialization, and a later RNG-sensitive
traversal despite leaving the quotient edge multiset unchanged.

GREEN: `bundled_edge` carries
`{source, target, weight, sender_sequence}`. `sender_sequence` is explicit and
unique within every source/destination segment. Semantic wire sorting retains
it; each receiver validates a contiguous sequence without duplicates or gaps
and reconstructs the pinned upstream per-source insertion order before
materializing `local_graph` and `Q`. A focused rank-3 order regression fails if
the receiver instead materializes semantic wire order (the mutation failed on
rank 1), then passes with sequence reconstruction. This preserves the
deterministic behavior of this pinned upstream build's inherited unordered
container iteration; it is not a claim of broad cross-platform determinism.

Ranks 1--5 passed the exact aggregation and legacy adjacency-order regression,
with one four-field dense edge payload and zero tag-7 P2P calls. The original
`e.weight / 4` and `e.weight / 2` mathematics is byte-for-byte unchanged. The
exact v3 oracle returned to the golden hashes before node-weight work began.

### 3. Quotient node weights

RED: the new cross-rank regression proved exact summed weights, including
duplicate contributions and zero-local-owner ranks, but observed no two-field
dense payload and observed the old tag-8 P2P protocol.

GREEN: explicit `{coarse_global_id, weight}` contributions are stably sorted
per owner, exchanged collectively, processed in source-rank order, owner
validated, and summed onto the exact local coarse node. Ranks 1--5 passed with
one two-field node-weight payload and zero tag-8 P2P calls. The exact v3 oracle
again matched the golden hashes.

### 4. Dense block-owner propagation

RED: the generalized rank-3 fixture proved exact blocks and trace records,
including identical duplicate coarse-node updates, but observed no dense
payload and observed the old tag-10 P2P protocol.

GREEN: explicit `{coarse_global_id, block}` updates are stably sorted per
owner, exchanged collectively, and gathered source-canonically into a sorted
map before application. Identical duplicates are accepted; conflicting values
or wrong-owner records fail closed. Ranks 1--5 passed, including uneven owners
and ranks with zero local coarse nodes, with exactly one two-field dense
payload and zero tag-10 P2P calls. The full two-rank projection/trace regression
also passed. The final exact v3 oracle matched the golden hashes.

## Final exact oracle

The final trace-ON debug executable was run on the pinned tuple:

```text
graph=examples/rgg_n_2_15_s0.graph
ranks=2
k=2
preconfiguration=ultrafastmesh
seed=0
```

Result:

```text
partition SHA256  a600acd0029ee9342e4f7c5b041d224a308b874c85fd35bdbcd3a5a73d48cdd0
aggregate SHA256  a179bb30213dbb26638657a1d611e951a8bf900b817647fc03d4c700a83f0a18
aggregate records 436721
```

All three values exactly match
`task-5-oracle-golden.txt`.

## Final verification

Every command was run through the required user `systemd-run` scope with
`MemoryHigh=28G`, `MemoryMax=30G`, and `MemorySwapMax=2G`; builds used at most
two jobs.

- Trace-ON debug configure and full build: passed.
- Trace-ON debug full CTest: **57/57 passed**.
- Default trace-OFF release configure and full build: passed.
- Default trace-OFF release full CTest: **52/52 passed**.
- Focused adapter and protocol/invariant CTest set: **16/16 passed**:
  adapter ranks 1--5, contraction ranks 1--5, dense block-owner ranks 1--5,
  and the full projection MPI test.
- Generated capability readback in both builds:
  `KAHIP_HAVE_MPI_ALLTOALLV_C 0`. The probes nevertheless count both
  `MPI_Alltoallv` and the guarded `MPI_Alltoallv_c` entry point.
- `git diff --check`: passed.

The production search now finds dense payload calls at label request/reply,
edge redistribution, node-weight redistribution, and block-owner propagation.
It finds no tag-7, tag-8, or tag-10 `MPI_Isend`, `MPI_Probe`, `MPI_Recv`, or
sentinel protocol. Sparse tag-6, tag-9, and tag-11 exchanges remain present as
required.

## Deliberate deferrals and hygiene

The following paths were not migrated in Task 6:

- `get_nodes_to_cnodes_ghost_nodes`;
- `update_ghost_nodes_weights`;
- `update_ghost_nodes_blocks` (Task 7 scope);
- graph ghost messaging and consistency checks;
- DSPAC;
- `mpi_tools` root gather;
- asynchronous evolutionary gossip.

The shared `m_send_buffers` storage used by sparse paths was preserved. Only
the dense-path `m_messages`/`m_out_messages`, sentinels, non-waited request
state, and tags 7, 8, and 10 were removed. No unrelated production files were
changed.

## Fix round 1: collective semantic validation

### Scope and review findings

This fix round starts from reviewed Task 6 commit
`3d2d985a452efb5b3d75244fc8afd1e3214540f4` and addresses all three Important
findings and both Minor test gaps from the scoped review. It does not migrate
any Task 7 sparse path.

The adapter now exposes a thin `validate_collectively` operation. It duplicates
the affected communicator, installs `MPI_ERRORS_RETURN` through the existing
RAII communicator, reduces the local validity predicate, and gives every rank
the same structured `mpi_error` context when any rank rejects a record. The
duplication is diagnostic isolation: `MPI_ERRORS_RETURN` is not a recovery
contract.

KaHIP retains a fail-fast HPC policy. In production, the structured exception
propagates to the existing application/interface exception barrier, which logs
through `spdlog::critical` and calls `MPI_Abort` on the affected communicator.
No validation failure recovers, continues, returns a partial graph, or applies
partial state. The focused test harness catches the structured exception only
to prove that every rank reaches the same common fail-fast point without a
hang; it does not model production recovery.

### RED/GREEN evidence

1. **Local block conflicts.** RED used a real three-rank fixture with two fine
   nodes on rank 0 contributing different blocks for the same coarse ID. Only
   one of three ranks caught the old owner-local exception. GREEN preserves
   every local `{coarse_global_id, block}` contribution, stable-sorts it, and
   collectively rejects differing duplicates before any payload exchange.
   Identical duplicates remain valid.

2. **Full coarse-ID domains and zero work.** The `N=4,size=3,ID=4` REDs showed
   that an invalid label and invalid node-weight contribution were silently
   admitted, invalid edge source/target records reached unsafe graph indexing,
   and an invalid block ID was not rejected. The zero-coarse-node fixture also
   exposed the old underflowed owner range. GREEN validates every edge source
   and target, node-weight ID, and block ID against the full half-open domain
   before division/routing, then validates received domain, semantic owner,
   and `Q.is_local_node_from_global_id` again before indexing or application.
   A divisor of one is used only as an empty-domain routing guard, so `N==0`
   performs empty collective exchanges without division by zero or sentinels.

3. **Collective failure symmetry.** All semantic scans now accumulate validity
   without `.at`, division, local graph indexing, or application on malformed
   records. Receiver-side label correlation, sender-sequence reconstruction,
   and block conflict/missing scans reuse and sort the already-received storage
   in place; malformed semantic fields do not drive an extra map or per-source
   vector allocation before the common validation decision. Label request
   owner/domain and reply domain/owner/known/conflict/
   missing checks, edge sequence/owner/domain checks, node-weight owner/domain
   checks, and block owner/domain/conflict/missing checks all converge through
   the collective validator. Edge receive validation occurs before
   `local_graph` materialization and before the existing edge-count
   `MPI_Allreduce`. The three-rank block REDs separately observed tail ID
   caught by 0/3 ranks, cross-rank conflict caught by 1/3, and missing updates
   caught by 0/3. GREEN makes all ranks catch the same structured context in
   every failure fixture; every dedicated malformed-input CTest has a
   five-second timeout.

4. **Minor coverage gaps.** A ranks-1--5 global-zero label fixture now requires
   exact count zero, an empty map, two empty dense payload exchanges, and zero
   `MPI_Isend`, `MPI_Probe`, or `MPI_Recv`. The normal label PMPI protocol test
   now also explicitly requires `MPI_Recv == 0`.

The valid block fixture now supplies all four coarse IDs from every rank. This
retains identical duplicate coverage while independently asserting the exact
block on each local coarse node, including uneven ownership and ranks with zero
local coarse nodes. The sender-sequence reconstruction, semantic wire sorting,
source-canonical processing, quotient `/4` and `/2` mathematics, node-weight
sums, and sparse deferrals are unchanged.

### Fix-round verification

Every shell command used the required `systemd-run --user --scope` memory
limits, and every build used at most two jobs.

- Trace-ON debug full build: passed; full CTest **65/65 passed**.
- Default-OFF release full build: passed; full CTest **60/60 passed**. The
  reused Ninja log emitted its known recoverable premature-EOF regeneration
  warning; generation, compilation, linking, and all tests completed.
- Focused trace-ON matrix: **24/24 passed**: adapter ranks 1--5, contraction
  ranks 1--5, block-owner ranks 1--5, projection, and all eight collective
  failure cases.
- Generated capability readback in both builds:
  `KAHIP_HAVE_MPI_ALLTOALLV_C 0`; debug cache has tracing ON and release cache
  has the default tracing OFF.
- Post-fix exact pinned tuple: partition SHA-256
  `a600acd0029ee9342e4f7c5b041d224a308b874c85fd35bdbcd3a5a73d48cdd0`,
  canonical 436,721-record aggregate SHA-256
  `a179bb30213dbb26638657a1d611e951a8bf900b817647fc03d4c700a83f0a18`.
  Both remain exact.
- Production protocol search still shows the five Task 6 dense
  `all_to_all_v` payloads and no tag-7, tag-8, or tag-10 dense
  `MPI_Isend`/`MPI_Probe`/`MPI_Recv`/sentinel protocol. Required sparse tag-6,
  tag-9, and tag-11 paths remain present.
- `git diff --check`: passed before the final commit.

The deliberately deferred list above remains exact: in particular,
`get_nodes_to_cnodes_ghost_nodes`, `update_ghost_nodes_weights`, and
`update_ghost_nodes_blocks` remain sparse P2P for Task 7, and graph ghost
messaging, consistency checks, DSPAC, `mpi_tools` root gather, and asynchronous
evolutionary gossip were not changed.

## Fix round 2: backend fail-fast and exact ownership

### Scope and backend failure policy

This round starts from reviewed fix-round-1 commit
`e60ac4c0cd49e871e105a7521aee9d09ee4d4d21` and closes all four Important
findings and the remaining test gaps from the second scoped review. Task 7
sparse exchanges remain deliberately deferred.

MPI backend failures no longer become exceptions while an internally
duplicated communicator is alive. `check_or_abort` records the structured MPI
diagnostic best-effort, calls `MPI_Abort` on the affected communicator, and
falls back to `std::abort` if MPI returns. `MPI_ERRORS_RETURN` is therefore
diagnostic-only, not a recovery policy. Communicator duplication,
`MPI_Comm_set_errhandler`, rank/size lookup, every dense count/payload
collective, datatype creation/commit/free, scans, broadcasts, barriers, and
the quotient edge-count reduction all use this path. A set-errhandler failure
does not first free the duplicated communicator. A failed `MPI_Comm_free`
uses `MPI_COMM_WORLD` because the freed handle may be stale. Before MPI
initialization or after finalization, failure reporting avoids both
`MPI_Error_string` and `MPI_Abort`, logs the raw return code, and terminates
with `std::abort`.

Successful collective semantic rejection remains distinguishable. Both
`validate_collectively` and the new common-value agreement helper complete
their reduction and leave the owned-communicator scope normally; only then do
all ranks throw the same structured `mpi_error`. `all_to_all_v` likewise
defers layout/option disagreement until after normal communicator destruction,
while unilateral allocation, representability, or other local failures abort
before its communicator destructor can run.

### Exact ownership and common domains

One `contiguous_owner_layout<NodeID>` now defines every Task 6 dense owner
range. It computes the pinned fixed-chunk partition entirely with checked
integer arithmetic, validates an ID before division, exposes half-open
boundaries, saturates boundary multiplication without overflow, and represents
empty trailing ranks without underflow. All owner-derived container access is
bounds checked. The label, quotient, and block paths no longer use floating
point `ceil(N / double(size))` arithmetic.

Before ownership, routing, or domain-sized graph state is created, ranks now
agree on the minimum and maximum global domain value:

- label mapping agrees `G.number_of_global_nodes()`;
- quotient redistribution agrees `number_of_cnodes`; and
- block propagation agrees `Q.number_of_global_nodes()`.

A mismatch is rejected commonly even when every payload is empty. The exact
owner helper has compile-time regressions for `N=0`, `N<p`, uneven `N=4,p=3`,
`2^53+1`, and maximum 64-bit `NodeID` with 2, 3, and 5 ranks, including the
last valid owner and exact terminal boundary.

### TDD and malformed-receive evidence

The ownership test first failed to compile because the helper did not exist.
The three empty-payload domain-skew tests then went RED with zero of three
ranks catching for label mapping, quotient redistribution, and block
propagation. GREEN makes all three ranks catch the named common context before
graph state changes.

The PMPI seam mutates a selected dense payload ordinal after a successful
`MPI_Alltoallv`; the same typed hook is present behind the guarded
`MPI_Alltoallv_c` interposer. It does not infer record identity from datatype
extent. Dedicated five-second three-rank tests cover:

- an in-domain label request delivered to the wrong owner;
- an in-domain, correct-source label reply with an unrequested semantic key;
- an in-domain quotient edge source delivered to the wrong owner;
- a quotient sender-sequence gap; and
- an in-domain node-weight ID delivered to the wrong owner.

Characterization mutations that removed the corresponding request-owner,
reply-correlation, and sequence checks made each dedicated CTest fail with
zero of three ranks observing the expected common exception; restoring the
checks made all five receive tests pass. The block fixture separately proves
that a valid identical duplicate from one sender is accepted while the full
coarse-ID domain remains covered.

Real backend failure injection is intentionally not emulated with a fake PMPI
return: a genuine backend error is a non-returning process-group event, so an
in-process recovery assertion would weaken the production contract. The
non-returning paths were instead audited call by call. This host reports
`KAHIP_HAVE_MPI_ALLTOALLV_C 0`; it exercises the real MPI-3 path, while the
typed MPI-4 interposer remains compile-guarded for MPI-4 CI.

### Fix-round-2 verification

Every build, test, oracle run, and verification command used the required
`systemd-run --user --scope` memory limits; builds used at most two jobs.

- Trace-ON debug full build passed; full CTest: **74/74 passed**.
- Default trace-OFF release full build passed; full CTest: **69/69 passed**.
  Ninja emitted its known recoverable premature-EOF warning and completed
  regeneration, compilation, and linking successfully.
- The final fixed two-rank tuple retained partition SHA-256
  `a600acd0029ee9342e4f7c5b041d224a308b874c85fd35bdbcd3a5a73d48cdd0`
  and canonical 436,721-record aggregate SHA-256
  `a179bb30213dbb26638657a1d611e951a8bf900b817647fc03d4c700a83f0a18`.
- Debug capability/cache readback has tracing ON and
  `KAHIP_HAVE_MPI_ALLTOALLV_C 0`; release has tracing OFF and the same
  capability result.
- The protocol audit still finds the five Task 6 dense `all_to_all_v`
  payloads, no tag-7/tag-8/tag-10 dense P2P protocol, and only the required
  sparse tag-6/tag-9/tag-11 deferrals.
- `git diff --check` passed before the final commit.

## Fix round 3: transactional projection and lifecycle queries

### Scope and projection protocol

This round starts from reviewed fix-round-2 commit
`d6afee826162a071d573a44161259f55327e4b96` and closes the remaining
projection and lifecycle findings. Task 7 sparse exchanges remain deliberately
deferred.

`parallel_projection::parallel_project` now uses the checked communicator
view, agrees the global coarse-node count before ownership or allocation, and
routes through the same exact `contiguous_owner_layout<NodeID>` as the other
dense owner phases. It contains no floating-point ownership arithmetic or raw
rank/size query. A first pass validates every finer coarse ID and the exact
local coarse range collectively before any owner optional is consumed.

Both locally resolvable and remote label assignments are staged. Incoming
requests are checked by source segment for the full coarse domain, exact
receiver ownership, safe local coarse presence, unique request IDs, and one
request per coarse ID. That common validation completes before labels are read
or any reply is constructed. Incoming replies are then checked by their actual
source segment for the exact coarse owner, known request ID, exact requested
coarse ID, uniqueness, and complete one-to-one coverage. No map entry is
erased and no finer label is written during validation. Only after the final
common validation does the function append request/reply trace records, apply
all staged labels, and enter the existing ghost update. The valid trace actor
fields and canonical ordering remain exact.

### Projection RED/GREEN evidence

The first focused RED run exposed all original failure modes:

- empty-payload coarse-count skew was caught by zero of three ranks;
- a tail coarse ID equal to `N` timed out rather than reaching a common error;
- typed request corruption timed out after payload one because some ranks
  built replies while another threw;
- typed reply correlation corruption and duplicate/missing reply corruption
  timed out after payload two; and
- the old function had already written locally resolvable labels before those
  failures.

The valid `N=0` and uneven `N=5,size=3` cases were retained as characterization
tests and then passed the exact implementation. GREEN now proves:

- empty domain performs exactly two empty dense payload exchanges;
- empty-payload domain skew and `cnode == N` fail commonly before traffic or
  mutation;
- uneven ownership projects the exact expected labels;
- request corruption performs exactly one payload exchange and constructs no
  reply payload;
- request, reply, and duplicate/missing-reply failures leave every finer label
  unchanged, including locally resolvable nodes; and
- both request and reply traces remain empty on failure.

The mutation hook is typed and selected by explicit payload ordinal in both
the MPI-3 `MPI_Alltoallv` interposer and guarded MPI-4 `MPI_Alltoallv_c`
interposer. A deliberate mutation that emitted request trace records before
reply validation made the new trace-transaction assertion RED on both ranks;
restoring final-validation-first ordering made the valid exact trace and all
three failure fixtures GREEN.

### Lifecycle query fail-fast

Lifecycle state is now explicitly classified as before initialization, active,
or finalized. A non-success result from `MPI_Initialized` or `MPI_Finalized`
cannot be treated as inactivity: it emits only the query name and raw return
code best-effort, then calls `std::abort`. Because runtime state is unknown,
that path does not call `MPI_Error_string`, `MPI_Abort`, or another lifecycle
query.

The dedicated subprocess probe interposes both failure cases, traps any
forbidden MPI diagnostic/abort call, and installs a test-only SIGABRT observer.
CTest requires the distinctive SIGABRT exit, the raw diagnostic, and absence
of a forbidden call; an arbitrary nonzero exit no longer passes. Separate
subprocess modes exercise the real MPI runtime before initialization and after
a real `MPI_Init`/`MPI_Finalize` pair, and require inactive state in both cases
while also checking active state between initialization and finalization. The
two normal-state tests first went RED as unsupported probe modes; all four
lifecycle tests are now GREEN.

### Remaining malformed-receive coverage

The shared typed PMPI mutation seam now also corrupts:

- a label reply to use `coarse_global_id == global_num_distinct_ids`; and
- a quotient edge to use `target == number_of_cnodes`.

Temporarily removing each production predicate proved the tests were
discriminating: the label test reported zero of three ranks catching, while
the edge test reached an out-of-range graph index and aborted. With the
predicates restored, every rank reaches the named common validation context.
The same mutation helper remains guarded consistently for MPI-4 `_c` builds.

### Fix-round-3 verification

Every configure, build, test, oracle, and audit command used the required
`systemd-run --user --scope` memory limits; builds used at most two jobs.

- Trace-ON debug full build passed; full CTest: **87/87 passed**.
- Default trace-OFF release full build passed; full CTest: **82/82 passed**.
  Ninja emitted its known recoverable premature-EOF warning, regenerated, and
  completed compilation and linking successfully.
- The final two-rank fixed tuple retained partition SHA-256
  `a600acd0029ee9342e4f7c5b041d224a308b874c85fd35bdbcd3a5a73d48cdd0`
  and canonical 436,721-record aggregate SHA-256
  `a179bb30213dbb26638657a1d611e951a8bf900b817647fc03d4c700a83f0a18`.
- Debug capability/cache readback has tracing ON and
  `KAHIP_HAVE_MPI_ALLTOALLV_C 0`; release has tracing OFF and the same
  capability result. The MPI-3 path ran locally; the typed `_c` seam remains
  compile-guarded for MPI-4 CI rather than being faked.
- The protocol audit finds seven production dense `all_to_all_v` payload call
  sites: the two transactional projection phases plus the five Task 6 owner
  phases. It finds no tag-7/tag-8/tag-10 dense P2P protocol and only the
  required sparse tag-6/tag-9/tag-11 deferrals.
- An independent read-only review found no remaining production correctness,
  deadlock, or partial-state defect after the lifecycle and trace-transaction
  coverage gaps were closed.
- `git diff --check` passed before the final commit.

## Fix round 4: lifecycle-probe test-quality closure

This final narrow round starts from fix-round-3 commit
`bc9e17bc56758bc3f0fd1bdfac7b57aad20db435` and changes only test code,
CTest wiring, one test comment, and this report. Production and trace sources
are unchanged.

The SIGABRT observer no longer calls the stdio-based `fputs` from a signal
handler. It emits its fixed short marker with one async-signal-safe POSIX
`write(STDERR_FILENO, literal, sizeof(literal) - 1)` and immediately calls
`_Exit(86)`. The marker is statically below the POSIX minimum atomic pipe-write
size; any failed or incomplete write makes the verifier fail closed because
the complete marker is required.

The interposer now returns the distinctive synthetic raw code `17293` rather
than relying on the implementation-specific numeric value of `MPI_ERR_OTHER`.
CTest passes the same expected code to the verifier, which requires that exact
numeric diagnostic. Per-query counters additionally make a retry, an
unexpected `MPI_Finalized` after an initialized-query failure, or an
out-of-sequence finalized query print the existing forbidden-call marker and
exit through a nonmatching status. The original traps still prove that neither
`MPI_Error_string` nor `MPI_Abort` is called while lifecycle state is unknown.

The exact-code assertion went RED in both failure modes against the prior
probe: this Open MPI returned raw code `16`, while the verifier required
`17293`. With the synthetic code and counters in place, all four lifecycle
tests are GREEN. The projection mutation comment now accurately describes the
grouped receiver validation; it no longer claims that only one predicate can
reject the deliberately malformed request.

Every command used the required `systemd-run --user --scope` limits and every
build used at most two jobs. Final evidence:

- focused lifecycle, exact projection, and request-corruption matrix:
  **6/6 passed**;
- trace-ON debug full build and CTest: **87/87 passed**, including the MPI
  trace integration smoke;
- default trace-OFF release full build and CTest: **82/82 passed**; Ninja's
  known recoverable premature-EOF warning was followed by successful
  regeneration, compilation, and linking;
- the exact partition/trace oracle was not rerun because no production or
  trace source changed; the full trace integration smoke is fresh and the
  fix-round-3 exact `a600`/`a179`/436,721-record evidence remains the applicable
  production result;
- final self-review found no production behavior change or test seam that can
  accept a retry, wrong raw code, forbidden MPI call, arbitrary nonzero exit,
  or incomplete SIGABRT marker; and
- `git diff --check` passed before the focused commit.
