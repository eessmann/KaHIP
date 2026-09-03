#!/bin/bash --login
#SBATCH --job-name=kahip-gnu-scale
#SBATCH --time=02:00:00
#SBATCH --exclusive
#SBATCH --nodes=8
#SBATCH --ntasks=2304
#SBATCH --ntasks-per-node=288
#SBATCH --cpus-per-task=1
#SBATCH --account=e609
#SBATCH --partition=standard
#SBATCH --qos=standard
#SBATCH --chdir=/work/e609/e609/eriche609/KaHIP
#SBATCH --output=/work/e609/e609/eriche609/KaHIP/out/slurm/%x-%j.out

set -euo pipefail

source "${PWD}/ci/cirrus/run-scale-probe-common.sh"
kahip_run_scale_probe \
  gnu ccs/gnu-2026-06 cirrus-gnu-tests build-cirrus-gnu-tests
