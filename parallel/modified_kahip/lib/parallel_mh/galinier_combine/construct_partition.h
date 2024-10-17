/******************************************************************************
 * construct_partition.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef CONSTRUCT_PARTITION_E86DQF5S
#define CONSTRUCT_PARTITION_E86DQF5S

#include "data_structure/graph_access.h"
#include "parallel_mh/population.h"
#include "partition_config.h"
namespace kahip::modified {
class construct_partition {
public:
  construct_partition() = default;  // Default constructor
  virtual ~construct_partition() = default; // Default destructor

  // Explicitly default the other special member functions
  construct_partition(const construct_partition&) = default;            // Copy constructor
  construct_partition& operator=(const construct_partition&) = default; // Copy assignment operator
  construct_partition(construct_partition&&) = default;                 // Move constructor
  construct_partition& operator=(construct_partition&&) = default;      // Move assignment operator

  void construct_starting_from_partition( PartitionConfig & config, graph_access & G);
  void createIndividuum( PartitionConfig & config, graph_access & G,
                         Individuum & ind, bool output);
};
}

#endif /* end of include guard: CONSTRUCT_PARTITION_E86DQF5S */
