/******************************************************************************
 * serial_kernel_profile_observer.h
 *
 * Test-only observation seam for the checked serial-kernel handoff.
 *****************************************************************************/

#ifndef SERIAL_KERNEL_PROFILE_OBSERVER_H
#define SERIAL_KERNEL_PROFILE_OBSERVER_H

#include "serial_kernel_profile.h"

namespace parhip::mpi_tools_detail {
using serial_kernel_profile_observer_callback = void (*)(
    void*, kahip::serial_kernel::serial_kernel_profile const&) noexcept;

// This private observer is deliberately process-sequential and non-reentrant.
// Its sole storage is defined in mpi_tools.cpp. The caller and observer scope
// must resolve to the same linked library image; the public C-call test uses
// that exact parhip_interface linkage and does not mix in parallel separately.
class scoped_serial_kernel_profile_observer final {
 public:
  explicit scoped_serial_kernel_profile_observer(
      serial_kernel_profile_observer_callback callback,
      void* context) noexcept;
  ~scoped_serial_kernel_profile_observer() noexcept;

  scoped_serial_kernel_profile_observer(
      scoped_serial_kernel_profile_observer const&) = delete;
  auto operator=(scoped_serial_kernel_profile_observer const&)
      -> scoped_serial_kernel_profile_observer& = delete;

 private:
  serial_kernel_profile_observer_callback previous_{};
  void* previous_context_{};
};

void observe_checked_serial_kernel_profile(
    kahip::serial_kernel::serial_kernel_profile const& profile) noexcept;
}  // namespace parhip::mpi_tools_detail

#endif
