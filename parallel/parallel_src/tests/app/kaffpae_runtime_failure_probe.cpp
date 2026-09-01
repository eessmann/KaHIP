#include <mpi.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/logger.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include "mpi_application_runtime.h"

namespace {
enum class failure_mode : unsigned char { rank_query, communicator_free };

inline auto selected_mode = failure_mode::rank_query;
inline bool injection_active = false;
inline MPI_Comm operation_communicator = MPI_COMM_NULL;
inline int rank_queries = 0;
inline int communicator_frees = 0;
inline int finalizations = 0;
inline volatile std::sig_atomic_t diagnostic_was_flushed = 0;

constexpr auto injected_rank_error = 17401;
constexpr auto injected_free_error = 17402;

void write_text(std::string_view text) noexcept;

class observing_sink final : public spdlog::sinks::sink {
 public:
  void log(spdlog::details::log_msg const& message) override {
    write_text(std::string_view{message.payload.data(), message.payload.size()});
    write_text("\n");
  }

  void flush() override {
    diagnostic_was_flushed = 1;
    write_text("observed synchronous spdlog flush\n");
  }

  void set_pattern(std::string const&) override {}
  void set_formatter(std::unique_ptr<spdlog::formatter>) override {}
};

[[noreturn]] void observed_process_abort(int) noexcept {
  if (diagnostic_was_flushed == 0) {
    write_text("process aborted before spdlog flush\n");
    std::_Exit(91);
  }
  write_text("observed process abort after spdlog flush\n");
  std::_Exit(86);
}

// KAHIP_PMPI_CALLBACK_REGION_BEGIN
void write_text(std::string_view text) noexcept {
  static_cast<void>(::write(STDERR_FILENO, text.data(), text.size()));
}

[[nodiscard]] auto operation_communicator_matches(MPI_Comm communicator) noexcept
    -> bool {
  auto relation = int{MPI_UNEQUAL};
  return communicator != MPI_COMM_WORLD &&
         operation_communicator != MPI_COMM_NULL &&
         PMPI_Comm_compare(communicator, operation_communicator, &relation) ==
             MPI_SUCCESS &&
         relation == MPI_IDENT;
}
}  // namespace

static_assert(noexcept(write_text({})));
static_assert(noexcept(operation_communicator_matches(MPI_COMM_NULL)));

extern "C" int MPI_Comm_rank(MPI_Comm communicator, int* rank) {
  if (injection_active && selected_mode == failure_mode::rank_query) {
    ++rank_queries;
    if (!operation_communicator_matches(communicator) || rank == nullptr) {
      write_text("forbidden MPI call: unexpected injected rank query\n");
      return MPI_ERR_OTHER;
    }
    return injected_rank_error;
  }
  return PMPI_Comm_rank(communicator, rank);
}

extern "C" int MPI_Comm_free(MPI_Comm* communicator) {
  if (injection_active && selected_mode == failure_mode::communicator_free) {
    ++communicator_frees;
    if (communicator == nullptr ||
        !operation_communicator_matches(*communicator)) {
      write_text("forbidden MPI call: unexpected communicator free\n");
      return MPI_ERR_OTHER;
    }
    return injected_free_error;
  }
  return PMPI_Comm_free(communicator);
}

extern "C" int MPI_Finalize() {
  if (injection_active) {
    ++finalizations;
    write_text("forbidden MPI call: finalize after injected failure\n");
    return MPI_ERR_OTHER;
  }
  return PMPI_Finalize();
}

extern "C" int MPI_Abort(MPI_Comm communicator, int error_code) {
  auto const expected_state =
      diagnostic_was_flushed != 0 && error_code == EXIT_FAILURE &&
      finalizations == 0 &&
      ((selected_mode == failure_mode::rank_query && rank_queries == 1 &&
        communicator_frees == 0 &&
        operation_communicator_matches(communicator)) ||
       (selected_mode == failure_mode::communicator_free &&
        rank_queries == 0 && communicator_frees == 1 &&
        communicator == MPI_COMM_WORLD));
  if (!expected_state) {
    write_text("forbidden MPI call: abort used unexpected state or scope\n");
    std::_Exit(92);
  }
  if (selected_mode == failure_mode::rank_query) {
    write_text(
        "observed operation communicator MPI_Abort after spdlog flush\n");
  } else {
    write_text("observed MPI_COMM_WORLD fallback abort after spdlog flush\n");
  }
  std::_Exit(86);
}
// KAHIP_PMPI_CALLBACK_REGION_END

int main(int argc, char** argv) {
  if (argc != 2) {
    return 64;
  }
  auto const mode = std::string_view{argv[1]};
  if (mode == "rank-query") {
    selected_mode = failure_mode::rank_query;
  } else if (mode == "communicator-free") {
    selected_mode = failure_mode::communicator_free;
  } else {
    return 64;
  }

  spdlog::set_default_logger(
      std::make_shared<spdlog::logger>("observing",
                                       std::make_shared<observing_sink>()));
  if (std::signal(SIGABRT, observed_process_abort) == SIG_ERR) {
    return 70;
  }

  kahip::mpi::application_runtime runtime{
      argc, argv, std::string{"root runtime failure probe"}};
  return runtime.execute([](MPI_Comm communicator) -> int {
    operation_communicator = communicator;
    injection_active = true;
    if (selected_mode == failure_mode::rank_query) {
      auto rank = -1;
      kahip::mpi::check_or_abort(
          MPI_Comm_rank(communicator, &rank), communicator,
          "root runtime failure probe", "MPI_Comm_rank(kaffpaE)");
    }
    return EXIT_SUCCESS;
  });
}
