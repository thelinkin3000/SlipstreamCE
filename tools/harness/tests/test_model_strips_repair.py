"""The real model_strips_repair (custom_edition_geometry.c): a Custom Edition model part's strip indices past its
vertices (HaloMD's acrophobia_1) take the index before them, so the map loads with those triangles drawn as none."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["repairs-past-index", "repairs-leading-index", "leaves-sound-strips", "leaves-hopeless-strips"]

# a fault in model_strips_repair and the case that must catch it
NEGATIVE_CONTROLS = {
    "no-repair": (("if (strip[index] >= part->vertex_count)\n\t\t\t\t{", "if (0)\n\t\t\t\t{"), "repairs-past-index"),
    "fill-zero": (("strip[index] = (word)fill;", "strip[index] = 0;"), "repairs-past-index"),
    "no-first-in-range": (("if (strip[index] < part->vertex_count)\n\t\t\t\t\tfill = strip[index];",
                           "fill = 0;"), "leaves-hopeless-strips"),
}


def generated(fault=None):
    code = function(read("port/linux/game/custom_edition_geometry.c"), "model_strips_repair")
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("model_strips_repair", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("model_strips_repair", generated(fault)), case)
    assert status == CHECK_FAILED, f"model_strips_repair with a fault ({control}) passed '{case}': the test cannot see it"
