#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>

namespace parhip::mpi {
template <std::unsigned_integral Id>
class contiguous_owner_layout {
public:
  constexpr contiguous_owner_layout(Id total, std::size_t partitions)
      : total_(total), partitions_(partitions) {
    if (partitions_ == 0 ||
        partitions_ > static_cast<std::size_t>(
                          std::numeric_limits<Id>::max())) {
      throw std::invalid_argument{
          "contiguous ownership requires a representable partition count"};
    }

    auto const divisor = static_cast<Id>(partitions_);
    chunk_size_ = total_ == 0
                      ? Id{1}
                      : total_ / divisor + Id{total_ % divisor != 0};
  }

  [[nodiscard]] constexpr auto total() const noexcept -> Id {
    return total_;
  }

  [[nodiscard]] constexpr auto partition_count() const noexcept
      -> std::size_t {
    return partitions_;
  }

  [[nodiscard]] constexpr auto chunk_size() const noexcept -> Id {
    return chunk_size_;
  }

  [[nodiscard]] constexpr auto owner(Id id) const noexcept
      -> std::optional<std::size_t> {
    if (id >= total_) {
      return std::nullopt;
    }
    auto const result = static_cast<std::size_t>(id / chunk_size_);
    return result < partitions_ ? std::optional{result} : std::nullopt;
  }

  [[nodiscard]] constexpr auto boundary(std::size_t partition) const noexcept
      -> Id {
    if (partition == 0 || total_ == 0) {
      return Id{0};
    }
    if (partition >= partitions_) {
      return total_;
    }

    auto const position = static_cast<Id>(partition);
    if (chunk_size_ > total_ / position) {
      return total_;
    }
    return std::min(total_, static_cast<Id>(chunk_size_ * position));
  }

  [[nodiscard]] constexpr auto begin(std::size_t partition) const noexcept
      -> Id {
    return boundary(partition);
  }

  [[nodiscard]] constexpr auto end(std::size_t partition) const noexcept
      -> Id {
    return partition >= partitions_ ? total_ : boundary(partition + 1);
  }

private:
  Id total_ = 0;
  std::size_t partitions_ = 0;
  Id chunk_size_ = 1;
};
}  // namespace parhip::mpi
