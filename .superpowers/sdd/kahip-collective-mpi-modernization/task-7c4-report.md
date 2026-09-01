# Task 7C4 report: transactional block-down owner and ghost propagation

## Scope and base

Task 7C4 was implemented from reviewed Task 7C3 head
`1624be49aa17d32bc2b366a4c6885a37d8836d11`. The production change is
deliberately limited to:

- strengthening block-down owner routing into the first half of one
  validate-before-commit transaction;
- replacing the legacy tag-11 ghost send/probe/receive phase with one
  synchronous neighborhood exchange on `Q.ghost_plan()`;
- replacing the alternating integer wire buffer with an explicit typed
  `{NodeID, PartitionID}` record and deleting the obsolete P2P state; and
- preserving the already-collected v-cycle block state on the complete root
  graph when its structure is broadcast to the other ranks.

The final item is a narrow correctness repair found by the required two-cycle
oracle. Root already owns the complete graph that supplies the broadcast
arrays. Rebuilding that same graph was redundant and, after the modern graph
reset work, correctly cleared its semantic node state. Non-root ranks still
build and receive the exact complete structure; root now retains the collected
`SecondPartitionIndex` values without relying on stale vector storage.

Tag 3, incremental A/B label generation, DSPAC communication, evolutionary
gossip, and nonblocking or persistent production migration remain outside this
task.

## TDD evidence

### Genuine tag-11 communication RED

Before production edits, the principal two-rank test used a real cross-rank
coarse graph and exact owner/ghost state. Its semantic vector and canonical
trace assertions already passed, while the PMPI protocol predicate failed.
The probe observed:

```text
dense payloads:             1
topology creations:         0
neighbor count exchanges:   0
neighbor payloads:          0
point-to-point calls:        3
tag-11 Isend/Probe/Recv:      1/1/1
```

This was a genuine runtime RED against the legacy tag-11 implementation. The
GREEN path performs one dense payload, one neighbor count exchange, and one
blocking neighborhood payload, with no tag-11 P2P or hidden request protocol.

The Task 7C4 PMPI evidence comes only from the new dedicated harness. Its
callbacks are fixed-capacity, allocation-free, Catch-free, and exception-free.
The redundant old block-down protocol case and its ranks-1-through-5
registrations were removed from the older projection interposer; remaining
legacy semantic cases leave that old probe inactive.

### V-cycle state-lifecycle RED

The first candidate two-cycle oracle produced a mathematically invalid all-zero
partition: cut 0, balance 2, and partition SHA
`d35c61faa229c9f4caf4f7bc1659f7b1f4ebca5ad126149b6edca7b207e0c954`.
The same failure reproduced on the reviewed Task 7C3 base, while a debug trace
showed 3,066 balanced block-propagation records before the state was lost.

A narrow two-rank regression then seeded a complete root graph with nonzero
v-cycle blocks and required exact structural replication on both ranks. Before
the repair it failed only at:

```text
REQUIRE(all_blocks == 1)
with expansion: 0 == 1
```

Five of six assertions passed, including every node, edge, weight, degree, and
adjacency check on both ranks. This isolated the failure to root semantic state:
`distribute_local_graph()` rebuilt root after collection, and modern
`start_construction()` correctly cleared `m_nodes_data`. After skipping only
the redundant root rebuild, the focused test passed and the exact external
two-cycle oracle matched pinned upstream byte for byte.

## Production design

### Exact record and common preflight

`block_down::block_update` is an explicit standard-layout,
trivially-copyable record:

```text
{ NodeID coarse_global_id; PartitionID block; }
```

Boost.Hana member metadata builds its scoped MPI datatype, whose resized extent
is exactly `sizeof(block_update)`. Block values are never transported as a
coincidentally representation-compatible node or weight type.

Before any payload the implementation collectively requires:

- IDENT/CONGRUENT caller, finer-graph, and coarse-graph communicators;
- positive, representable, rank-identical `config.k`;
- rank-identical coarse global count and exact range metadata;
- bounded finer CNodes, blocks, global/local lookup round trips, and exact
  owner existence; and
- agreement among repeated local mappings for a coarse semantic ID.

Empty coarse graphs, rank one, zero-local-work ranks, source-only or
destination-only topology roles, isolated work, and zero-degree ranks
participate without sentinel messages or early return.

### One dense-plus-neighborhood transaction

Local owner updates are keyed and sorted by `(coarse_global_id, block)`. The
cached graph plan is acquired before the dense payload, then arbitrary owners
receive one `all_to_all_v<block_update>` transaction. Receive structure,
source/owner semantics, domains, identical-versus-conflicting duplicates, and
exact owned coverage converge before graph mutation. Staged owner blocks remain
in local-ID order with an assigned bit per owned vertex.

Outgoing ghost segments are constructed in MPI's authoritative destination
order from `plan.outgoing_local_nodes(index)`. Every planned local ID must be
strictly ordered and unique, truly owned, lookup-stable, assigned, and in both
coarse-ID and block domains. Parallel edges do not multiply records; one vertex
visible at several destinations appears once in each destination segment.

Exactly one ordinary synchronous
`neighbor_all_to_all_v<block_update>` follows. Each queried source segment is
validated against `plan.expected_ghost_nodes(source_index)` with
`find_ghost_local_id(id, source)`. Segment count, ID and block domains,
wrong-source, duplicate, missing, extra, unknown, and exact ghost coverage all
converge before a staged local ID is dereferenced for commit.

Only after both phases have converged are owned and ghost blocks committed in
deterministic order. The combined setter loop is protected by a live-topology
fail-fast boundary, so an unexpected debug bounds exception cannot return a
partial partition. Trace emission is a later, separately protected phase and
occurs only after the complete state commit: one owner record per owned coarse
vertex and one source-to-receiver record per ghost.

Collectively agreed semantic failures preserve every pre-call owned/ghost block
and the trace snapshot, then permit a clean retry with the same cached plan.
Allocation, datatype, unilateral, backend, commit, or unexpected failures log
and abort the affected live communicator. `MPI_ERRORS_RETURN` is diagnostic
plumbing only; production never continues from a failed MPI operation.

## Focused coverage

The dedicated suite covers ranks 1 through 5:

- rank 1 zero-degree and globally empty graphs;
- rank 2 real cross-rank interfaces and duplicate parallel edges;
- rank 3 connected work plus a zero-local rank;
- rank 4 a two-peer ring;
- rank 5 a path plus isolated work;
- non-power-of-two ownership, `k` not dividing rank count, and cold/warm plan
  reuse with changed blocks.

It asserts exact owner and ghost values, exact queried order and multiplicity,
separate dense and neighborhood datatype extents equal to
`sizeof(block_update)`, one dense payload, one neighbor count exchange, one
blocking payload, and zero P2P, immediate, persistent, completion,
cancellation, probe, barrier, or sentinel traffic.

Post-success PMPI mutation covers out-of-domain IDs, wrong-source records,
duplicate-replacing-missing records, missing/extra coverage, block equal to
`k`, and conflicting duplicates in both dense and neighbor phases. Every
failure fires its intended injection, is observed commonly, preserves the full
owned/sentinel/ghost block vector and trace, and then succeeds on a clean
same-plan retry. Separate cases prove identical duplicates remain valid,
rank-skewed `k`, local/received block-domain rejection, SIMILAR communicator
rejection, CONGRUENT acceptance, ownership/range skew, and asymmetric topology
rejection.

After final formatting and review fixes, the affected debug matrix passed
28/28 registrations: all 21 dedicated block-down registrations, the complete
graph v-cycle state regression, and six retained legacy semantic cases with the
old probe inactive.

## Deterministic oracles

### Exact cross-rank block stage

Pinned upstream `5935f349f65f1788a9b68fcf6d853e698d86956d` has no Task 5 trace layer, so
provenance is intentionally split rather than misrepresented:

- the externally visible two-cycle partition oracle comes from pinned pristine
  upstream algorithm sources; and
- the trace-instrumented block-stage oracle comes from the reviewed legacy base
  `1624be49aa17d32bc2b366a4c6885a37d8836d11` before tag-11 migration.

For the principal real two-rank cross-interface fixture, the exact four-record
owner/ghost vector SHA-256 is:

```text
1f5db34f0222bab748b1b5aec8930c4dd8381fd424376445545fecc6b3bcf473
```

The exact four-record canonical block-propagation trace SHA-256 is:

```text
8378ca475340469fd2cf47067486adfbeeff60b06d997166b2e6f0d682420fb5
```

The final dedicated fixture requires those exact semantics after migration.

### Exact two-vcycle external parity

The installed `ParHIPPartitionKWay` C API does not expose an arbitrary cycle
count, but a FASTMESH API call is genuinely a two-cycle path:
`configuration::standard` sets `num_vcycles = 2`, and `configuration::fast`
does not override it. The active CLI argtable likewise has no arbitrary cycle
knob; its parser contained only a dormant option object. For the exact
ULTRAFASTMESH oracle below, whose preset otherwise forces one cycle, the same
one-line test-harness exposure was applied in isolated pinned-upstream and
candidate builds. No algorithm source changed, and the exposure was removed
from the final worktree.

The tuple was:

```text
graph=examples/rgg_n_2_15_s0.graph
ranks=2
k=2
preconfiguration=ultrafastmesh
seed=0
num_vcycles=2
```

Pinned upstream produced the same result twice. The final candidate then
matched it byte for byte:

```text
partition lines: 32768
partition SHA-256: b414f04471a0f7d371e031f55f9a456907f1a275cb55a43b7287e836ae03875f
cut: 185
balance: 1.01996
byte cmp status: 0
```

The focused complete-graph distribution regression automates the exact state
lifecycle that caused the original candidate failure. This task deliberately
uses that narrow automated regression plus the stronger 32,768-vertex external
two-cycle parity gate. A checked-in public-C-API FASTMESH integration fixture is
deferred to Task 8's shared cube harness so this already-large PMPI task does
not duplicate another graph fixture or generator.

### Established full oracle

The definitive final trace-ON debug run used the ordinary tuple:

```text
graph=examples/rgg_n_2_15_s0.graph
ranks=2
k=2
preconfiguration=ultrafastmesh
seed=0
```

It retained cut 196 and balance 1.0094. The exact 32,768-line partition has
SHA-256:

```text
a600acd0029ee9342e4f7c5b041d224a308b874c85fd35bdbcd3a5a73d48cdd0
```

Removing rank-local v3 headers and globally sorting complete semantic records
produced exactly 436,721 records with SHA-256:

```text
a179bb30213dbb26638657a1d611e951a8bf900b817647fc03d4c700a83f0a18
```

Both artifacts compare byte-for-byte equal to the established oracle. The
ordinary tuple has zero block-propagation records, which is why it is retained
as the full-algorithm parity gate but is not presented as block-stage coverage.

## Build and verification

Every shell, configure, build, test, oracle, formatting, audit, and Git command
used:

```text
systemd-run --user --scope -p MemoryHigh=28G -p MemoryMax=30G -p MemorySwapMax=2G
```

Builds used at most two jobs. The verified environment was CMake 4.3.0, GCC
16.2.1, and Open MPI 5.0.10. Debug and release capability headers both contain:

```text
KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_C=0
KAHIP_HAVE_MPI_INEIGHBOR_ALLTOALLV_C=0
KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT=1
KAHIP_HAVE_MPI_NEIGHBOR_ALLTOALLV_INIT_C=0
```

This host therefore executes the MPI-3.1 blocking path. The guarded PMPI
interposer proves exact classic-versus-genuinely-available `_c` selection; the
sporadic production operation never selects immediate or persistent paths.

Final results:

- trace-ON full debug build completed at `-j2`;
- affected debug matrix: 28/28 passed in 2.25 seconds;
- full trace-ON debug CTest: 154/154 passed in 11.31 seconds;
- trace-OFF full release build completed at `-j2`, followed by an explicit
  `ninja: no work to do` build;
- full trace-OFF release CTest: 149/149 passed in 8.14 seconds;
- exact two-vcycle candidate/upstream partition comparison: byte equal;
- exact established partition and 436,721-record trace comparison: byte equal;
- production source audit: one dense and one synchronous neighborhood call,
  with no tag-11 P2P, obsolete `m_send_buffers`, immediate, or persistent code;
- touched C++ formatting and final `git diff --check`: clean.

## Review and residuals

Self-review checked transaction ordering, common domains, owner and ghost exact
coverage, queried MPI order, duplicate semantics, zero-work behavior, block
type fidelity, no pre-convergence mutation or trace, same-plan retry, state and
trace commit ordering, root v-cycle state preservation, and fail-fast
boundaries. Independent source review found the final production source sound;
its commit-boundary and legacy-interposer findings were closed before the full
verification above. Independent final live-diff/report review issued ACCEPT
with no remaining Critical, Important, or Minor Task 7C4 findings.

Deliberately deferred:

- a checked-in public-C-API FASTMESH two-cycle integration fixture using Task
  8's shared cube harness;
- MPI-4 `_c` runtime execution on a host whose real link probe reports it
  unavailable;
- tag 3, incremental labels, DSPAC, and evolutionary gossip migrations; and
- the shared cube generator, million-cell run, and performance/RSS gates in
  Task 8.
