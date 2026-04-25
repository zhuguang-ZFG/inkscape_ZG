// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-workers.h"

#include <utility>

namespace Inkscape::UI::Dialog {

GrblPanelWorkers::~GrblPanelWorkers()
{
    request_stop();
    join_all();
}

bool GrblPanelWorkers::start(Work work)
{
    if (!work) {
        return false;
    }

    std::lock_guard const guard(_mutex);
    if (_stop_requested.load(std::memory_order_acquire)) {
        return false;
    }

    _threads.emplace_back([this, work = std::move(work)]() mutable { work(_stop_requested); });
    return true;
}

void GrblPanelWorkers::request_stop() noexcept
{
    _stop_requested.store(true, std::memory_order_release);
}

void GrblPanelWorkers::join_all() noexcept
{
    std::vector<std::thread> threads;
    {
        std::lock_guard const guard(_mutex);
        threads.swap(_threads);
    }

    for (auto &thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

bool GrblPanelWorkers::stop_requested() const noexcept
{
    return _stop_requested.load(std::memory_order_acquire);
}

} // namespace Inkscape::UI::Dialog
