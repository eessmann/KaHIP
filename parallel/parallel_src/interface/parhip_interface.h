/*
 *  * ParHIP.h
 *  *
 *  Author: Christian Schulz <christian.schulz.phone@gmail.com>
 *  */

#ifndef PARHIP_INTERFACE
#define PARHIP_INTERFACE
#include <mpi.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
#define PARHIP_NOEXCEPT noexcept
#else
#define PARHIP_NOEXCEPT
#endif
typedef unsigned long long idxtype;

#ifdef __cplusplus
inline constexpr int ULTRAFASTMESH = 0;
inline constexpr int FASTMESH = 1;
inline constexpr int ECOMESH = 2;
inline constexpr int ULTRAFASTSOCIAL = 3;
inline constexpr int PARHIP_FASTSOCIAL = 4;
inline constexpr int PARHIP_ECOSOCIAL = 5;
#else
enum {
  ULTRAFASTMESH = 0,
  FASTMESH = 1,
  ECOMESH = 2,
  ULTRAFASTSOCIAL = 3,
  PARHIP_FASTSOCIAL = 4,
  PARHIP_ECOSOCIAL = 5
};
#endif

/* See kaHIP_interface.h for the legacy-name compatibility contract. */
#ifndef KAHIP_LEGACY_SOCIAL_MODE_NAMES_DEFINED
#define KAHIP_LEGACY_SOCIAL_MODE_NAMES_DEFINED
#ifdef __cplusplus
inline constexpr int FASTSOCIAL = PARHIP_FASTSOCIAL;
inline constexpr int ECOSOCIAL = PARHIP_ECOSOCIAL;
#else
enum { FASTSOCIAL = PARHIP_FASTSOCIAL, ECOSOCIAL = PARHIP_ECOSOCIAL };
#endif
#endif

/*
 * Calls are sequential and non-reentrant within a process.  ParHIP preserves
 * the deterministic upstream PRNG and refinement caches, whose state is
 * process/thread global.  Repeated sequential calls are supported; concurrent
 * calls from multiple threads are not.
 */
#ifdef __cplusplus
extern "C" {
#endif

void ParHIPPartitionKWay(idxtype *vtxdist, idxtype *xadj, idxtype *adjncy, idxtype *vwgt, idxtype *adjwgt,
                         int *nparts, double* imbalance, bool suppress_output, int seed, int mode, int *edgecut, idxtype *part, 
                         MPI_Comm *comm) PARHIP_NOEXCEPT;
#ifdef __cplusplus
}
#endif

#undef PARHIP_NOEXCEPT


#endif /* end of include guard: PARHIP_INTERFAVE */
