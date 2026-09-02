#!/bin/bash --login
#SBATCH --job-name=kahip-cray-tests
#SBATCH --time=02:00:00
#SBATCH --exclusive
#SBATCH --nodes=1
#SBATCH --ntasks=5
#SBATCH --cpus-per-task=1
#SBATCH --account=e609
#SBATCH --partition=standard
#SBATCH --qos=standard
#SBATCH --chdir=/work/e609/e609/eriche609/KaHIP
#SBATCH --output=/work/e609/e609/eriche609/KaHIP/out/slurm/%x-%j.out

set -euo pipefail

module restore
module load PrgEnv-cray/8.6.0
module load cmake/4.1.2

export OMP_NUM_THREADS=1

readonly catch2_prefix=/work/e609/e609/eriche609/opt/catch2
test -d "${catch2_prefix}"
command -v srun

cmake --fresh --preset cirrus-cray-tests \
  "-DCMAKE_PREFIX_PATH:PATH=${catch2_prefix}"
cmake --build --preset build-cirrus-cray-tests
# Individual MPI tests use srun and its preflags from the configure preset.
# Keep CTest itself outside srun and execute one test/job step at a time.
ctest --preset test-cirrus-cray-tests --parallel 1
