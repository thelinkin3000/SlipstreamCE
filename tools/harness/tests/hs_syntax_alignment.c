/* The real checks of a map's script syntax (hs.c's hs_scenario_syntax_data_valid,
 * hs_scenario_string_constants_valid) and its realignment
 * (hs_scenario_syntax_data_realign) over a fake tag cache: syntax data sound
 * but 2 bytes off a 4-byte boundary (Covenant Edition's ui.map) runs from an
 * aligned copy; damaged syntax, syntax outside the tag cache or of another
 * size is still refused. */
#include "harness.h"

/* (the game's data array, not the harness's own) */
struct game_data_array
{
	char name[32];
	short maximum_count;
	short size;
	unsigned char valid;
	unsigned char identifier_zero_invalid;
	unsigned long signature;
	short first_free_absolute_index;
	short count;
	short actual_count;
	short next_identifier;
	void *data;
};
#define data_array game_data_array
struct hs_syntax_node { char bytes[20]; };
struct tag_data { long size; unsigned long pad; long file_offset; void *address; void *definition; };
struct scenario { struct tag_data hs_syntax_data; struct tag_data hs_string_constants; };
#define TAG_DATA_ADDRESS(data) ((data).address)
#define MAXIMUM_HS_SYNTAX_NODES_PER_SCENARIO 19001
#define csmemcpy memcpy
#define _error_silent 0

static unsigned char *tag_cache;
#define TAG_CACHE_BYTES (1 << 20)
static boolean cache_file_tag_cache_contains(void const *address, long size)
{
	unsigned char const *at = address;

	return at >= tag_cache && size >= 0 && at + size <= tag_cache + TAG_CACHE_BYTES;
}
static int mcc_syntax_scenario_valid(struct scenario const *scenario) { (void)scenario; return -1; }
static int errors;
static void error(int severity, char const *format, ...) { (void)severity; (void)format; errors++; }
static struct data_array *hs_syntax_data_realigned = NULL;

#include "under_test.inc"

static struct scenario scenario;

/* syntax data at offset into the tag cache, sound unless damaged, and its strings */
static void map_with_syntax(long offset, boolean damaged)
{
	long size = sizeof(struct data_array) + MAXIMUM_HS_SYNTAX_NODES_PER_SCENARIO * sizeof(struct hs_syntax_node);
	struct data_array array;

	memset(tag_cache, 0, TAG_CACHE_BYTES);
	memset(&array, 0, sizeof(array));
	strcpy(array.name, "script node");
	array.maximum_count = damaged ? 100 : MAXIMUM_HS_SYNTAX_NODES_PER_SCENARIO;
	array.size = sizeof(struct hs_syntax_node);
	array.valid = TRUE;
	array.signature = 'd@t@';
	array.first_free_absolute_index = 0;
	array.count = 83;
	array.actual_count = 79;
	memcpy(tag_cache + offset, &array, sizeof(array));
	/* (a node, to see the copy carries the nodes too) */
	tag_cache[offset + sizeof(array) + 5] = 0x5A;
	scenario.hs_syntax_data.size = size;
	scenario.hs_syntax_data.address = tag_cache + offset;
	scenario.hs_string_constants.size = 0x400;
	scenario.hs_string_constants.address = tag_cache + 0xF0000;
}

int main(int argc, char **argv)
{
	char const *case_name;
	unsigned char *map_address;

	CHECK(argc == 2, "case required");
	case_name = argv[1];
	tag_cache = malloc(TAG_CACHE_BYTES);
	CHECK(tag_cache && !((unsigned long)tag_cache & 3), "an aligned tag cache");
	CASE("aligned")
	{
		map_with_syntax(0x1000, FALSE);
		map_address = scenario.hs_syntax_data.address;
		hs_scenario_syntax_data_realign(&scenario);
		CHECK(scenario.hs_syntax_data.address == map_address, "aligned syntax is run where the map has it");
		CHECK(hs_scenario_syntax_data_valid(&scenario), "sound aligned syntax runs");
		return 0;
	}
	CASE("misaligned")
	{
		map_with_syntax(0x1002, FALSE);
		map_address = scenario.hs_syntax_data.address;
		CHECK(!hs_scenario_syntax_data_valid(&scenario), "misaligned syntax is not run in place");
		hs_scenario_syntax_data_realign(&scenario);
		CHECK(scenario.hs_syntax_data.address != map_address, "misaligned syntax is copied");
		CHECK(!((unsigned long)scenario.hs_syntax_data.address & 3), "the copy is aligned");
		CHECK(!memcmp(scenario.hs_syntax_data.address, map_address, scenario.hs_syntax_data.size),
			"the copy is the map's syntax, nodes and all");
		CHECK(hs_scenario_syntax_data_valid(&scenario), "the aligned copy of sound syntax runs");
		/* (asked again, as hs_scenario_postprocess's hs_allocate does: the copy stands) */
		hs_scenario_syntax_data_realign(&scenario);
		CHECK(hs_scenario_syntax_data_valid(&scenario), "the copy is not copied again");
		return 0;
	}
	CASE("misaligned-damaged")
	{
		map_with_syntax(0x1002, TRUE);
		hs_scenario_syntax_data_realign(&scenario);
		CHECK(!hs_scenario_syntax_data_valid(&scenario), "damaged syntax is refused, copied or not");
		return 0;
	}
	CASE("outside-the-tag-cache")
	{
		/* (aligned, so that only its place refuses it) */
		static unsigned long elsewhere[100000];

		map_with_syntax(0x1000, FALSE);
		memcpy(elsewhere, scenario.hs_syntax_data.address, scenario.hs_syntax_data.size);
		scenario.hs_syntax_data.address = elsewhere;
		map_address = scenario.hs_syntax_data.address;
		hs_scenario_syntax_data_realign(&scenario);
		CHECK(scenario.hs_syntax_data.address == map_address, "syntax outside the tag cache is not copied");
		CHECK(!hs_scenario_syntax_data_valid(&scenario), "syntax outside the tag cache is refused");
		return 0;
	}
	CASE("another-size")
	{
		map_with_syntax(0x1002, FALSE);
		scenario.hs_syntax_data.size -= 20;
		map_address = scenario.hs_syntax_data.address;
		hs_scenario_syntax_data_realign(&scenario);
		CHECK(scenario.hs_syntax_data.address == map_address, "syntax of another size is not copied");
		CHECK(!hs_scenario_syntax_data_valid(&scenario), "syntax of another size is refused");
		return 0;
	}
	CHECK(FALSE, "unknown case %s", case_name);
	return 1;
}
