#include "communication/mpi_adapter.h"

#include <utility>

namespace parhip::mpi {
communicator::communicator(communicator_view source) {
  check_or_abort(MPI_Comm_dup(source.native_handle(), &communicator_),
                 source.native_handle(),
                 "MPI_Comm_dup");
  auto const handler_result =
      MPI_Comm_set_errhandler(communicator_, MPI_ERRORS_RETURN);
  if (handler_result != MPI_SUCCESS) {
    abort_on_mpi_error(
        communicator_, handler_result, "MPI_Comm_set_errhandler");
  }
}

communicator::~communicator() noexcept { reset(); }

communicator::communicator(communicator&& other) noexcept
    : communicator_(std::exchange(other.communicator_, MPI_COMM_NULL)) {}

auto communicator::operator=(communicator&& other) noexcept -> communicator& {
  if (this != &other) {
    reset();
    communicator_ = std::exchange(other.communicator_, MPI_COMM_NULL);
  }
  return *this;
}

void communicator::reset() noexcept {
  if (communicator_ != MPI_COMM_NULL && runtime_is_active()) {
    auto const free_result = MPI_Comm_free(&communicator_);
    if (free_result != MPI_SUCCESS) {
      // MPI_Comm_free may already have invalidated the saved handle. The
      // process group is still known through MPI_COMM_WORLD, so fail fast
      // there instead of issuing MPI_Abort on a possibly stale communicator.
      abort_on_mpi_error(MPI_COMM_WORLD, free_result, "MPI_Comm_free");
    }
  }
  communicator_ = MPI_COMM_NULL;
}

topology::topology(communicator_view source) : communicator_(source) {
  int topology_kind = MPI_UNDEFINED;
  check_or_abort(MPI_Topo_test(communicator_.native_handle(), &topology_kind),
                 communicator_.native_handle(),
                 "MPI_Topo_test");
  if (topology_kind == MPI_UNDEFINED) {
    throw mpi_error{MPI_ERR_TOPOLOGY,
                    "topology requires an MPI topology communicator"};
  }
}

datatype::~datatype() noexcept { reset(); }

datatype::datatype(datatype&& other) noexcept
    : handle_(std::exchange(other.handle_, MPI_DATATYPE_NULL)),
      owns_(std::exchange(other.owns_, false)),
      failure_communicator_(
          std::exchange(other.failure_communicator_, MPI_COMM_WORLD)) {}

auto datatype::operator=(datatype&& other) noexcept -> datatype& {
  if (this != &other) {
    reset();
    handle_ = std::exchange(other.handle_, MPI_DATATYPE_NULL);
    owns_ = std::exchange(other.owns_, false);
    failure_communicator_ =
        std::exchange(other.failure_communicator_, MPI_COMM_WORLD);
  }
  return *this;
}

void datatype::reset() noexcept {
  if (owns_ && handle_ != MPI_DATATYPE_NULL && runtime_is_active()) {
    check_or_abort(MPI_Type_free(&handle_),
                   failure_communicator_,
                   "MPI_Type_free");
  }
  handle_ = MPI_DATATYPE_NULL;
  owns_ = false;
  failure_communicator_ = MPI_COMM_WORLD;
}
}  // namespace parhip::mpi
