#!/usr/bin/env bash

kahip_scale_probe_die() {
  printf 'KaHIP Cirrus scale runner: %s\n' "$*" >&2
  exit 2
}

kahip_run_scale_probe() {
  if [[ $# -ne 4 ]]; then
    kahip_scale_probe_die 'internal runner configuration is incomplete'
  fi

  local -r compiler_name=$1
  local -r programming_environment_module=$2
  local -r configure_preset=$3
  local -r build_preset=$4
  local -r ranks_per_node=288
  local -r side=${KAHIP_SCALE_PROBE_SIDE:-600}

  local expected_nodes
  local expected_ranks
  local expected_bound
  case "${side}" in
    600)
      expected_nodes=8
      expected_ranks=2304
      expected_bound=96562
      ;;
    755)
      expected_nodes=16
      expected_ranks=4608
      expected_bound=96198
      ;;
    900)
      expected_nodes=27
      expected_ranks=7776
      expected_bound=96562
      ;;
    1008)
      expected_nodes=38
      expected_ranks=10944
      expected_bound=96392
      ;;
    *)
      kahip_scale_probe_die \
        "unsupported KAHIP_SCALE_PROBE_SIDE '${side}'; expected 600, 755, 900, or 1008"
      ;;
  esac

  if [[ ${SLURM_JOB_NUM_NODES-} != "${expected_nodes}" ]]; then
    kahip_scale_probe_die \
      "side ${side} requires SLURM_JOB_NUM_NODES=${expected_nodes}, got '${SLURM_JOB_NUM_NODES-<unset>}'"
  fi
  if [[ ${SLURM_NTASKS-} != "${expected_ranks}" ]]; then
    kahip_scale_probe_die \
      "side ${side} requires SLURM_NTASKS=${expected_ranks}, got '${SLURM_NTASKS-<unset>}'"
  fi
  if [[ ${SLURM_NTASKS_PER_NODE-} != "${ranks_per_node}" ]]; then
    kahip_scale_probe_die \
      "scale runs require SLURM_NTASKS_PER_NODE=${ranks_per_node}, got '${SLURM_NTASKS_PER_NODE-<unset>}'"
  fi
  if [[ ${SLURM_CPUS_PER_TASK-} != 1 ]]; then
    kahip_scale_probe_die \
      "scale runs require SLURM_CPUS_PER_TASK=1, got '${SLURM_CPUS_PER_TASK-<unset>}'"
  fi
  if [[ ! ${SLURM_JOB_ID-} =~ ^[0-9]+$ ]]; then
    kahip_scale_probe_die \
      "SLURM_JOB_ID must be a decimal job identifier, got '${SLURM_JOB_ID-<unset>}'"
  fi

  local repository_root
  repository_root=$(pwd -P)
  readonly repository_root
  local -r catch2_prefix="${repository_root%/*}/opt/catch2"
  local -r temporary_directory="${repository_root}/out/tmp/cirrus-scale-${compiler_name}-${SLURM_JOB_ID}"

  mkdir -p "${repository_root}/out/slurm" "${temporary_directory}"
  export TMPDIR="${temporary_directory}"
  export OMP_NUM_THREADS=1
  cd -- "${repository_root}" ||
    kahip_scale_probe_die "cannot enter repository: ${repository_root}"

  module restore
  module load "${programming_environment_module}"
  module load cmake/4.1.2

  if [[ ! -d ${catch2_prefix} ]]; then
    kahip_scale_probe_die \
      "required Catch2 prefix is missing: ${catch2_prefix}"
  fi
  if ! command -v srun >/dev/null; then
    kahip_scale_probe_die 'srun is unavailable in the selected environment'
  fi

  local git_toplevel
  git_toplevel=$(git -C "${repository_root}" rev-parse --show-toplevel)
  if [[ ${git_toplevel} != "${repository_root}" ]]; then
    kahip_scale_probe_die \
      "runner path is not the Git repository root: ${repository_root}"
  fi

  local source_revision
  source_revision=$(
    git -C "${repository_root}" rev-parse --verify 'HEAD^{commit}'
  )
  if [[ ! ${source_revision} =~ ^[0-9a-f]{40}([0-9a-f]{24})?$ ]]; then
    kahip_scale_probe_die \
      "Git returned an invalid source revision: ${source_revision}"
  fi

  local diff_status
  if git -C "${repository_root}" diff --quiet --no-ext-diff HEAD --; then
    diff_status=0
  else
    diff_status=$?
  fi
  case "${diff_status}" in
    0)
      ;;
    1)
      source_revision="${source_revision}-dirty"
      ;;
    *)
      kahip_scale_probe_die \
        "Git tracked-dirty inspection failed with status ${diff_status}"
      ;;
  esac

  cmake --fresh --preset "${configure_preset}" \
    "-DCMAKE_PREFIX_PATH:PATH=${catch2_prefix}" \
    "-DKAHIP_SCALE_PROBE_SOURCE_REVISION=${source_revision}"
  cmake --build --preset "${build_preset}" \
    --target parhip_cube_scale_probe

  local -r probe="${repository_root}/out/build/${configure_preset}/parallel/parallel_src/tests/parhip_cube_scale_probe"
  if [[ ! -x ${probe} ]]; then
    kahip_scale_probe_die "scale probe was not built at ${probe}"
  fi

  printf \
    'KaHIP Cirrus scale probe: compiler=%s side=%s nodes=%s ranks=%s bound=%s source_revision=%s\n' \
    "${compiler_name}" "${side}" "${expected_nodes}" "${expected_ranks}" \
    "${expected_bound}" "${source_revision}"

  exec srun \
    "--nodes=${expected_nodes}" \
    "--ntasks=${expected_ranks}" \
    "--ntasks-per-node=${ranks_per_node}" \
    --cpus-per-task=1 \
    --hint=nomultithread \
    --distribution=block:block \
    --kill-on-bad-exit \
    --unbuffered \
    "${probe}" \
    --side "${side}" \
    --expected-ranks "${expected_ranks}"
}
