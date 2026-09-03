#pragma once

#include <concepts>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string_view>
#include <utility>

namespace kahip::diagnostics {
struct sink final {
  void (*write)(std::string_view) noexcept;
  void (*flush)() noexcept;
};

namespace detail {
inline void write_to_stderr(std::string_view message) noexcept {
  try {
    std::cerr.write(message.data(),
                    static_cast<std::streamsize>(message.size()));
    std::cerr.put('\n');
  } catch (...) {
    // Diagnostics must never replace the fail-fast path.
  }
}

inline void flush_stderr() noexcept {
  try {
    std::cerr.flush();
  } catch (...) {
    // Diagnostics must never replace the fail-fast path.
  }
}

template <typename T>
concept stream_insertable = requires(std::ostream& output, T&& value) {
  {
    output << std::forward<T>(value)
  } -> std::same_as<std::ostream&>;
};

inline constexpr auto default_sink = sink{
    .write = write_to_stderr,
    .flush = flush_stderr,
};
inline sink const* active_sink = &default_sink;
inline constexpr auto formatting_failure_message =
    std::string_view{"fatal diagnostic formatting failed"};
}  // namespace detail

[[nodiscard]] inline auto exchange_sink_for_testing(
    sink const* replacement) noexcept -> sink const* {
  auto const* previous = detail::active_sink;
  detail::active_sink =
      replacement == nullptr ? &detail::default_sink : replacement;
  return previous;
}

template <typename... Fragments>
  requires(sizeof...(Fragments) > 0 &&
           (detail::stream_insertable<Fragments> && ...))
inline void critical(Fragments&&... fragments) noexcept {
  auto const* destination = detail::active_sink;
  try {
    auto message = std::ostringstream{};
    (message << ... << std::forward<Fragments>(fragments));
    if (message) {
      destination->write(message.str());
    } else {
      destination->write(detail::formatting_failure_message);
    }
  } catch (...) {
    destination->write(detail::formatting_failure_message);
  }
  destination->flush();
}
}  // namespace kahip::diagnostics
