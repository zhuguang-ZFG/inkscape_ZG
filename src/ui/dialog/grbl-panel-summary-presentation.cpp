// SPDX-License-Identifier: GPL-2.0-or-later

#include "grbl-panel-summary-presentation.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include <glibmm/i18n.h>
#include <glibmm/markup.h>

namespace Inkscape::UI::Dialog {
namespace {

struct LayoutScaleMetrics
{
    double content_w_mm = 0.0;
    double content_h_mm = 0.0;
    double bed_w_mm = 0.0;
    double bed_h_mm = 0.0;
    double fit_scale = 0.0;
    double fill_x_pct = 0.0;
    double fill_y_pct = 0.0;
    bool fits_without_scaling = false;
};

Glib::ustring format_duration_compact(double const seconds)
{
    if (!(seconds > 0.0)) {
        return _("未估算");
    }
    auto const rounded = static_cast<long long>(std::llround(seconds));
    long long const hours = rounded / 3600;
    long long const minutes = (rounded % 3600) / 60;
    long long const secs = rounded % 60;
    Glib::ustring out;
    if (hours > 0) {
        out += std::to_string(hours);
        out += "小时";
    }
    if (minutes > 0 || hours > 0) {
        out += std::to_string(minutes);
        out += "分";
    }
    out += std::to_string(secs);
    out += "秒";
    return out;
}

bool get_air_travel_ratio_text(Inkscape::Axidraw::GrblPlotStats const &stats, Glib::ustring &ratio_out)
{
    if (!stats.has_length_stats) {
        return false;
    }
    double const total = stats.draw_length_mm + stats.travel_length_mm;
    if (!(total > 1e-9)) {
        return false;
    }
    std::ostringstream ratio;
    ratio << std::fixed << std::setprecision(1) << ((stats.travel_length_mm / total) * 100.0);
    ratio_out = ratio.str();
    return true;
}

bool get_plot_bounds_text(Inkscape::Axidraw::GrblPlotStats const &stats, Glib::ustring &bounds_out)
{
    if (!stats.has_bounds_mm) {
        return false;
    }
    std::ostringstream wxh;
    wxh << std::fixed << std::setprecision(1) << (stats.max_x_mm - stats.min_x_mm) << " x "
        << (stats.max_y_mm - stats.min_y_mm);
    bounds_out = wxh.str();
    return true;
}

bool get_plot_lengths_text(Inkscape::Axidraw::GrblPlotStats const &stats, Glib::ustring &lengths_out)
{
    if (!stats.has_length_stats) {
        return false;
    }
    std::ostringstream lengths;
    lengths << std::fixed << std::setprecision(1) << stats.draw_length_mm << " / " << stats.travel_length_mm;
    lengths_out = lengths.str();
    return true;
}

bool get_travel_optimization_text(Inkscape::Axidraw::GrblPlotStats const &stats, Glib::ustring &text_out)
{
    if (!stats.has_travel_optimization_stats || !(stats.travel_length_before_optimization_mm > 1e-9)) {
        return false;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << stats.travel_length_before_optimization_mm << " -> "
        << stats.travel_length_mm << " mm";
    if (stats.travel_length_saved_by_optimization_mm > 1e-9) {
        out << " (-" << std::fixed << std::setprecision(1) << stats.travel_length_saved_by_optimization_mm << " mm)";
    }
    text_out = out.str();
    return true;
}

bool get_layout_scale_metrics_from_bounds_mm(double const content_w_mm, double const content_h_mm,
                                             double const bed_width_mm, double const bed_height_mm,
                                             LayoutScaleMetrics &metrics, Glib::ustring &error)
{
    if (!(content_w_mm > 1e-9) || !(content_h_mm > 1e-9)) {
        error = _("当前文档没有有效的绘图范围。");
        return false;
    }
    if (!(bed_width_mm > 1e-9) || !(bed_height_mm > 1e-9)) {
        error = _("机器行程无效，请先同步或设置床面宽度/深度。");
        return false;
    }

    metrics.content_w_mm = content_w_mm;
    metrics.content_h_mm = content_h_mm;
    metrics.bed_w_mm = bed_width_mm;
    metrics.bed_h_mm = bed_height_mm;
    metrics.fit_scale = std::min(bed_width_mm / content_w_mm, bed_height_mm / content_h_mm);
    metrics.fill_x_pct = (content_w_mm / bed_width_mm) * 100.0;
    metrics.fill_y_pct = (content_h_mm / bed_height_mm) * 100.0;
    metrics.fits_without_scaling = metrics.fit_scale >= 0.999999;
    return true;
}

Glib::ustring build_summary_markup(Glib::ustring const &title, Glib::ustring const &message)
{
    return Glib::ustring::compose("<b>%1</b>\n%2", title, Glib::Markup::escape_text(message));
}

} // namespace

Glib::ustring build_grbl_layout_scale_summary_markup(Inkscape::Axidraw::GrblPlotStats const &stats,
                                                     double const bed_width_mm, double const bed_height_mm)
{
    LayoutScaleMetrics metrics;
    Glib::ustring error;
    if (!stats.has_bounds_mm ||
        !get_layout_scale_metrics_from_bounds_mm(stats.max_x_mm - stats.min_x_mm, stats.max_y_mm - stats.min_y_mm,
                                                 bed_width_mm, bed_height_mm, metrics, error)) {
        return Glib::ustring::compose("<b>%1</b>\n%2", _("当前缩放"), Glib::Markup::escape_text(error));
    }

    std::ostringstream out;
    out << "<b>当前缩放</b>\n";
    out << _("图稿尺寸：") << std::fixed << std::setprecision(1) << metrics.content_w_mm << " x "
        << metrics.content_h_mm << " mm";
    out << "\n" << _("机器行程占用：X ") << std::fixed << std::setprecision(1) << metrics.fill_x_pct << "% / Y "
        << metrics.fill_y_pct << "%";
    out << "\n" << _("一键适配后比例：");
    if (metrics.fits_without_scaling) {
        double const enlarge_pct = metrics.fit_scale * 100.0;
        out << _("当前已在行程内");
        if (enlarge_pct > 100.1) {
            out << "（" << _("若放大到铺满可达") << " " << std::fixed << std::setprecision(1) << enlarge_pct
                << "%，当前按钮默认不放大）";
        }
    } else {
        out << std::fixed << std::setprecision(1) << (metrics.fit_scale * 100.0) << "%";
    }
    return out.str();
}

Glib::ustring build_grbl_job_summary_markup(Inkscape::Axidraw::GrblPlotStats const &stats, double const bed_width_mm,
                                            double const bed_height_mm)
{
    std::ostringstream summary;
    summary << "<b>任务概览</b>\n";
    summary << _("图层数：") << stats.layer_count << _(" 个");
    summary << "    " << _("笔画数：") << stats.stroke_count << _(" 条");
    if (stats.tool_change_count > 0) {
        summary << "    " << _("换笔：") << stats.tool_change_count << _(" 次");
    }
    summary << "\n" << _("预计时长：") << Glib::Markup::escape_text(format_duration_compact(stats.estimated_duration_sec));

    Glib::ustring bounds;
    if (get_plot_bounds_text(stats, bounds)) {
        summary << "    " << _("范围：") << bounds << " mm";
    }

    Glib::ustring lengths;
    Glib::ustring ratio;
    if (get_plot_lengths_text(stats, lengths) && get_air_travel_ratio_text(stats, ratio)) {
        summary << "\n" << _("落笔/空走：") << lengths << " mm";
        summary << "    " << _("空走占比：") << ratio << "%";
    }

    LayoutScaleMetrics metrics;
    Glib::ustring layout_error;
    if (stats.has_bounds_mm &&
        get_layout_scale_metrics_from_bounds_mm(stats.max_x_mm - stats.min_x_mm, stats.max_y_mm - stats.min_y_mm,
                                                bed_width_mm, bed_height_mm, metrics, layout_error)) {
        summary << "\n" << _("适配缩放：");
        if (metrics.fits_without_scaling) {
            summary << _("无需缩小");
        } else {
            summary << std::fixed << std::setprecision(1) << (metrics.fit_scale * 100.0) << "%";
        }
        summary << "    " << _("床面占用：X ") << std::fixed << std::setprecision(1) << metrics.fill_x_pct << "%";
        summary << " / Y " << std::fixed << std::setprecision(1) << metrics.fill_y_pct << "%";
    }

    Glib::ustring travel_optimization;
    if (get_travel_optimization_text(stats, travel_optimization)) {
        summary << "\n" << _("路径排序空走：") << travel_optimization;
    }

    return summary.str();
}

Glib::ustring build_grbl_fill_gcode_status(std::size_t const strokes, Inkscape::Axidraw::GrblPlotStats const &stats)
{
    Glib::ustring bounds;
    if (get_plot_bounds_text(stats, bounds)) {
        Glib::ustring lengths;
        Glib::ustring ratio;
        if (get_plot_lengths_text(stats, lengths) && get_air_travel_ratio_text(stats, ratio)) {
            return Glib::ustring::compose(
                _("编辑器已填入 %1 条笔画，对应工作区域约 %2 mm，绘制/空走长度 %3 mm，空走占比 %4%%。请先检查，如有需要可“另存为 G-code”，然后再“发送到机器”。"),
                static_cast<guint64>(strokes), bounds, lengths, ratio);
        }
        return Glib::ustring::compose(
            _("编辑器已填入 %1 条笔画，对应工作区域约 %2 mm（按首选项换算后的机器坐标）。请先检查，如有需要可“另存为 G-code”，然后再“发送到机器”。"),
            static_cast<guint64>(strokes), bounds);
    }
    return Glib::ustring::compose(_("编辑器已为 %1 条笔画生成 G-code。请先检查内容，确认后再“发送到机器”。"),
                                  static_cast<guint64>(strokes));
}

Glib::ustring build_grbl_no_active_document_job_summary_markup()
{
    return _("<b>任务概览</b>\n暂无活动文档。");
}

Glib::ustring build_grbl_no_active_document_scale_summary_markup()
{
    return _("<b>当前缩放</b>\n暂无活动文档。");
}

Glib::ustring build_grbl_analysis_error_job_summary_markup(std::string const &err)
{
    auto const message = err.empty() ? _("当前无法估算任务信息。") : Glib::ustring(err);
    return build_summary_markup(_("任务概览"), message);
}

Glib::ustring build_grbl_analysis_error_scale_summary_markup(std::string const &err)
{
    auto const message = err.empty() ? _("当前无法估算缩放信息。") : Glib::ustring(err);
    return build_summary_markup(_("当前缩放"), message);
}

} // namespace Inkscape::UI::Dialog
