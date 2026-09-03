from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from ci.performance import parhip_harness
from ci.performance import rank_runner


FIXTURES = Path(__file__).with_name("fixtures")


def synthetic_record(
    variant: str,
    *,
    fixture: str = "cube100",
    ranks: int = 4,
    seed: int = 1,
    repetition: int = 0,
    cut: int = 100,
    runtime: float = 10.0,
    rss: int = 1_000_000,
) -> dict[str, object]:
    return {
        "variant": variant,
        "case": {
            "fixture": fixture,
            "ranks": ranks,
            "blocks": 4,
            "preconfiguration": "fastmesh",
            "imbalance_percent": 3,
            "seed": seed,
            "repetition": repetition,
        },
        "end_to_end_seconds": runtime,
        "max_rank_rss_bytes": rss,
        "rank_placement": [
            {"rank": rank, "hostname": "node-a", "cpu_affinity": [rank]}
            for rank in range(ranks)
        ],
        "parhip": {
            "final_cut": cut,
            "stage_events": [
                {
                    "cycle": 0,
                    "level": None,
                    "stage": "coarsening_total",
                    "seconds": 1.0,
                }
            ],
        },
        "verification": {
            "balanced": True,
            "vertices": 1_000_000,
            "blocks": 4,
            "maximum_block_weight": 257_500,
            "block_weights": [250_000, 250_000, 250_000, 250_000],
            "weighted_cut": cut,
        },
    }


def synthetic_config() -> dict[str, object]:
    return {
        "schema_version": 1,
        "pinned_upstream_revision": "a" * 40,
        "run_limited": "/repo/ci/run-limited",
        "mpiexec": {
            "executable": "/usr/bin/mpiexec",
            "numproc_flag": "-n",
            "preflags": ["--bind-to", "core"],
            "postflags": [],
        },
        "variants": {
            "baseline": {
                "source_directory": "/src/baseline",
                "build_directory": "/build/baseline",
                "executable": "/build/baseline/parhip",
                "configure_command": ["cmake", "--preset", "release"],
                "build_command": ["cmake", "--build", "--preset", "release"],
            },
            "candidate": {
                "source_directory": "/src/candidate",
                "build_directory": "/build/candidate",
                "executable": "/build/candidate/parhip",
                "configure_command": ["cmake", "--preset", "release"],
                "build_command": ["cmake", "--build", "--preset", "release"],
            },
        },
        "fixtures": [
            {
                "name": "cube4",
                "dimensions": [4, 4, 4],
                "generator": "/build/candidate/kahip_cube_generator",
                "verifier": "/build/candidate/kahip_cube_partition_verify",
            }
        ],
        "matrix": {
            "seeds": [1, 2],
            "ranks": [2, 4],
            "blocks": [4],
            "preconfigurations": ["fastmesh"],
            "imbalance_percent": [3],
            "repetitions": 2,
        },
        "bootstrap": {"iterations": 1_000, "seed": 17, "min_pairs": 4},
        "output_directory": "/results",
        "timeout_seconds": 3_600,
        "concurrency": 2,
    }


class OutputParserTests(unittest.TestCase):
    def test_parser_preserves_stage_events_and_sums_repeated_levels(self) -> None:
        # Break caught: accepting only the last recursive level loses stage time.
        parsed = parhip_harness.parse_parhip_output(
            (FIXTURES / "parhip-output.log").read_text(encoding="utf-8")
        )

        self.assertEqual(parsed["final_cut"], 41)
        self.assertEqual(parsed["final_balance"], 1.02)
        self.assertEqual(parsed["partition_seconds"], 2.5)
        self.assertEqual(parsed["startup_dummy_seconds"], 0.125)
        self.assertEqual(parsed["input_ready_elapsed_seconds"], 0.4)
        self.assertAlmostEqual(parsed["stage_totals_seconds"]["contraction"], 0.5)
        self.assertAlmostEqual(
            parsed["stage_totals_seconds"]["label_compression_coarsening"],
            0.25,
        )
        self.assertEqual(len(parsed["stage_events"]), 11)
        self.assertEqual(
            parsed["stage_events"][0],
            {
                "cycle": 0,
                "level": 1,
                "stage": "label_compression_coarsening",
                "seconds": 0.1,
            },
        )

    def test_parser_rejects_output_without_required_final_metrics(self) -> None:
        # Break caught: a crashed/truncated run must not become a benchmark sample.
        with self.assertRaisesRegex(
            parhip_harness.ParseError, "missing final partition metrics"
        ):
            parhip_harness.parse_parhip_output(
                "log>cycle: 0 level: 1 contraction took 0.2\n"
            )

    def test_verifier_parser_requires_the_canonical_invariant_record(self) -> None:
        # Break caught: trusting ParHIP's self-reported cut instead of the verifier.
        parsed = parhip_harness.parse_verifier_output(
            "verified vertices=64 blocks=2 maximum-block-weight=33 "
            "block-weights=[32,32] weighted-cut=28\n"
        )
        self.assertEqual(
            parsed,
            {
                "balanced": True,
                "vertices": 64,
                "blocks": 2,
                "maximum_block_weight": 33,
                "block_weights": [32, 32],
                "weighted_cut": 28,
            },
        )
        with self.assertRaises(parhip_harness.ParseError):
            parhip_harness.parse_verifier_output(
                "verified vertices=64 blocks=2 weighted-cut=28\n"
            )

    def test_parser_rejects_duplicate_setup_times_and_truncated_stages(self) -> None:
        # Break caught: accepting a partial or concatenated log as a complete run.
        duplicate = (FIXTURES / "parhip-output.log").read_text(encoding="utf-8")
        duplicate += "running collective dummy operations took 0.250\n"
        with self.assertRaisesRegex(parhip_harness.ParseError, "duplicate"):
            parhip_harness.parse_parhip_output(duplicate)

        with self.assertRaisesRegex(parhip_harness.ParseError, "stage timing"):
            parhip_harness.parse_parhip_output(
                "log>cycle: 0 level: 1 contraction took 0.2\n"
                "log>total partitioning time elapsed 0.2\n"
                "log>final edge cut 1\n"
                "log>final balance 1.0\n"
            )


class SchedulingAndCommandTests(unittest.TestCase):
    def test_pair_order_alternates_ab_then_ba(self) -> None:
        # Break caught: always running baseline first biases paired timings.
        self.assertEqual(
            [parhip_harness.alternating_variant_order(index) for index in range(4)],
            [
                ("baseline", "candidate"),
                ("candidate", "baseline"),
                ("baseline", "candidate"),
                ("candidate", "baseline"),
            ],
        )

    def test_every_generated_operation_is_prefixed_by_run_limited(self) -> None:
        # Break caught: bypassing the requested cgroup wrapper for one operation.
        wrapper = Path("/repo/ci/run-limited")
        command = parhip_harness.limited_command(
            wrapper, ["mpiexec", "-n", "4", "parhip"]
        )
        self.assertEqual(
            command,
            ["/repo/ci/run-limited", "mpiexec", "-n", "4", "parhip"],
        )

    def test_benchmark_command_wraps_each_rank_without_moving_mpi_preflags(
        self,
    ) -> None:
        # Break caught: putting the Python wrapper before mpiexec measures the
        # launcher, while putting MPI postflags after it changes ParHIP argv.
        case = {
            "fixture": "cube100",
            "ranks": 4,
            "blocks": 4,
            "preconfiguration": "fastmesh",
            "imbalance_percent": 3,
            "seed": 1,
            "repetition": 0,
        }
        command = parhip_harness.build_benchmark_command(
            run_limited=Path("/repo/ci/run-limited"),
            mpiexec=Path("/usr/bin/mpiexec"),
            numproc_flag="-n",
            mpi_preflags=["--bind-to", "core"],
            python_executable=Path("/usr/bin/python3"),
            rank_runner=Path("/repo/ci/performance/rank_runner.py"),
            rank_metrics_directory=Path("/results/ranks"),
            parhip=Path("/build/parhip"),
            graph=Path("/results/cube100.graph"),
            case=case,
        )
        self.assertEqual(
            command,
            [
                "/repo/ci/run-limited",
                "/usr/bin/mpiexec",
                "-n",
                "4",
                "--bind-to",
                "core",
                "/usr/bin/python3",
                "/repo/ci/performance/rank_runner.py",
                "--metrics-directory",
                "/results/ranks",
                "--",
                "/build/parhip",
                "/results/cube100.graph",
                "--k=4",
                "--preconfiguration=fastmesh",
                "--seed=1",
                "--imbalance=3",
                "--save_partition",
            ],
        )
        instrumented = parhip_harness.build_benchmark_command(
            run_limited=Path("/repo/ci/run-limited"),
            mpiexec=Path("/usr/bin/mpiexec"),
            numproc_flag="-n",
            mpi_preflags=[],
            python_executable=Path("/usr/bin/python3"),
            rank_runner=Path("/repo/ci/performance/rank_runner.py"),
            rank_metrics_directory=Path("/results/ranks"),
            parhip=Path("/build/parhip"),
            graph=Path("/results/cube100.graph"),
            case=case,
            collective_bytes_interposer=Path("/tools/pmpi-bytes.so"),
        )
        separator = instrumented.index("--")
        self.assertEqual(
            instrumented[separator - 2 : separator],
            ["--preload", "/tools/pmpi-bytes.so"],
        )
        self.assertNotIn("LD_PRELOAD", instrumented)

    def test_case_matrix_is_deterministic_and_keeps_repetitions_paired(self) -> None:
        # Break caught: independently shuffling variants destroys paired samples.
        cases = parhip_harness.enumerate_cases(synthetic_config())
        self.assertEqual(len(cases), 8)
        self.assertEqual(
            cases[0],
            {
                "fixture": "cube4",
                "ranks": 2,
                "blocks": 4,
                "preconfiguration": "fastmesh",
                "imbalance_percent": 3,
                "seed": 1,
                "repetition": 0,
            },
        )
        self.assertEqual(cases[1]["repetition"], 1)
        self.assertEqual(cases[-1]["ranks"], 4)


class ConfigurationTests(unittest.TestCase):
    def test_valid_config_is_accepted_but_mpi_postflags_are_rejected(self) -> None:
        # Break caught: CMake-style postflags would land between the rank wrapper
        # and ParHIP and could be interpreted as application arguments.
        parhip_harness.validate_config(synthetic_config())
        invalid = synthetic_config()
        invalid["mpiexec"] = dict(invalid["mpiexec"], postflags=["--oversubscribe"])
        with self.assertRaisesRegex(ValueError, "postflags.*empty"):
            parhip_harness.validate_config(invalid)

    def test_config_rejects_unknown_schema_and_more_than_two_jobs(self) -> None:
        # Break caught: silently accepting a newer schema or excessive concurrency.
        invalid_schema = synthetic_config()
        invalid_schema["schema_version"] = 2
        with self.assertRaisesRegex(ValueError, "schema_version"):
            parhip_harness.validate_config(invalid_schema)

        excessive = synthetic_config()
        excessive["concurrency"] = 3
        with self.assertRaisesRegex(ValueError, "concurrency"):
            parhip_harness.validate_config(excessive)

    def test_explicit_build_parallelism_cannot_exceed_configured_cap(self) -> None:
        # Break caught: CMAKE_BUILD_PARALLEL_LEVEL=2 cannot override `-j32`.
        parhip_harness.validate_job_cap(
            ["cmake", "--build", "--preset", "release", "--parallel", "2"],
            cap=2,
        )
        for command in (
            ["cmake", "--build", ".", "-j32"],
            ["cmake", "--build", ".", "--parallel=4"],
            ["cmake", "--build", ".", "--parallel", "8"],
        ):
            with self.assertRaisesRegex(ValueError, "parallelism"):
                parhip_harness.validate_job_cap(command, cap=2)

        unbounded = synthetic_config()
        unbounded["variants"]["candidate"]["build_command"] = ["ninja"]
        with self.assertRaisesRegex(ValueError, "explicit bounded parallelism"):
            parhip_harness.validate_config(unbounded)

        bounded = synthetic_config()
        bounded["variants"]["candidate"]["build_command"] = ["ninja", "-j2"]
        parhip_harness.validate_config(bounded)


class BootstrapAndGateTests(unittest.TestCase):
    def test_bootstrap_estimate_is_ratio_of_medians_not_median_of_ratios(self) -> None:
        # Break caught: the two estimators differ materially for heterogeneous pairs.
        result = parhip_harness.bootstrap_ratio_of_medians(
            [(1.0, 2.0), (10.0, 90.0), (100.0, 110.0)],
            iterations=2_000,
            seed=17,
        )
        self.assertEqual(result["estimate"], 9.0)
        self.assertLessEqual(result["lower"], result["estimate"])
        self.assertGreaterEqual(result["upper"], result["estimate"])
        self.assertEqual(
            result,
            parhip_harness.bootstrap_ratio_of_medians(
                [(1.0, 2.0), (10.0, 90.0), (100.0, 110.0)],
                iterations=2_000,
                seed=17,
            ),
        )

    def test_constant_scaling_has_an_exact_bootstrap_interval(self) -> None:
        # Break caught: resampling baseline and candidate independently breaks pairing.
        result = parhip_harness.bootstrap_ratio_of_medians(
            [(10.0, 10.4), (20.0, 20.8), (30.0, 31.2), (40.0, 41.6)],
            iterations=500,
            seed=3,
        )
        self.assertAlmostEqual(result["estimate"], 1.04)
        self.assertAlmostEqual(result["lower"], 1.04)
        self.assertAlmostEqual(result["upper"], 1.04)

    def test_acceptance_passes_only_when_all_quality_and_ci_gates_pass(self) -> None:
        # Break caught: reporting success from runtime while ignoring cut/RSS/balance.
        records: list[dict[str, object]] = []
        for repetition in range(6):
            records.append(
                synthetic_record(
                    "baseline",
                    repetition=repetition,
                    seed=1 + repetition % 2,
                    runtime=10.0 + repetition,
                    rss=1_000_000 + repetition * 1_000,
                )
            )
            records.append(
                synthetic_record(
                    "candidate",
                    repetition=repetition,
                    seed=1 + repetition % 2,
                    runtime=(10.0 + repetition) * 1.04,
                    rss=int((1_000_000 + repetition * 1_000) * 1.03),
                )
            )

        result = parhip_harness.evaluate_acceptance(
            records, bootstrap_iterations=1_000, bootstrap_seed=9, min_pairs=6
        )

        self.assertTrue(result["passed"])
        self.assertTrue(result["gates"]["balanced_partitions"]["passed"])
        self.assertTrue(result["gates"]["aggregate_cut"]["passed"])
        self.assertTrue(result["gates"]["per_configuration_cut"]["passed"])
        self.assertLessEqual(
            result["gates"]["runtime_ci"]["upper"], 1.05
        )
        self.assertLessEqual(result["gates"]["rss_ci"]["upper"], 1.05)

    def test_per_configuration_cut_can_fail_while_aggregate_cut_passes(self) -> None:
        # Break caught: an aggregate improvement must not hide one 4% regression.
        records = [
            synthetic_record("baseline", fixture="cube-a", cut=100),
            synthetic_record("candidate", fixture="cube-a", cut=104),
            synthetic_record("baseline", fixture="cube-b", cut=100),
            synthetic_record("candidate", fixture="cube-b", cut=96),
        ]
        result = parhip_harness.evaluate_acceptance(
            records, bootstrap_iterations=100, bootstrap_seed=1, min_pairs=2
        )
        self.assertTrue(result["gates"]["aggregate_cut"]["passed"])
        self.assertFalse(result["gates"]["per_configuration_cut"]["passed"])
        self.assertFalse(result["passed"])

    def test_positive_candidate_cut_fails_against_zero_baseline(self) -> None:
        # Break caught: serializing infinity/NaN or treating division by zero as pass.
        records = [
            synthetic_record("baseline", cut=0),
            synthetic_record("candidate", cut=1),
        ]
        result = parhip_harness.evaluate_acceptance(
            records, bootstrap_iterations=100, bootstrap_seed=1, min_pairs=1
        )
        self.assertFalse(result["gates"]["aggregate_cut"]["passed"])
        self.assertIsNone(result["gates"]["aggregate_cut"]["ratio"])
        json.dumps(result, allow_nan=False)

    def test_upper_confidence_bound_not_point_estimate_controls_runtime_gate(
        self,
    ) -> None:
        # Break caught: an apparently favorable median can hide an unstable tail.
        candidate_times = [0.5, 0.5, 1.5]
        records = []
        for repetition, candidate_time in enumerate(candidate_times):
            records.extend(
                [
                    synthetic_record(
                        "baseline", repetition=repetition, runtime=1.0
                    ),
                    synthetic_record(
                        "candidate",
                        repetition=repetition,
                        runtime=candidate_time,
                    ),
                ]
            )
        result = parhip_harness.evaluate_acceptance(
            records, bootstrap_iterations=2_000, bootstrap_seed=2, min_pairs=3
        )
        self.assertLessEqual(result["gates"]["runtime_ci"]["estimate"], 1.05)
        self.assertGreater(result["gates"]["runtime_ci"]["upper"], 1.05)
        self.assertFalse(result["gates"]["runtime_ci"]["passed"])

    def test_expected_matrix_rejects_wholly_missing_and_unplanned_pairs(self) -> None:
        # Break caught: removing both variants of the slowest tuple must not pass.
        planned = [
            synthetic_record("baseline", repetition=index)["case"]
            for index in range(2)
        ]
        records = [
            synthetic_record("baseline", repetition=0),
            synthetic_record("candidate", repetition=0),
        ]
        with self.assertRaisesRegex(ValueError, "missing planned"):
            parhip_harness.evaluate_acceptance(
                records,
                bootstrap_iterations=100,
                bootstrap_seed=1,
                min_pairs=1,
                expected_cases=planned,
            )
        with self.assertRaisesRegex(ValueError, "unplanned"):
            parhip_harness.evaluate_acceptance(
                [
                    *records,
                    synthetic_record("baseline", fixture="unplanned"),
                    synthetic_record("candidate", fixture="unplanned"),
                ],
                bootstrap_iterations=100,
                bootstrap_seed=1,
                min_pairs=1,
                expected_cases=[planned[0]],
            )

    def test_rank_placement_must_match_within_each_pair(self) -> None:
        # Break caught: comparing different host/core placement as if paired.
        baseline = synthetic_record("baseline")
        candidate = synthetic_record("candidate")
        candidate["rank_placement"][2]["cpu_affinity"] = [63]
        result = parhip_harness.evaluate_acceptance(
            [baseline, candidate],
            bootstrap_iterations=100,
            bootstrap_seed=1,
            min_pairs=1,
        )
        self.assertFalse(result["gates"]["rank_placement"]["passed"])
        self.assertFalse(result["passed"])


class RankMeasurementTests(unittest.TestCase):
    def test_rank_environment_accepts_known_names_and_rejects_disagreement(self) -> None:
        # Break caught: attributing one rank's RSS to another rank.
        self.assertEqual(rank_runner.resolve_rank({"OMPI_COMM_WORLD_RANK": "3"}), 3)
        self.assertEqual(rank_runner.resolve_rank({"PMI_RANK": "2"}), 2)
        self.assertEqual(rank_runner.resolve_rank({"PMIX_RANK": "1"}), 1)
        self.assertEqual(
            rank_runner.resolve_rank(
                {"OMPI_COMM_WORLD_RANK": "4", "PMI_RANK": "4"}
            ),
            4,
        )
        with self.assertRaisesRegex(ValueError, "disagree"):
            rank_runner.resolve_rank(
                {"OMPI_COMM_WORLD_RANK": "0", "PMI_RANK": "1"}
            )
        with self.assertRaisesRegex(ValueError, "no MPI rank"):
            rank_runner.resolve_rank({})

    def test_rank_runner_preserves_inheritable_descriptors_and_records_wait4_rss(
        self,
    ) -> None:
        # Break caught: subprocess close_fds severs PMI/PMIx descriptors.
        with tempfile.TemporaryDirectory() as directory:
            read_fd, write_fd = os.pipe()
            os.set_inheritable(write_fd, True)
            try:
                environment = dict(os.environ)
                environment["OMPI_COMM_WORLD_RANK"] = "0"
                environment["KAHIP_TEST_INHERITED_FD"] = str(write_fd)
                command = [
                    sys.executable,
                    "-c",
                    "import os; os.write(int(os.environ['KAHIP_TEST_INHERITED_FD']), b'ok')",
                ]
                result = rank_runner.run_rank(
                    command,
                    metrics_directory=Path(directory),
                    environment=environment,
                )
                os.close(write_fd)
                write_fd = -1
                self.assertEqual(os.read(read_fd, 2), b"ok")
            finally:
                os.close(read_fd)
                if write_fd >= 0:
                    os.close(write_fd)

            self.assertEqual(result, 0)
            metrics = json.loads(
                (Path(directory) / "rank-0.json").read_text(encoding="utf-8")
            )
            self.assertEqual(metrics["rank"], 0)
            self.assertEqual(metrics["return_code"], 0)
            self.assertGreater(metrics["max_rss_bytes"], 0)
            self.assertTrue(metrics["cpu_affinity"])

    def test_rank_runner_refuses_to_replace_an_existing_rank_record(self) -> None:
        # Break caught: two writers for one rank silently replace evidence.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            environment = dict(os.environ, OMPI_COMM_WORLD_RANK="0")
            self.assertEqual(
                rank_runner.run_rank(
                    [sys.executable, "-c", "pass"],
                    metrics_directory=root,
                    environment=environment,
                ),
                0,
            )
            with self.assertRaises(FileExistsError):
                rank_runner.run_rank(
                    [sys.executable, "-c", "pass"],
                    metrics_directory=root,
                    environment=environment,
                )

    def test_rank_runner_forwards_termination_to_its_parhip_child(self) -> None:
        # Break caught: a launcher timeout must not orphan a continuing rank.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            child_pid_path = root / "child.pid"
            environment = dict(os.environ, OMPI_COMM_WORLD_RANK="0")
            process = subprocess.Popen(
                [
                    sys.executable,
                    str(Path(rank_runner.__file__)),
                    "--metrics-directory",
                    str(root / "metrics"),
                    "--",
                    sys.executable,
                    "-c",
                    (
                        "import os,time,pathlib; "
                        f"pathlib.Path({str(child_pid_path)!r}).write_text(str(os.getpid())); "
                        "time.sleep(30)"
                    ),
                ],
                env=environment,
            )
            deadline = time.monotonic() + 5
            while not child_pid_path.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(child_pid_path.exists())
            child_pid = int(child_pid_path.read_text(encoding="utf-8"))
            process.terminate()
            self.assertEqual(process.wait(timeout=5), 128 + 15)
            metrics = json.loads(
                (root / "metrics" / "rank-0.json").read_text(encoding="utf-8")
            )
            self.assertEqual(metrics["return_code"], 128 + 15)
            with self.assertRaises(ProcessLookupError):
                os.kill(child_pid, 0)

    def test_rank_runner_applies_preload_only_to_the_exec_child(self) -> None:
        # Break caught: preloading mpiexec/the wrapper can create false rank files.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            preload = next(
                (
                    path
                    for path in (
                        Path("/usr/lib64/libm.so.6"),
                        Path("/lib/x86_64-linux-gnu/libm.so.6"),
                        Path("/usr/lib/x86_64-linux-gnu/libm.so.6"),
                    )
                    if path.is_file()
                ),
                None,
            )
            if preload is None:
                self.skipTest("no harmless shared library is available")
            observed = root / "observed.txt"
            environment = dict(os.environ, OMPI_COMM_WORLD_RANK="0")
            original = os.environ.get("LD_PRELOAD")
            result = rank_runner.run_rank(
                [
                    sys.executable,
                    "-c",
                    (
                        "import os,pathlib; "
                        f"pathlib.Path({str(observed)!r}).write_text(os.environ['LD_PRELOAD'])"
                    ),
                ],
                metrics_directory=root / "metrics",
                environment=environment,
                preload=preload,
            )
            self.assertEqual(result, 0)
            self.assertEqual(observed.read_text(encoding="utf-8"), str(preload))
            self.assertEqual(os.environ.get("LD_PRELOAD"), original)

    def test_rank_metric_aggregation_uses_maximum_rank_not_a_launcher_value(
        self,
    ) -> None:
        # Break caught: summing rank RSS or reading mpiexec's RSS instead.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for rank, rss in enumerate((100, 700, 250)):
                (root / f"rank-{rank}.json").write_text(
                    json.dumps(
                        {
                            "rank": rank,
                            "return_code": 0,
                            "max_rss_bytes": rss,
                            "hostname": "node-a",
                            "cpu_affinity": [rank],
                        }
                    ),
                    encoding="utf-8",
                )
            result = parhip_harness.read_rank_metrics(root, expected_ranks=3)
        self.assertEqual(result["max_rank_rss_bytes"], 700)
        self.assertEqual(result["per_rank_rss_bytes"], [100, 700, 250])
        self.assertEqual(result["rank_placement"][2]["cpu_affinity"], [2])


class PerRunValidationTests(unittest.TestCase):
    def test_partition_metrics_must_match_fixture_case_and_parhip_cut(self) -> None:
        # Break caught: accepting the verifier record for a different graph/k.
        case = synthetic_record("baseline")["case"]
        fixture = {"name": "cube100", "dimensions": [100, 100, 100]}
        parsed = {"final_cut": 41}
        verification = {
            "balanced": True,
            "vertices": 1_000_000,
            "blocks": 4,
            "weighted_cut": 41,
        }
        parhip_harness.validate_partition_metrics(
            case=case,
            fixture=fixture,
            parhip=parsed,
            verification=verification,
        )
        with self.assertRaisesRegex(ValueError, "self-reported cut"):
            parhip_harness.validate_partition_metrics(
                case=case,
                fixture=fixture,
                parhip={"final_cut": 42},
                verification=verification,
            )
        with self.assertRaisesRegex(ValueError, "vertex count"):
            parhip_harness.validate_partition_metrics(
                case=case,
                fixture=fixture,
                parhip=parsed,
                verification=dict(verification, vertices=64),
            )
        with self.assertRaisesRegex(ValueError, "block count"):
            parhip_harness.validate_partition_metrics(
                case=case,
                fixture=fixture,
                parhip=parsed,
                verification=dict(verification, blocks=2),
            )

    def test_run_directory_must_be_new_so_partition_cannot_be_stale(self) -> None:
        # Break caught: a failed ParHIP launch must not reuse tmppartition.txtp.
        with tempfile.TemporaryDirectory() as directory:
            run_directory = Path(directory) / "run"
            parhip_harness.create_run_directory(run_directory)
            (run_directory / "tmppartition.txtp").write_text(
                "stale\n", encoding="utf-8"
            )
            with self.assertRaises(FileExistsError):
                parhip_harness.create_run_directory(run_directory)

    def test_synthetic_run_records_logs_rank_rss_and_fresh_verification(self) -> None:
        # Break caught: a runner that never invokes the verifier can appear valid.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            graph = root / "cube4.graph"
            graph.write_text("synthetic graph\n", encoding="utf-8")
            calls: list[list[str]] = []

            def fake_executor(
                command: list[str],
                *,
                cwd: Path,
                environment: dict[str, str],
                timeout_seconds: float,
            ) -> parhip_harness.CommandResult:
                del environment, timeout_seconds
                calls.append(command)
                if "rank_runner.py" in " ".join(command):
                    (cwd / "tmppartition.txtp").write_text(
                        "0\n" * 64, encoding="utf-8"
                    )
                    metrics_flag = command.index("--metrics-directory")
                    metrics = Path(command[metrics_flag + 1])
                    metrics.mkdir()
                    for rank in range(2):
                        (metrics / f"rank-{rank}.json").write_text(
                            json.dumps(
                                {
                                    "rank": rank,
                                    "return_code": 0,
                                    "max_rss_bytes": 100 + rank,
                                    "hostname": "node-a",
                                    "cpu_affinity": [rank],
                                }
                            ),
                            encoding="utf-8",
                        )
                    return parhip_harness.CommandResult(
                        return_code=0,
                        stdout=(FIXTURES / "parhip-output.log").read_text(
                            encoding="utf-8"
                        ),
                        stderr="",
                        elapsed_seconds=3.5,
                    )
                return parhip_harness.CommandResult(
                    return_code=0,
                    stdout=(
                        "verified vertices=64 blocks=4 maximum-block-weight=17 "
                        "block-weights=[16,16,16,16] weighted-cut=41\n"
                    ),
                    stderr="",
                    elapsed_seconds=0.01,
                )

            fixture = {
                "name": "cube4",
                "dimensions": [4, 4, 4],
                "verifier": "/tools/verifier",
                "graph_path": str(graph),
            }
            case = {
                "fixture": "cube4",
                "ranks": 2,
                "blocks": 4,
                "preconfiguration": "fastmesh",
                "imbalance_percent": 3,
                "seed": 1,
                "repetition": 0,
            }
            record = parhip_harness.execute_one_run(
                variant="candidate",
                case=case,
                fixture=fixture,
                graph=graph,
                parhip=Path("/build/parhip"),
                run_directory=root / "run",
                run_limited=Path("/repo/ci/run-limited"),
                mpiexec=Path("/usr/bin/mpiexec"),
                numproc_flag="-n",
                mpi_preflags=[],
                python_executable=Path("/usr/bin/python3"),
                rank_runner=Path("/repo/ci/performance/rank_runner.py"),
                environment={},
                timeout_seconds=60,
                executor=fake_executor,
            )

        self.assertEqual(len(calls), 2)
        self.assertTrue(all(call[0] == "/repo/ci/run-limited" for call in calls))
        self.assertEqual(record["verification"]["weighted_cut"], 41)
        self.assertEqual(record["max_rank_rss_bytes"], 101)
        self.assertEqual(record["end_to_end_seconds"], 3.5)
        self.assertEqual(record["partition_sha256"], record["artifacts"]["partition_sha256"])
        self.assertIn("stdout_sha256", record["artifacts"])


class BuildAndGitProvenanceTests(unittest.TestCase):
    def test_stage_timing_requires_matching_output_enabled_release_builds(self) -> None:
        # Break caught: comparing a NOOUTPUT build with no parseable stage records.
        baseline = {
            "build_type": "Release",
            "optimized_output": "ON",
            "compiler": "/usr/bin/g++",
            "compiler_version": "g++ 16.2.1",
            "release_flags": "-O3 -DNDEBUG",
            "link_flags": "-Wl,--as-needed",
            "cmake_generator": "Ninja",
            "cmake_version": "cmake version 4.1.0",
            "mpi_executable": "/usr/bin/mpiexec",
            "mpi_version": "Open MPI 5.0.10",
            "mpi_cache_identity": {"MPI_CXX_COMPILER": "/usr/bin/mpicxx"},
            "linked_mpi_libraries": [
                {
                    "name": "libmpi.so.40",
                    "path": "/usr/lib/libmpi.so.40",
                    "sha256": "e" * 64,
                }
            ],
        }
        parhip_harness.validate_build_equivalence(
            baseline, dict(baseline), require_stage_timings=True
        )
        disabled = dict(baseline, optimized_output="OFF")
        with self.assertRaisesRegex(ValueError, "OPTIMIZED_OUTPUT=ON"):
            parhip_harness.validate_build_equivalence(
                baseline, disabled, require_stage_timings=True
            )
        debug = dict(baseline, build_type="Debug")
        with self.assertRaisesRegex(ValueError, "Release"):
            parhip_harness.validate_build_equivalence(
                baseline, debug, require_stage_timings=True
            )

    def test_git_provenance_distinguishes_pristine_baseline_and_dirty_candidate(
        self,
    ) -> None:
        # Break caught: accepting a dirty baseline or omitting candidate dirtiness.
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            subprocess.run(["git", "init", "-q", repository], check=True)
            tracked = repository / "tracked.txt"
            tracked.write_text("baseline\n", encoding="utf-8")
            subprocess.run(["git", "-C", repository, "add", "tracked.txt"], check=True)
            subprocess.run(
                [
                    "git",
                    "-C",
                    repository,
                    "-c",
                    "user.name=Harness Test",
                    "-c",
                    "user.email=harness@example.invalid",
                    "-c",
                    "commit.gpgsign=false",
                    "commit",
                    "-qm",
                    "baseline",
                ],
                check=True,
            )
            baseline = parhip_harness.collect_git_provenance(repository)
            parhip_harness.validate_pristine_baseline(
                baseline, baseline["revision"]
            )

            tracked.write_text("candidate\n", encoding="utf-8")
            (repository / "new.txt").write_text("untracked\n", encoding="utf-8")
            candidate = parhip_harness.collect_git_provenance(repository)

        self.assertFalse(candidate["clean"])
        self.assertNotEqual(candidate["diff_sha256"], baseline["diff_sha256"])
        self.assertEqual(candidate["untracked_files"], ["new.txt"])
        self.assertIsNotNone(candidate["untracked_manifest_sha256"])
        with self.assertRaisesRegex(ValueError, "not pristine"):
            parhip_harness.validate_pristine_baseline(
                candidate, baseline["revision"]
            )

    def test_cmake_cache_provenance_uses_effective_release_flags(self) -> None:
        # Break caught: comparing only CMAKE_CXX_FLAGS_RELEASE misses common flags.
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "CMakeCache.txt"
            cache.write_text(
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/g++\n"
                "CMAKE_CXX_FLAGS:STRING=-march=native\n"
                "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\n"
                "CMAKE_EXE_LINKER_FLAGS:STRING=-Wl,--as-needed\n"
                "CMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=\n"
                "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                "MPIEXEC_EXECUTABLE:FILEPATH=/usr/bin/mpiexec\n"
                "MPI_CXX_COMPILER:FILEPATH=/usr/bin/mpicxx\n"
                "OPTIMIZED_OUTPUT:BOOL=ON\n",
                encoding="utf-8",
            )
            values = parhip_harness.parse_cmake_cache(cache)
            provenance = parhip_harness.build_provenance_from_cache(
                values,
                compiler_version="g++ 16.2.1",
                mpi_version="Open MPI 5.0.10",
                executable_sha256="f" * 64,
                cmake_version="cmake version 4.1.0",
                linked_mpi_libraries=[
                    {
                        "name": "libmpi.so.40",
                        "path": "/usr/lib/libmpi.so.40",
                        "sha256": "e" * 64,
                    }
                ],
            )
        self.assertEqual(provenance["release_flags"], "-march=native -O3 -DNDEBUG")
        self.assertEqual(provenance["optimized_output"], "ON")
        self.assertEqual(provenance["executable_sha256"], "f" * 64)
        self.assertEqual(provenance["link_flags"], "-Wl,--as-needed")
        self.assertEqual(provenance["cmake_generator"], "Ninja")
        self.assertEqual(
            provenance["mpi_cache_identity"],
            {"MPI_CXX_COMPILER": "/usr/bin/mpicxx"},
        )


class ResultContractTests(unittest.TestCase):
    def test_unavailable_collective_bytes_make_full_acceptance_incomplete(
        self,
    ) -> None:
        # Break caught: an uninstrumented run must not be serialized as accepted.
        records = [synthetic_record("baseline"), synthetic_record("candidate")]
        document = parhip_harness.assemble_result_document(
            records=records,
            provenance={"test": True},
            bootstrap_iterations=100,
            bootstrap_seed=3,
            min_pairs=1,
        )
        self.assertTrue(document["analysis"]["quality_performance_passed"])
        self.assertEqual(
            document["analysis"]["collective_bytes"]["status"], "incomplete"
        )
        self.assertFalse(document["analysis"]["collective_bytes"]["passed"])
        self.assertFalse(document["analysis"]["acceptance_complete"])
        self.assertFalse(document["analysis"]["passed"])
        json.dumps(document, allow_nan=False)

    def test_collective_record_aggregation_requires_every_rank_and_exact_totals(
        self,
    ) -> None:
        # Break caught: launcher-level or partial-rank counters are not complete.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for rank, sent in enumerate((10, 30)):
                operation = {
                    "name": "MPI_Alltoallv",
                    "calls": 1,
                    "sent_bytes": sent,
                    "received_bytes": sent,
                    "self_sent_bytes": 2,
                    "self_received_bytes": 2,
                }
                (root / f"rank-{rank}.json").write_text(
                    json.dumps(
                        {
                            "schema_version": 1,
                            "rank": rank,
                            "pid": 1000 + rank,
                            "hostname": "node-a",
                            "complete": True,
                            "error": None,
                            "live_persistent_requests": 0,
                            "operations": [operation],
                            "topology_setup": {
                                "operations": [
                                    {
                                        "name": "MPI_Dist_graph_create_adjacent",
                                        "calls": rank + 1,
                                        "elapsed_nanoseconds": 100 + rank,
                                    },
                                    {
                                        "name": "MPI_Dist_graph_create",
                                        "calls": 0,
                                        "elapsed_nanoseconds": 0,
                                    },
                                ],
                                "totals": {
                                    "calls": rank + 1,
                                    "elapsed_nanoseconds": 100 + rank,
                                },
                            },
                            "totals": {
                                "calls": 1,
                                "sent_bytes": sent,
                                "received_bytes": sent,
                                "self_sent_bytes": 2,
                                "self_received_bytes": 2,
                            },
                        }
                    ),
                    encoding="utf-8",
                )
            result = parhip_harness.read_collective_metrics(
                root, expected_ranks=2
            )
            self.assertEqual(result["global_sent_bytes"], 40)
            self.assertEqual(result["global_received_bytes"], 40)
            self.assertEqual(result["max_rank_endpoint_bytes"], 60)
            self.assertEqual(result["global_topology_setup_calls"], 3)
            self.assertEqual(
                result["max_rank_topology_setup_nanoseconds"], 101
            )

            (root / "rank-1.json").unlink()
            with self.assertRaisesRegex(ValueError, "expected 2"):
                parhip_harness.read_collective_metrics(root, expected_ranks=2)

            (root / "rank-1.json").write_text("{malformed\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                parhip_harness.read_collective_metrics(root, expected_ranks=2)

    def test_complete_topology_measurements_close_the_reporting_gate(self) -> None:
        # Break caught: complete external topology timing must not remain marked
        # incomplete merely because ParHIP itself prints no topology timer.
        records = [synthetic_record("baseline"), synthetic_record("candidate")]
        for record, topology_nanoseconds in zip(
            records, (0, 25_000), strict=True
        ):
            record["collective_bytes"] = {
                "status": "complete",
                "global_sent_bytes": 100,
                "global_received_bytes": 100,
            }
            record["topology_timing"] = {
                "status": "complete",
                "measurement": "PMPI distributed-graph construction wall time",
                "global_calls": 0 if topology_nanoseconds == 0 else 2,
                "global_rank_nanoseconds": topology_nanoseconds,
                "max_rank_nanoseconds": topology_nanoseconds,
            }

        document = parhip_harness.assemble_result_document(
            records=records,
            provenance={"test": True},
            bootstrap_iterations=100,
            bootstrap_seed=3,
            min_pairs=1,
        )

        topology = document["analysis"]["setup_topology_timing"]
        self.assertEqual(topology["status"], "complete")
        self.assertTrue(topology["passed"])
        self.assertEqual(
            topology["variants"]["candidate"]["max_rank_nanoseconds"],
            25_000,
        )
        self.assertTrue(document["analysis"]["acceptance_complete"])
        self.assertTrue(document["analysis"]["passed"])


class PmpiSourceContractTests(unittest.TestCase):
    def test_interposer_covers_branch_collectives_and_persistent_lifecycle(
        self,
    ) -> None:
        # Break caught: an interposer that counts init rather than Start inflates
        # reusable persistent neighborhood exchanges.
        source = (
            Path(parhip_harness.__file__).with_name("pmpi_collective_bytes.cpp")
        ).read_text(encoding="utf-8")
        required = {
            "MPI_Alltoall",
            "MPI_Alltoallv",
            "MPI_Ialltoallv",
            "MPI_Neighbor_alltoall",
            "MPI_Neighbor_alltoallv",
            "MPI_Ineighbor_alltoallv",
            "MPI_Alltoallv_c",
            "MPI_Ialltoallv_c",
            "MPI_Neighbor_alltoallv_c",
            "MPI_Ineighbor_alltoallv_c",
            "MPI_Neighbor_alltoallv_init",
            "MPI_Neighbor_alltoallv_init_c",
            "MPI_Start",
            "MPI_Startall",
            "MPI_Request_free",
            "MPI_Dist_graph_create",
            "MPI_Dist_graph_create_adjacent",
            "MPI_Finalize",
        }
        for symbol in required:
            self.assertIn(f'extern "C" int {symbol}', source)
        self.assertIn("PMPI_Type_size_x", source)
        self.assertIn("charge_persistent", source)
        self.assertIn("elapsed_nanoseconds", source)
        self.assertIn("KAHIP_PMPI_BYTES_DIRECTORY", source)
        self.assertNotIn("PMPI_Allreduce", source)


if __name__ == "__main__":
    unittest.main()
