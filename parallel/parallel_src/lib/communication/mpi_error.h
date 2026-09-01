#pragma once

#include <mpi.h>

#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace parhip::mpi {
class mpi_error final : public std::runtime_error {
public:
  explicit mpi_error(
      int error_code,
      std::string context,
      std::source_location location = std::source_location::current())
      : std::runtime_error(make_message(error_code, context, location)),
        error_code_(error_code),
        context_(std::move(context)),
        location_(location) {}

  [[nodiscard]] auto error_code() const noexcept -> int { return error_code_; }
  [[nodiscard]] auto context() const noexcept -> std::string_view {
    return context_;
  }
  [[nodiscard]] auto location() const noexcept -> std::source_location {
    return location_;
  }

private:
  static auto make_message(int error_code,
                           std::string_view context,
                           std::source_location location) -> std::string {
    return fmt::format("{} at {}:{} (MPI error {})", context,
                       location.file_name(), location.line(), error_code);
  }

  int error_code_;
  std::string context_;
  std::source_location location_;
};

inline void check(
    int error_code,
    std::string_view context,
    std::source_location location = std::source_location::current()) {
  if (error_code != MPI_SUCCESS) {
    throw mpi_error{error_code, std::string{context}, location};
  }
}
}  // namespace parhip::mpi
