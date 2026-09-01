#pragma once

#include <boost/hana/tuple.hpp>

#include <type_traits>

#include "communication/mpi_types.h"
#include "definitions.h"

namespace parhip {
struct ghost_label_update final {
  NodeID global_id;
  NodeID label;

  auto operator==(ghost_label_update const&) const -> bool = default;
};

static_assert(std::is_standard_layout_v<ghost_label_update>);
static_assert(std::is_trivially_copyable_v<ghost_label_update>);
}  // namespace parhip

template <>
struct parhip::mpi::wire_members<parhip::ghost_label_update> {
  inline static constexpr auto value =
      boost::hana::make_tuple(&parhip::ghost_label_update::global_id,
                              &parhip::ghost_label_update::label);
};
