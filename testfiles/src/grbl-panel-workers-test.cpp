// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "src/ui/dialog/grbl-panel-workers.h"

#include <atomic>
#include <chrono>
#include <thread>

using Inkscape::UI::Dialog::GrblPanelWorkers;

TEST(GrblPanelWorkersTest, StartsWorkerAndJoinAllWaitsForCompletion)
{
    GrblPanelWorkers workers;
    std::atomic<bool> ran{false};

    ASSERT_TRUE(workers.start([&](std::atomic<bool> const &) { ran.store(true, std::memory_order_release); }));

    workers.join_all();

    EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST(GrblPanelWorkersTest, RequestStopIsVisibleToWorker)
{
    GrblPanelWorkers workers;
    std::atomic<bool> saw_stop{false};

    ASSERT_TRUE(workers.start([&](std::atomic<bool> const &stop) {
        while (!stop.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        saw_stop.store(true, std::memory_order_release);
    }));

    workers.request_stop();
    workers.join_all();

    EXPECT_TRUE(saw_stop.load(std::memory_order_acquire));
}

TEST(GrblPanelWorkersTest, RejectsNewWorkAfterStopRequest)
{
    GrblPanelWorkers workers;
    workers.request_stop();

    EXPECT_FALSE(workers.start([](std::atomic<bool> const &) {}));
}

TEST(GrblPanelWorkersTest, JoinAllWaitsForOutstandingWorker)
{
    GrblPanelWorkers workers;
    std::atomic<bool> finished{false};

    ASSERT_TRUE(workers.start([&](std::atomic<bool> const &) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        finished.store(true, std::memory_order_release);
    }));

    workers.join_all();

    EXPECT_TRUE(finished.load(std::memory_order_acquire));
}
