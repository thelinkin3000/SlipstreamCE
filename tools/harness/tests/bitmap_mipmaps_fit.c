/*
BITMAP_MIPMAPS_FIT.C (test)

The real bitmap_mipmaps_fit (custom_edition_bitmaps.c). A Custom Edition
bitmap whose pixels hold fewer mipmaps than it says (HaloMD's bgulch_1: a
128x128 DXT1 decal of 7 mipmaps in 0x2000 bytes, its first alone) is given
the mipmaps its pixels hold, so the map is no longer refused; one whose
first mipmap does not fit is left as it was, to be refused. The pixel sizes
here are bitmaps.c's for DXT1 (4x4 blocks of 8 bytes) and A8R8G8B8.
test_bitmap_mipmaps_fit.py takes the code (under_test.inc).
*/

#include "harness.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum { _bitmap_type_2d, _bitmap_type_3d, _bitmap_type_cube_map };
enum { _bitmap_format_a8r8g8b8 = 11, _bitmap_format_dxt1 = 14, _bitmap_format_p8_bump = 17 };

struct bitmap_data
{
	short width;
	short height;
	short depth;
	short type;
	short format;
	short mipmap_count;
	long pixels_size;
};

static short floor_log2(long value)
{
	short result = -1;

	while (value > 0)
	{
		value >>= 1;
		result++;
	}
	return result;
}

static long bitmap_get_pixel_data_size(struct bitmap_data *bitmap)
{
	long size = 0;
	short mipmap;

	for (mipmap = 0; mipmap <= bitmap->mipmap_count; mipmap++)
	{
		long width = MAX(1, bitmap->width >> mipmap);
		long height = MAX(1, bitmap->height >> mipmap);

		if (bitmap->format == _bitmap_format_dxt1)
			size += MAX(1, (width + 3) / 4) * MAX(1, (height + 3) / 4) * 8;
		else
			size += width * height * 4;
	}
	return size;
}

#include "under_test.inc"

static struct bitmap_data bitmap(short size, short format, short mipmaps, long pixels)
{
	struct bitmap_data result = { size, size, 1, _bitmap_type_2d, format, mipmaps, pixels };

	return result;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	/* bgulch_1's decal: its first mipmap alone */
	CASE("keeps-the-mipmaps-held")
	{
		struct bitmap_data decal = bitmap(128, _bitmap_format_dxt1, 7, 0x2000);
		short dropped = bitmap_mipmaps_fit(&decal);

		CHECK(dropped == 7, "dropped %d mipmaps", dropped);
		CHECK(decal.mipmap_count == 0, "left %d mipmaps", decal.mipmap_count);
		return 0;
	}
	/* pixels for three of seven: three kept */
	CASE("keeps-a-partial-chain")
	{
		struct bitmap_data texture = bitmap(64, _bitmap_format_a8r8g8b8, 6, 64 * 64 * 4 + 32 * 32 * 4 + 16 * 16 * 4);
		short dropped = bitmap_mipmaps_fit(&texture);

		CHECK(dropped == 4 && texture.mipmap_count == 2, "dropped %d, left %d", dropped, texture.mipmap_count);
		return 0;
	}
	/* a bitmap whose pixels hold all it says, or more, is not changed */
	CASE("leaves-whole-bitmaps")
	{
		struct bitmap_data whole = bitmap(128, _bitmap_format_dxt1, 7, 0x2AB8);
		struct bitmap_data more = bitmap(128, _bitmap_format_dxt1, 7, 0x4000);

		CHECK(bitmap_mipmaps_fit(&whole) == 0 && whole.mipmap_count == 7, "a whole bitmap changed");
		CHECK(bitmap_mipmaps_fit(&more) == 0 && more.mipmap_count == 7, "a bitmap of more pixels changed");
		return 0;
	}
	/* not even its first: left for custom_edition_bitmap_drawable to refuse */
	CASE("leaves-hopeless-bitmaps")
	{
		struct bitmap_data short_of_one = bitmap(128, _bitmap_format_dxt1, 7, 0x1000);
		struct bitmap_data bad_format = bitmap(128, 40, 7, 0);

		CHECK(bitmap_mipmaps_fit(&short_of_one) == 0 && short_of_one.mipmap_count == 7,
			"a bitmap short of its first mipmap was changed to %d", short_of_one.mipmap_count);
		CHECK(bitmap_mipmaps_fit(&bad_format) == 0 && bad_format.mipmap_count == 7, "a bitmap of no format changed");
		return 0;
	}

	return 2;
}
