/******************************************************************************
 * parallel_projection.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef PARALLEL_PROJECTION_HBRCPQ0P
#define PARALLEL_PROJECTION_HBRCPQ0P

#include <boost/hana/tuple.hpp>

#include "communication/mpi_types.h"
#include "data_structure/parallel_graph_access.h"
namespace parhip {
namespace projection {
struct request {
        NodeID request_id;
        NodeID coarse_global_id;
};

struct reply {
        NodeID request_id;
        NodeID coarse_global_id;
        NodeID label;
};
}  // namespace projection

class parallel_projection {
public:
        parallel_projection();
        virtual ~parallel_projection();

        void parallel_project( MPI_Comm communicator, parallel_graph_access & finer, parallel_graph_access & coarser );

        //initial assignment after initial partitioning
        void initial_assignment( parallel_graph_access & G, complete_graph_access & Q);
};
}

template <>
struct parhip::mpi::wire_members<parhip::projection::request> {
        inline static constexpr auto value = boost::hana::make_tuple(
            &parhip::projection::request::request_id,
            &parhip::projection::request::coarse_global_id);
};

template <>
struct parhip::mpi::wire_members<parhip::projection::reply> {
        inline static constexpr auto value = boost::hana::make_tuple(
            &parhip::projection::reply::request_id,
            &parhip::projection::reply::coarse_global_id,
            &parhip::projection::reply::label);
};


#endif /* end of include guard: PARALLEL_PROJECTION_HBRCPQ0P */
