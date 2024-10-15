/******************************************************************************
 * timer.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef TIMER_9KPDEP
#define TIMER_9KPDEP

#include <chrono>

namespace parhip {

class timer {
public:
        timer() : m_start{clock::now()} {}

        void restart() {
                m_start = clock::now();
        }

        [[nodiscard]] double elapsed() const noexcept {
                return std::chrono::duration<double>(clock::now() - m_start).count();
        }

private:
        using clock = std::chrono::steady_clock;
        std::chrono::time_point<clock> m_start;
};

}
#endif /* end of include guard: TIMER_9KPDEP */
