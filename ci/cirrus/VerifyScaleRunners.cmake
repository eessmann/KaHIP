cmake_minimum_required(VERSION 4.0)

foreach(required_variable IN ITEMS SOURCE_DIR BASH_EXECUTABLE TEST_ROOT)
    if(
        NOT DEFINED ${required_variable}
        OR "${${required_variable}}" STREQUAL ""
    )
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

function(require_contains value expected context)
    string(FIND "${value}" "${expected}" expected_index)
    if(expected_index EQUAL -1)
        message(
            FATAL_ERROR
            "${context}: missing '${expected}'\n--- value ---\n${value}"
        )
    endif()
endfunction()

function(require_not_contains value forbidden context)
    string(FIND "${value}" "${forbidden}" forbidden_index)
    if(NOT forbidden_index EQUAL -1)
        message(
            FATAL_ERROR
            "${context}: found forbidden '${forbidden}'\n--- value ---\n${value}"
        )
    endif()
endfunction()

function(require_equal actual expected context)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(
            FATAL_ERROR
            "${context}: values differ\n--- expected ---\n${expected}--- actual ---\n${actual}"
        )
    endif()
endfunction()

function(require_success result output context)
    if(NOT "${result}" STREQUAL "0")
        message(
            FATAL_ERROR
            "${context}: runner failed with ${result}\n--- output ---\n${output}"
        )
    endif()
endfunction()

function(require_failure result output context)
    if("${result}" STREQUAL "0")
        message(
            FATAL_ERROR
            "${context}: runner unexpectedly succeeded\n--- output ---\n${output}"
        )
    endif()
endfunction()

function(write_executable path content)
    file(WRITE "${path}" "${content}")
    file(
        CHMOD "${path}"
        PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE
    )
endfunction()

set(module_stub [=[#!/usr/bin/env bash
set -euo pipefail
printf 'module|pwd=<%s>|tmpdir=<%s>|omp=<%s>' \
  "${PWD}" "${TMPDIR-}" "${OMP_NUM_THREADS-}" >> "${KAHIP_STUB_LOG}"
for argument in "$@"; do
  printf '|arg=<%s>' "${argument}" >> "${KAHIP_STUB_LOG}"
done
printf '\n' >> "${KAHIP_STUB_LOG}"
]=])

set(git_stub [=[#!/usr/bin/env bash
set -euo pipefail
printf 'git|pwd=<%s>|tmpdir=<%s>|omp=<%s>' \
  "${PWD}" "${TMPDIR-}" "${OMP_NUM_THREADS-}" >> "${KAHIP_STUB_LOG}"
for argument in "$@"; do
  printf '|arg=<%s>' "${argument}" >> "${KAHIP_STUB_LOG}"
done
printf '\n' >> "${KAHIP_STUB_LOG}"

if [[ ${1-} != -C || ${2-} != "${KAHIP_STUB_REPO_ROOT}" ]]; then
  exit 90
fi
shift 2
case "${1-}" in
  rev-parse)
    if [[ $# == 2 && ${2-} == --show-toplevel ]]; then
      printf '%s\n' "${KAHIP_STUB_REPO_ROOT}"
    elif [[ $# == 3 && ${2-} == --verify && ${3-} == 'HEAD^{commit}' ]]; then
      printf '%s\n' "${KAHIP_STUB_HEAD}"
    else
      exit 91
    fi
    ;;
  diff)
    if [[ $# != 5 || ${2-} != --quiet || ${3-} != --no-ext-diff || ${4-} != HEAD || ${5-} != -- ]]; then
      exit 92
    fi
    exit "${KAHIP_STUB_DIFF_STATUS}"
    ;;
  *)
    exit 93
    ;;
esac
]=])

set(cmake_stub [=[#!/usr/bin/env bash
set -euo pipefail
printf 'cmake|pwd=<%s>|tmpdir=<%s>|omp=<%s>' \
  "${PWD}" "${TMPDIR-}" "${OMP_NUM_THREADS-}" >> "${KAHIP_STUB_LOG}"
for argument in "$@"; do
  printf '|arg=<%s>' "${argument}" >> "${KAHIP_STUB_LOG}"
done
printf '\n' >> "${KAHIP_STUB_LOG}"

case "${1-}" in
  --fresh)
    [[ $# == 5 ]] || exit 94
    [[ ${2-} == --preset && ${3-} == "${KAHIP_STUB_CONFIGURE_PRESET}" ]] || exit 94
    [[ ${4-} == "-DCMAKE_PREFIX_PATH:PATH=${KAHIP_STUB_CATCH2_PREFIX}" ]] || exit 94
    [[ ${5-} == "-DKAHIP_SCALE_PROBE_SOURCE_REVISION=${KAHIP_STUB_SOURCE_REVISION}" ]] || exit 94
    ;;
  --build)
    [[ $# == 5 ]] || exit 94
    [[ ${2-} == --preset && ${3-} == "${KAHIP_STUB_BUILD_PRESET}" ]] || exit 94
    [[ ${4-} == --target && ${5-} == parhip_cube_scale_probe ]] || exit 94
    probe="${KAHIP_STUB_REPO_ROOT}/out/build/${KAHIP_STUB_CONFIGURE_PRESET}/parallel/parallel_src/tests/parhip_cube_scale_probe"
    mkdir -p "$(dirname "${probe}")"
    printf '#!/usr/bin/env bash\nexit 95\n' > "${probe}"
    chmod +x "${probe}"
    ;;
  *)
    exit 94
    ;;
esac
]=])

set(srun_stub [=[#!/usr/bin/env bash
set -euo pipefail
printf 'srun|pwd=<%s>|tmpdir=<%s>|omp=<%s>' \
  "${PWD}" "${TMPDIR-}" "${OMP_NUM_THREADS-}" >> "${KAHIP_STUB_LOG}"
for argument in "$@"; do
  printf '|arg=<%s>' "${argument}" >> "${KAHIP_STUB_LOG}"
done
printf '\n' >> "${KAHIP_STUB_LOG}"
probe="${KAHIP_STUB_REPO_ROOT}/out/build/${KAHIP_STUB_CONFIGURE_PRESET}/parallel/parallel_src/tests/parhip_cube_scale_probe"
[[ $# == 13 ]] || exit 95
[[ ${1-} == "--nodes=${KAHIP_STUB_NODES}" ]] || exit 95
[[ ${2-} == "--ntasks=${KAHIP_STUB_TASKS}" ]] || exit 95
[[ ${3-} == --ntasks-per-node=288 ]] || exit 95
[[ ${4-} == --cpus-per-task=1 ]] || exit 95
[[ ${5-} == --hint=nomultithread ]] || exit 95
[[ ${6-} == --distribution=block:block ]] || exit 95
[[ ${7-} == --kill-on-bad-exit ]] || exit 95
[[ ${8-} == --unbuffered ]] || exit 95
[[ ${9-} == "${probe}" ]] || exit 95
[[ ${10-} == --side && ${11-} == "${KAHIP_STUB_SIDE}" ]] || exit 95
[[ ${12-} == --expected-ranks && ${13-} == "${KAHIP_STUB_TASKS}" ]] || exit 95
printf 'scale-probe-stdout-marker\n'
printf 'scale-probe-stderr-marker\n' >&2
exit "${KAHIP_STUB_SRUN_STATUS}"
]=])

set(sbatch_stub [=[#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 1 || ! -f $1 ]]; then
  exit 96
fi
runner=$1

job_name=
wall_time=
exclusive=0
nodes=
tasks=
tasks_per_node=
cpus_per_task=
account=
partition=
qos=
chdir_path=
output_path=
while IFS= read -r line; do
  compact_line=${line//[[:space:]]/}
  if [[ -z ${compact_line} || ${line} == '#!'* ]]; then
    continue
  fi
  if [[ ${line} != '#'* ]]; then
    break
  fi
  case "${line}" in
    '#SBATCH --job-name='*) job_name=${line#*=} ;;
    '#SBATCH --time='*) wall_time=${line#*=} ;;
    '#SBATCH --exclusive') exclusive=1 ;;
    '#SBATCH --nodes='*) nodes=${line#*=} ;;
    '#SBATCH --ntasks='*) tasks=${line#*=} ;;
    '#SBATCH --ntasks-per-node='*) tasks_per_node=${line#*=} ;;
    '#SBATCH --cpus-per-task='*) cpus_per_task=${line#*=} ;;
    '#SBATCH --account='*) account=${line#*=} ;;
    '#SBATCH --partition='*) partition=${line#*=} ;;
    '#SBATCH --qos='*) qos=${line#*=} ;;
    '#SBATCH --chdir='*) chdir_path=${line#*=} ;;
    '#SBATCH --output='*) output_path=${line#*=} ;;
  esac
done < "${runner}"

[[ ${job_name} == "kahip-${KAHIP_STUB_COMPILER}-scale" ]] || exit 97
[[ ${wall_time} == 02:00:00 ]] || exit 98
[[ ${exclusive} == 1 ]] || exit 99
[[ ${nodes} == 8 && ${tasks} == 2304 ]] || exit 100
[[ ${tasks_per_node} == 288 && ${cpus_per_task} == 1 ]] || exit 101
[[ ${account} == e609 ]] || exit 102
[[ ${partition} == standard && ${qos} == standard ]] || exit 103
readonly fixed_repository=/work/e609/e609/eriche609/KaHIP
[[ ${chdir_path} == "${fixed_repository}" ]] || exit 104
[[ ${output_path} == "${fixed_repository}/out/slurm/%x-%j.out" ]] || exit 105

mapped_output=${output_path/#${fixed_repository}/${KAHIP_STUB_REPO_ROOT}}
mapped_output=${mapped_output//%x/${job_name}}
mapped_output=${mapped_output//%j/42001}
[[ -d $(dirname "${mapped_output}") ]] || exit 106
: > "${mapped_output}"

printf \
  'sbatch|job=<%s>|time=<%s>|exclusive=<%s>|nodes=<%s>|tasks=<%s>|tasks-per-node=<%s>|cpus-per-task=<%s>|account=<%s>|partition=<%s>|qos=<%s>|chdir=<%s>|output=<%s>\n' \
  "${job_name}" "${wall_time}" "${exclusive}" "${nodes}" "${tasks}" \
  "${tasks_per_node}" "${cpus_per_task}" "${account}" "${partition}" \
  "${qos}" "${chdir_path}" "${output_path}" >> "${KAHIP_STUB_LOG}"

export SLURM_JOB_ID=42001
export SLURM_JOB_NUM_NODES=${nodes}
export SLURM_NTASKS=${tasks}
export SLURM_NTASKS_PER_NODE=${tasks_per_node}
export SLURM_CPUS_PER_TASK=${cpus_per_task}
cd -- "${KAHIP_STUB_REPO_ROOT}"
exec "${KAHIP_STUB_BASH}" "${runner}"
]=])

function(make_expected_runner_log output_variable)
    set(
        one_value_arguments
        COMPILER
        MODULE
        CONFIGURE_PRESET
        BUILD_PRESET
        REPOSITORY
        CATCH2_PREFIX
        HEAD
        SOURCE_REVISION
        SIDE
        NODES
        TASKS
    )
    cmake_parse_arguments(ARG "" "${one_value_arguments}" "" ${ARGN})
    foreach(required_argument IN LISTS one_value_arguments)
        if(NOT DEFINED ARG_${required_argument})
            message(
                FATAL_ERROR
                "make_expected_runner_log requires ${required_argument}"
            )
        endif()
    endforeach()

    set(
        log
        "module|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<restore>\n"
    )
    string(
        APPEND log
        "module|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<load>|arg=<${ARG_MODULE}>\n"
        "module|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<load>|arg=<cmake/4.1.2>\n"
        "git|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<-C>|arg=<${ARG_REPOSITORY}>|arg=<rev-parse>|arg=<--show-toplevel>\n"
        "git|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<-C>|arg=<${ARG_REPOSITORY}>|arg=<rev-parse>|arg=<--verify>|arg=<HEAD^{commit}>\n"
        "git|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<-C>|arg=<${ARG_REPOSITORY}>|arg=<diff>|arg=<--quiet>|arg=<--no-ext-diff>|arg=<HEAD>|arg=<-->\n"
        "cmake|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<--fresh>|arg=<--preset>|arg=<${ARG_CONFIGURE_PRESET}>|arg=<-DCMAKE_PREFIX_PATH:PATH=${ARG_CATCH2_PREFIX}>|arg=<-DKAHIP_SCALE_PROBE_SOURCE_REVISION=${ARG_SOURCE_REVISION}>\n"
        "cmake|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<--build>|arg=<--preset>|arg=<${ARG_BUILD_PRESET}>|arg=<--target>|arg=<parhip_cube_scale_probe>\n"
        "srun|pwd=<${ARG_REPOSITORY}>|tmpdir=<${ARG_REPOSITORY}/out/tmp/cirrus-scale-${ARG_COMPILER}-42001>|omp=<1>|arg=<--nodes=${ARG_NODES}>|arg=<--ntasks=${ARG_TASKS}>|arg=<--ntasks-per-node=288>|arg=<--cpus-per-task=1>|arg=<--hint=nomultithread>|arg=<--distribution=block:block>|arg=<--kill-on-bad-exit>|arg=<--unbuffered>|arg=<${ARG_REPOSITORY}/out/build/${ARG_CONFIGURE_PRESET}/parallel/parallel_src/tests/parhip_cube_scale_probe>|arg=<--side>|arg=<${ARG_SIDE}>|arg=<--expected-ranks>|arg=<${ARG_TASKS}>\n"
    )
    set(${output_variable} "${log}" PARENT_SCOPE)
endfunction()

function(run_runner_case)
    set(options DEFAULT_SIDE SBATCH_SUBMISSION MISSING_SLURM_OUTPUT)
    set(
        one_value_arguments
        NAME
        RUNNER
        SIDE
        NODES
        TASKS
        TASKS_PER_NODE
        CPUS_PER_TASK
        HEAD
        DIFF_STATUS
        SRUN_STATUS
    )
    cmake_parse_arguments(ARG "${options}" "${one_value_arguments}" "" ${ARGN})

    foreach(
        required_argument
        IN ITEMS
            NAME
            RUNNER
            NODES
            TASKS
            TASKS_PER_NODE
            CPUS_PER_TASK
            HEAD
            DIFF_STATUS
            SRUN_STATUS
    )
        if(NOT DEFINED ARG_${required_argument})
            message(FATAL_ERROR "run_runner_case requires ${required_argument}")
        endif()
    endforeach()
    if(NOT ARG_DEFAULT_SIDE AND NOT DEFINED ARG_SIDE)
        message(FATAL_ERROR "run_runner_case requires SIDE or DEFAULT_SIDE")
    endif()

    if(ARG_DEFAULT_SIDE)
        set(runner_side 600)
    else()
        set(runner_side "${ARG_SIDE}")
    endif()
    if(ARG_DIFF_STATUS EQUAL 1)
        set(runner_source_revision "${ARG_HEAD}-dirty")
    else()
        set(runner_source_revision "${ARG_HEAD}")
    endif()
    if(ARG_RUNNER STREQUAL "run-gnu-scale-probe.slurm")
        set(runner_compiler gnu)
        set(runner_configure_preset cirrus-gnu-tests)
        set(runner_build_preset build-cirrus-gnu-tests)
    elseif(ARG_RUNNER STREQUAL "run-cray-scale-probe.slurm")
        set(runner_compiler cray)
        set(runner_configure_preset cirrus-cray-tests)
        set(runner_build_preset build-cirrus-cray-tests)
    else()
        message(FATAL_ERROR "unknown runner: ${ARG_RUNNER}")
    endif()

    set(case_root "${TEST_ROOT}/${ARG_NAME}")
    set(repository "${case_root}/KaHIP")
    set(stub_directory "${case_root}/stubs")
    set(log_path "${case_root}/commands.log")
    set(source_runner "${SOURCE_DIR}/ci/cirrus/${ARG_RUNNER}")
    set(copied_runner "${case_root}/slurm-spool/${ARG_RUNNER}")
    set(
        source_common_runner
        "${SOURCE_DIR}/ci/cirrus/run-scale-probe-common.sh"
    )
    set(
        copied_common_runner
        "${repository}/ci/cirrus/run-scale-probe-common.sh"
    )

    if(NOT EXISTS "${source_runner}")
        message(FATAL_ERROR "runner is missing: ${source_runner}")
    endif()
    if(NOT EXISTS "${source_common_runner}")
        message(
            FATAL_ERROR
            "common runner is missing: ${source_common_runner}"
        )
    endif()

    file(REMOVE_RECURSE "${case_root}")
    file(
        MAKE_DIRECTORY
        "${repository}/ci/cirrus"
        "${case_root}/opt/catch2"
        "${case_root}/slurm-spool"
    )
    if(NOT ARG_MISSING_SLURM_OUTPUT)
        file(MAKE_DIRECTORY "${repository}/out/slurm")
    endif()
    file(COPY_FILE "${source_runner}" "${copied_runner}")
    file(COPY_FILE "${source_common_runner}" "${copied_common_runner}")
    file(MAKE_DIRECTORY "${stub_directory}")
    write_executable("${stub_directory}/module" "${module_stub}")
    write_executable("${stub_directory}/git" "${git_stub}")
    write_executable("${stub_directory}/cmake" "${cmake_stub}")
    write_executable("${stub_directory}/srun" "${srun_stub}")
    write_executable("${stub_directory}/sbatch" "${sbatch_stub}")
    file(WRITE "${log_path}" "")

    set(
        environment_arguments
        --unset=BASH_FUNC_module%%
        --unset=BASH_FUNC_ml%%
        --unset=BASH_FUNC__module_raw%%
        "PATH=${stub_directory}:$ENV{PATH}"
        "KAHIP_STUB_LOG=${log_path}"
        "KAHIP_STUB_REPO_ROOT=${repository}"
        "KAHIP_STUB_HEAD=${ARG_HEAD}"
        "KAHIP_STUB_DIFF_STATUS=${ARG_DIFF_STATUS}"
        "KAHIP_STUB_SRUN_STATUS=${ARG_SRUN_STATUS}"
        "KAHIP_STUB_CONFIGURE_PRESET=${runner_configure_preset}"
        "KAHIP_STUB_BUILD_PRESET=${runner_build_preset}"
        "KAHIP_STUB_CATCH2_PREFIX=${case_root}/opt/catch2"
        "KAHIP_STUB_SOURCE_REVISION=${runner_source_revision}"
        "KAHIP_STUB_SIDE=${runner_side}"
        "KAHIP_STUB_NODES=${ARG_NODES}"
        "KAHIP_STUB_TASKS=${ARG_TASKS}"
    )
    if(ARG_SBATCH_SUBMISSION)
        list(
            APPEND environment_arguments
            "KAHIP_STUB_BASH=${BASH_EXECUTABLE}"
            "KAHIP_STUB_COMPILER=${runner_compiler}"
        )
        set(runner_command "${stub_directory}/sbatch" "${copied_runner}")
    else()
        list(
            APPEND environment_arguments
            "SLURM_JOB_ID=42001"
            "SLURM_JOB_NUM_NODES=${ARG_NODES}"
            "SLURM_NTASKS=${ARG_TASKS}"
            "SLURM_NTASKS_PER_NODE=${ARG_TASKS_PER_NODE}"
            "SLURM_CPUS_PER_TASK=${ARG_CPUS_PER_TASK}"
        )
        set(runner_command "${BASH_EXECUTABLE}" "${copied_runner}")
    endif()
    if(ARG_DEFAULT_SIDE)
        list(APPEND environment_arguments --unset=KAHIP_SCALE_PROBE_SIDE)
    else()
        list(
            APPEND environment_arguments
            "KAHIP_SCALE_PROBE_SIDE=${ARG_SIDE}"
        )
    endif()

    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env ${environment_arguments}
            ${runner_command}
        RESULT_VARIABLE runner_result
        OUTPUT_VARIABLE runner_stdout
        ERROR_VARIABLE runner_stderr
        TIMEOUT 10
        WORKING_DIRECTORY "${repository}"
    )
    file(READ "${log_path}" command_log)

    set(RUN_RESULT "${runner_result}" PARENT_SCOPE)
    set(RUN_OUTPUT "${runner_stdout}\n${runner_stderr}" PARENT_SCOPE)
    set(RUN_LOG "${command_log}" PARENT_SCOPE)
    set(RUN_REPOSITORY "${repository}" PARENT_SCOPE)
    set(RUN_CATCH2_PREFIX "${case_root}/opt/catch2" PARENT_SCOPE)
    set(
        RUN_SLURM_OUTPUT
        "${repository}/out/slurm/kahip-${runner_compiler}-scale-42001.out"
        PARENT_SCOPE
    )
endfunction()

set(clean_head "0123456789abcdef0123456789abcdef01234567")
set(dirty_head "89abcdef0123456789abcdef0123456789abcdef")

run_runner_case(
    NAME gnu-default-clean
    RUNNER run-gnu-scale-probe.slurm
    DEFAULT_SIDE
    SBATCH_SUBMISSION
    NODES 8
    TASKS 2304
    TASKS_PER_NODE 288
    CPUS_PER_TASK 1
    HEAD "${clean_head}"
    DIFF_STATUS 0
    SRUN_STATUS 0
)
require_success("${RUN_RESULT}" "${RUN_OUTPUT}" "GNU default clean case")
if(NOT EXISTS "${RUN_SLURM_OUTPUT}")
    message(FATAL_ERROR "stubbed Slurm did not pre-open ${RUN_SLURM_OUTPUT}")
endif()
require_contains(
    "${RUN_OUTPUT}"
    "compiler=gnu side=600 nodes=8 ranks=2304 bound=96562 source_revision=${clean_head}"
    "GNU default record"
)
require_contains("${RUN_LOG}" "module|pwd=<${RUN_REPOSITORY}>|" "GNU module cwd")
require_contains("${RUN_LOG}" "|arg=<restore>" "GNU module restore")
require_contains(
    "${RUN_LOG}"
    "|arg=<load>|arg=<ccs/gnu-2026-06>"
    "GNU programming environment"
)
require_contains(
    "${RUN_LOG}"
    "|arg=<load>|arg=<cmake/4.1.2>"
    "GNU CMake module"
)
require_contains(
    "${RUN_LOG}"
    "git|pwd=<${RUN_REPOSITORY}>|tmpdir=<${RUN_REPOSITORY}/out/tmp/cirrus-scale-gnu-42001>|omp=<1>|arg=<-C>|arg=<${RUN_REPOSITORY}>|arg=<rev-parse>|arg=<--show-toplevel>"
    "GNU repository identity"
)
require_contains(
    "${RUN_LOG}"
    "|arg=<rev-parse>|arg=<--verify>|arg=<HEAD^{commit}>"
    "GNU source revision"
)
require_contains(
    "${RUN_LOG}"
    "|arg=<diff>|arg=<--quiet>|arg=<--no-ext-diff>|arg=<HEAD>|arg=<-->"
    "GNU tracked dirty check"
)
require_contains(
    "${RUN_LOG}"
    "cmake|pwd=<${RUN_REPOSITORY}>|tmpdir=<${RUN_REPOSITORY}/out/tmp/cirrus-scale-gnu-42001>|omp=<1>|arg=<--fresh>|arg=<--preset>|arg=<cirrus-gnu-tests>|arg=<-DCMAKE_PREFIX_PATH:PATH=${RUN_CATCH2_PREFIX}>|arg=<-DKAHIP_SCALE_PROBE_SOURCE_REVISION=${clean_head}>"
    "GNU configure contract"
)
require_contains(
    "${RUN_LOG}"
    "cmake|pwd=<${RUN_REPOSITORY}>|tmpdir=<${RUN_REPOSITORY}/out/tmp/cirrus-scale-gnu-42001>|omp=<1>|arg=<--build>|arg=<--preset>|arg=<build-cirrus-gnu-tests>|arg=<--target>|arg=<parhip_cube_scale_probe>"
    "GNU target-only build"
)
string(
    REGEX MATCH
    "cmake\\|[^\n]*arg=<--build>[^\n]*"
    gnu_build_log
    "${RUN_LOG}"
)
require_not_contains(
    "${gnu_build_log}"
    "CMAKE_PREFIX_PATH"
    "Catch2 prefix must be configure-only"
)
require_contains(
    "${RUN_LOG}"
    "srun|pwd=<${RUN_REPOSITORY}>|tmpdir=<${RUN_REPOSITORY}/out/tmp/cirrus-scale-gnu-42001>|omp=<1>|arg=<--nodes=8>|arg=<--ntasks=2304>|arg=<--ntasks-per-node=288>|arg=<--cpus-per-task=1>|arg=<--hint=nomultithread>|arg=<--distribution=block:block>|arg=<--kill-on-bad-exit>|arg=<--unbuffered>|arg=<${RUN_REPOSITORY}/out/build/cirrus-gnu-tests/parallel/parallel_src/tests/parhip_cube_scale_probe>|arg=<--side>|arg=<600>|arg=<--expected-ranks>|arg=<2304>"
    "GNU launch contract"
)
make_expected_runner_log(
    expected_gnu_log
    COMPILER gnu
    MODULE ccs/gnu-2026-06
    CONFIGURE_PRESET cirrus-gnu-tests
    BUILD_PRESET build-cirrus-gnu-tests
    REPOSITORY "${RUN_REPOSITORY}"
    CATCH2_PREFIX "${RUN_CATCH2_PREFIX}"
    HEAD "${clean_head}"
    SOURCE_REVISION "${clean_head}"
    SIDE 600
    NODES 8
    TASKS 2304
)
set(
    expected_sbatch_log
    "sbatch|job=<kahip-gnu-scale>|time=<02:00:00>|exclusive=<1>|nodes=<8>|tasks=<2304>|tasks-per-node=<288>|cpus-per-task=<1>|account=<e609>|partition=<standard>|qos=<standard>|chdir=</work/e609/e609/eriche609/KaHIP>|output=</work/e609/e609/eriche609/KaHIP/out/slurm/%x-%j.out>\n"
)
require_equal(
    "${RUN_LOG}"
    "${expected_sbatch_log}${expected_gnu_log}"
    "exact GNU submission and runner trace"
)
require_contains(
    "${RUN_OUTPUT}" "scale-probe-stdout-marker" "unfiltered GNU stdout"
)
require_contains(
    "${RUN_OUTPUT}" "scale-probe-stderr-marker" "unfiltered GNU stderr"
)

run_runner_case(
    NAME cray-default-submission
    RUNNER run-cray-scale-probe.slurm
    DEFAULT_SIDE
    SBATCH_SUBMISSION
    NODES 8
    TASKS 2304
    TASKS_PER_NODE 288
    CPUS_PER_TASK 1
    HEAD "${clean_head}"
    DIFF_STATUS 0
    SRUN_STATUS 0
)
require_success(
    "${RUN_RESULT}" "${RUN_OUTPUT}" "Cray default submission"
)
if(NOT EXISTS "${RUN_SLURM_OUTPUT}")
    message(FATAL_ERROR "stubbed Slurm did not pre-open ${RUN_SLURM_OUTPUT}")
endif()
make_expected_runner_log(
    expected_cray_default_log
    COMPILER cray
    MODULE PrgEnv-cray/8.6.0
    CONFIGURE_PRESET cirrus-cray-tests
    BUILD_PRESET build-cirrus-cray-tests
    REPOSITORY "${RUN_REPOSITORY}"
    CATCH2_PREFIX "${RUN_CATCH2_PREFIX}"
    HEAD "${clean_head}"
    SOURCE_REVISION "${clean_head}"
    SIDE 600
    NODES 8
    TASKS 2304
)
set(
    expected_cray_sbatch_log
    "sbatch|job=<kahip-cray-scale>|time=<02:00:00>|exclusive=<1>|nodes=<8>|tasks=<2304>|tasks-per-node=<288>|cpus-per-task=<1>|account=<e609>|partition=<standard>|qos=<standard>|chdir=</work/e609/e609/eriche609/KaHIP>|output=</work/e609/e609/eriche609/KaHIP/out/slurm/%x-%j.out>\n"
)
require_equal(
    "${RUN_LOG}"
    "${expected_cray_sbatch_log}${expected_cray_default_log}"
    "exact Cray submission and runner trace"
)

run_runner_case(
    NAME missing-slurm-output-directory
    RUNNER run-gnu-scale-probe.slurm
    DEFAULT_SIDE
    SBATCH_SUBMISSION
    MISSING_SLURM_OUTPUT
    NODES 8
    TASKS 2304
    TASKS_PER_NODE 288
    CPUS_PER_TASK 1
    HEAD "${clean_head}"
    DIFF_STATUS 0
    SRUN_STATUS 0
)
require_failure(
    "${RUN_RESULT}"
    "${RUN_OUTPUT}"
    "missing pre-submit Slurm output directory"
)
if(NOT "${RUN_LOG}" STREQUAL "")
    message(
        FATAL_ERROR
        "Slurm started a job before opening its configured output\n${RUN_LOG}"
    )
endif()

run_runner_case(
    NAME cray-largest-dirty
    RUNNER run-cray-scale-probe.slurm
    SIDE 1008
    NODES 38
    TASKS 10944
    TASKS_PER_NODE 288
    CPUS_PER_TASK 1
    HEAD "${dirty_head}"
    DIFF_STATUS 1
    SRUN_STATUS 0
)
require_success("${RUN_RESULT}" "${RUN_OUTPUT}" "Cray dirty case")
require_contains(
    "${RUN_OUTPUT}"
    "compiler=cray side=1008 nodes=38 ranks=10944 bound=96392 source_revision=${dirty_head}-dirty"
    "Cray dirty record"
)
require_contains(
    "${RUN_LOG}"
    "|arg=<load>|arg=<PrgEnv-cray/8.6.0>"
    "Cray programming environment"
)
require_contains(
    "${RUN_LOG}"
    "|arg=<--preset>|arg=<cirrus-cray-tests>|arg=<-DCMAKE_PREFIX_PATH:PATH=${RUN_CATCH2_PREFIX}>|arg=<-DKAHIP_SCALE_PROBE_SOURCE_REVISION=${dirty_head}-dirty>"
    "Cray configure contract"
)
require_contains(
    "${RUN_LOG}"
    "|arg=<--preset>|arg=<build-cirrus-cray-tests>|arg=<--target>|arg=<parhip_cube_scale_probe>"
    "Cray target-only build"
)
require_contains(
    "${RUN_LOG}"
    "srun|pwd=<${RUN_REPOSITORY}>|tmpdir=<${RUN_REPOSITORY}/out/tmp/cirrus-scale-cray-42001>|omp=<1>|arg=<--nodes=38>|arg=<--ntasks=10944>|arg=<--ntasks-per-node=288>|arg=<--cpus-per-task=1>|arg=<--hint=nomultithread>|arg=<--distribution=block:block>|arg=<--kill-on-bad-exit>|arg=<--unbuffered>|arg=<${RUN_REPOSITORY}/out/build/cirrus-cray-tests/parallel/parallel_src/tests/parhip_cube_scale_probe>|arg=<--side>|arg=<1008>|arg=<--expected-ranks>|arg=<10944>"
    "Cray launch contract"
)
make_expected_runner_log(
    expected_cray_log
    COMPILER cray
    MODULE PrgEnv-cray/8.6.0
    CONFIGURE_PRESET cirrus-cray-tests
    BUILD_PRESET build-cirrus-cray-tests
    REPOSITORY "${RUN_REPOSITORY}"
    CATCH2_PREFIX "${RUN_CATCH2_PREFIX}"
    HEAD "${dirty_head}"
    SOURCE_REVISION "${dirty_head}-dirty"
    SIDE 1008
    NODES 38
    TASKS 10944
)
require_equal(
    "${RUN_LOG}" "${expected_cray_log}" "exact Cray runner trace"
)

foreach(tuple IN ITEMS "755,16,4608,96198" "900,27,7776,96562")
    string(REPLACE "," ";" tuple_fields "${tuple}")
    list(GET tuple_fields 0 tuple_side)
    list(GET tuple_fields 1 tuple_nodes)
    list(GET tuple_fields 2 tuple_tasks)
    list(GET tuple_fields 3 tuple_bound)
    run_runner_case(
        NAME "gnu-tuple-${tuple_side}"
        RUNNER run-gnu-scale-probe.slurm
        SIDE "${tuple_side}"
        NODES "${tuple_nodes}"
        TASKS "${tuple_tasks}"
        TASKS_PER_NODE 288
        CPUS_PER_TASK 1
        HEAD "${clean_head}"
        DIFF_STATUS 0
        SRUN_STATUS 0
    )
    require_success(
        "${RUN_RESULT}" "${RUN_OUTPUT}" "GNU tuple ${tuple_side}"
    )
    require_contains(
        "${RUN_OUTPUT}"
        "side=${tuple_side} nodes=${tuple_nodes} ranks=${tuple_tasks} bound=${tuple_bound}"
        "GNU tuple ${tuple_side} record"
    )
    make_expected_runner_log(
        expected_tuple_log
        COMPILER gnu
        MODULE ccs/gnu-2026-06
        CONFIGURE_PRESET cirrus-gnu-tests
        BUILD_PRESET build-cirrus-gnu-tests
        REPOSITORY "${RUN_REPOSITORY}"
        CATCH2_PREFIX "${RUN_CATCH2_PREFIX}"
        HEAD "${clean_head}"
        SOURCE_REVISION "${clean_head}"
        SIDE "${tuple_side}"
        NODES "${tuple_nodes}"
        TASKS "${tuple_tasks}"
    )
    require_equal(
        "${RUN_LOG}"
        "${expected_tuple_log}"
        "exact GNU tuple ${tuple_side} trace"
    )
endforeach()

foreach(
    invalid_case
    IN ITEMS
        "unsupported-side,601,8,2304,288,1"
        "wrong-nodes,755,8,4608,288,1"
        "wrong-ranks,755,16,2304,288,1"
        "wrong-ranks-per-node,755,16,4608,144,1"
        "wrong-cpus-per-rank,755,16,4608,288,2"
)
    string(REPLACE "," ";" invalid_fields "${invalid_case}")
    list(GET invalid_fields 0 invalid_name)
    list(GET invalid_fields 1 invalid_side)
    list(GET invalid_fields 2 invalid_nodes)
    list(GET invalid_fields 3 invalid_tasks)
    list(GET invalid_fields 4 invalid_tasks_per_node)
    list(GET invalid_fields 5 invalid_cpus_per_task)
    run_runner_case(
        NAME "invalid-${invalid_name}"
        RUNNER run-gnu-scale-probe.slurm
        SIDE "${invalid_side}"
        NODES "${invalid_nodes}"
        TASKS "${invalid_tasks}"
        TASKS_PER_NODE "${invalid_tasks_per_node}"
        CPUS_PER_TASK "${invalid_cpus_per_task}"
        HEAD "${clean_head}"
        DIFF_STATUS 0
        SRUN_STATUS 0
    )
    require_failure(
        "${RUN_RESULT}" "${RUN_OUTPUT}" "invalid case ${invalid_name}"
    )
    if(EXISTS "${TEST_ROOT}/invalid-${invalid_name}/commands.log")
        file(
            READ
            "${TEST_ROOT}/invalid-${invalid_name}/commands.log"
            invalid_log
        )
        if(NOT "${invalid_log}" STREQUAL "")
            message(
                FATAL_ERROR
                "invalid case ${invalid_name} invoked tools\n${invalid_log}"
            )
        endif()
    endif()
endforeach()

run_runner_case(
    NAME invalid-source-revision
    RUNNER run-gnu-scale-probe.slurm
    DEFAULT_SIDE
    NODES 8
    TASKS 2304
    TASKS_PER_NODE 288
    CPUS_PER_TASK 1
    HEAD unknown
    DIFF_STATUS 0
    SRUN_STATUS 0
)
require_failure(
    "${RUN_RESULT}" "${RUN_OUTPUT}" "unknown source revision rejection"
)
require_not_contains(
    "${RUN_LOG}" "cmake|" "unknown source revision reached configure"
)
require_not_contains(
    "${RUN_LOG}" "srun|" "unknown source revision reached launcher"
)

run_runner_case(
    NAME git-inspection-failure
    RUNNER run-gnu-scale-probe.slurm
    DEFAULT_SIDE
    NODES 8
    TASKS 2304
    TASKS_PER_NODE 288
    CPUS_PER_TASK 1
    HEAD "${clean_head}"
    DIFF_STATUS 2
    SRUN_STATUS 0
)
require_failure(
    "${RUN_RESULT}" "${RUN_OUTPUT}" "Git dirty-inspection failure"
)
require_not_contains(
    "${RUN_LOG}" "cmake|" "failed Git inspection reached configure"
)
require_not_contains(
    "${RUN_LOG}" "srun|" "failed Git inspection reached launcher"
)

run_runner_case(
    NAME nonzero-launch
    RUNNER run-cray-scale-probe.slurm
    DEFAULT_SIDE
    NODES 8
    TASKS 2304
    TASKS_PER_NODE 288
    CPUS_PER_TASK 1
    HEAD "${clean_head}"
    DIFF_STATUS 0
    SRUN_STATUS 37
)
if(NOT "${RUN_RESULT}" STREQUAL "37")
    message(
        FATAL_ERROR
        "nonzero srun status was not preserved: got ${RUN_RESULT}\n${RUN_OUTPUT}"
    )
endif()
require_contains("${RUN_LOG}" "srun|" "nonzero launcher invocation")
