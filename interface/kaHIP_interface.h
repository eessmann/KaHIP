/******************************************************************************
 * kaffpa_interface.h
 *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 *****************************************************************************/


#ifndef KAFFPA_INTERFACE_RYEEZ6WJ
#define KAFFPA_INTERFACE_RYEEZ6WJ

#include <stdint.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef KAHIP_64BIT
typedef int64_t kahip_idx;
#else
typedef int32_t kahip_idx;
#endif

#ifdef __cplusplus
inline constexpr int FAST = 0;
inline constexpr int ECO = 1;
inline constexpr int STRONG = 2;
inline constexpr int KAHIP_FASTSOCIAL = 3;
inline constexpr int KAHIP_ECOSOCIAL = 4;
#else
enum {
  FAST = 0,
  ECO = 1,
  STRONG = 2,
  KAHIP_FASTSOCIAL = 3,
  KAHIP_ECOSOCIAL = 4
};
#endif

/*
 * KaHIP and ParHIP historically exposed different values under the same two
 * unprefixed names.  Keep those source-compatible aliases when either header
 * is used alone.  A translation unit that includes both interfaces must use
 * KAHIP_* or PARHIP_* for the two ambiguous social modes; the first included
 * header retains the legacy aliases.
 */
#ifndef KAHIP_LEGACY_SOCIAL_MODE_NAMES_DEFINED
#define KAHIP_LEGACY_SOCIAL_MODE_NAMES_DEFINED
#ifdef __cplusplus
inline constexpr int FASTSOCIAL = KAHIP_FASTSOCIAL;
inline constexpr int ECOSOCIAL = KAHIP_ECOSOCIAL;
#else
enum { FASTSOCIAL = KAHIP_FASTSOCIAL, ECOSOCIAL = KAHIP_ECOSOCIAL };
#endif
#endif
#ifdef __cplusplus
inline constexpr int STRONGSOCIAL = 5;
inline constexpr int MAPMODE_MULTISECTION = 0;
inline constexpr int MAPMODE_BISECTION = 1;
#else
enum {
  STRONGSOCIAL = 5,
  MAPMODE_MULTISECTION = 0,
  MAPMODE_BISECTION = 1
};
#endif

#ifdef __cplusplus
extern "C" {
#define KAHIP_DEFAULT_ARGUMENT(value) = value
#define KAHIP_NOEXCEPT noexcept
#else
#define KAHIP_DEFAULT_ARGUMENT(value)
#define KAHIP_NOEXCEPT
#endif

// returns the size of kahip_idx in bytes (4 for 32-bit, 8 for 64-bit)
int kahip_sizeof_idx(void);

// same data structures as in metis
// edgecut and part are output parameters
// part has to be an array of n ints
void kaffpa(int* n, int* vwgt, kahip_idx* xadj,
                   kahip_idx* adjcwgt, kahip_idx* adjncy, int* nparts,
                   double* imbalance, bool suppress_output, int seed, int mode,
                   kahip_idx* edgecut, int* part) KAHIP_NOEXCEPT;

// same as kaffpa, provides an additional parameter for perfect balance
void kaffpa_balance(int* n, int* vwgt, kahip_idx* xadj,
                   kahip_idx* adjcwgt, kahip_idx* adjncy, int* nparts,
                   double* imbalance,
                   bool perfectly_balance,
                   bool suppress_output, int seed, int mode,
                   kahip_idx* edgecut, int* part) KAHIP_NOEXCEPT;

// balance constraint on nodes and edges
void kaffpa_balance_NE(int* n, int* vwgt, kahip_idx* xadj,
                kahip_idx* adjcwgt, kahip_idx* adjncy, int* nparts,
                double* imbalance,  bool suppress_output, int seed, int mode,
                kahip_idx* edgecut, int* part) KAHIP_NOEXCEPT;

// same data structures as in metis
// edgecut and part and qap are output parameters
// part has to be an array of n ints
void process_mapping(int* n, int* vwgt, kahip_idx* xadj,
                   kahip_idx* adjcwgt, kahip_idx* adjncy,
                   int* hierarchy_parameter,  int* distance_parameter, int hierarchy_depth,
                   int mode_partitioning, int mode_mapping,
                   double* imbalance,
                   bool suppress_output, int seed,
                   kahip_idx* edgecut, int* qap, int* part) KAHIP_NOEXCEPT;


void node_separator(int* n, int* vwgt, kahip_idx* xadj,
                    kahip_idx* adjcwgt, kahip_idx* adjncy, int* nparts,
                    double* imbalance,  bool suppress_output, int seed, int mode,
                    int* num_separator_vertices, int** separator)
                    KAHIP_NOEXCEPT;

// takes an unweighted graph and performs reduced nested dissection
// ordering is the output parameter, an array of n ints
void reduced_nd(int* n, kahip_idx* xadj, kahip_idx* adjncy,
                bool suppress_output, int seed, int mode,
                int* ordering) KAHIP_NOEXCEPT;

void edge_partitioning(int* n, int* vwgt, kahip_idx* xadj,
                   kahip_idx* adjcwgt, kahip_idx* adjncy, int* nparts,
                   double* imbalance, bool suppress_output, int seed, int mode,
                   int* vertexcut, int* part,
                   kahip_idx infinity_edge_weight KAHIP_DEFAULT_ARGUMENT(1000))
                   KAHIP_NOEXCEPT;

#ifdef USEMETIS
// reduced nested dissection with metis
void reduced_nd_fast(int* n, kahip_idx* xadj, kahip_idx* adjncy,
                      bool suppress_output, int seed, int* ordering)
                      KAHIP_NOEXCEPT;
#endif

#ifdef __cplusplus
}
#endif

#undef KAHIP_DEFAULT_ARGUMENT
#undef KAHIP_NOEXCEPT

#endif /* end of include guard: KAFFPA_INTERFACE_RYEEZ6WJ */
