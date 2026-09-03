#!/bin/bash --login
#SBATCH --job-name=kahip-cray-scale
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
  cray PrgEnv-cray/8.6.0 cirrus-cray-tests build-cirrus-cray-tests
