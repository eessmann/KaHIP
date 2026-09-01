# ParHIP acceptance and performance harness

`parhip_harness.py` compares a pristine upstream build with the current
candidate using paired, alternating runs. It is deliberately fail-closed: an
incomplete matrix, a failed verifier, mismatched rank placement, missing stage
records, missing per-rank RSS, missing collective-byte records, or missing
distributed-graph topology timing cannot be reported as acceptance.

## Required build contract

Both executables must come from equivalent Release builds with
`OPTIMIZED_OUTPUT=ON`. The harness reads each `CMakeCache.txt`, probes the
compiler, CMake, and MPI launcher, hashes the executable and linked MPI
libraries, and rejects differing compiler/MPI/flags/generator provenance. The
baseline source must be clean and exactly match `pinned_upstream_revision`.
The candidate may be dirty; its binary diff hash and an untracked-file manifest
are recorded before and after the run and must remain unchanged.

Every configured configure, build, generator, MPI benchmark, verifier, and
version/provenance command is prefixed with `run_limited`. Builds are capped at
two jobs through the command validator and environment. Benchmark runs are
serial and alternate baseline/candidate, then candidate/baseline, to limit
order bias.

## Configuration

The input is JSON with `schema_version` 1. Paths are resolved relative to the
configuration file. MPI `postflags` must be empty because launcher-specific
postflag placement could put them after the per-rank wrapper and change
ParHIP's arguments.

```json
{
  "schema_version": 1,
  "pinned_upstream_revision": "0123456789abcdef0123456789abcdef01234567",
  "run_limited": "/work/KaHIP/ci/run-limited",
  "mpiexec": {
    "executable": "mpiexec",
    "numproc_flag": "-n",
    "preflags": ["--bind-to", "core"],
    "postflags": []
  },
  "variants": {
    "baseline": {
      "source_directory": "/work/kahip-upstream",
      "build_directory": "/work/kahip-upstream-build",
      "executable": "/work/kahip-upstream-build/parallel/parallel_src/parhip",
      "configure_command": ["cmake", "-S", ".", "-B", "/work/kahip-upstream-build", "-DCMAKE_BUILD_TYPE=Release", "-DOPTIMIZED_OUTPUT=ON"],
      "build_command": ["cmake", "--build", "/work/kahip-upstream-build", "--parallel", "2"]
    },
    "candidate": {
      "source_directory": "/work/KaHIP",
      "build_directory": "/work/kahip-candidate-build",
      "executable": "/work/kahip-candidate-build/parallel/parallel_src/parhip",
      "configure_command": ["cmake", "-S", ".", "-B", "/work/kahip-candidate-build", "-DCMAKE_BUILD_TYPE=Release", "-DOPTIMIZED_OUTPUT=ON"],
      "build_command": ["cmake", "--build", "/work/kahip-candidate-build", "--parallel", "2"]
    }
  },
  "fixtures": [
    {
      "name": "cube100",
      "dimensions": [100, 100, 100],
      "generator": "/work/kahip-candidate-build/parallel/parallel_src/tests/kahip_cube_generator",
      "verifier": "/work/kahip-candidate-build/parallel/parallel_src/tests/kahip_cube_partition_verify"
    }
  ],
  "matrix": {
    "seeds": [1, 2, 3, 4, 5],
    "ranks": [2, 4],
    "blocks": [4],
    "preconfigurations": ["fastmesh"],
    "imbalance_percent": [3],
    "repetitions": 5
  },
  "bootstrap": {"iterations": 10000, "seed": 1729, "min_pairs": 20},
  "collective_bytes_interposer": "/work/libkahip_pmpi_collective_bytes.so",
  "output_directory": "/work/results/kahip-acceptance-001",
  "timeout_seconds": 7200,
  "concurrency": 2,
  "environment": {}
}
```

Use `--prepare` to run the configured build commands; omit it for existing
builds:

```shell
ci/run-limited python3 -m ci.performance.parhip_harness \
  --config /work/kahip-acceptance.json --prepare
```

The outer invocation is scoped as well as every operation launched by the
harness. The harness refuses an existing output directory so fixed-name
`tmppartition.txtp` files can never be stale.

## Results and gates

`events.jsonl` is fsynced after each fixture/run, raw stdout and stderr are
retained and hashed, and `results.json` is written atomically. Each partition
is independently checked by `kahip_cube_partition_verify`; its vertex count,
block count, balance, and weighted cut must agree with the case and ParHIP's
self-reported cut.

Cut quality uses the median across repetitions for each paired seed. Aggregate
cut may regress by at most 1%, and no graph/configuration median may regress by
more than 3%. Runtime and maximum per-rank RSS use paired bootstrap resampling
of whole samples and compute `median(candidate*) / median(baseline*)`; the 95%
upper confidence bound must be at most 1.05. Hostname and CPU-affinity maps must
match rank by rank.

The external PMPI instrument records `MPI_Dist_graph_create` and
`MPI_Dist_graph_create_adjacent` call counts and monotonic wall time separately
for every rank. This measures the MPI topology constructor itself without
adding mutable timing state to either KaHIP binary. The result reports summed
rank time, the maximum rank time, and the constructor counts for baseline and
candidate; topology timing is a reporting requirement, while the paired
end-to-end confidence interval remains the performance gate.

## Synthetic tests

The tests do not build or benchmark KaHIP:

```shell
ci/run-limited python3 -m unittest discover -s ci/performance/tests -v
```

See `pmpi-collective-bytes.md` for the independently built PMPI measurement
library and its exact accounting boundary.
