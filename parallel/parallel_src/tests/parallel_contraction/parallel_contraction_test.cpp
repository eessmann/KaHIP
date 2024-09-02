//
// Created by Erich Essmann on 16/08/2024.
//
#include <catch2/catch_all.hpp>
#include <vector>
#include <span>
#include <ranges>
#include <type_traits>

#include "parallel_contraction_projection/parallel_contraction.h"

#include <communication/mpi_tools.h>

TEST_CASE("flattening vector of messages", "[unit][mpi]") {
    SECTION("Empty Vector") {
        std::vector<std::vector<NodeID> > m_empty{};
        auto [flattened, offsets, lengths] = mpi_collective_tools::pack_messages(m_empty);
        REQUIRE(flattened.empty());
        REQUIRE(offsets.empty());
        REQUIRE(lengths.empty());

        std::vector<std::vector<NodeID> > m_empty2{{}};
        auto [flattened2, offsets2, lengths2] = mpi_collective_tools::pack_messages(m_empty2);
        REQUIRE(flattened2.empty());
        REQUIRE(offsets2.size() == 1);
        REQUIRE(lengths2.size() == 1);
    }
    SECTION("Simple Vector") {
        std::vector<std::vector<NodeID> > m_simple{{1, 2, 3, 4}};
        auto [flattened, offsets, lengths] = mpi_collective_tools::pack_messages(m_simple);

        // Testing sizes
        REQUIRE(flattened.size() == 4);
        REQUIRE(offsets.size() == 1);
        REQUIRE(lengths.size() == 1);

        // Testing content
        REQUIRE(flattened == m_simple.at(0));
    }

    SECTION("Complex Vector") {
        std::vector<std::vector<NodeID> > data = {
            {1, 2, 3},
            {},
            {4, 5},
            {6, 7, 8, 9}
        };

        auto [flattened, offsets, lengths] = mpi_collective_tools::pack_messages(data);

        // Testing sizes
        REQUIRE(flattened.size() == 9);
        REQUIRE(offsets.size() == 4);
        REQUIRE(lengths.size() == 4);

        // Creating Subspans
        auto s0 = std::span(flattened);
        auto s1 = s0.subspan(offsets[0], lengths[0]) | std::ranges::to<std::vector<NodeID> >();
        auto s2 = s0.subspan(offsets[1], lengths[1]) | std::ranges::to<std::vector<NodeID> >();
        auto s3 = s0.subspan(offsets[2], lengths[2]) | std::ranges::to<std::vector<NodeID> >();
        auto s4 = s0.subspan(offsets[3], lengths[3]) | std::ranges::to<std::vector<NodeID> >();

        REQUIRE(s1 == data[0]);
        REQUIRE(s2 == data[1]);
        REQUIRE(s3 == data[2]);
        REQUIRE(s4 == data[3]);
    }

    SECTION("Sliced Vector") {
        std::vector<NodeID> orig = {1, 2, 3, 1, 2, 3, 3, 3, 1, 2, 3};
        auto fun = std::ranges::less{};
        auto sliced = orig | std::views::chunk_by(fun) | std::ranges::to<std::vector<std::vector<NodeID> > >();

        auto [flattened, offsets, lengths] = mpi_collective_tools::pack_messages(sliced);
        REQUIRE(flattened.size() == orig.size());
        REQUIRE(orig == flattened);
    }
}

TEST_CASE("Packing and Unpacking for messages", "[unit][mpi]") {
    SECTION("Empty Vector") {
        constexpr std::vector<std::vector<NodeID> > m_empty{};
        auto const packed = mpi_collective_tools::pack_messages(m_empty);
        auto const unpacked = mpi_collective_tools::unpack_messages(packed);

        REQUIRE(m_empty == unpacked);
    }

    SECTION("Message of an empty Vector") {
        std::vector<std::vector<NodeID> > const m_empty{{}};
        auto const packed = mpi_collective_tools::pack_messages(m_empty);
        auto const unpacked = mpi_collective_tools::unpack_messages(packed);

        REQUIRE(m_empty == unpacked);
    }

    SECTION("Complex Message") {
        std::vector<std::vector<NodeID> > data = {
            {1, 2, 3},
            {},
            {4, 5},
            {},{},
            {6, 7, 8, 9}
        };
        auto const packed = mpi_collective_tools::pack_messages(data);
        auto const unpacked = mpi_collective_tools::unpack_messages(packed);
        REQUIRE(data == unpacked);

    }
}
