/******************************************************************************
 * kaffpa.cpp 
 *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 *
 *****************************************************************************/

#include <array>
#include <iostream>
#include <vector>

#include "kaHIP_interface.h"


int main() {

        std::cout <<  "partitioning graph from the manual"  << std::endl;

        int n            = 5;
        std::array<kahip_idx, 6> xadj{0, 2, 5, 7, 9, 12};
        std::array<kahip_idx, 12> adjncy{1, 4, 0, 2, 4, 1,
                                         3, 2, 4, 0, 1, 3};

        double imbalance = 0.03;
        std::vector<int> part(static_cast<std::size_t>(n));
        kahip_idx edge_cut = 0;
        int nparts       = 2;
        int* vwgt          = nullptr;
        kahip_idx* adjcwgt = nullptr;

        //void kaffpa(int* n, int* vwgt, int* xadj, 
                   //int* adjcwgt, int* adjncy, int* nparts, 
                   //double* imbalance,  bool suppress_output, int seed, int mode, 
                   //int* edgecut, int* part);

        kaffpa(&n, vwgt, xadj.data(), adjcwgt, adjncy.data(), &nparts,
               &imbalance, false, 0, ECO, &edge_cut, part.data());

        std::cout <<  "edge cut " <<  edge_cut  << std::endl;

        //void process_mapping(int* n, int* vwgt, int* xadj, 
                   //int* adjcwgt, int* adjncy, 
                   //int* hierarchy_parameter,  int* distance_parameter, int hierarchy_depth, 
                   //int mode_partitioning, int mode_mapping,
                   //double* imbalance,  
                   //bool suppress_output, int seed,
                   //int* edgecut, int* qap, int* part); 

        std::array<int, 2> hierarchy_parameter{2, 2};
        std::array<int, 2> distance_parameter{1, 100};
        int qap = 0;

        process_mapping(&n, vwgt, xadj.data(), adjcwgt, adjncy.data(),
                        hierarchy_parameter.data(), distance_parameter.data(),
                        2, STRONG, MAPMODE_MULTISECTION, &imbalance, false, 0,
                        &edge_cut, &qap, part.data());

        std::cout <<  "edge cut " <<  edge_cut  << std::endl;
        std::cout <<  "qap " <<  qap << std::endl;
                
}
