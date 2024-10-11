/******************************************************************************
 * gal_combine.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef GAL_COMBINE_XDMU5YB7
#define GAL_COMBINE_XDMU5YB7

#include "partition_config.h"
#include "data_structure/graph_access.h"
namespace kahip::modified {
class gal_combine {
public:
  gal_combine() = default;  // Default constructor
  virtual ~gal_combine() = default; // Default destructor

  // Explicitly default the other special member functions
  gal_combine(const gal_combine&) = default;            // Copy constructor
  gal_combine& operator=(const gal_combine&) = default; // Copy assignment operator
  gal_combine(gal_combine&&) = default;                 // Move constructor
  gal_combine& operator=(gal_combine&&) = default;      // Move assignment operator

  void perform_gal_combine( PartitionConfig & config, graph_access & G);
};
}

#endif /* end of include guard: GAL_COMBINE_XDMU5YB7 */
