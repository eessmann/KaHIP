//
// Created by Erich Essmann on 16/08/2024.
//
#include <communication/mpi_tools.h>
#include <definitions.h>
#include <mpi.h>
#include <catch2/catch_all.hpp>
#include <vector>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>

#include "parallel_contraction_projection/parallel_contraction.h"

template <>
class fmt::formatter<MyType> {
public:
	constexpr auto parse (format_parse_context& ctx) { return ctx.begin(); }
	template <typename Context>
	constexpr auto format (MyType const& foo, Context& ctx) const {
		return format_to(ctx.out(), "({},{},{}+i{})", foo.a, foo.b, foo.c.real(), foo.c.imag());
	}
};

struct MyTestType{
	int  a;
	float b;
	char c;
	double d;
	long double e;
	long long f;

};

struct SuperStruct {
	int a;
	MyTestType b;
};

struct MetaStruct {
	SuperStruct s;
};

TEST_CASE("all to all vector of vectors", "[unit][mpi]") {
	SECTION("empty cases") {
		PEID rank, size;
		MPI_Comm_rank(MPI_COMM_WORLD, &rank);
		MPI_Comm_size(MPI_COMM_WORLD, &size);

		const std::vector<std::vector<MyType>> v_empty{{{1,2,{3, -3}}}, {{1,2,{3, -3}}}, {{1,2,{3, -3}}}, {{1,2,{3, -3}}}, {{1,2,{3, -3}}}};
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
		const std::vector<std::vector<contraction::bundled_node_weight>> empty_meta{
						{{}}, {{}}, {{}}, {{}}, {{}}};
		auto vec_1 = mpi::all_to_all(empty_edges, MPI_COMM_WORLD);
		auto vec_2 = mpi::all_to_all(empty_weights, MPI_COMM_WORLD);
		auto vec_3 = mpi::all_to_all(empty_meta, MPI_COMM_WORLD);
		MPI_Barrier(MPI_COMM_WORLD);
		REQUIRE(empty_edges.size() == vec_1.size());
		REQUIRE(empty_weights.size() == vec_2.size());
		REQUIRE(empty_meta.size() == vec_3.size());
	}
}



TEST_CASE("MPI Custom Datatype mapping", "[unit][mpi]") {
	SECTION("Aggrigate MPI data kinds") {
		STATIC_REQUIRE(mpi::mpi_composite_datatype<SuperStruct>);
		STATIC_REQUIRE(mpi::mpi_composite_datatype<MyTestType>);
	}
	SECTION("Aggrigate MPI data type") {
		REQUIRE(mpi::get_mpi_datatype<MetaStruct>() != MPI_DATATYPE_NULL);
	}
}