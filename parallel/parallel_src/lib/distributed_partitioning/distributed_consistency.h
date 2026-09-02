#pragma once

#include <tuple>
#include <type_traits>

#include "communication/mpi_types.h"
#include "definitions.h"

namespace parhip::distributed_consistency {
struct node_value {
  NodeID global_id;
  NodeID value;

  auto operator==(node_value const&) const -> bool = default;
};

static_assert(std::is_standard_layout_v<node_value>);
static_assert(std::is_trivially_copyable_v<node_value>);
}  // namespace parhip::distributed_consistency

template <>
struct parhip::mpi::wire_members<parhip::distributed_consistency::node_value> {
  inline static constexpr auto value = std::tuple{
      &parhip::distributed_consistency::node_value::global_id,
      &parhip::distributed_consistency::node_value::value};
};
