/*
MODEL_STRIPS_REPAIR.C (test)

The real model_strips_repair (custom_edition_geometry.c). Some Custom Edition
maps' model parts have strip indices past their part's vertices, which Halo
PC draws as stray triangles (HaloMD's acrophobia_1: its flamethrower). Each
such index takes the one before it in the strip, so its triangles draw as
none and the map is no longer refused; a strip with no index in range, or a
part with no vertices, is left for custom_edition_model_part_verify to
refuse. test_model_strips_repair.py takes the code (under_test.inc).
*/

#include "harness.h"

#include <string.h>

typedef unsigned short word;

#define NONE (-1)

struct tag_block
{
	long count;
	void *address;
	void *definition;
};

#define TAG_BLOCK_GET_ELEMENT(block, index, type) (((type *)(block)->address) + (index))

struct custom_edition_model_part
{
	long vertex_count;
	long strip_triangle_count;
	unsigned long strip_offset;
};

struct model_geometry
{
	struct tag_block parts;
};

struct model
{
	struct tag_block geometries;
};

struct custom_edition_load_report
{
	unsigned long model_index_data_offset;
};

#include "under_test.inc"

/* one model of one geometry of one part, its strip `before` (count
indices, strip_triangle_count + 2) at 0x10 into the index data at 0x20 of
the model data */
static long repair(long vertex_count, word *strip, long count, word *after)
{
	static byte model_data[0x1000];
	struct custom_edition_model_part part;
	struct model_geometry geometry;
	struct model model;
	struct custom_edition_load_report report;
	long repaired;

	memset(model_data, 0xCD, sizeof(model_data));
	memcpy(model_data + 0x20 + 0x10, strip, count * sizeof(word));
	part.vertex_count = vertex_count;
	part.strip_triangle_count = count - 2;
	part.strip_offset = 0x10;
	geometry.parts.count = 1;
	geometry.parts.address = &part;
	model.geometries.count = 1;
	model.geometries.address = &geometry;
	report.model_index_data_offset = 0x20;
	repaired = model_strips_repair(&model, &report, model_data);
	memcpy(after, model_data + 0x20 + 0x10, count * sizeof(word));
	return repaired;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	word after[16];

	/* acrophobia_1's flamethrower: one index past 985 vertices */
	CASE("repairs-past-index")
	{
		word strip[] = { 0, 1, 2, 1416, 3, 4 };
		word want[] = { 0, 1, 2, 2, 3, 4 };
		long repaired = repair(5, strip, 6, after);

		CHECK(repaired == 1, "repaired %ld indices", repaired);
		CHECK(!memcmp(after, want, sizeof(want)), "strip %u %u %u %u %u %u", after[0], after[1], after[2], after[3], after[4], after[5]);
		return 0;
	}
	/* the strip's first index past the vertices takes its first in range */
	CASE("repairs-leading-index")
	{
		word strip[] = { 9, 0, 1, 2 };
		word want[] = { 0, 0, 1, 2 };
		long repaired = repair(3, strip, 4, after);

		CHECK(repaired == 1, "repaired %ld indices", repaired);
		CHECK(!memcmp(after, want, sizeof(want)), "strip %u %u %u %u", after[0], after[1], after[2], after[3]);
		return 0;
	}
	/* a sound strip is not changed */
	CASE("leaves-sound-strips")
	{
		word strip[] = { 0, 1, 2, 1, 3, 2 };
		long repaired = repair(4, strip, 6, after);

		CHECK(repaired == 0, "repaired %ld indices", repaired);
		CHECK(!memcmp(after, strip, sizeof(strip)), "a sound strip changed");
		return 0;
	}
	/* nothing to take an index from: left for the verify to refuse */
	CASE("leaves-hopeless-strips")
	{
		word strip[] = { 7, 8, 9 };
		word none[] = { 0, 1, 2 };
		long repaired = repair(3, strip, 3, after);

		CHECK(repaired == 0, "repaired %ld indices of a strip with none in range", repaired);
		CHECK(!memcmp(after, strip, sizeof(strip)), "a strip with no index in range changed");
		repaired = repair(0, none, 3, after);
		CHECK(repaired == 0, "repaired %ld indices of a part with no vertices", repaired);
		CHECK(!memcmp(after, none, sizeof(none)), "a part with no vertices had its strip changed");
		return 0;
	}

	return 2;
}
