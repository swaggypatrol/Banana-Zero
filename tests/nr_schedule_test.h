#pragma once

#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "nr_schedule.h"

// The preview's deadlines against frame clocks; no wall-clock waits or GPU work.
template <typename CheckFn> void TestNrSchedule(CheckFn check)
{
    for (const unsigned hz : { 30u, 10u })
    {
        const double seconds = 1.0 / hz;
        for (const unsigned fps : { 24u, 30u, 40u, 60u, 120u, 144u, 240u })
        {
            NrSchedule schedule;
            unsigned updates = 0;
            for (unsigned frame = 0; frame < fps * 10; ++frame)
            {
                const double now = double(frame) / fps;
                if (schedule.Due(now))
                {
                    schedule.Advance(now, seconds);
                    ++updates;
                }
            }
            char label[128];
            std::snprintf(label, sizeof label, "schedule: %u Hz at %u fps: %u updates in ten seconds", hz, fps, updates);
            check(updates == (fps < hz ? fps : hz) * 10, label);
        }

        NrSchedule jittered;
        const double steps[] = { 0.6, 1.3, 0.8, 1.5, 0.7, 1.1 };
        double now = 0.0;
        unsigned updates = 0, frame = 0;
        while (now < 60.0)
        {
            if (jittered.Due(now))
            {
                jittered.Advance(now, seconds);
                ++updates;
            }
            now += steps[frame++ % 6] / 144.0;
        }
        char label[128];
        std::snprintf(label, sizeof label, "schedule: %u Hz with frame jitter: %u updates in a minute", hz, updates);
        check(std::abs(double(updates) - double(hz * 60)) <= 1.0, label);
    }

    NrSchedule schedule;
    check(schedule.Due(5.0), "schedule: opening is due immediately");
    check(schedule.Due(5.01), "schedule: a request stays due until work completes");
    schedule.Advance(5.01, 1.0 / 30.0);
    check(!schedule.Due(5.02), "schedule: completed work waits for its next deadline");
    schedule = {};
    check(schedule.Due(5.02), "schedule: changing the preview starts immediately");
    schedule.Advance(5.02, 1.0 / 30.0);
    check(!schedule.Due(5.02), "schedule: changing the preview restarts its cadence");

    check(schedule.Due(120.0), "schedule: work resumes after a long stall");
    schedule.Advance(120.0, 1.0 / 30.0);
    check(!schedule.Due(120.0) && !schedule.Due(120.001) && schedule.Due(120.03),
          "schedule: missed periods cause one update, without a catch-up burst or lost cadence");
}
