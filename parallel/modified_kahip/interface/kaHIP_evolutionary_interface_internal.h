#pragma once

#include <cstdint>

#include <mpi.h>

namespace kahip::modified {
void kaffpaE_with_upper_bound(int* n,
                              int* vwgt,
                              int* xadj,
                              int* adjcwgt,
                              int* adjncy,
                              int* nparts,
                              double* imbalance,
                              bool suppress_output,
                              bool graph_partitioned,
                              int time_limit,
                              int seed,
                              int mode,
                              MPI_Comm communicator,
                              std::uint64_t authoritative_upper_bound,
                              int* edgecut,
                              double* balance,
                              int* part);
}  // namespace kahip::modified
