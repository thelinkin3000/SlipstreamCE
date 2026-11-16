"""The real bitmap_mipmaps_fit (custom_edition_bitmaps.c): a Custom Edition bitmap whose pixels hold fewer mipmaps than
it says (HaloMD's bgulch_1) is drawn with those it holds, instead of the map being refused."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["keeps-the-mipmaps-held", "keeps-a-partial-chain", "leaves-whole-bitmaps", "leaves-hopeless-bitmaps"]

# a fault in bitmap_mipmaps_fit and the case that must catch it
NEGATIVE_CONTROLS = {
    "no-trim": (("while (bitmap->mipmap_count > 0 && bitmap->pixels_size < bitmap_get_pixel_data_size(bitmap))",
                 "while (0)"), "keeps-the-mipmaps-held"),
    "no-restore": (("\t\tbitmap->mipmap_count = said;\n", ""), "leaves-hopeless-bitmaps"),
}


def generated(fault=None):
    code = function(read("port/linux/game/custom_edition_bitmaps.c"), "bitmap_mipmaps_fit")
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("bitmap_mipmaps_fit", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("bitmap_mipmaps_fit", generated(fault)), case)
    assert status == CHECK_FAILED, f"bitmap_mipmaps_fit with a fault ({control}) passed '{case}': the test cannot see it"
