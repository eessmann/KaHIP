//
// Created by Erich Essmann on 16/08/2024.
//
#include <definitions.h>
#include <catch2/catch_all.hpp>
#include <vector>
#include <mpi.h>
#include <communication/mpi_tools.h>


TEST_CASE("all to all vector of vectos", "[unit][mpi]") {
    SECTION("emtpy case") {
        const std::vector<std::vector<NodeID>> v_empty{{1}};
        auto vec = mpi_collective_tools::all_to_all(v_empty, MPI_COMM_WORLD);
        REQUIRE(vec.empty());
    }
}