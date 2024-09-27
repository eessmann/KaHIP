//
// Created by Erich Essmann on 16/08/2024.
//
#include <communication/mpi_tools.h>
#include <definitions.h>
#include <mpi.h>
#include <catch2/catch_all.hpp>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "parallel_contraction_projection/parallel_contraction.h"

template <>
class fmt::formatter<MyType> {
public:
	constexpr auto parse (format_parse_context& ctx) { return ctx.begin(); }
	template <typename Context>
	constexpr auto format (MyType const& foo, Context& ctx) const {
		return format_to(ctx.out(), "({},{})", foo.a, foo.b);
	}
};

TEST_CASE("all to all vector of vectors", "[unit][mpi]") {
	SECTION("empty cases") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		const std::vector<std::vector<MyType>> v_empty{{{1,2}}, {{1,2}}, {{1,2}}, {{1,2}}, {{1,2}}};
		auto vec = mpi::all_to_all(v_empty, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		fmt::println("rank: {} -> {}", rank, vec);
		REQUIRE(v_empty == vec);
	}
	SECTION("complex case") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		const std::vector<std::vector<unsigned short>> v_empty{
				{}, {1}, {2, 2}, {3, 3, 3}, {4, 4, 4, 4}};
		auto vec = mpi::all_to_all(v_empty, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		fmt::println("rank: {} -> {}", rank, vec);
		REQUIRE(v_empty.size() == vec.size());
	}

	SECTION("custom types") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		const std::vector<std::vector<contraction::bundled_edge>> empty_edges{
						{{}}, {{}}, {{}}, {{}}, {{}}};
		const std::vector<std::vector<contraction::bundled_node_weight>> empty_weights{
					{{}}, {{}}, {{}}, {{}}, {{}}};
		auto vec_1 = mpi::all_to_all(empty_edges, MPI_COMM_WORLD);
		auto vec_2 = mpi::all_to_all(empty_weights, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		REQUIRE(empty_edges.size() == vec_1.size());
		REQUIRE(empty_weights.size() == vec_2.size());
	}
}