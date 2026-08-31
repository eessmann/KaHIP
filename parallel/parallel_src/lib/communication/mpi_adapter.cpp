#include "communication/mpi_adapter.h"

#include <utility>

namespace parhip::mpi {
namespace {
[[nodiscard]] auto mpi_is_active() noexcept -> bool {
  int initialized = 0;
  int finalized = 0;
  if (MPI_Initialized(&initialized) != MPI_SUCCESS || initialized == 0) {
    return false;
  }
  if (MPI_Finalized(&finalized) != MPI_SUCCESS) {
    return false;
  }
  return finalized == 0;
}
}  // namespace

communicator::communicator(communicator_view source) {
  check(MPI_Comm_dup(source.native_handle(), &communicator_), "MPI_Comm_dup");
  auto const handler_result =
      MPI_Comm_set_errhandler(communicator_, MPI_ERRORS_RETURN);
  if (handler_result != MPI_SUCCESS) {
    MPI_Comm_free(&communicator_);
    check(handler_result, "MPI_Comm_set_errhandler");
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
  if (communicator_ != MPI_COMM_NULL && mpi_is_active()) {
    MPI_Comm_free(&communicator_);
  }
  communicator_ = MPI_COMM_NULL;
}

topology::topology(communicator_view source) : communicator_(source) {
  int topology_kind = MPI_UNDEFINED;
  check(MPI_Topo_test(communicator_.native_handle(), &topology_kind),
        "MPI_Topo_test");
  if (topology_kind == MPI_UNDEFINED) {
    throw mpi_error{MPI_ERR_TOPOLOGY,
                    "topology requires an MPI topology communicator"};
  }
}

datatype::~datatype() noexcept { reset(); }

datatype::datatype(datatype&& other) noexcept
    : handle_(std::exchange(other.handle_, MPI_DATATYPE_NULL)),
      owns_(std::exchange(other.owns_, false)) {}

auto datatype::operator=(datatype&& other) noexcept -> datatype& {
  if (this != &other) {
    reset();
    handle_ = std::exchange(other.handle_, MPI_DATATYPE_NULL);
    owns_ = std::exchange(other.owns_, false);
  }
  return *this;
}

void datatype::reset() noexcept {
  if (owns_ && handle_ != MPI_DATATYPE_NULL && mpi_is_active()) {
    MPI_Type_free(&handle_);
  }
  handle_ = MPI_DATATYPE_NULL;
  owns_ = false;
}
}  // namespace parhip::mpi
