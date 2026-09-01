# PMPI collective-byte and topology-setup measurement

`pmpi_collective_bytes.cpp` is a standalone `LD_PRELOAD` interposer. It is not
registered in KaHIP's CMake build, so enabling measurement cannot change the
candidate or pristine-baseline binaries.

## Measurement definition

For every successful intercepted call, each rank records logical endpoint
payload bytes:

- `sent_bytes` is the sum of nonnegative send counts times
  `PMPI_Type_size_x(sendtype)`;
- `received_bytes` is the corresponding receive sum;
- self traffic is reported separately and remains included in sent/received;
- `MPI_PROC_NULL` Cartesian neighbors contribute zero;
- dense `MPI_IN_PLACE` sends are derived from the receive layout, while
  neighborhood in-place use is marked invalid;
- derived datatypes use their MPI type size, not extent or buffer displacement.

These are application-level logical bytes. They do not estimate eager/rendezvous
protocol headers, retransmission, shared-memory copies, NIC traffic, topology
effects, compression, or physical network-link bytes. Summing sent and received
counts both endpoints of a transfer by design.

The same interposer measures successful `MPI_Dist_graph_create` and
`MPI_Dist_graph_create_adjacent` calls with `CLOCK_MONOTONIC`. Each rank records
the exact call count and elapsed nanoseconds for both constructors. This timing
covers the MPI constructor call only; KaHIP's graph analysis, sorting, and
payload exchanges remain in their normal stage and end-to-end measurements.

The interposer covers `MPI_Alltoall`, `MPI_Alltoallv`, `MPI_Ialltoallv`, their
fixed/v neighborhood counterparts, MPI-4 large-count `_c` v variants, and
persistent neighborhood v variants. Optional MPI-4 symbols are resolved from
the next PMPI library at runtime, so an MPI-3 header can still build an
interposer capable of counting vendor-provided persistent collectives.

Persistent init captures immutable layout metadata but charges no bytes. A
successful `MPI_Start` or `MPI_Startall` charges one generation; successful
`MPI_Request_free` removes the metadata. Nonblocking ordinary collectives are
charged once at successful initiation. A mutex protects counters and request
metadata for `MPI_THREAD_MULTIPLE`.

Counts use checked unsigned 128-bit arithmetic. Negative counts, PMPI topology
or datatype-query failures, overflow, duplicate request identities, or live
persistent records at finalization set `complete=false`. The instrumentation
never replaces the underlying MPI return code.

## Output and finalization

Set `KAHIP_PMPI_BYTES_DIRECTORY` to an existing shared directory unique to one
MPI run. Immediately before `PMPI_Finalize`, each rank writes
`rank-<rank>.json` using an exclusive temporary file, `fsync`, and an atomic
hard link. No collective is called during finalization; the harness aggregates
rank files after `mpiexec` exits. Missing, duplicate, malformed, internally
inconsistent, or `complete=false` files make both byte and topology reporting
incomplete.

## Standalone build and smoke test

Use the same MPI compiler that built ParHIP:

```shell
ci/run-limited mpicxx -std=c++23 -fPIC -shared \
  -Wall -Wextra -Wconversion -Werror \
  ci/performance/pmpi_collective_bytes.cpp -ldl \
  -o /tmp/kahip-pmpi-collective-bytes.so

ci/run-limited mpicxx -std=c++23 -Wall -Wextra -Wconversion -Werror \
  -DKAHIP_PMPI_SMOKE_HAVE_PERSISTENT=1 \
  ci/performance/pmpi_collective_bytes_smoke.cpp \
  -o /tmp/kahip-pmpi-collective-bytes-smoke
```

Only set a smoke capability macro when that symbol is declared by the local
MPI headers. `KAHIP_PMPI_SMOKE_HAVE_LARGE_COUNTS=1` and
`KAHIP_PMPI_SMOKE_HAVE_PERSISTENT_C=1` exercise the corresponding `_c` paths on
an MPI-4 implementation.

Run with a new output directory:

```shell
ci/run-limited env \
  KAHIP_PMPI_BYTES_DIRECTORY=/tmp/kahip-pmpi-records \
  LD_PRELOAD=/tmp/kahip-pmpi-collective-bytes.so \
  mpiexec -n 2 /tmp/kahip-pmpi-collective-bytes-smoke
```

The harness passes the preload path to the per-rank `fork`/`execvpe` wrapper,
which prepends it to any existing `LD_PRELOAD` only in the ParHIP child. It is
not loaded into `systemd-run`, `mpiexec`, or the Python wrapper. The harness
creates a unique per-run output directory, requires exactly one complete record
per rank, matches its PID/hostname to the measured child, recomputes every rank
total from operation records, and records global send/receive, maximum-rank
endpoint bytes, and exact distributed-graph topology constructor timings.
