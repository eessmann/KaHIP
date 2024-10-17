//
// Created by Erich Essmann on 16/08/2024.
//
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <catch2/catch_all.hpp>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

#include "parallel_contraction_projection/parallel_contraction.h"
#include "tools/timer.h"

#include "communication/mpi_tools.h"
using namespace parhip;

TEST_CASE("flattening vector of messages", "[unit][mpi]") {
	SECTION("Empty Vector") {
		std::vector<std::vector<NodeID> > m_empty{};
		auto [flattened, offsets, lengths] = mpi::pack_messages(m_empty);
		REQUIRE(flattened.empty());
		REQUIRE(offsets.empty());
		REQUIRE(lengths.empty());

		std::vector<std::vector<NodeID> > m_empty2{{}};
		auto [flattened2, offsets2, lengths2] = mpi::pack_messages(m_empty2);
		REQUIRE(flattened2.empty());
		REQUIRE(offsets2.size() == 1);
		REQUIRE(lengths2.size() == 1);
	}
	SECTION("Simple Vector") {
		std::vector<std::vector<NodeID> > m_simple{{1, 2, 3, 4}};
		auto [flattened, offsets, lengths] = mpi::pack_messages(m_simple);

		// Testing sizes
		REQUIRE(flattened.size() == 4);
		REQUIRE(offsets.size() == 1);
		REQUIRE(lengths.size() == 1);

		// Testing content
		REQUIRE(flattened == m_simple.at(0));
	}

	SECTION("Complex Vector") {
		std::vector<std::vector<NodeID> > data = {
				{1, 2, 3}, {}, {4, 5}, {6, 7, 8, 9}, {}};

		auto [flattened, offsets, lengths] = mpi::pack_messages(data);

		// Testing sizes
		REQUIRE(flattened.size() == 9);
		REQUIRE(offsets.size() == 5);
		REQUIRE(lengths.size() == 5);

		// Creating Subspans
		std::vector<NodeID> s1, s2, s3, s4, s5;
		s1.insert(s1.begin(), flattened.begin() + offsets[0],
							flattened.begin() + offsets[0] + lengths[0]);
		s2.insert(s2.begin(), flattened.begin() + offsets[1],
							flattened.begin() + offsets[1] + lengths[1]);
		s3.insert(s3.begin(), flattened.begin() + offsets[2],
							flattened.begin() + offsets[2] + lengths[2]);
		s4.insert(s4.begin(), flattened.begin() + offsets[3],
							flattened.begin() + offsets[3] + lengths[3]);
		s5.insert(s5.begin(), flattened.begin() + offsets[4],
							flattened.begin() + offsets[4] + lengths[4]);

		REQUIRE(s1 == data[0]);
		REQUIRE(s2 == data[1]);
		REQUIRE(s3 == data[2]);
		REQUIRE(s4 == data[3]);
		REQUIRE(s5 == data[4]);
	}
}

TEST_CASE("Packing and Unpacking for messages", "[unit][mpi]") {
	SECTION("Empty Vector") {
		const std::vector<std::vector<NodeID> > m_empty{};
		auto const packed = mpi::pack_messages(m_empty);
		auto const unpacked = mpi::unpack_messages(packed);

		REQUIRE(m_empty == unpacked);
	}

	SECTION("Message of an empty Vector") {
		std::vector<std::vector<NodeID> > const m_empty{{}};
		auto const packed = mpi::pack_messages(m_empty);
		auto const unpacked = mpi::unpack_messages(packed);

		REQUIRE(m_empty == unpacked);
	}

	SECTION("Complex Message") {
		std::vector<std::vector<NodeID> > data = {{1, 2, 3}, {}, {4, 5},
																							{},        {}, {6, 7, 8, 9}};
		auto const packed = mpi::pack_messages(data);
		auto const unpacked = mpi::unpack_messages(packed);
		REQUIRE(data == unpacked);
	}
}

using mpi_native_types = std::tuple<char,
																		wchar_t,
																		float,
																		double,
																		long double,
																		bool,
																		int8_t,
																		int16_t,
																		int16_t,
																		int32_t,
																		int64_t,
																		uint8_t,
																		uint16_t,
																		uint32_t,
																		uint64_t,
																		short,
																		int,
																		long,
																		long long,
																		unsigned short,
																		unsigned int,
																		unsigned long,
																		unsigned long long,
																		signed char,
																		unsigned char>;
static auto const mpi_datatypes = std::array{
		MPI_CHAR,  MPI_WCHAR,          MPI_SIGNED_CHAR,   MPI_UNSIGNED_CHAR,
		MPI_SHORT, MPI_UNSIGNED_SHORT, MPI_INT,           MPI_UNSIGNED,
		MPI_LONG,  MPI_UNSIGNED_LONG,  MPI_LONG_LONG_INT, MPI_UNSIGNED_LONG_LONG,
		MPI_FLOAT, MPI_DOUBLE,         MPI_LONG_DOUBLE,   MPI_CXX_BOOL};

struct MyTestType {
	int a;
	float b;
	char c;
	double d;
	long double e;
	long long f;
};
TEMPLATE_LIST_TEST_CASE("MPI Native Datatype mapping",
												"[unit][mpi]",
												mpi_native_types) {
	SECTION("Native MPI data kinds") {
		STATIC_REQUIRE(mpi::mpi_native_datatype<TestType>);
	}
	SECTION("Native MPI data type") {
		auto matches =
				std::ranges::views::transform(mpi_datatypes, [](auto datatype) -> bool {
					return (datatype == mpi::get_mpi_datatype<TestType>());
				});
		auto any_match = std::accumulate(std::begin(matches), std::end(matches),
																		 false, std::logical_or<bool>());
		REQUIRE(any_match == true);
	}
}
