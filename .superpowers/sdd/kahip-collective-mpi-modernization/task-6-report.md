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
