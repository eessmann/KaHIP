//
// Created by Erich Essmann on 16/08/2024.
//
#include <definitions.h>
#include <catch2/catch_all.hpp>
#include <vector>
#include <mpi.h>
#include <communication/mpi_tools.h>
#include <fmt/format.h>
#include <fmt/ranges.h>


TEST_CASE("all to all vector of vectors", "[unit][mpi]") {
    SECTION("emtpy case") {
        PEID rank, size;
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        MPI_Comm_size(MPI_COMM_WORLD, &size);

        const std::vector<std::vector<NodeID>> v_empty{{1,1,1,1},{2,2,2,2},{3,3,3,3},{4,4,4,4}};
        auto vec = mpi_collective_tools::all_to_all(v_empty, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);
        fmt::println("rank: {} -> {}", rank, vec);
        REQUIRE(v_empty == vec);
    }
}