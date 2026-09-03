/******************************************************************************
 * definitions.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef DEFINITIONS_H_CHRA
#define DEFINITIONS_H_CHRA
#include <cstdint>

#include <climits>
#include "macros_assertions.h" 
#include <cstdio>


// allows us to disable most of the output during partitioning
#ifndef NOOUTPUT 
        #define PRINT(x) x
#else
        #define PRINT(x) do {} while (false);
#endif

namespace parhip {
/**********************************************
 * Constants
 * ********************************************/
//Types needed for the parallel graph ds
//we use long since we want to partition huge graphs
using ULONG = unsigned long long;
using UINT = unsigned int;
using NodeID = unsigned long long;
using EdgeID = unsigned long long;
using PartitionID = unsigned long long;
using NodeWeight = unsigned long long;
using EdgeWeight = unsigned long long;
using PEID = int;

constexpr PEID ROOT = 0;

enum class PermutationQuality : std::uint8_t {
        PERMUTATION_QUALITY_NONE, 
        PERMUTATION_QUALITY_FAST,
        PERMUTATION_QUALITY_GOOD
};

enum class InitialPartitioningAlgorithm : std::uint8_t {
        KAFFPAESTRONG,
        KAFFPAEECO,
        KAFFPAEFAST,
        KAFFPAEULTRAFASTSNW,
        KAFFPAEFASTSNW,
        KAFFPAEECOSNW,
        KAFFPAESTRONGSNW,
        RANDOMIP
};

struct source_target_pair {
        NodeID source;
        NodeID target;
};

enum class NodeOrderingType : std::uint8_t {
        RANDOM_NODEORDERING,
        DEGREE_NODEORDERING,
        LEASTGHOSTNODESFIRST_DEGREE_NODEODERING,
        DEGREE_LEASTGHOSTNODESFIRST_NODEODERING
};
}
#endif

  //Tag Listing of Isend Operations(they should be unique per level) 
  //**************************************************************************
  //rank +   size                projection algorithm
  //rank + 2*size                projection algorithm
  //rank + 3*size                update labels global
  //rank + 4*size                contraction algorithm / label mapping
  //rank + 5*size                 --  ""  --
  //rank + 6*size                contracion algorithm / get nodes to cnodes
  //rank + 7*size                redist hashed graph
  //rank + 8*size                redist hashed graph
  //rank + 9*size                communicate node weights
  //rank + 10*size               down propagation
  //rank + 11*size               down propagation
  //rank + 12*size               MPI Tools
  //rank + 13*size               MPI Tools
  //rank + 100*size + x          Label Isends  
 


