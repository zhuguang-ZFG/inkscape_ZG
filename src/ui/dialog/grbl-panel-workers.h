// SPDX-License-Identifier: GPL-2.0-or-later
/** \file
 * Short-lived worker lifetime boundary for GRBL panel tasks.
 */
#ifndef INKSCAPE_UI_DIALOG_GRBL_PANEL_WORKERS_H
#define INKSCAPE_UI_DIALOG_GRBL_PANEL_WORKERS_H

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Inkscape::UI::Dialog {

class GrblPanelWorkers {
public:
    using StopFlag = std::atomic<bool>;
    using Work = std::function<void(StopFlag const &stop)>;

    GrblPanelWorkers() = default;
    ~GrblPanelWorkers();

    GrblPanelWorkers(GrblPanelWorkers const &) = delete;
    GrblPanelWorkers &operator=(GrblPanelWorkers const &) = delete;
    GrblPanelWorkers(GrblPanelWorkers &&) = delete;
    GrblPanelWorkers &operator=(GrblPanelWorkers &&) = delete;

    bool start(Work work);
    void request_stop() noexcept;
    void join_all() noexcept;
    [[nodiscard]] bool stop_requested() const noexcept;

private:
    mutable std::mutex _mutex;
    std::vector<std::thread> _threads;
    StopFlag _stop_requested{false};
};

} // namespace Inkscape::UI::Dialog

#endif // INKSCAPE_UI_DIALOG_GRBL_PANEL_WORKERS_H
