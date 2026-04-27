// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <string_view>

#include <src/document.h>
#include <src/inkscape.h>
#include <src/object/sp-namedview.h>
#include <src/object/sp-root.h>
#include <src/ui/dialog/grbl-panel-document-geometry.h>
#include <src/util/units.h>

using Inkscape::UI::Dialog::apply_grbl_sync_page_to_bed_action;
using Inkscape::UI::Dialog::GrblPageRestoreState;

class GrblPanelDocumentGeometryTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        Inkscape::Application::create(false);
    }
};

TEST_F(GrblPanelDocumentGeometryTest, SyncPageToBedSwitchesDisplayUnitsToMillimetersWithoutChangingDocumentScale)
{
    using namespace std::literals;

    constexpr auto doc_string = R"A(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="100mm" height="100mm" viewBox="0 0 100 100"
     xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd">
  <sodipodi:namedview id="namedview1" inkscape:document-units="px"/>
  <rect id="rect1" x="10" y="20" width="30" height="40"/>
</svg>)A"sv;

    auto doc = SPDocument::createNewDocFromMem(doc_string);
    ASSERT_TRUE(doc);
    ASSERT_TRUE(doc->getRoot());
    ASSERT_TRUE(doc->getNamedView());

    doc->ensureUpToDate();

    auto const before_scale = doc->getDocumentScale();
    GrblPageRestoreState restore_state;

    auto const result = apply_grbl_sync_page_to_bed_action(doc.get(), 210.0, 297.0, restore_state);

    EXPECT_TRUE(result.page_synced);
    EXPECT_TRUE(result.unit_synced);
    EXPECT_STREQ(doc->getNamedView()->getDisplayUnit()->abbr.c_str(), "mm");
    EXPECT_STREQ(doc->getNamedView()->getRepr()->attribute("inkscape:document-units"), "mm");
    EXPECT_NEAR(doc->getWidth().value("mm"), 210.0, 1e-5);
    EXPECT_NEAR(doc->getHeight().value("mm"), 297.0, 1e-5);

    doc->ensureUpToDate();

    auto const after_scale = doc->getDocumentScale();
    EXPECT_NEAR(after_scale[Geom::X], before_scale[Geom::X], 1e-6);
    EXPECT_NEAR(after_scale[Geom::Y], before_scale[Geom::Y], 1e-6);
}
