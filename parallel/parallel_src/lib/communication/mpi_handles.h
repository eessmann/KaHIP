#pragma once

#include <mpi.h>

#include "communication/mpi_error.h"

namespace parhip::mpi {
class communicator_view {
public:
  explicit communicator_view(MPI_Comm communicator) noexcept
      : communicator_(communicator) {}

  [[nodiscard]] auto native_handle() const noexcept -> MPI_Comm {
    return communicator_;
  }

  [[nodiscard]] auto rank() const -> int {
    int result = 0;
    check(MPI_Comm_rank(communicator_, &result), "MPI_Comm_rank");
    return result;
  }

  [[nodiscard]] auto size() const -> int {
    int result = 0;
    check(MPI_Comm_size(communicator_, &result), "MPI_Comm_size");
    return result;
  }

private:
  MPI_Comm communicator_;
};

class communicator {
public:
  explicit communicator(communicator_view source);
  ~communicator() noexcept;

  communicator(communicator const&) = delete;
  auto operator=(communicator const&) -> communicator& = delete;
  communicator(communicator&& other) noexcept;
  auto operator=(communicator&& other) noexcept -> communicator&;

  [[nodiscard]] auto native_handle() const noexcept -> MPI_Comm {
    return communicator_;
  }
  [[nodiscard]] auto view() const noexcept -> communicator_view {
    return communicator_view{communicator_};
  }

private:
  void reset() noexcept;

  MPI_Comm communicator_ = MPI_COMM_NULL;
};

class topology {
public:
  explicit topology(communicator_view source);

  topology(topology const&) = delete;
  auto operator=(topology const&) -> topology& = delete;
  topology(topology&&) noexcept = default;
  auto operator=(topology&&) noexcept -> topology& = default;

  [[nodiscard]] auto native_handle() const noexcept -> MPI_Comm {
    return communicator_.native_handle();
  }
  [[nodiscard]] auto view() const noexcept -> communicator_view {
    return communicator_.view();
  }

private:
  communicator communicator_;
};
}  // namespace parhip::mpi
