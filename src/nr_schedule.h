#pragma once

#include <cmath>

// A frame-driven periodic task. Zero starts it immediately; only a completed task advances the deadline.
struct NrSchedule
{
    double next = 0.0;

    // The tiny allowance covers floating-point addition at an otherwise exact frame boundary.
    bool Due(double now) const { return now + 1.0e-9 >= next; }

    // Keep the original cadence, skipping elapsed periods after a slow frame instead of catching them up.
    void Advance(double now, double seconds)
    {
        if (next == 0.0)
            next = now;
        next += seconds;
        const double after = now + 1.0e-9;
        if (next <= after)
            next += (std::floor((after - next) / seconds) + 1.0) * seconds;
    }
};
