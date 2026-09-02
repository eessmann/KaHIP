#pragma once

#include <concepts>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "communication/implicit_lifetime.h"

namespace parhip::mpi {
namespace detail {
template <typename T>
class lifetime_storage {
public:
  explicit lifetime_storage(std::size_t size)
    requires is_implicit_lifetime_v<T>
      : size_(size) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (size_ == 0) {
      return;
    }
    if (size_ > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::length_error{"segmented buffer storage is too large"};
    }
    allocation_ =
        ::operator new(size_ * sizeof(T), std::align_val_t{alignof(T)});
    data_ = static_cast<T*>(allocation_);
  }

  ~lifetime_storage() noexcept { reset(); }

  lifetime_storage(lifetime_storage const&) = delete;
  auto operator=(lifetime_storage const&) -> lifetime_storage& = delete;

  lifetime_storage(lifetime_storage&& other) noexcept
      : allocation_(std::exchange(other.allocation_, nullptr)),
        data_(std::exchange(other.data_, nullptr)),
        size_(std::exchange(other.size_, 0)) {}

  auto operator=(lifetime_storage&& other) noexcept -> lifetime_storage& {
    if (this != &other) {
      reset();
      allocation_ = std::exchange(other.allocation_, nullptr);
      data_ = std::exchange(other.data_, nullptr);
      size_ = std::exchange(other.size_, 0);
    }
    return *this;
  }

  [[nodiscard]] auto data() noexcept -> T* { return data_; }
  [[nodiscard]] auto data() const noexcept -> T const* { return data_; }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }

private:
  void reset() noexcept {
    if (allocation_ != nullptr) {
      ::operator delete(allocation_, std::align_val_t{alignof(T)});
    }
    allocation_ = nullptr;
    data_ = nullptr;
    size_ = 0;
  }

  void* allocation_ = nullptr;
  T* data_ = nullptr;
  std::size_t size_ = 0;
};
}  // namespace detail

template <typename T>
class segmented_buffer {
public:
  segmented_buffer() = default;

  segmented_buffer(segmented_buffer const&) = delete;
  auto operator=(segmented_buffer const&) -> segmented_buffer& = delete;
  segmented_buffer(segmented_buffer&&) noexcept = default;
  auto operator=(segmented_buffer&&) noexcept -> segmented_buffer& = default;

  segmented_buffer(std::vector<T> storage,
                   std::vector<std::size_t> counts,
                   std::vector<std::size_t> offsets)
      : storage_(std::in_place_type<std::vector<T>>, std::move(storage)),
        counts_(std::move(counts)),
        offsets_(std::move(offsets)) {}

  [[nodiscard]] static auto uninitialized(
      std::size_t storage_size,
      std::vector<std::size_t> counts,
      std::vector<std::size_t> offsets) -> segmented_buffer
    requires std::is_trivially_copyable_v<T> &&
             detail::is_implicit_lifetime_v<T>
  {
    return segmented_buffer{uninitialized_tag{},
                            storage_size,
                            std::move(counts),
                            std::move(offsets)};
  }

  template <std::ranges::forward_range Segments>
    requires std::ranges::forward_range<std::ranges::range_value_t<Segments>> &&
             std::convertible_to<
                 std::ranges::range_value_t<std::ranges::range_value_t<Segments>>,
                 T>
  [[nodiscard]] static auto from_segments(Segments const& segments)
      -> segmented_buffer {
    std::vector<T> storage;
    std::vector<std::size_t> counts;
    std::vector<std::size_t> offsets;
    counts.reserve(static_cast<std::size_t>(std::ranges::distance(segments)));
    offsets.reserve(counts.capacity());

    for (auto const& segment : segments) {
      offsets.push_back(storage.size());
      counts.push_back(
          static_cast<std::size_t>(std::ranges::distance(segment)));
      storage.insert(storage.end(), std::ranges::begin(segment),
                     std::ranges::end(segment));
    }
    return segmented_buffer{
        std::move(storage), std::move(counts), std::move(offsets)};
  }

  [[nodiscard]] auto storage() noexcept -> std::span<T> {
    return std::visit(
        [](auto& owner) -> std::span<T> {
          return {owner.data(), owner.size()};
        },
        storage_);
  }
  [[nodiscard]] auto storage() const noexcept -> std::span<T const> {
    return std::visit(
        [](auto const& owner) -> std::span<T const> {
          return {owner.data(), owner.size()};
        },
        storage_);
  }
  [[nodiscard]] auto counts() const noexcept
      -> std::vector<std::size_t> const& {
    return counts_;
  }
  [[nodiscard]] auto offsets() const noexcept
      -> std::vector<std::size_t> const& {
    return offsets_;
  }
  [[nodiscard]] auto segment_count() const noexcept -> std::size_t {
    return counts_.size();
  }

  [[nodiscard]] auto segment(std::size_t index) -> std::span<T> {
    validate_segment(index);
    return storage().subspan(offsets_[index], counts_[index]);
  }
  [[nodiscard]] auto segment(std::size_t index) const -> std::span<T const> {
    validate_segment(index);
    return storage().subspan(offsets_[index], counts_[index]);
  }

  [[nodiscard]] auto has_canonical_layout(std::size_t expected_segments) const
      noexcept -> bool {
    if (counts_.size() != expected_segments ||
        offsets_.size() != expected_segments) {
      return false;
    }

    std::size_t expected_offset = 0;
    for (std::size_t index = 0; index < expected_segments; ++index) {
      if (offsets_[index] != expected_offset ||
          counts_[index] >
              std::numeric_limits<std::size_t>::max() - expected_offset) {
        return false;
      }
      expected_offset += counts_[index];
    }
    return expected_offset == storage().size();
  }

private:
  struct uninitialized_tag {};

  segmented_buffer(uninitialized_tag,
                   std::size_t storage_size,
                   std::vector<std::size_t> counts,
                   std::vector<std::size_t> offsets)
      : storage_(std::in_place_type<detail::lifetime_storage<T>>,
                 storage_size),
        counts_(std::move(counts)),
        offsets_(std::move(offsets)) {}

  void validate_segment(std::size_t index) const {
    if (index >= counts_.size() || index >= offsets_.size() ||
        offsets_[index] > storage().size() ||
        counts_[index] > storage().size() - offsets_[index]) {
      throw std::out_of_range{"invalid segmented buffer segment"};
    }
  }

  std::variant<std::vector<T>, detail::lifetime_storage<T>> storage_;
  std::vector<std::size_t> counts_;
  std::vector<std::size_t> offsets_;
};
}  // namespace parhip::mpi
