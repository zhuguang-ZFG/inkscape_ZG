# coding=utf-8
"""
Tests for DPISwitcher extensions

python3 -m pytest tests/test_dpiswitcher.py

Generate references:
python3 dpiswitcher.py tests/data/svg/shapes.svg > tests/data/refs/dpiswitcher.out
python3 dpiswitcher.py --switcher=0 tests/data/svg/dpiswitcher_96dpi.svg >
        tests/data/refs/dpiswitcher__--switcher__0.out
python3 dpiswitcher.py --switcher=1 tests/data/svg/dpiswitcher_96dpi.svg >
        tests/data/refs/dpiswitcher__--switcher__1.out

"""

from dpiswitcher import DPISwitcher, check_text_on_path
from inkex.elements._parser import load_svg
from inkex.tester import ComparisonMixin, TestCase
from inkex.tester.filters import CompareNumericFuzzy


class TestDPISwitcherBasic(ComparisonMixin, TestCase):
    """Default Test with shapes.svg"""

    effect_class = DPISwitcher
    compare_filters = [CompareNumericFuzzy()]


class TestDPIto90to96(ComparisonMixin, TestCase):
    """Test file with transformed objects in root"""

    compare_file = "svg/dpiswitcher_96dpi.svg"
    comparisons = [("--switcher=0",), ("--switcher=1",)]
    compare_filters = [CompareNumericFuzzy()]
    effect_class = DPISwitcher


class TestDPISwitcherTextOnPath(TestCase):
    """Tests for special text-on-path scaling."""

    def test_scales_nested_tspan_font_size(self):
        doc = load_svg(
            """<svg xmlns="http://www.w3.org/2000/svg"
                    xmlns:xlink="http://www.w3.org/1999/xlink">
                <path id="path" d="M0,0 L10,0"/>
                <text id="text" style="font-size:10">
                    <textPath xlink:href="#path">
                        <tspan id="span" style="font-size:5">Text</tspan>
                    </textPath>
                </text>
            </svg>"""
        )
        svg = doc.getroot()

        self.assertTrue(check_text_on_path(svg, svg.getElementById("text"), 2, 2))

        self.assertEqual(svg.getElementById("text").style["font-size"], "20.0")
        self.assertEqual(svg.getElementById("span").style["font-size"], "10.0")
