/******************************************************************************
 * parallel_vector_io.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef PARALLEL_VECTOR_IO_BZVNZ570A
#define PARALLEL_VECTOR_IO_BZVNZ570A

#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <ostream>
#include <sstream>
#include <vector>

#include "parallel_graph_io.h"
#include "partition_config.h"
namespace parhip {
const ULONG fileTypeVersionNumberPartition = 1;
const ULONG header_count_partition         = 2;

class parallel_vector_io {
public:
        parallel_vector_io();
        virtual ~parallel_vector_io();

        template<typename vectortype> 
        void readVectorSequentially(std::vector<vectortype> & vec, std::string filename);

        template<typename vectortype> 
        void writeVectorSequentially(std::vector<vectortype> & vec, std::string filename);

        void writePartitionSimpleParallel(parallel_graph_access & G, std::string filename);

        void writePartitionBinaryParallel(PPartitionConfig & config, parallel_graph_access & G, std::string filename);
        void writePartitionBinaryParallelPosix(PPartitionConfig & config, parallel_graph_access & G, std::string filename);

        void readPartitionSimpleParallel(parallel_graph_access & G, std::string filename);

        void readPartitionBinaryParallel(PPartitionConfig & config, parallel_graph_access & G, std::string filename);

        void readPartition(PPartitionConfig & config, parallel_graph_access & G, std::string filename);
};

template<typename vectortype> 
void parallel_vector_io::writeVectorSequentially(std::vector<vectortype> & vec, std::string filename) {
        std::ofstream f(filename.c_str());
        for( ULONG i = 0; i < vec.size(); ++i) {
                f << vec[i] <<  std::endl;
        }

        f.close();
}

template<typename vectortype> 
void parallel_vector_io::readVectorSequentially(std::vector<vectortype> & vec, std::string filename) {

        std::string line;

        // open file for reading
        std::ifstream in(filename.c_str());
        if (!in) {
                std::cerr << "Error opening vectorfile" << filename << std::endl;
                return;
        }

        ULONG pos = 0;
        while (pos < vec.size() && std::getline(in, line)) {
          if (line.empty() || line.front() == '%') {
            continue;
          }

          auto parser = std::istringstream{line};
          auto value = vectortype{};
          if (parser >> value) {
            vec[static_cast<std::size_t>(pos++)] = value;
          }
        }

        in.close();
}
}
#endif /* end of include guard: PARALLEL_VECTOR_IO_BZVNZ570 */
