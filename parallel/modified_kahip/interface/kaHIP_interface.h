/******************************************************************************
 * kaHIP_interface.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef KAFFPA_INTERFACE_RYEEZ6WJ
#define KAFFPA_INTERFACE_RYEEZ6WJ

#ifdef __cplusplus

#include <mpi.h>

#define KAHIP_MODIFIED_NOEXCEPT noexcept

extern "C"
{
#else
#define KAHIP_MODIFIED_NOEXCEPT
#endif

const int FAST            = 0;
const int ECO             = 1;
const int STRONG          = 2;
const int FASTSOCIAL      = 3;
const int ECOSOCIAL       = 4;
const int STRONGSOCIAL    = 5;
const int ULTRAFASTSOCIAL = 6;

// same data structures as in metis 
// edgecut and part are output parameters
// part has to be an array of n ints
void kaffpa(int* n, int* vwgt, int* xadj, 
                   int* adjcwgt, int* adjncy, int* nparts, 
                   double* imbalance,  bool suppress_output, int seed, int mode,
                   int* edgecut, int* part);


void node_separator(int* n, int* vwgt, int* xadj, 
                    int* adjcwgt, int* adjncy, int* nparts, 
                    double* imbalance,  bool suppress_output, int seed, int mode,
                    int* num_separator_vertices, int** separator); 

void kaffpaE(int* n, int* vwgt, int* xadj, int* adjcwgt, 
                    int* adjncy, int* nparts, 
                    double* inbalance,  bool suppress_output, 
                    bool graph_partitioned, int time_limit, int seed, 
                    int mode, 
                    MPI_Comm communicator, 
                    int* edgecut, double* balance, int* part)
                    KAHIP_MODIFIED_NOEXCEPT;


#ifdef __cplusplus
}
#endif

#undef KAHIP_MODIFIED_NOEXCEPT

#endif /* end of include guard: KAFFPA_INTERFACE_RYEEZ6WJ */
