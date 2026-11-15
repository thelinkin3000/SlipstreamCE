"""A map's script syntax sound but off a 4-byte boundary (Covenant Edition's ui.map, a modding tool's packing) runs from
an aligned copy (hs.c's hs_scenario_syntax_data_realign); damaged syntax, syntax outside the tag cache or of another size
is still refused (hs_scenario_syntax_data_valid)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ["aligned", "misaligned", "misaligned-damaged", "outside-the-tag-cache", "another-size"]

# a fault in hs.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "copy-not-named": (("\tscenario->hs_syntax_data.address = hs_syntax_data_realigned;\n", ""),
                       "hs_scenario_syntax_data_realign", "misaligned"),
    "alignment-not-checked": (("\t\t((unsigned long)address & 3))\n\t{\n\t\treturn FALSE;",
                               "\t\tFALSE)\n\t{\n\t\treturn FALSE;"), "hs_scenario_syntax_data_valid", "misaligned"),
    "copy-trusted": (("\t\t(address != (byte const *)hs_syntax_data_realigned &&\n", "\t\t(FALSE &&\n"),
                     "hs_scenario_syntax_data_valid", "outside-the-tag-cache"),
}

FUNCTIONS = ("hs_scenario_string_constants_valid", "hs_scenario_syntax_data_valid", "hs_scenario_syntax_data_realign")


def generated(fault=None, faulty_function=None):
    source = read("source/hs/hs.c")
    text = ""
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    # (declared ahead, as hs.c's prototypes do)
    head = ("static boolean hs_scenario_string_constants_valid(struct scenario const *scenario);\n"
            "static boolean hs_scenario_syntax_data_valid(struct scenario const *scenario);\n")
    return (("under_test.inc", head + text),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("hs_syntax_alignment", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("hs_syntax_alignment", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"hs.c with a fault ({control}) passed '{case}': the test cannot see it"
