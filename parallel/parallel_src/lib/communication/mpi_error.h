#pragma once

#include <mpi.h>

#include <array>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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
    std::array<char, MPI_MAX_ERROR_STRING> error_text{};
    int error_text_length = 0;
    auto const result =
        MPI_Error_string(error_code, error_text.data(), &error_text_length);

    std::ostringstream message;
    message << context << " at " << location.file_name() << ':'
            << location.line() << " (MPI error " << error_code;
    if (result == MPI_SUCCESS) {
      message << ": "
              << std::string_view{error_text.data(),
                                  static_cast<std::size_t>(error_text_length)};
    }
    message << ')';
    return message.str();
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
