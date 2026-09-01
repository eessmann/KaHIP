"""Acceptance-quality ParHIP comparison harness.

The module keeps parsing, scheduling, and statistical decisions pure so they
can be pressure-tested without compiling or running ParHIP.  The executable
orchestrator is defined below those independently testable functions.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import copy
from dataclasses import dataclass
from datetime import datetime, timezone
from fractions import Fraction
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import shutil
import signal
import socket
import statistics
import subprocess
import sys
import time
from typing import Any, Callable, Iterable, Mapping, Sequence


class ParseError(ValueError):
    """Raised when a tool emits a noncanonical result record."""


@dataclass(frozen=True)
class CommandResult:
    return_code: int
    stdout: str
    stderr: str
    elapsed_seconds: float


CommandExecutor = Callable[..., CommandResult]


_FLOAT = r"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?"
_LEVEL_STAGE = re.compile(
    rf"^log>cycle:\s*([0-9]+)\s+level:\s*(-?[0-9]+)\s+"
    rf"(parallel label compression|contraction|projection|"
    rf"label compression refinement)\s+took\s+({_FLOAT})\s*$"
)
_CYCLE_STAGE = re.compile(
    rf"^log>cycle:\s*([0-9]+)\s+"
    rf"(coarsening|initial partitioning|uncoarsening)\s+took\s+({_FLOAT})\s*$"
)
_STAGE_NAMES = {
    "parallel label compression": "label_compression_coarsening",
    "contraction": "contraction",
    "coarsening": "coarsening_total",
    "initial partitioning": "initial_partitioning",
    "projection": "projection",
    "label compression refinement": "label_compression_refinement",
    "uncoarsening": "uncoarsening",
}
_PARTITION_TOTAL = re.compile(
    rf"^log>total partitioning time elapsed\s+({_FLOAT})\s*$"
)
_FINAL_CUT = re.compile(r"^log>final edge cut\s+([0-9]+)\s*$")
_FINAL_BALANCE = re.compile(rf"^log>final balance\s+({_FLOAT})\s*$")
_DUMMY = re.compile(rf"^running collective dummy operations took\s+({_FLOAT})\s*$")
_BARE_TOOK = re.compile(rf"^took\s+({_FLOAT})\s*$")
_VERIFIER = re.compile(
    r"^verified vertices=([0-9]+) blocks=([0-9]+) "
    r"maximum-block-weight=([0-9]+) block-weights=\[([0-9]+(?:,[0-9]+)*)\] "
    r"weighted-cut=([0-9]+)$"
)


def _finite_float(text: str, context: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value < 0:
        raise ParseError(f"{context} is not a finite nonnegative number")
    return value


def parse_parhip_output(output: str) -> dict[str, Any]:
    events: list[dict[str, Any]] = []
    totals: defaultdict[str, float] = defaultdict(float)
    partition_seconds: float | None = None
    final_cut: int | None = None
    final_balance: float | None = None
    dummy_seconds: float | None = None
    input_elapsed: float | None = None

    for line in output.splitlines():
        if match := _LEVEL_STAGE.fullmatch(line):
            cycle, level, raw_stage, seconds = match.groups()
            stage = _STAGE_NAMES[raw_stage]
            value = _finite_float(seconds, f"{stage} duration")
            event = {
                "cycle": int(cycle),
                "level": int(level),
                "stage": stage,
                "seconds": value,
            }
            events.append(event)
            totals[stage] += value
            continue
        if match := _CYCLE_STAGE.fullmatch(line):
            cycle, raw_stage, seconds = match.groups()
            stage = _STAGE_NAMES[raw_stage]
            value = _finite_float(seconds, f"{stage} duration")
            events.append(
                {
                    "cycle": int(cycle),
                    "level": None,
                    "stage": stage,
                    "seconds": value,
                }
            )
            totals[stage] += value
            continue
        if match := _PARTITION_TOTAL.fullmatch(line):
            if partition_seconds is not None:
                raise ParseError("duplicate total partitioning time")
            partition_seconds = _finite_float(match.group(1), "partition duration")
            continue
        if match := _FINAL_CUT.fullmatch(line):
            if final_cut is not None:
                raise ParseError("duplicate final edge cut")
            final_cut = int(match.group(1))
            continue
        if match := _FINAL_BALANCE.fullmatch(line):
            if final_balance is not None:
                raise ParseError("duplicate final balance")
            final_balance = _finite_float(match.group(1), "final balance")
            continue
        if match := _DUMMY.fullmatch(line):
            if dummy_seconds is not None:
                raise ParseError("duplicate dummy-operation duration")
            dummy_seconds = _finite_float(match.group(1), "dummy-operation duration")
            continue
        if match := _BARE_TOOK.fullmatch(line):
            if input_elapsed is not None:
                raise ParseError("duplicate input-ready elapsed time")
            input_elapsed = _finite_float(match.group(1), "input-ready elapsed time")

    if partition_seconds is None or final_cut is None or final_balance is None:
        raise ParseError("missing final partition metrics")
    required_stages = {"coarsening_total", "initial_partitioning", "uncoarsening"}
    missing_stages = sorted(required_stages.difference(totals))
    if missing_stages:
        raise ParseError(
            "missing required stage timing records "
            f"({', '.join(missing_stages)}); use Release with OPTIMIZED_OUTPUT=ON"
        )
    return {
        "partition_seconds": partition_seconds,
        "final_cut": final_cut,
        "final_balance": final_balance,
        "startup_dummy_seconds": dummy_seconds,
        "input_ready_elapsed_seconds": input_elapsed,
        "stage_events": events,
        "stage_totals_seconds": dict(sorted(totals.items())),
    }


def parse_verifier_output(output: str) -> dict[str, Any]:
    record = output.strip()
    match = _VERIFIER.fullmatch(record)
    if match is None:
        raise ParseError("partition verifier emitted a noncanonical record")
    vertices, blocks, maximum, weights, cut = match.groups()
    parsed_weights = [int(weight) for weight in weights.split(",")]
    if len(parsed_weights) != int(blocks):
        raise ParseError("partition verifier block-weight count is inconsistent")
    return {
        "balanced": True,
        "vertices": int(vertices),
        "blocks": int(blocks),
        "maximum_block_weight": int(maximum),
        "block_weights": parsed_weights,
        "weighted_cut": int(cut),
    }


def alternating_variant_order(pair_index: int) -> tuple[str, str]:
    if pair_index < 0:
        raise ValueError("pair index must be nonnegative")
    return (
        ("baseline", "candidate")
        if pair_index % 2 == 0
        else ("candidate", "baseline")
    )


def limited_command(wrapper: Path, command: Sequence[str]) -> list[str]:
    if not command:
        raise ValueError("limited command must not be empty")
    return [str(wrapper), *map(str, command)]


def run_external_command(
    command: list[str],
    *,
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: float,
) -> CommandResult:
    if not command:
        raise ValueError("external command must not be empty")
    started_ns = time.monotonic_ns()
    process = subprocess.Popen(
        command,
        cwd=cwd,
        env=dict(environment),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        start_new_session=True,
    )
    timed_out = False
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
    elapsed_seconds = (time.monotonic_ns() - started_ns) / 1_000_000_000
    if timed_out:
        stderr += (
            f"\nacceptance harness timeout after {timeout_seconds:g} seconds\n"
        )
        return_code = 124
    else:
        return_code = process.returncode
    return CommandResult(
        return_code=return_code,
        stdout=stdout,
        stderr=stderr,
        elapsed_seconds=elapsed_seconds,
    )


def build_benchmark_command(
    *,
    run_limited: Path,
    mpiexec: Path,
    numproc_flag: str,
    mpi_preflags: Sequence[str],
    python_executable: Path,
    rank_runner: Path,
    rank_metrics_directory: Path,
    parhip: Path,
    graph: Path,
    case: Mapping[str, Any],
    collective_bytes_interposer: Path | None = None,
) -> list[str]:
    """Build the exact launcher command for one paired benchmark sample.

    The rank wrapper belongs *after* the MPI launcher: one wrapper process is
    created for each rank and can therefore use ``wait4`` to measure that
    rank's ParHIP child.  MPI postflags are deliberately absent from this API;
    their placement is launcher-specific and can leak into ParHIP's argv.
    """

    ranks = _positive_integer(case.get("ranks"), "case ranks")
    blocks = _positive_integer(case.get("blocks"), "case blocks")
    seed = _nonnegative_integer(case.get("seed"), "case seed")
    imbalance = _nonnegative_integer(
        case.get("imbalance_percent"), "case imbalance_percent"
    )
    preconfiguration = case.get("preconfiguration")
    if not isinstance(preconfiguration, str) or not preconfiguration:
        raise ValueError("case preconfiguration must be a nonempty string")
    if not numproc_flag:
        raise ValueError("MPI process-count flag must not be empty")
    if any(not isinstance(flag, str) or not flag for flag in mpi_preflags):
        raise ValueError("MPI preflags must be nonempty strings")

    rank_arguments = [
        str(mpiexec),
        numproc_flag,
        str(ranks),
        *mpi_preflags,
        str(python_executable),
        str(rank_runner),
        "--metrics-directory",
        str(rank_metrics_directory),
    ]
    if collective_bytes_interposer is not None:
        rank_arguments.extend(["--preload", str(collective_bytes_interposer)])
    rank_arguments.extend(
        [
            "--",
            str(parhip),
            str(graph),
            f"--k={blocks}",
            f"--preconfiguration={preconfiguration}",
            f"--seed={seed}",
            f"--imbalance={imbalance}",
            "--save_partition",
        ]
    )
    return limited_command(run_limited, rank_arguments)


def _integer(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{name} must be an integer")
    return value


def _positive_integer(value: Any, name: str) -> int:
    result = _integer(value, name)
    if result <= 0:
        raise ValueError(f"{name} must be positive")
    return result


def _nonnegative_integer(value: Any, name: str) -> int:
    result = _integer(value, name)
    if result < 0:
        raise ValueError(f"{name} must be nonnegative")
    return result


def _mapping(parent: Mapping[str, Any], name: str) -> Mapping[str, Any]:
    value = parent.get(name)
    if not isinstance(value, Mapping):
        raise ValueError(f"{name} must be an object")
    return value


def _nonempty_string(parent: Mapping[str, Any], name: str) -> str:
    value = parent.get(name)
    if not isinstance(value, str) or not value:
        raise ValueError(f"{name} must be a nonempty string")
    return value


def _string_list(parent: Mapping[str, Any], name: str, *, empty: bool) -> list[str]:
    value = parent.get(name)
    if not isinstance(value, list) or any(
        not isinstance(item, str) or not item for item in value
    ):
        raise ValueError(f"{name} must be a list of nonempty strings")
    if not empty and not value:
        raise ValueError(f"{name} must not be empty")
    return value


def _integer_list(
    parent: Mapping[str, Any], name: str, *, positive: bool
) -> list[int]:
    value = parent.get(name)
    if not isinstance(value, list) or not value:
        raise ValueError(f"{name} must be a nonempty list")
    parser = _positive_integer if positive else _nonnegative_integer
    return [parser(item, f"{name} item") for item in value]


def validate_config(config: Mapping[str, Any]) -> None:
    if not isinstance(config, Mapping):
        raise ValueError("configuration must be an object")
    if config.get("schema_version") != 1:
        raise ValueError("schema_version must be exactly 1")
    pinned = _nonempty_string(config, "pinned_upstream_revision")
    if re.fullmatch(r"[0-9a-f]{40}", pinned) is None:
        raise ValueError("pinned_upstream_revision must be a full lowercase SHA-1")
    _nonempty_string(config, "run_limited")
    _nonempty_string(config, "output_directory")
    if "python_executable" in config:
        _nonempty_string(config, "python_executable")
    if "collective_bytes_interposer" in config:
        _nonempty_string(config, "collective_bytes_interposer")
    if "environment" in config:
        configured_environment = config["environment"]
        if not isinstance(configured_environment, Mapping) or any(
            not isinstance(name, str)
            or not name
            or not isinstance(value, str)
            for name, value in configured_environment.items()
        ):
            raise ValueError("environment must map nonempty names to strings")

    concurrency = _positive_integer(config.get("concurrency", 2), "concurrency")
    if concurrency > 2:
        raise ValueError("concurrency must not exceed two")
    timeout = config.get("timeout_seconds")
    if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or timeout <= 0:
        raise ValueError("timeout_seconds must be positive")

    mpi = _mapping(config, "mpiexec")
    _nonempty_string(mpi, "executable")
    _nonempty_string(mpi, "numproc_flag")
    _string_list(mpi, "preflags", empty=True)
    postflags = _string_list(mpi, "postflags", empty=True)
    if postflags:
        raise ValueError("mpiexec postflags must be empty; use preflags")

    variants = _mapping(config, "variants")
    if set(variants) != {"baseline", "candidate"}:
        raise ValueError("variants must contain exactly baseline and candidate")
    for variant_name in ("baseline", "candidate"):
        variant = variants[variant_name]
        if not isinstance(variant, Mapping):
            raise ValueError(f"{variant_name} variant must be an object")
        for field in ("source_directory", "build_directory", "executable"):
            _nonempty_string(variant, field)
        configure_command = _string_list(variant, "configure_command", empty=False)
        build_command = _string_list(variant, "build_command", empty=False)
        validate_job_cap(configure_command, cap=concurrency)
        explicit_build_cap = validate_job_cap(build_command, cap=concurrency)
        build_launcher = Path(build_command[0]).name
        environment_capped = (
            (build_launcher == "cmake" and "--build" in build_command)
            or build_launcher in ("make", "gmake")
        )
        if not explicit_build_cap and not environment_capped:
            raise ValueError(
                f"{variant_name} build_command requires explicit bounded parallelism"
            )

    fixtures = config.get("fixtures")
    if not isinstance(fixtures, list) or not fixtures:
        raise ValueError("fixtures must be a nonempty list")
    fixture_names: set[str] = set()
    for fixture in fixtures:
        if not isinstance(fixture, Mapping):
            raise ValueError("each fixture must be an object")
        name = _nonempty_string(fixture, "name")
        if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", name) is None:
            raise ValueError(f"fixture name is not path-safe: {name}")
        if name in fixture_names:
            raise ValueError(f"duplicate fixture name: {name}")
        fixture_names.add(name)
        dimensions = fixture.get("dimensions")
        if not isinstance(dimensions, list) or len(dimensions) != 3:
            raise ValueError(f"fixture {name} dimensions must contain nx, ny, nz")
        for dimension in dimensions:
            _positive_integer(dimension, f"fixture {name} dimension")
        _nonempty_string(fixture, "verifier")
        graph_path = fixture.get("graph_path")
        if graph_path is None:
            _nonempty_string(fixture, "generator")
        elif not isinstance(graph_path, str) or not graph_path:
            raise ValueError(f"fixture {name} graph_path must be a nonempty string")

    matrix = _mapping(config, "matrix")
    _integer_list(matrix, "seeds", positive=False)
    _integer_list(matrix, "ranks", positive=True)
    _integer_list(matrix, "blocks", positive=True)
    _integer_list(matrix, "imbalance_percent", positive=False)
    _string_list(matrix, "preconfigurations", empty=False)
    repetitions = _positive_integer(matrix.get("repetitions"), "matrix repetitions")
    if repetitions < 2:
        raise ValueError("matrix repetitions must be at least two")

    bootstrap = _mapping(config, "bootstrap")
    _positive_integer(bootstrap.get("iterations"), "bootstrap iterations")
    _nonnegative_integer(bootstrap.get("seed"), "bootstrap seed")
    minimum_pairs = _positive_integer(
        bootstrap.get("min_pairs"), "bootstrap min_pairs"
    )
    if minimum_pairs < 2:
        raise ValueError("bootstrap min_pairs must be at least two")
    planned_pairs = (
        len(fixtures)
        * len(matrix["seeds"])
        * len(matrix["ranks"])
        * len(matrix["blocks"])
        * len(matrix["preconfigurations"])
        * len(matrix["imbalance_percent"])
        * repetitions
    )
    if minimum_pairs > planned_pairs:
        raise ValueError("bootstrap min_pairs exceeds the planned paired matrix")


def validate_job_cap(command: Sequence[str], *, cap: int) -> bool:
    _positive_integer(cap, "parallelism cap")
    index = 0
    explicitly_bounded = False
    while index < len(command):
        argument = command[index]
        requested: int | None = None
        if argument in ("-j", "--parallel"):
            if index + 1 >= len(command):
                raise ValueError(f"unbounded parallelism flag {argument!r}")
            index += 1
            try:
                requested = int(command[index], 10)
            except ValueError as error:
                raise ValueError("parallelism must be an integer") from error
        elif re.fullmatch(r"-j[0-9]+", argument):
            requested = int(argument[2:], 10)
        elif argument.startswith("--parallel="):
            try:
                requested = int(argument.partition("=")[2], 10)
            except ValueError as error:
                raise ValueError("parallelism must be an integer") from error
        if requested is not None and (requested <= 0 or requested > cap):
            raise ValueError(
                f"requested parallelism {requested} exceeds configured cap {cap}"
            )
        if requested is not None:
            explicitly_bounded = True
        index += 1
    return explicitly_bounded


def _resolved_path(base_directory: Path, value: str) -> str:
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = base_directory / path
    return str(path.resolve())


def _resolved_executable(base_directory: Path, value: str) -> str:
    if "/" in value or value.startswith("."):
        return _resolved_path(base_directory, value)
    located = shutil.which(value)
    return str(Path(located).resolve()) if located is not None else value


def load_config(path: Path) -> dict[str, Any]:
    path = path.resolve()
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid JSON configuration {path}: {error}") from error
    if not isinstance(raw, dict):
        raise ValueError("configuration root must be an object")
    validate_config(raw)
    config = copy.deepcopy(raw)
    base = path.parent
    for name in ("run_limited", "output_directory"):
        config[name] = _resolved_path(base, config[name])
    config["mpiexec"]["executable"] = _resolved_executable(
        base, config["mpiexec"]["executable"]
    )
    for variant in config["variants"].values():
        for name in ("source_directory", "build_directory", "executable"):
            variant[name] = _resolved_path(base, variant[name])
    for fixture in config["fixtures"]:
        for name in ("generator", "verifier", "graph_path"):
            if name in fixture:
                fixture[name] = _resolved_path(base, fixture[name])
    if "python_executable" in config:
        config["python_executable"] = _resolved_executable(
            base, config["python_executable"]
        )
    else:
        config["python_executable"] = str(Path(sys.executable).resolve())
    if "collective_bytes_interposer" in config:
        config["collective_bytes_interposer"] = _resolved_path(
            base, config["collective_bytes_interposer"]
        )
    config["configuration_file"] = str(path)
    return config


def benchmark_environment(
    config: Mapping[str, Any], environment: Mapping[str, str]
) -> dict[str, str]:
    result = dict(environment)
    configured = config.get("environment", {})
    if not isinstance(configured, Mapping) or any(
        not isinstance(name, str)
        or not name
        or not isinstance(value, str)
        for name, value in configured.items()
    ):
        raise ValueError("environment must map nonempty names to strings")
    result.update(configured)
    concurrency = str(_positive_integer(config.get("concurrency", 2), "concurrency"))
    result["CMAKE_BUILD_PARALLEL_LEVEL"] = concurrency
    result["VCPKG_MAX_CONCURRENCY"] = concurrency
    result["MAKEFLAGS"] = f"-j{concurrency}"
    return result


def enumerate_cases(config: Mapping[str, Any]) -> list[dict[str, Any]]:
    validate_config(config)
    matrix = config["matrix"]
    repetitions = range(matrix["repetitions"])
    cases: list[dict[str, Any]] = []
    for fixture, ranks, blocks, preconfiguration, imbalance, seed, repetition in itertools.product(
        (entry["name"] for entry in config["fixtures"]),
        matrix["ranks"],
        matrix["blocks"],
        matrix["preconfigurations"],
        matrix["imbalance_percent"],
        matrix["seeds"],
        repetitions,
    ):
        cases.append(
            {
                "fixture": fixture,
                "ranks": ranks,
                "blocks": blocks,
                "preconfiguration": preconfiguration,
                "imbalance_percent": imbalance,
                "seed": seed,
                "repetition": repetition,
            }
        )
    return cases


def _percentile(sorted_values: Sequence[float], probability: float) -> float:
    if not sorted_values:
        raise ValueError("percentile requires at least one value")
    position = (len(sorted_values) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return sorted_values[lower]
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def _ratio_of_medians(pairs: Sequence[tuple[float, float]]) -> float:
    baseline = statistics.median(pair[0] for pair in pairs)
    candidate = statistics.median(pair[1] for pair in pairs)
    if baseline <= 0:
        raise ValueError("baseline timing/RSS median must be positive")
    if candidate < 0 or not math.isfinite(baseline) or not math.isfinite(candidate):
        raise ValueError("timing/RSS samples must be finite and nonnegative")
    return candidate / baseline


def bootstrap_ratio_of_medians(
    pairs: Sequence[tuple[float, float]],
    *,
    iterations: int,
    seed: int,
) -> dict[str, float | int]:
    if not pairs:
        raise ValueError("bootstrap requires at least one paired sample")
    if iterations <= 0:
        raise ValueError("bootstrap iteration count must be positive")
    normalized = [(float(left), float(right)) for left, right in pairs]
    estimate = _ratio_of_medians(normalized)
    generator = random.Random(seed)
    count = len(normalized)
    distribution = []
    for _ in range(iterations):
        sample = [normalized[generator.randrange(count)] for _ in range(count)]
        distribution.append(_ratio_of_medians(sample))
    distribution.sort()
    return {
        "method": "paired-percentile-bootstrap-ratio-of-medians",
        "confidence": 0.95,
        "iterations": iterations,
        "seed": seed,
        "sample_count": count,
        "estimate": estimate,
        "lower": _percentile(distribution, 0.025),
        "upper": _percentile(distribution, 0.975),
    }


_CASE_FIELDS = (
    "fixture",
    "ranks",
    "blocks",
    "preconfiguration",
    "imbalance_percent",
    "seed",
    "repetition",
)
_SEED_FIELDS = _CASE_FIELDS[:-1]
_CONFIGURATION_FIELDS = _SEED_FIELDS[:-1]


def _key(case: Mapping[str, Any], fields: Sequence[str]) -> tuple[Any, ...]:
    try:
        return tuple(case[field] for field in fields)
    except KeyError as error:
        raise ValueError(f"run case is missing {error.args[0]!r}") from error


def _median_fraction(values: Sequence[int | Fraction]) -> Fraction:
    if not values:
        raise ValueError("median requires at least one value")
    ordered = sorted(Fraction(value) for value in values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def _finite_ratio(numerator: Fraction, denominator: Fraction) -> float | None:
    if denominator == 0:
        return 1.0 if numerator == 0 else None
    return float(numerator / denominator)


def _cut_gate(
    baseline: Fraction, candidate: Fraction, *, numerator: int, denominator: int
) -> dict[str, Any]:
    passed = (
        candidate == 0
        if baseline == 0
        else candidate * denominator <= baseline * numerator
    )
    return {
        "passed": passed,
        "limit": numerator / denominator,
        "ratio": _finite_ratio(candidate, baseline),
        "baseline": float(baseline),
        "candidate": float(candidate),
    }


def evaluate_acceptance(
    records: Iterable[Mapping[str, Any]],
    *,
    bootstrap_iterations: int,
    bootstrap_seed: int,
    min_pairs: int,
    expected_cases: Iterable[Mapping[str, Any]] | None = None,
) -> dict[str, Any]:
    if min_pairs <= 0:
        raise ValueError("minimum pair count must be positive")
    paired: dict[tuple[Any, ...], dict[str, Mapping[str, Any]]] = {}
    for record in records:
        variant = record.get("variant")
        if variant not in ("baseline", "candidate"):
            raise ValueError("run variant must be baseline or candidate")
        case = record.get("case")
        if not isinstance(case, Mapping):
            raise ValueError("run record is missing its case")
        pair_key = _key(case, _CASE_FIELDS)
        variants = paired.setdefault(pair_key, {})
        if variant in variants:
            raise ValueError(f"duplicate {variant} run for pair {pair_key}")
        variants[variant] = record
    incomplete = [key for key, values in paired.items() if len(values) != 2]
    if incomplete:
        raise ValueError(f"unpaired benchmark records: {incomplete}")

    if expected_cases is not None:
        planned_keys = [_key(case, _CASE_FIELDS) for case in expected_cases]
        if len(set(planned_keys)) != len(planned_keys):
            raise ValueError("planned benchmark matrix contains duplicate cases")
        actual_keys = set(paired)
        missing = sorted(set(planned_keys).difference(actual_keys))
        unplanned = sorted(actual_keys.difference(planned_keys))
        if missing:
            raise ValueError(f"missing planned benchmark cases: {missing}")
        if unplanned:
            raise ValueError(f"unplanned benchmark cases: {unplanned}")

    ordered_pairs = sorted(paired.items(), key=lambda item: item[0])
    balanced = True
    placement_mismatches = []

    seed_cuts: dict[tuple[Any, ...], dict[str, list[int]]] = defaultdict(
        lambda: {"baseline": [], "candidate": []}
    )
    runtime_pairs: list[tuple[float, float]] = []
    rss_pairs: list[tuple[float, float]] = []
    for pair_key, variants in ordered_pairs:
        seed_key = pair_key[:-1]
        for variant in ("baseline", "candidate"):
            verification = variants[variant].get("verification")
            if not isinstance(verification, Mapping):
                raise ValueError("run record is missing verification metrics")
            if verification.get("balanced") is not True:
                balanced = False
            cut = verification.get("weighted_cut")
            if isinstance(cut, bool) or not isinstance(cut, int) or cut < 0:
                raise ValueError("verified cut must be a nonnegative integer")
            parhip = variants[variant].get("parhip")
            if parhip is not None:
                if not isinstance(parhip, Mapping) or parhip.get("final_cut") != cut:
                    raise ValueError("ParHIP self-reported cut differs from verifier")
            seed_cuts[seed_key][variant].append(cut)
        baseline_placement = _placement_signature(variants["baseline"])
        candidate_placement = _placement_signature(variants["candidate"])
        if baseline_placement != candidate_placement:
            placement_mismatches.append(
                {
                    "case": dict(zip(_CASE_FIELDS, pair_key, strict=True)),
                    "baseline": baseline_placement,
                    "candidate": candidate_placement,
                }
            )
        baseline_runtime = _positive_finite_sample(
            variants["baseline"].get("end_to_end_seconds"),
            "baseline end-to-end time",
        )
        candidate_runtime = _positive_finite_sample(
            variants["candidate"].get("end_to_end_seconds"),
            "candidate end-to-end time",
        )
        runtime_pairs.append(
            (baseline_runtime, candidate_runtime)
        )
        baseline_rss = _positive_integer_sample(
            variants["baseline"].get("max_rank_rss_bytes"),
            "baseline peak RSS",
        )
        candidate_rss = _positive_integer_sample(
            variants["candidate"].get("max_rank_rss_bytes"),
            "candidate peak RSS",
        )
        rss_pairs.append(
            (float(baseline_rss), float(candidate_rss))
        )

    seed_medians: dict[tuple[Any, ...], dict[str, Fraction]] = {}
    for seed_key, cuts in seed_cuts.items():
        seed_medians[seed_key] = {
            variant: _median_fraction(values) for variant, values in cuts.items()
        }
    baseline_sum = sum(
        (cuts["baseline"] for cuts in seed_medians.values()), Fraction(0)
    )
    candidate_sum = sum(
        (cuts["candidate"] for cuts in seed_medians.values()), Fraction(0)
    )
    aggregate_cut = _cut_gate(
        baseline_sum, candidate_sum, numerator=101, denominator=100
    )

    grouped: dict[tuple[Any, ...], dict[str, list[Fraction]]] = defaultdict(
        lambda: {"baseline": [], "candidate": []}
    )
    for seed_key, cuts in seed_medians.items():
        configuration_key = seed_key[:-1]
        for variant in ("baseline", "candidate"):
            grouped[configuration_key][variant].append(cuts[variant])
    configuration_results = []
    for configuration_key, cuts in sorted(grouped.items()):
        gate = _cut_gate(
            _median_fraction(cuts["baseline"]),
            _median_fraction(cuts["candidate"]),
            numerator=103,
            denominator=100,
        )
        configuration_results.append(
            {
                "configuration": dict(
                    zip(_CONFIGURATION_FIELDS, configuration_key, strict=True)
                ),
                **gate,
            }
        )
    per_configuration_cut = {
        "passed": all(result["passed"] for result in configuration_results),
        "limit": 1.03,
        "configurations": configuration_results,
    }

    enough_pairs = len(ordered_pairs) >= min_pairs
    if enough_pairs:
        runtime_ci = bootstrap_ratio_of_medians(
            runtime_pairs,
            iterations=bootstrap_iterations,
            seed=bootstrap_seed,
        )
        rss_ci = bootstrap_ratio_of_medians(
            rss_pairs,
            iterations=bootstrap_iterations,
            seed=bootstrap_seed + 1,
        )
        runtime_gate = {**runtime_ci, "limit": 1.05, "passed": runtime_ci["upper"] <= 1.05}
        rss_gate = {**rss_ci, "limit": 1.05, "passed": rss_ci["upper"] <= 1.05}
    else:
        runtime_gate = {
            "passed": False,
            "status": "insufficient-pairs",
            "sample_count": len(ordered_pairs),
            "required_pairs": min_pairs,
            "limit": 1.05,
        }
        rss_gate = dict(runtime_gate)

    gates = {
        "balanced_partitions": {"passed": balanced},
        "rank_placement": {
            "passed": not placement_mismatches,
            "mismatches": placement_mismatches,
        },
        "aggregate_cut": aggregate_cut,
        "per_configuration_cut": per_configuration_cut,
        "runtime_ci": runtime_gate,
        "rss_ci": rss_gate,
    }
    return {
        "paired_run_count": len(ordered_pairs),
        "paired_seed_count": len(seed_medians),
        "gates": gates,
        "passed": all(gate["passed"] for gate in gates.values()),
    }


def _positive_finite_sample(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be numeric")
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise ValueError(f"{name} must be finite and positive")
    return result


def _positive_integer_sample(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"{name} must be a positive integer")
    return value


def _placement_signature(record: Mapping[str, Any]) -> list[dict[str, Any]]:
    case = record.get("case")
    if not isinstance(case, Mapping):
        raise ValueError("run record is missing its case")
    ranks = _positive_integer(case.get("ranks"), "case ranks")
    placement = record.get("rank_placement")
    if not isinstance(placement, list) or len(placement) != ranks:
        raise ValueError("run record has incomplete rank placement")
    by_rank: list[dict[str, Any] | None] = [None] * ranks
    for item in placement:
        if not isinstance(item, Mapping):
            raise ValueError("rank placement entry must be an object")
        rank = item.get("rank")
        hostname = item.get("hostname")
        affinity = item.get("cpu_affinity")
        if isinstance(rank, bool) or not isinstance(rank, int) or not 0 <= rank < ranks:
            raise ValueError("rank placement identity is invalid")
        if not isinstance(hostname, str) or not hostname:
            raise ValueError("rank placement hostname is invalid")
        if not isinstance(affinity, list) or not affinity:
            raise ValueError("rank placement CPU affinity is invalid")
        cpus = []
        for cpu in affinity:
            if isinstance(cpu, bool) or not isinstance(cpu, int) or cpu < 0:
                raise ValueError("rank placement CPU affinity is invalid")
            cpus.append(cpu)
        if len(set(cpus)) != len(cpus):
            raise ValueError("rank placement CPU affinity contains duplicates")
        if by_rank[rank] is not None:
            raise ValueError(f"duplicate placement for rank {rank}")
        by_rank[rank] = {
            "rank": rank,
            "hostname": hostname,
            "cpu_affinity": sorted(cpus),
        }
    if any(item is None for item in by_rank):
        raise ValueError("rank placement identities are not contiguous")
    return [dict(item) for item in by_rank if item is not None]


def read_rank_metrics(directory: Path, *, expected_ranks: int) -> dict[str, Any]:
    if expected_ranks <= 0:
        raise ValueError("expected rank count must be positive")
    paths = sorted(directory.glob("rank-*.json"))
    if len(paths) != expected_ranks:
        raise ValueError(
            f"found {len(paths)} rank metrics, expected {expected_ranks}"
        )
    rss_by_rank: list[int | None] = [None] * expected_ranks
    records = []
    for path in paths:
        record = json.loads(path.read_text(encoding="utf-8"))
        rank = record.get("rank")
        rss = record.get("max_rss_bytes")
        if not isinstance(rank, int) or not 0 <= rank < expected_ranks:
            raise ValueError(f"invalid rank metric identity in {path}")
        if rss_by_rank[rank] is not None:
            raise ValueError(f"duplicate rank metric for rank {rank}")
        if record.get("return_code") != 0:
            raise ValueError(f"rank {rank} exited unsuccessfully")
        if not isinstance(rss, int) or rss <= 0:
            raise ValueError(f"rank {rank} has invalid peak RSS")
        hostname = record.get("hostname")
        affinity = record.get("cpu_affinity")
        if not isinstance(hostname, str) or not hostname:
            raise ValueError(f"rank {rank} has invalid hostname")
        if not isinstance(affinity, list) or not affinity or any(
            isinstance(cpu, bool) or not isinstance(cpu, int) or cpu < 0
            for cpu in affinity
        ):
            raise ValueError(f"rank {rank} has invalid CPU affinity")
        rss_by_rank[rank] = rss
        records.append(record)
    if any(value is None for value in rss_by_rank):
        raise ValueError("rank metrics are not contiguous")
    rss_values = [int(value) for value in rss_by_rank]
    return {
        "max_rank_rss_bytes": max(rss_values),
        "per_rank_rss_bytes": rss_values,
        "rank_records": sorted(records, key=lambda record: record["rank"]),
        "rank_placement": [
            {
                "rank": record["rank"],
                "hostname": record["hostname"],
                "cpu_affinity": sorted(record["cpu_affinity"]),
            }
            for record in sorted(records, key=lambda record: record["rank"])
        ],
    }


_COLLECTIVE_COUNTER_FIELDS = (
    "calls",
    "sent_bytes",
    "received_bytes",
    "self_sent_bytes",
    "self_received_bytes",
)
_TOPOLOGY_COUNTER_FIELDS = ("calls", "elapsed_nanoseconds")
_TOPOLOGY_OPERATIONS = {
    "MPI_Dist_graph_create",
    "MPI_Dist_graph_create_adjacent",
}


def _nonnegative_counter(value: Any, context: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{context} must be a nonnegative integer")
    return value


def read_collective_metrics(
    directory: Path, *, expected_ranks: int
) -> dict[str, Any]:
    _positive_integer(expected_ranks, "expected rank count")
    paths = sorted(directory.glob("rank-*.json"))
    if len(paths) != expected_ranks:
        raise ValueError(
            f"found {len(paths)} collective records, expected {expected_ranks}"
        )
    by_rank: list[dict[str, Any] | None] = [None] * expected_ranks
    global_totals = {field: 0 for field in _COLLECTIVE_COUNTER_FIELDS}
    global_topology_totals = {
        field: 0 for field in _TOPOLOGY_COUNTER_FIELDS
    }
    max_rank_endpoint_bytes = 0
    max_rank_topology_setup_nanoseconds = 0
    for path in paths:
        record = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(record, Mapping) or record.get("schema_version") != 1:
            raise ValueError(f"invalid collective record schema in {path}")
        rank = record.get("rank")
        if isinstance(rank, bool) or not isinstance(rank, int) or not 0 <= rank < expected_ranks:
            raise ValueError(f"invalid collective rank in {path}")
        if by_rank[rank] is not None:
            raise ValueError(f"duplicate collective record for rank {rank}")
        if record.get("complete") is not True or record.get("error") is not None:
            raise ValueError(f"collective accounting is incomplete on rank {rank}")
        if record.get("live_persistent_requests") != 0:
            raise ValueError(f"rank {rank} retained persistent collective requests")
        hostname = record.get("hostname")
        pid = record.get("pid")
        if not isinstance(hostname, str) or not hostname:
            raise ValueError(f"rank {rank} has invalid collective hostname")
        if isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0:
            raise ValueError(f"rank {rank} has invalid collective PID")
        operations = record.get("operations")
        totals = record.get("totals")
        if not isinstance(operations, list) or not isinstance(totals, Mapping):
            raise ValueError(f"rank {rank} has invalid collective counters")
        recomputed = {field: 0 for field in _COLLECTIVE_COUNTER_FIELDS}
        names = set()
        for operation_record in operations:
            if not isinstance(operation_record, Mapping):
                raise ValueError(f"rank {rank} has invalid operation counter")
            name = operation_record.get("name")
            if not isinstance(name, str) or not name or name in names:
                raise ValueError(f"rank {rank} has duplicate/invalid operation name")
            names.add(name)
            for field in _COLLECTIVE_COUNTER_FIELDS:
                recomputed[field] += _nonnegative_counter(
                    operation_record.get(field), f"rank {rank} {name} {field}"
                )
        normalized_totals = {
            field: _nonnegative_counter(
                totals.get(field), f"rank {rank} total {field}"
            )
            for field in _COLLECTIVE_COUNTER_FIELDS
        }
        if recomputed != normalized_totals:
            raise ValueError(f"rank {rank} collective totals are inconsistent")

        topology = record.get("topology_setup")
        if not isinstance(topology, Mapping):
            raise ValueError(f"rank {rank} has no topology setup timing")
        topology_operations = topology.get("operations")
        topology_totals = topology.get("totals")
        if not isinstance(topology_operations, list) or not isinstance(
            topology_totals, Mapping
        ):
            raise ValueError(f"rank {rank} has invalid topology setup timing")
        recomputed_topology = {
            field: 0 for field in _TOPOLOGY_COUNTER_FIELDS
        }
        topology_names = set()
        normalized_topology_operations = []
        for operation_record in topology_operations:
            if not isinstance(operation_record, Mapping):
                raise ValueError(
                    f"rank {rank} has invalid topology operation timing"
                )
            name = operation_record.get("name")
            if (
                not isinstance(name, str)
                or name not in _TOPOLOGY_OPERATIONS
                or name in topology_names
            ):
                raise ValueError(
                    f"rank {rank} has duplicate/invalid topology operation"
                )
            topology_names.add(name)
            normalized_operation = {"name": name}
            for field in _TOPOLOGY_COUNTER_FIELDS:
                value = _nonnegative_counter(
                    operation_record.get(field),
                    f"rank {rank} {name} {field}",
                )
                recomputed_topology[field] += value
                normalized_operation[field] = value
            normalized_topology_operations.append(normalized_operation)
        if topology_names != _TOPOLOGY_OPERATIONS:
            raise ValueError(f"rank {rank} topology operation set is incomplete")
        normalized_topology_totals = {
            field: _nonnegative_counter(
                topology_totals.get(field),
                f"rank {rank} topology total {field}",
            )
            for field in _TOPOLOGY_COUNTER_FIELDS
        }
        if recomputed_topology != normalized_topology_totals:
            raise ValueError(
                f"rank {rank} topology setup totals are inconsistent"
            )
        for field, value in normalized_totals.items():
            global_totals[field] += value
        for field, value in normalized_topology_totals.items():
            global_topology_totals[field] += value
        max_rank_endpoint_bytes = max(
            max_rank_endpoint_bytes,
            normalized_totals["sent_bytes"] + normalized_totals["received_bytes"],
        )
        max_rank_topology_setup_nanoseconds = max(
            max_rank_topology_setup_nanoseconds,
            normalized_topology_totals["elapsed_nanoseconds"],
        )
        normalized = dict(record)
        normalized["totals"] = normalized_totals
        normalized["topology_setup"] = {
            "operations": normalized_topology_operations,
            "totals": normalized_topology_totals,
        }
        by_rank[rank] = normalized
    if any(record is None for record in by_rank):
        raise ValueError("collective rank records are not contiguous")
    return {
        "status": "complete",
        "per_rank": [dict(record) for record in by_rank if record is not None],
        "global_calls": global_totals["calls"],
        "global_sent_bytes": global_totals["sent_bytes"],
        "global_received_bytes": global_totals["received_bytes"],
        "global_self_sent_bytes": global_totals["self_sent_bytes"],
        "global_self_received_bytes": global_totals["self_received_bytes"],
        "max_rank_endpoint_bytes": max_rank_endpoint_bytes,
        "global_topology_setup_calls": global_topology_totals["calls"],
        "global_topology_setup_nanoseconds": global_topology_totals[
            "elapsed_nanoseconds"
        ],
        "max_rank_topology_setup_nanoseconds": (
            max_rank_topology_setup_nanoseconds
        ),
    }


def validate_partition_metrics(
    *,
    case: Mapping[str, Any],
    fixture: Mapping[str, Any],
    parhip: Mapping[str, Any],
    verification: Mapping[str, Any],
) -> None:
    dimensions = fixture.get("dimensions")
    if not isinstance(dimensions, list) or len(dimensions) != 3:
        raise ValueError("fixture dimensions are invalid")
    expected_vertices = math.prod(
        _positive_integer(value, "fixture dimension") for value in dimensions
    )
    if verification.get("vertices") != expected_vertices:
        raise ValueError("partition verifier vertex count differs from fixture")
    if verification.get("blocks") != case.get("blocks"):
        raise ValueError("partition verifier block count differs from case")
    if parhip.get("final_cut") != verification.get("weighted_cut"):
        raise ValueError("ParHIP self-reported cut differs from verifier")
    if verification.get("balanced") is not True:
        raise ValueError("partition verifier did not establish balance")


def create_run_directory(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=False)


def _write_text_atomic(path: Path, value: str) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("x", encoding="utf-8") as output:
        output.write(value)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)


def _write_json_atomic(path: Path, value: Mapping[str, Any]) -> None:
    _write_text_atomic(
        path,
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
    )


def _append_json_line(path: Path, value: Mapping[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as output:
        json.dump(value, output, sort_keys=True, allow_nan=False)
        output.write("\n")
        output.flush()
        os.fsync(output.fileno())


def _run_logged_command(
    command: Sequence[str],
    *,
    log_stem: Path,
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: float,
    executor: CommandExecutor,
) -> CommandResult:
    result = executor(
        list(command),
        cwd=cwd,
        environment=dict(environment),
        timeout_seconds=timeout_seconds,
    )
    _write_text_atomic(Path(f"{log_stem}.stdout.log"), result.stdout)
    _write_text_atomic(Path(f"{log_stem}.stderr.log"), result.stderr)
    if result.return_code != 0:
        raise RuntimeError(
            f"command failed with status {result.return_code}: {' '.join(command)}"
        )
    return result


def prepare_builds(
    config: Mapping[str, Any],
    *,
    output_directory: Path,
    environment: Mapping[str, str],
    executor: CommandExecutor,
) -> None:
    setup_directory = output_directory / "setup"
    setup_directory.mkdir()
    run_limited = Path(config["run_limited"])
    timeout_seconds = float(config["timeout_seconds"])
    for variant_name in ("baseline", "candidate"):
        variant = config["variants"][variant_name]
        source_directory = Path(variant["source_directory"])
        for phase in ("configure", "build"):
            bare_command = variant[f"{phase}_command"]
            validate_job_cap(bare_command, cap=config.get("concurrency", 2))
            _run_logged_command(
                limited_command(run_limited, bare_command),
                log_stem=setup_directory / f"{variant_name}-{phase}",
                cwd=source_directory,
                environment=environment,
                timeout_seconds=timeout_seconds,
                executor=executor,
            )


def prepare_fixture_graphs(
    config: Mapping[str, Any],
    *,
    output_directory: Path,
    environment: Mapping[str, str],
    executor: CommandExecutor,
) -> tuple[dict[str, Path], list[dict[str, Any]]]:
    fixtures_directory = output_directory / "fixtures"
    fixtures_directory.mkdir()
    graphs: dict[str, Path] = {}
    provenance = []
    run_limited = Path(config["run_limited"])
    timeout_seconds = float(config["timeout_seconds"])
    for fixture in config["fixtures"]:
        name = fixture["name"]
        if "graph_path" in fixture:
            graph = Path(fixture["graph_path"])
            if not graph.is_file():
                raise ValueError(f"fixture graph does not exist: {graph}")
            generator_command = None
        else:
            graph = fixtures_directory / f"{name}.graph"
            generator_path = Path(fixture["generator"])
            if not generator_path.is_file():
                raise ValueError(
                    f"fixture generator does not exist: {generator_path}"
                )
            generator_command = limited_command(
                run_limited,
                [
                    str(generator_path),
                    *(str(value) for value in fixture["dimensions"]),
                    str(graph),
                ],
            )
            _run_logged_command(
                generator_command,
                log_stem=fixtures_directory / f"{name}-generator",
                cwd=fixtures_directory,
                environment=environment,
                timeout_seconds=timeout_seconds,
                executor=executor,
            )
            if not graph.is_file():
                raise RuntimeError(f"cube generator did not produce {graph}")
        graphs[name] = graph.resolve()
        generator = fixture.get("generator")
        if generator is not None and not Path(generator).is_file():
            raise ValueError(f"fixture generator does not exist: {generator}")
        verifier = Path(fixture["verifier"])
        if not verifier.is_file():
            raise ValueError(f"fixture verifier does not exist: {verifier}")
        provenance.append(
            {
                "name": name,
                "dimensions": list(fixture["dimensions"]),
                "graph": str(graph.resolve()),
                "graph_sha256": _file_digest(graph),
                "generator": generator,
                "generator_sha256": (
                    _file_digest(Path(generator)) if generator is not None else None
                ),
                "generator_command": generator_command,
                "verifier": str(verifier.resolve()),
                "verifier_sha256": _file_digest(verifier),
            }
        )
    return graphs, provenance


def execute_one_run(
    *,
    variant: str,
    case: Mapping[str, Any],
    fixture: Mapping[str, Any],
    graph: Path,
    parhip: Path,
    run_directory: Path,
    run_limited: Path,
    mpiexec: Path,
    numproc_flag: str,
    mpi_preflags: Sequence[str],
    python_executable: Path,
    rank_runner: Path,
    environment: Mapping[str, str],
    timeout_seconds: float,
    executor: CommandExecutor,
    collective_bytes_interposer: Path | None = None,
) -> dict[str, Any]:
    if variant not in ("baseline", "candidate"):
        raise ValueError("run variant must be baseline or candidate")
    create_run_directory(run_directory)
    rank_metrics_directory = run_directory / "rank-metrics"
    run_environment = dict(environment)
    collective_directory: Path | None = None
    if collective_bytes_interposer is not None:
        if not collective_bytes_interposer.is_file():
            raise ValueError(
                f"collective-byte interposer does not exist: {collective_bytes_interposer}"
            )
        collective_directory = run_directory / "collective-bytes"
        collective_directory.mkdir()
        run_environment["KAHIP_PMPI_BYTES_DIRECTORY"] = str(
            collective_directory.resolve()
        )
    command = build_benchmark_command(
        run_limited=run_limited,
        mpiexec=mpiexec,
        numproc_flag=numproc_flag,
        mpi_preflags=mpi_preflags,
        python_executable=python_executable,
        rank_runner=rank_runner,
        rank_metrics_directory=rank_metrics_directory,
        parhip=parhip,
        graph=graph,
        case=case,
        collective_bytes_interposer=collective_bytes_interposer,
    )
    result = executor(
        command,
        cwd=run_directory,
        environment=run_environment,
        timeout_seconds=timeout_seconds,
    )
    stdout_path = run_directory / "parhip.stdout.log"
    stderr_path = run_directory / "parhip.stderr.log"
    _write_text_atomic(stdout_path, result.stdout)
    _write_text_atomic(stderr_path, result.stderr)
    if result.return_code != 0:
        raise RuntimeError(
            f"{variant} ParHIP run failed with status {result.return_code}; "
            f"see {stderr_path}"
        )
    partition_path = run_directory / "tmppartition.txtp"
    if not partition_path.is_file():
        raise RuntimeError(f"ParHIP did not produce {partition_path}")

    parhip_metrics = parse_parhip_output(result.stdout)
    ranks = _positive_integer(case.get("ranks"), "case ranks")
    rank_metrics = read_rank_metrics(
        rank_metrics_directory, expected_ranks=ranks
    )
    collective_metrics = (
        read_collective_metrics(collective_directory, expected_ranks=ranks)
        if collective_directory is not None
        else {
            "status": "incomplete",
            "reason": "no PMPI collective-byte interposer was configured",
        }
    )
    if collective_metrics.get("status") == "complete":
        for placement, rank_record, collective_record in zip(
            rank_metrics["rank_placement"],
            rank_metrics["rank_records"],
            collective_metrics["per_rank"],
            strict=True,
        ):
            if collective_record["hostname"] != placement["hostname"]:
                raise ValueError("collective/rank metric hostname differs")
            child_pid = rank_record.get("child_pid")
            if child_pid is not None and collective_record["pid"] != child_pid:
                raise ValueError("collective record does not belong to rank child")
    dimensions = fixture.get("dimensions")
    if not isinstance(dimensions, list) or len(dimensions) != 3:
        raise ValueError("fixture dimensions are invalid")
    verifier = Path(_nonempty_string(fixture, "verifier"))
    verifier_command = limited_command(
        run_limited,
        [
            str(verifier),
            *(str(value) for value in dimensions),
            str(case["blocks"]),
            str(case["imbalance_percent"]),
            str(partition_path),
        ],
    )
    verifier_result = executor(
        verifier_command,
        cwd=run_directory,
        environment=dict(environment),
        timeout_seconds=timeout_seconds,
    )
    verifier_stdout_path = run_directory / "verifier.stdout.log"
    verifier_stderr_path = run_directory / "verifier.stderr.log"
    _write_text_atomic(verifier_stdout_path, verifier_result.stdout)
    _write_text_atomic(verifier_stderr_path, verifier_result.stderr)
    if verifier_result.return_code != 0:
        raise RuntimeError(
            f"partition verifier failed with status {verifier_result.return_code}; "
            f"see {verifier_stderr_path}"
        )
    verification = parse_verifier_output(verifier_result.stdout)
    validate_partition_metrics(
        case=case,
        fixture=fixture,
        parhip=parhip_metrics,
        verification=verification,
    )
    partition_sha256 = _file_digest(partition_path)
    return {
        "variant": variant,
        "case": dict(case),
        "end_to_end_seconds": _positive_finite_sample(
            result.elapsed_seconds, "end-to-end time"
        ),
        "max_rank_rss_bytes": rank_metrics["max_rank_rss_bytes"],
        "per_rank_rss_bytes": rank_metrics["per_rank_rss_bytes"],
        "rank_placement": rank_metrics["rank_placement"],
        "rank_metrics": rank_metrics["rank_records"],
        "collective_bytes": collective_metrics,
        "parhip": parhip_metrics,
        "verification": verification,
        "partition_sha256": partition_sha256,
        "setup_timing": {
            "startup_dummy_seconds": parhip_metrics["startup_dummy_seconds"],
            "input_ready_elapsed_seconds": parhip_metrics[
                "input_ready_elapsed_seconds"
            ],
        },
        "topology_timing": (
            {
                "status": "complete",
                "measurement": (
                    "PMPI distributed-graph construction wall time"
                ),
                "global_calls": collective_metrics[
                    "global_topology_setup_calls"
                ],
                "global_rank_nanoseconds": collective_metrics[
                    "global_topology_setup_nanoseconds"
                ],
                "max_rank_nanoseconds": collective_metrics[
                    "max_rank_topology_setup_nanoseconds"
                ],
            }
            if collective_metrics.get("status") == "complete"
            else {
                "status": "incomplete",
                "reason": (
                    "no complete PMPI distributed-graph topology timing "
                    "was supplied"
                ),
            }
        ),
        "commands": {
            "benchmark": command,
            "verifier": verifier_command,
        },
        "artifacts": {
            "run_directory": str(run_directory.resolve()),
            "graph_sha256": _file_digest(graph),
            "partition_sha256": partition_sha256,
            "stdout_sha256": _file_digest(stdout_path),
            "stderr_sha256": _file_digest(stderr_path),
            "verifier_stdout_sha256": _file_digest(verifier_stdout_path),
            "verifier_stderr_sha256": _file_digest(verifier_stderr_path),
        },
    }


def validate_build_equivalence(
    baseline: Mapping[str, Any],
    candidate: Mapping[str, Any],
    *,
    require_stage_timings: bool,
) -> None:
    required = (
        "build_type",
        "optimized_output",
        "compiler",
        "compiler_version",
        "release_flags",
        "link_flags",
        "cmake_generator",
        "cmake_version",
        "mpi_executable",
        "mpi_version",
        "mpi_cache_identity",
        "linked_mpi_libraries",
    )
    for name in required:
        if name not in baseline or name not in candidate:
            raise ValueError(f"build provenance is missing {name}")
    if baseline["build_type"] != "Release" or candidate["build_type"] != "Release":
        raise ValueError("performance comparison requires Release builds")
    if require_stage_timings and (
        baseline["optimized_output"].upper() != "ON"
        or candidate["optimized_output"].upper() != "ON"
    ):
        raise ValueError("stage timing requires OPTIMIZED_OUTPUT=ON")
    mismatches = [name for name in required if baseline[name] != candidate[name]]
    if mismatches:
        raise ValueError(
            "baseline and candidate build provenance differs: "
            + ", ".join(mismatches)
        )


def parse_cmake_cache(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise ValueError(f"CMake cache is not UTF-8: {path}") from error
    for line_number, line in enumerate(lines, start=1):
        if not line or line.startswith(("//", "#")):
            continue
        match = re.fullmatch(r"([^:=]+):[^=]*=(.*)", line)
        if match is None:
            continue
        name, value = match.groups()
        if name in values:
            raise ValueError(
                f"duplicate CMake cache entry {name!r} at line {line_number}"
            )
        values[name] = value
    return values


def _cache_value(cache: Mapping[str, str], name: str) -> str:
    value = cache.get(name)
    if not isinstance(value, str) or not value:
        raise ValueError(f"CMake cache is missing {name}")
    return value


def build_provenance_from_cache(
    cache: Mapping[str, str],
    *,
    compiler_version: str,
    mpi_version: str,
    executable_sha256: str,
    cmake_version: str,
    linked_mpi_libraries: Sequence[Mapping[str, str]],
) -> dict[str, Any]:
    if (
        not compiler_version.strip()
        or not mpi_version.strip()
        or not cmake_version.strip()
    ):
        raise ValueError("compiler, MPI, and CMake version records must not be empty")
    if re.fullmatch(r"[0-9a-f]{64}", executable_sha256) is None:
        raise ValueError("executable SHA-256 must be canonical lowercase hex")
    common_flags = cache.get("CMAKE_CXX_FLAGS", "").strip()
    release_flags = _cache_value(cache, "CMAKE_CXX_FLAGS_RELEASE").strip()
    effective_flags = " ".join(
        part for part in (common_flags, release_flags) if part
    )
    common_link_flags = cache.get("CMAKE_EXE_LINKER_FLAGS", "").strip()
    release_link_flags = cache.get("CMAKE_EXE_LINKER_FLAGS_RELEASE", "").strip()
    effective_link_flags = " ".join(
        part for part in (common_link_flags, release_link_flags) if part
    )
    mpi_cache_identity = {
        name: value
        for name, value in sorted(cache.items())
        if name.startswith("MPI_")
        and any(
            token in name
            for token in ("COMPILER", "INCLUDE", "LIBRARY", "LIBRARIES")
        )
    }
    return {
        "build_type": _cache_value(cache, "CMAKE_BUILD_TYPE"),
        "optimized_output": _cache_value(cache, "OPTIMIZED_OUTPUT"),
        "compiler": _cache_value(cache, "CMAKE_CXX_COMPILER"),
        "compiler_version": compiler_version.strip(),
        "release_flags": effective_flags,
        "link_flags": effective_link_flags,
        "cmake_generator": _cache_value(cache, "CMAKE_GENERATOR"),
        "cmake_version": cmake_version.strip(),
        "mpi_executable": _cache_value(cache, "MPIEXEC_EXECUTABLE"),
        "mpi_version": mpi_version.strip(),
        "mpi_cache_identity": mpi_cache_identity,
        "linked_mpi_libraries": [dict(record) for record in linked_mpi_libraries],
        "executable_sha256": executable_sha256,
    }


def _probe_version(
    command: Sequence[str],
    *,
    run_limited: Path,
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: float,
    executor: CommandExecutor,
) -> str:
    result = executor(
        limited_command(run_limited, command),
        cwd=cwd,
        environment=dict(environment),
        timeout_seconds=timeout_seconds,
    )
    if result.return_code != 0:
        raise RuntimeError(
            f"version probe {command[0]!r} failed with status {result.return_code}"
        )
    lines = []
    for line in (result.stdout + "\n" + result.stderr).splitlines():
        if line.startswith(("Running as unit:", "Finished with result:")):
            continue
        if line.strip():
            lines.append(line.rstrip())
    if not lines:
        raise RuntimeError(f"version probe {command[0]!r} emitted no version")
    return "\n".join(lines)


def _linked_mpi_libraries(
    executable: Path,
    *,
    run_limited: Path,
    cwd: Path,
    environment: Mapping[str, str],
    timeout_seconds: float,
    executor: CommandExecutor,
) -> list[dict[str, str]]:
    ldd = shutil.which("ldd")
    if ldd is None:
        raise RuntimeError("ldd is required to identify the linked MPI runtime")
    result = executor(
        limited_command(run_limited, [ldd, str(executable)]),
        cwd=cwd,
        environment=dict(environment),
        timeout_seconds=timeout_seconds,
    )
    if result.return_code != 0:
        raise RuntimeError(
            f"cannot inspect linked MPI libraries for {executable}: {result.stderr}"
        )
    libraries: list[dict[str, str]] = []
    for line in result.stdout.splitlines():
        match = re.match(r"\s*(\S+)\s+=>\s+(\S+)\s+\(", line)
        if match is None:
            continue
        name, raw_path = match.groups()
        lowered = name.lower()
        if not any(token in lowered for token in ("libmpi", "libmpich", "libpmix")):
            continue
        library = Path(raw_path)
        if not library.is_file():
            raise RuntimeError(f"linked MPI library does not exist: {library}")
        libraries.append(
            {
                "name": name,
                "path": str(library.resolve()),
                "sha256": _file_digest(library),
            }
        )
    if not libraries:
        raise RuntimeError(f"no linked MPI library was found for {executable}")
    return sorted(libraries, key=lambda record: (record["name"], record["path"]))


def collect_build_provenance(
    variant: Mapping[str, Any],
    *,
    configured_mpiexec: Path,
    run_limited: Path,
    environment: Mapping[str, str],
    timeout_seconds: float,
    executor: CommandExecutor,
) -> dict[str, Any]:
    build_directory = Path(_nonempty_string(variant, "build_directory"))
    executable = Path(_nonempty_string(variant, "executable"))
    if not executable.is_file():
        raise ValueError(f"ParHIP executable does not exist: {executable}")
    cache = parse_cmake_cache(build_directory / "CMakeCache.txt")
    compiler = Path(_cache_value(cache, "CMAKE_CXX_COMPILER"))
    cached_mpiexec = Path(_cache_value(cache, "MPIEXEC_EXECUTABLE"))
    if compiler.is_absolute():
        compiler_command = str(compiler)
    else:
        located_compiler = shutil.which(str(compiler))
        if located_compiler is None:
            raise ValueError(f"cannot resolve configured compiler {compiler}")
        compiler_command = located_compiler
    configured_mpiexec_resolved = Path(
        shutil.which(str(configured_mpiexec)) or configured_mpiexec
    ).resolve()
    cached_mpiexec_resolved = Path(
        shutil.which(str(cached_mpiexec)) or cached_mpiexec
    ).resolve()
    if configured_mpiexec_resolved != cached_mpiexec_resolved:
        raise ValueError(
            "configured mpiexec differs from the executable recorded by CMake"
        )
    compiler_version = _probe_version(
        [compiler_command, "--version"],
        run_limited=run_limited,
        cwd=build_directory,
        environment=environment,
        timeout_seconds=timeout_seconds,
        executor=executor,
    )
    mpi_version = _probe_version(
        [str(configured_mpiexec_resolved), "--version"],
        run_limited=run_limited,
        cwd=build_directory,
        environment=environment,
        timeout_seconds=timeout_seconds,
        executor=executor,
    )
    cmake = shutil.which("cmake")
    if cmake is None:
        raise RuntimeError("cmake is required for build provenance")
    cmake_version = _probe_version(
        [cmake, "--version"],
        run_limited=run_limited,
        cwd=build_directory,
        environment=environment,
        timeout_seconds=timeout_seconds,
        executor=executor,
    )
    provenance = build_provenance_from_cache(
        cache,
        compiler_version=compiler_version,
        mpi_version=mpi_version,
        executable_sha256=_file_digest(executable),
        cmake_version=cmake_version,
        linked_mpi_libraries=_linked_mpi_libraries(
            executable,
            run_limited=run_limited,
            cwd=build_directory,
            environment=environment,
            timeout_seconds=timeout_seconds,
            executor=executor,
        ),
    )
    provenance.update(
        {
            "source_directory": str(
                Path(_nonempty_string(variant, "source_directory")).resolve()
            ),
            "build_directory": str(build_directory.resolve()),
            "executable": str(executable.resolve()),
        }
    )
    return provenance


def _git(source_directory: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", source_directory, *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def _file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    if path.is_symlink():
        digest.update(b"symlink\0")
        digest.update(os.readlink(path).encode("utf-8", errors="surrogateescape"))
        return digest.hexdigest()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_git_provenance(source_directory: Path) -> dict[str, Any]:
    source_directory = source_directory.resolve()
    revision = _git(source_directory, "rev-parse", "HEAD").decode().strip()
    diff = _git(source_directory, "diff", "--binary", "HEAD", "--")
    tracked_names = [
        name
        for name in _git(source_directory, "diff", "--name-only", "HEAD", "--")
        .decode("utf-8", errors="surrogateescape")
        .splitlines()
        if name
    ]
    untracked_bytes = _git(
        source_directory, "ls-files", "--others", "--exclude-standard", "-z"
    )
    untracked = sorted(
        item.decode("utf-8", errors="surrogateescape")
        for item in untracked_bytes.split(b"\0")
        if item
    )
    manifest = hashlib.sha256()
    untracked_records = []
    for relative in untracked:
        encoded = relative.encode("utf-8", errors="surrogateescape")
        full_path = source_directory / relative
        digest = _file_digest(full_path)
        size = full_path.lstat().st_size
        manifest.update(len(encoded).to_bytes(8, "big"))
        manifest.update(encoded)
        manifest.update(size.to_bytes(8, "big"))
        manifest.update(bytes.fromhex(digest))
        untracked_records.append(
            {"path": relative, "size_bytes": size, "sha256": digest}
        )
    return {
        "source_directory": str(source_directory),
        "revision": revision,
        "clean": not diff and not untracked,
        "tracked_changed_files": sorted(tracked_names),
        "diff_sha256": hashlib.sha256(diff).hexdigest(),
        "diff_bytes": len(diff),
        "untracked_files": untracked,
        "untracked_manifest": untracked_records,
        "untracked_manifest_sha256": manifest.hexdigest() if untracked else None,
    }


def validate_pristine_baseline(
    provenance: Mapping[str, Any], pinned_revision: str
) -> None:
    if provenance.get("revision") != pinned_revision:
        raise ValueError(
            "baseline revision does not match the pinned pristine upstream SHA"
        )
    if not provenance.get("clean"):
        raise ValueError("baseline source is not pristine")


_GIT_STABILITY_FIELDS = (
    "revision",
    "clean",
    "tracked_changed_files",
    "diff_sha256",
    "diff_bytes",
    "untracked_files",
    "untracked_manifest_sha256",
)


def validate_unchanged_git_provenance(
    before: Mapping[str, Any], after: Mapping[str, Any], *, variant: str
) -> None:
    changed = [name for name in _GIT_STABILITY_FIELDS if before.get(name) != after.get(name)]
    if changed:
        raise ValueError(
            f"{variant} source changed during acceptance run: {', '.join(changed)}"
        )


def _read_optional_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8").strip() or None
    except (OSError, UnicodeDecodeError):
        return None


def machine_provenance(environment: Mapping[str, str]) -> dict[str, Any]:
    affinity = (
        sorted(os.sched_getaffinity(0))
        if hasattr(os, "sched_getaffinity")
        else list(range(os.cpu_count() or 1))
    )
    cpu_models: set[str] = set()
    cpuinfo = _read_optional_text(Path("/proc/cpuinfo"))
    if cpuinfo is not None:
        for line in cpuinfo.splitlines():
            if line.lower().startswith("model name") and ":" in line:
                cpu_models.add(line.partition(":")[2].strip())
    numa_nodes = []
    node_root = Path("/sys/devices/system/node")
    try:
        numa_nodes = sorted(
            path.name for path in node_root.glob("node[0-9]*") if path.is_dir()
        )
    except OSError:
        pass
    governors = set()
    for path in Path("/sys/devices/system/cpu").glob(
        "cpu[0-9]*/cpufreq/scaling_governor"
    ):
        value = _read_optional_text(path)
        if value is not None:
            governors.add(value)
    relevant_prefixes = (
        "OMP_",
        "OMPI_",
        "PMI_",
        "PMIX_",
        "UCX_",
        "FI_",
        "I_MPI_",
        "MPICH_",
        "SLURM_",
    )
    relevant_names = {"PATH", "LD_LIBRARY_PATH"}
    relevant_environment = {
        name: value
        for name, value in sorted(environment.items())
        if name in relevant_names or name.startswith(relevant_prefixes)
    }
    return {
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "logical_cpu_count": os.cpu_count(),
        "allowed_cpus": affinity,
        "cpu_models": sorted(cpu_models),
        "numa_nodes": numa_nodes,
        "memory": _read_optional_text(Path("/proc/meminfo")),
        "scaling_governors": sorted(governors),
        "turbo_disabled": _read_optional_text(
            Path("/sys/devices/system/cpu/intel_pstate/no_turbo")
        ),
        "environment": relevant_environment,
    }


def assemble_result_document(
    *,
    records: Iterable[Mapping[str, Any]],
    provenance: Mapping[str, Any],
    bootstrap_iterations: int,
    bootstrap_seed: int,
    min_pairs: int,
    expected_cases: Iterable[Mapping[str, Any]] | None = None,
) -> dict[str, Any]:
    materialized_records = [dict(record) for record in records]
    quality = evaluate_acceptance(
        materialized_records,
        bootstrap_iterations=bootstrap_iterations,
        bootstrap_seed=bootstrap_seed,
        min_pairs=min_pairs,
        expected_cases=expected_cases,
    )
    collective_records = [record.get("collective_bytes") for record in materialized_records]
    collective_complete = bool(collective_records) and all(
        isinstance(record, Mapping) and record.get("status") == "complete"
        for record in collective_records
    )
    if collective_complete:
        by_variant = {
            variant: {
                "global_sent_bytes": sum(
                    record["collective_bytes"]["global_sent_bytes"]
                    for record in materialized_records
                    if record["variant"] == variant
                ),
                "global_received_bytes": sum(
                    record["collective_bytes"]["global_received_bytes"]
                    for record in materialized_records
                    if record["variant"] == variant
                ),
            }
            for variant in ("baseline", "candidate")
        }
        collective_bytes = {
            "status": "complete",
            "passed": True,
            "measurement": "logical MPI endpoint payload bytes",
            "variants": by_variant,
        }
    else:
        collective_bytes = {
            "status": "incomplete",
            "passed": False,
            "reason": (
                "no complete PMPI collective-byte records were supplied for "
                "every rank of every run"
            ),
        }
    stage_metrics = {
        "status": "complete",
        "passed": all(
            isinstance(record.get("parhip"), Mapping)
            and bool(record["parhip"].get("stage_events"))
            for record in materialized_records
        ),
    }
    if not stage_metrics["passed"]:
        stage_metrics.update(
            {
                "status": "incomplete",
                "reason": "one or more runs lack required stage timing records",
            }
        )
    topology_records = [
        record.get("topology_timing") for record in materialized_records
    ]
    topology_complete = bool(topology_records) and all(
        isinstance(record, Mapping) and record.get("status") == "complete"
        for record in topology_records
    )
    if topology_complete:
        topology_variants = {
            variant: {
                "global_calls": sum(
                    record["topology_timing"]["global_calls"]
                    for record in materialized_records
                    if record["variant"] == variant
                ),
                "global_rank_nanoseconds": sum(
                    record["topology_timing"]["global_rank_nanoseconds"]
                    for record in materialized_records
                    if record["variant"] == variant
                ),
                "total_run_max_rank_nanoseconds": sum(
                    record["topology_timing"]["max_rank_nanoseconds"]
                    for record in materialized_records
                    if record["variant"] == variant
                ),
                "max_rank_nanoseconds": max(
                    (
                        record["topology_timing"]["max_rank_nanoseconds"]
                        for record in materialized_records
                        if record["variant"] == variant
                    ),
                    default=0,
                ),
            }
            for variant in ("baseline", "candidate")
        }
        setup_topology = {
            "status": "complete",
            "passed": True,
            "measurement": "PMPI distributed-graph construction wall time",
            "variants": topology_variants,
        }
    else:
        setup_topology = {
            "status": "incomplete",
            "passed": False,
            "reason": (
                "one or more runs lack complete PMPI distributed-graph "
                "topology timing"
            ),
        }
    acceptance_complete = (
        stage_metrics["passed"]
        and setup_topology["passed"]
        and collective_bytes["passed"]
    )
    return {
        "schema_version": 1,
        "provenance": dict(provenance),
        "records": materialized_records,
        "analysis": {
            "quality_performance": quality,
            "quality_performance_passed": bool(
                quality["passed"] and stage_metrics["passed"]
            ),
            "stage_metrics": stage_metrics,
            "setup_topology_timing": setup_topology,
            "collective_bytes": collective_bytes,
            "acceptance_complete": acceptance_complete,
            "passed": bool(
                quality["passed"]
                and stage_metrics["passed"]
                and acceptance_complete
            ),
        },
    }


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def execute_harness(
    config: Mapping[str, Any],
    *,
    prepare: bool,
    executor: CommandExecutor = run_external_command,
) -> tuple[dict[str, Any], Path]:
    validate_config(config)
    expected_wrapper = Path(__file__).resolve().parents[1] / "run-limited"
    configured_wrapper = Path(config["run_limited"]).resolve()
    if configured_wrapper != expected_wrapper or not os.access(
        configured_wrapper, os.X_OK
    ):
        raise ValueError(
            f"run_limited must be the executable repository wrapper {expected_wrapper}"
        )
    source_provenance_start = {
        name: collect_git_provenance(Path(config["variants"][name]["source_directory"]))
        for name in ("baseline", "candidate")
    }
    validate_pristine_baseline(
        source_provenance_start["baseline"], config["pinned_upstream_revision"]
    )

    output_directory = Path(config["output_directory"])
    create_run_directory(output_directory)
    event_path = output_directory / "events.jsonl"
    started_at = _utc_now()
    environment = benchmark_environment(config, os.environ)
    config_snapshot = copy.deepcopy(dict(config))
    _write_json_atomic(output_directory / "configuration.json", config_snapshot)
    _append_json_line(
        event_path,
        {"event": "started", "timestamp": started_at, "prepare": prepare},
    )

    if prepare:
        prepare_builds(
            config,
            output_directory=output_directory,
            environment=environment,
            executor=executor,
        )

    run_limited = Path(config["run_limited"])
    configured_mpiexec = Path(config["mpiexec"]["executable"])
    timeout_seconds = float(config["timeout_seconds"])
    build_provenance = {
        name: collect_build_provenance(
            config["variants"][name],
            configured_mpiexec=configured_mpiexec,
            run_limited=run_limited,
            environment=environment,
            timeout_seconds=timeout_seconds,
            executor=executor,
        )
        for name in ("baseline", "candidate")
    }
    validate_build_equivalence(
        build_provenance["baseline"],
        build_provenance["candidate"],
        require_stage_timings=True,
    )
    graphs, fixture_provenance = prepare_fixture_graphs(
        config,
        output_directory=output_directory,
        environment=environment,
        executor=executor,
    )
    for record in fixture_provenance:
        _append_json_line(
            event_path,
            {"event": "fixture-ready", "timestamp": _utc_now(), **record},
        )

    fixtures = {fixture["name"]: fixture for fixture in config["fixtures"]}
    cases = enumerate_cases(config)
    records = []
    rank_runner = Path(__file__).with_name("rank_runner.py").resolve()
    python_executable = Path(config.get("python_executable", sys.executable))
    collective_interposer = (
        Path(config["collective_bytes_interposer"])
        if "collective_bytes_interposer" in config
        else None
    )
    runs_root = output_directory / "runs"
    for pair_index, case in enumerate(cases):
        for execution_order, variant_name in enumerate(
            alternating_variant_order(pair_index)
        ):
            run_directory = (
                runs_root / f"pair-{pair_index:06d}" / variant_name
            )
            record = execute_one_run(
                variant=variant_name,
                case=case,
                fixture=fixtures[case["fixture"]],
                graph=graphs[case["fixture"]],
                parhip=Path(config["variants"][variant_name]["executable"]),
                run_directory=run_directory,
                run_limited=run_limited,
                mpiexec=configured_mpiexec,
                numproc_flag=config["mpiexec"]["numproc_flag"],
                mpi_preflags=config["mpiexec"]["preflags"],
                python_executable=python_executable,
                rank_runner=rank_runner,
                environment=environment,
                timeout_seconds=timeout_seconds,
                executor=executor,
                collective_bytes_interposer=collective_interposer,
            )
            record["pair_index"] = pair_index
            record["execution_order"] = execution_order
            records.append(record)
            _append_json_line(
                event_path,
                {"event": "run-complete", "timestamp": _utc_now(), **record},
            )

    for fixture_record in fixture_provenance:
        graph = Path(fixture_record["graph"])
        if _file_digest(graph) != fixture_record["graph_sha256"]:
            raise ValueError(f"fixture graph changed during run: {graph}")
    source_provenance_end = {
        name: collect_git_provenance(Path(config["variants"][name]["source_directory"]))
        for name in ("baseline", "candidate")
    }
    for name in ("baseline", "candidate"):
        validate_unchanged_git_provenance(
            source_provenance_start[name],
            source_provenance_end[name],
            variant=name,
        )
    validate_pristine_baseline(
        source_provenance_end["baseline"], config["pinned_upstream_revision"]
    )
    finished_at = _utc_now()
    provenance = {
        "started_at": started_at,
        "finished_at": finished_at,
        "configuration_sha256": _file_digest(
            output_directory / "configuration.json"
        ),
        "machine": machine_provenance(environment),
        "mpi_launch": copy.deepcopy(config["mpiexec"]),
        "concurrency": config.get("concurrency", 2),
        "run_limited": {
            "path": str(configured_wrapper),
            "sha256": _file_digest(configured_wrapper),
        },
        "git_start": source_provenance_start,
        "git_end": source_provenance_end,
        "builds": build_provenance,
        "fixtures": fixture_provenance,
        "collective_bytes_interposer": (
            {
                "path": str(collective_interposer.resolve()),
                "sha256": _file_digest(collective_interposer),
            }
            if collective_interposer is not None
            else None
        ),
    }
    bootstrap = config["bootstrap"]
    document = assemble_result_document(
        records=records,
        provenance=provenance,
        bootstrap_iterations=bootstrap["iterations"],
        bootstrap_seed=bootstrap["seed"],
        min_pairs=bootstrap["min_pairs"],
        expected_cases=cases,
    )
    result_path = output_directory / "results.json"
    _write_json_atomic(result_path, document)
    _append_json_line(
        event_path,
        {
            "event": "finished",
            "timestamp": finished_at,
            "quality_performance_passed": document["analysis"][
                "quality_performance_passed"
            ],
            "acceptance_complete": document["analysis"]["acceptance_complete"],
            "passed": document["analysis"]["passed"],
            "results": str(result_path.resolve()),
        },
    )
    return document, result_path


def main(arguments: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="compare pristine-upstream and candidate ParHIP builds"
    )
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument(
        "--prepare",
        action="store_true",
        help="run the configured Release configure/build commands first",
    )
    options = parser.parse_args(arguments)
    config: dict[str, Any] | None = None
    try:
        config = load_config(options.config)
        document, result_path = execute_harness(config, prepare=options.prepare)
    except (OSError, RuntimeError, ValueError) as error:
        if config is not None:
            output_directory = Path(config["output_directory"])
            if output_directory.is_dir():
                try:
                    _write_json_atomic(
                        output_directory / "failure.json",
                        {
                            "schema_version": 1,
                            "status": "failed",
                            "timestamp": _utc_now(),
                            "error": str(error),
                        },
                    )
                except OSError:
                    pass
        print(f"acceptance harness failed: {error}", file=sys.stderr)
        return 2
    print(f"results: {result_path}")
    if not document["analysis"]["acceptance_complete"]:
        print(
            "acceptance incomplete: collective bytes and isolated topology setup "
            "timing are not yet available",
            file=sys.stderr,
        )
        return 2
    return 0 if document["analysis"]["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
