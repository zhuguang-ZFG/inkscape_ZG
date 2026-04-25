#!/usr/bin/env python3
import sys

from webslicer_export import Export
from inkex.tester import ComparisonMixin, TestCase


class TestWebSlicerExportBasic(ComparisonMixin, TestCase):
    stderr_protect = False  # Cover lack of ImageMagic in CI builder
    effect_class = Export

    @property
    def comparisons(self):
        return [("--dir", self.tempdir)]


class TestWebSlicerCommandOutput(TestCase):
    def test_get_cmd_output_returns_stdout_text(self):
        status, output = Export().get_cmd_output(
            [sys.executable, "-c", "print('ImageMagick 7')"]
        )

        self.assertEqual(status, 0)
        self.assertEqual(output, "ImageMagick 7")


class TestWebSlicerCssRegistration(TestCase):
    def test_img_float_left_registers_left_float(self):
        class Element:
            attrib = {"id": "slice"}

        effect = Export()
        effect.reg_html = lambda *args: None

        effect.register_unity_code(
            Element(),
            {
                "html-id": "slice",
                "format": "png",
                "layout-disposition": "img-float-left",
            },
            "#body#",
        )

        self.assertEqual(effect._css, [{"selector": "#slice", "atts": {"float": ["left"]}}])
