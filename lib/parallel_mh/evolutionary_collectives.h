#pragma once

#include <mpi.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "tools/fatal_diagnostics.h"

namespace kahip::parallel_mh {
struct evolutionary_broadcast_options final {
  std::size_t mpi3_round_ceiling =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  bool force_mpi3 = false;
};

namespace detail {
[[nodiscard]] inline auto active_rank(MPI_Comm communicator) noexcept -> int {
  auto rank = -1;
  if (communicator != MPI_COMM_NULL &&
      PMPI_Comm_rank(communicator, &rank) == MPI_SUCCESS) {
    return rank;
  }
  return -1;
}

template <typename T>
using unqualified_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T>
concept evolutionary_mpi_scalar =
    std::same_as<unqualified_t<T>, int> ||
    std::same_as<unqualified_t<T>, unsigned> ||
    std::same_as<unqualified_t<T>, long> ||
    std::same_as<unqualified_t<T>, unsigned long> ||
    std::same_as<unqualified_t<T>, long long> ||
    std::same_as<unqualified_t<T>, unsigned long long>;

template <evolutionary_mpi_scalar T>
[[nodiscard]] auto native_datatype() noexcept -> MPI_Datatype {
  using value_type = unqualified_t<T>;
  if constexpr (std::same_as<value_type, int>) {
    return MPI_INT;
  } else if constexpr (std::same_as<value_type, unsigned>) {
    return MPI_UNSIGNED;
  } else if constexpr (std::same_as<value_type, long>) {
    return MPI_LONG;
  } else if constexpr (std::same_as<value_type, unsigned long>) {
    return MPI_UNSIGNED_LONG;
  } else if constexpr (std::same_as<value_type, long long>) {
    return MPI_LONG_LONG_INT;
  } else {
    return MPI_UNSIGNED_LONG_LONG;
  }
}

[[noreturn]] inline void abort_evolutionary_collective(
    MPI_Comm communicator,
    std::string_view operation,
    std::string_view diagnostic) noexcept {
  auto const rank = active_rank(communicator);
  if (rank >= 0) {
    kahip::diagnostics::critical(
        "MPI evolutionary collective failure in ", operation, " on rank ",
        rank, ": ", diagnostic);
  } else {
    kahip::diagnostics::critical(
        "MPI evolutionary collective failure in ", operation, ": ",
        diagnostic);
  }
  static_cast<void>(MPI_Abort(communicator, EXIT_FAILURE));
  std::abort();
}

[[noreturn]] inline void abort_evolutionary_lifecycle(
    std::string_view diagnostic) noexcept {
  kahip::diagnostics::critical("MPI evolutionary lifecycle failure: ",
                               diagnostic);
  std::abort();
}

[[noreturn]] inline void abort_evolutionary_mpi_error(
    MPI_Comm communicator,
    int error_code,
    std::string_view operation) noexcept {
  auto const rank = active_rank(communicator);
  if (rank >= 0) {
    kahip::diagnostics::critical(
        "MPI backend failure: ", operation, " returned raw error ", error_code,
        " on rank ", rank);
  } else {
    kahip::diagnostics::critical(
        "MPI backend failure: ", operation, " returned raw error ",
        error_code);
  }
  static_cast<void>(MPI_Abort(communicator, EXIT_FAILURE));
  std::abort();
}

inline void check_mpi(int result,
                      MPI_Comm communicator,
                      std::string_view operation) noexcept {
  if (result != MPI_SUCCESS) {
    abort_evolutionary_mpi_error(communicator, result, operation);
  }
}

[[nodiscard]] inline auto mpi_runtime_is_active() noexcept -> bool {
  auto initialized = 0;
  auto finalized = 0;
  auto const initialized_result = MPI_Initialized(&initialized);
  if (initialized_result != MPI_SUCCESS) {
    abort_evolutionary_lifecycle("MPI_Initialized failed");
  }
  if (initialized == 0) {
    return false;
  }
  auto const finalized_result = MPI_Finalized(&finalized);
  if (finalized_result != MPI_SUCCESS) {
    abort_evolutionary_lifecycle("MPI_Finalized failed");
  }
  return finalized == 0;
}

[[nodiscard]] inline auto checked_count(std::size_t count,
                                        MPI_Comm communicator,
                                        std::string_view operation) noexcept
    -> int {
  if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    abort_evolutionary_collective(
        communicator, operation,
        "partition-vector count exceeds the MPI int interface boundary");
  }
  return static_cast<int>(count);
}

template <evolutionary_mpi_scalar T>
void broadcast_partition_payload(
    MPI_Comm communicator,
    T* data,
    std::size_t count,
    int root,
    evolutionary_broadcast_options options) noexcept {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));

  auto communicator_size = 0;
  check_mpi(MPI_Comm_size(communicator, &communicator_size), communicator,
            "MPI_Comm_size(evolutionary partition broadcast)");

  auto const encoded_root = root >= 0
                                ? static_cast<std::uint64_t>(root)
                                : std::numeric_limits<std::uint64_t>::max();
  auto const local_signature = std::array<std::uint64_t, 4>{
      static_cast<std::uint64_t>(count),
      static_cast<std::uint64_t>(options.mpi3_round_ceiling),
      options.force_mpi3 ? std::uint64_t{1} : std::uint64_t{0}, encoded_root};
  auto minimum_signature = std::array<std::uint64_t, 4>{};
  auto maximum_signature = std::array<std::uint64_t, 4>{};
  check_mpi(MPI_Allreduce(local_signature.data(), minimum_signature.data(),
                          static_cast<int>(local_signature.size()),
                          MPI_UINT64_T, MPI_MIN, communicator),
            communicator,
            "MPI_Allreduce(evolutionary partition signature minimum)");
  check_mpi(MPI_Allreduce(local_signature.data(), maximum_signature.data(),
                          static_cast<int>(local_signature.size()),
                          MPI_UINT64_T, MPI_MAX, communicator),
            communicator,
            "MPI_Allreduce(evolutionary partition signature maximum)");

  auto const locally_valid = options.mpi3_round_ceiling != 0 && root >= 0 &&
                             root < communicator_size &&
                             (count == 0 || data != nullptr);
  auto local_valid = locally_valid ? 1 : 0;
  auto all_valid = 0;
  check_mpi(MPI_Allreduce(&local_valid, &all_valid, 1, MPI_INT, MPI_MIN,
                          communicator),
            communicator,
            "MPI_Allreduce(evolutionary partition signature validity)");

  if (minimum_signature != maximum_signature) {
    abort_evolutionary_collective(
        communicator, "MPI_Bcast(evolutionary best partition)",
        "partition broadcast arguments differ across communicator");
  }
  if (all_valid == 0) {
    abort_evolutionary_collective(communicator,
                                  "MPI_Bcast(evolutionary best partition)",
                                  "partition broadcast arguments are invalid");
  }
  if (count == 0) {
    return;
  }

#if KAHIP_HAVE_MPI_BCAST_C
  if (!options.force_mpi3 && std::in_range<MPI_Count>(count)) {
    check_mpi(MPI_Bcast_c(data, static_cast<MPI_Count>(count),
                          native_datatype<T>(), root, communicator),
              communicator, "MPI_Bcast_c(evolutionary best partition)");
    return;
  }
#endif

  auto const ceiling =
      std::min(options.mpi3_round_ceiling,
               static_cast<std::size_t>(std::numeric_limits<int>::max()));
  for (std::size_t offset = 0; offset < count;) {
    auto const chunk = std::min(ceiling, count - offset);
    check_mpi(MPI_Bcast(data + offset, static_cast<int>(chunk),
                        native_datatype<T>(), root, communicator),
              communicator,
              "MPI_Bcast(evolutionary best partition MPI-3 round)");
    offset += chunk;
  }
}
}  // namespace detail

class owned_evolutionary_communicator final {
 public:
  explicit owned_evolutionary_communicator(MPI_Comm source) noexcept {
    if (!detail::mpi_runtime_is_active()) {
      detail::abort_evolutionary_lifecycle(
          "communicator ownership requires an active MPI runtime");
    }
    if (source == MPI_COMM_NULL) {
      detail::abort_evolutionary_collective(
          MPI_COMM_WORLD, "MPI_Comm_dup(evolutionary communicator)",
          "communicator ownership requires a live intracommunicator");
    }

    auto is_intercommunicator = 0;
    detail::check_mpi(MPI_Comm_test_inter(source, &is_intercommunicator),
                      source, "MPI_Comm_test_inter(evolutionary communicator)");
    if (is_intercommunicator != 0) {
      detail::abort_evolutionary_collective(
          source, "MPI_Comm_dup(evolutionary communicator)",
          "communicator ownership requires an intracommunicator");
    }

    detail::check_mpi(MPI_Comm_dup(source, &communicator_), source,
                      "MPI_Comm_dup(evolutionary communicator)");
    detail::check_mpi(MPI_Comm_set_errhandler(communicator_, MPI_ERRORS_RETURN),
                      communicator_,
                      "MPI_Comm_set_errhandler(evolutionary communicator)");
  }

  ~owned_evolutionary_communicator() noexcept { reset(); }

  owned_evolutionary_communicator(owned_evolutionary_communicator const&) =
      delete;
  auto operator=(owned_evolutionary_communicator const&)
      -> owned_evolutionary_communicator& = delete;

  owned_evolutionary_communicator(
      owned_evolutionary_communicator&& other) noexcept
      : communicator_(std::exchange(other.communicator_, MPI_COMM_NULL)) {}

  auto operator=(owned_evolutionary_communicator&& other) noexcept
      -> owned_evolutionary_communicator& {
    if (this != &other) {
      reset();
      communicator_ = std::exchange(other.communicator_, MPI_COMM_NULL);
    }
    return *this;
  }

  [[nodiscard]] auto native_handle() const noexcept -> MPI_Comm {
    return communicator_;
  }

  [[nodiscard]] auto rank() const noexcept -> int {
    auto result = -1;
    detail::check_mpi(MPI_Comm_rank(communicator_, &result), communicator_,
                      "MPI_Comm_rank(evolutionary communicator)");
    return result;
  }

  [[nodiscard]] auto size() const noexcept -> int {
    auto result = 0;
    detail::check_mpi(MPI_Comm_size(communicator_, &result), communicator_,
                      "MPI_Comm_size(evolutionary communicator)");
    return result;
  }

 private:
  void reset() noexcept {
    if (communicator_ == MPI_COMM_NULL) {
      return;
    }
    if (!detail::mpi_runtime_is_active()) {
      detail::abort_evolutionary_lifecycle(
          "owned communicator outlived the active MPI runtime");
    }
    auto communicator = std::exchange(communicator_, MPI_COMM_NULL);
    auto const result = MPI_Comm_free(&communicator);
    if (result != MPI_SUCCESS) {
      detail::abort_evolutionary_mpi_error(
          MPI_COMM_WORLD, result, "MPI_Comm_free(evolutionary communicator)");
    }
  }

  MPI_Comm communicator_ = MPI_COMM_NULL;
};

template <std::totally_ordered Objective>
[[nodiscard]] constexpr auto objective_improved(Objective candidate,
                                                Objective incumbent) noexcept
    -> bool {
  return candidate < incumbent;
}

inline void broadcast_permutation(MPI_Comm communicator,
                                  std::span<unsigned> permutation,
                                  int root) noexcept {
  auto const count = detail::checked_count(
      permutation.size(), communicator, "MPI_Bcast(evolutionary permutation)");
  detail::check_mpi(
      MPI_Bcast(permutation.data(), count, MPI_UNSIGNED, root, communicator),
      communicator, "MPI_Bcast(evolutionary permutation)");
}

template <detail::evolutionary_mpi_scalar EdgeWeight,
          detail::evolutionary_mpi_scalar NodeWeight,
          detail::evolutionary_mpi_scalar PartitionID>
[[nodiscard]] auto select_and_broadcast_best_partition(
    MPI_Comm communicator,
    EdgeWeight local_objective,
    NodeWeight local_max_block_weight,
    NodeWeight upper_bound_partition,
    PartitionID* local_partition,
    std::size_t partition_size,
    evolutionary_broadcast_options broadcast_options = {}) noexcept
    -> EdgeWeight {
  auto rank = -1;
  detail::check_mpi(MPI_Comm_rank(communicator, &rank), communicator,
                    "MPI_Comm_rank(evolutionary best partition)");

  auto const local_infeasible =
      local_max_block_weight > upper_bound_partition ? 1 : 0;
  auto all_infeasible = 0;
  detail::check_mpi(MPI_Allreduce(&local_infeasible, &all_infeasible, 1,
                                  MPI_INT, MPI_MIN, communicator),
                    communicator, "MPI_Allreduce(evolutionary feasibility)");

  auto const eligible = all_infeasible != 0 || local_infeasible == 0;
  auto const candidate_objective =
      eligible ? local_objective : std::numeric_limits<EdgeWeight>::max();
  auto best_objective = std::numeric_limits<EdgeWeight>::max();
  detail::check_mpi(MPI_Allreduce(&candidate_objective, &best_objective, 1,
                                  detail::native_datatype<EdgeWeight>(),
                                  MPI_MIN, communicator),
                    communicator, "MPI_Allreduce(evolutionary objective)");

  auto const candidate_weight = eligible && local_objective == best_objective
                                    ? local_max_block_weight
                                    : std::numeric_limits<NodeWeight>::max();
  auto best_block_weight = std::numeric_limits<NodeWeight>::max();
  detail::check_mpi(MPI_Allreduce(&candidate_weight, &best_block_weight, 1,
                                  detail::native_datatype<NodeWeight>(),
                                  MPI_MIN, communicator),
                    communicator,
                    "MPI_Allreduce(evolutionary maximum block weight)");

  auto const candidate_root =
      eligible && local_objective == best_objective &&
              local_max_block_weight == best_block_weight
          ? rank
          : std::numeric_limits<int>::max();
  auto selected_root = std::numeric_limits<int>::max();
  detail::check_mpi(MPI_Allreduce(&candidate_root, &selected_root, 1, MPI_INT,
                                  MPI_MIN, communicator),
                    communicator,
                    "MPI_Allreduce(evolutionary broadcaster rank)");

  detail::broadcast_partition_payload(communicator, local_partition,
                                      partition_size, selected_root,
                                      broadcast_options);
  return best_objective;
}
}  // namespace kahip::parallel_mh
