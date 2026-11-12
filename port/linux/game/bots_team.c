/*
BOTS_TEAM.C

(port) What a team of bots of either kind agrees on (bots_team.h):
capture the flag's carrier, and a Warthog's crew.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "items/weapons.h"
#include "scenario/scenario.h"

#include "bots.h"
#include "bots_common.h"
#include "bots_team.h"

#include <math.h>

/* (a flag lying untouched this long goes home: game_engine_ctf.c's 17 s; the
carrier back on it well before) */
#define BOTS_TEAM_REGRAB_SECONDS 9
/* (the carrier, the flag lying, keeps its job while this near it) */
#define BOTS_TEAM_CARRIER_LEASH 15.0f

struct bots_team_ctf
{
	/* the team's carrier of the other team's flag (a player), NONE */
	long carrier;
	/* the tick the other team's flag was last held (NONE: held now, or home) */
	long lying_since_tick;
	/* the player holding it a tick ago (who let it go) */
	long last_holder;
	/* the guard of the hiding carrier, picked again each second */
	long escort;
	long escort_tick;
	/* the hiding place near home, found once a map */
	real_point3d hide;
	boolean hide_found;
	unsigned long hide_generation;
};

static struct
{
	long tick;
	boolean ctf;
	boolean home_to_score;
	/* each flag's state (by its team) */
	boolean present[2];
	real_point3d position[2];
	real_point3d home[2];
	long holder[2];
	boolean at_home[2];
	struct bots_team_ctf teams[2];
} bots_team_globals;

static real bots_team_distance(real_point3d const *a, real_point3d const *b)
{
	real dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;

	return (real)sqrt(dx * dx + dy * dy + dz * dz);
}

/* a player alive on the team: its unit's position */
static boolean bots_team_player_position(long player_index, long team_index, real_point3d *position)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	struct object_datum *object;

	if (!player || player->team_index != team_index || player->unit_index == NONE)
		return FALSE;
	object = object_try_and_get(player->unit_index);
	if (!object || TEST_FLAG(object->object.damage_flags, _object_dead_bit))
		return FALSE;
	*position = object->object.position;
	return TRUE;
}

static boolean bots_team_player_is_bot(long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player && player->local_player_index == NONE &&
		bots_machine_is_bot(player->network_player_data.machine_index);
}

/* the team's bot alive nearest a point (but one), NONE */
static long bots_team_nearest_bot(long team_index, real_point3d const *point, long except, real *nearest)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long best = NONE;
	real best_distance = 1e30f;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		real_point3d position;
		real distance;

		if (iterator.datum_index == except || !bots_team_player_is_bot(iterator.datum_index) ||
			!bots_team_player_position(iterator.datum_index, team_index, &position))
		{
			continue;
		}
		distance = bots_team_distance(&position, point);
		if (distance < best_distance)
		{
			best_distance = distance;
			best = iterator.datum_index;
		}
	}
	if (nearest)
		*nearest = best_distance;
	return best;
}

/* the hiding place near the team's stand: ground 4-10 units off it, about
level with it, reached from it in a straight line, out of sight of the way
the enemy comes (a point a third of the way to their stand) and far from
it; the stand itself if none */
static void bots_team_find_hide(long team_index)
{
	struct bots_team_ctf *team = &bots_team_globals.teams[team_index];
	real_point3d const *stand = &bots_team_globals.home[team_index];
	real_point3d const *theirs = &bots_team_globals.home[1 - team_index];
	real_point3d approach, stand_eye;
	real best_score = -1e30f;
	static real const radii[] = { 4.0f, 6.0f, 8.0f, 10.0f };
	long angle_index, radius_index;

	team->hide = *stand;
	team->hide_found = TRUE;
	team->hide_generation = bots_common_map_generation();
	approach.x = stand->x + (theirs->x - stand->x) / 3.0f;
	approach.y = stand->y + (theirs->y - stand->y) / 3.0f;
	approach.z = stand->z + (theirs->z - stand->z) / 3.0f + 0.7f;
	stand_eye = *stand;
	stand_eye.z += 0.6f;
	for (angle_index = 0; angle_index < 16; angle_index++)
	{
		real angle = (real)angle_index * (2.0f * 3.14159265f / 16.0f);

		for (radius_index = 0; radius_index < (long)(sizeof(radii) / sizeof(radii[0])); radius_index++)
		{
			real_point3d probe, ground, eye;
			long surface;
			real score;

			probe.x = stand->x + (real)cos(angle) * radii[radius_index];
			probe.y = stand->y + (real)sin(angle) * radii[radius_index];
			probe.z = stand->z + 1.5f;
			surface = bots_common_surface_below(&probe, &ground);
			if (surface == NONE || !bots_common_surface_walkable(surface) || (real)fabs(ground.z - stand->z) > 1.5f)
				continue;
			eye = ground;
			eye.z += 0.6f;
			if (!bots_common_line_of_sight(NONE, &stand_eye, &eye, NONE))
				continue;
			score = bots_team_distance(&ground, theirs) * 0.2f;
			if (!bots_common_line_of_sight(NONE, &approach, &eye, NONE))
				score += 6.0f;
			/* (not on the stand's doorstep either) */
			score -= radii[radius_index] < 5.0f ? 1.0f : 0.0f;
			if (score > best_score)
			{
				best_score = score;
				team->hide = ground;
			}
		}
	}
}

static void bots_team_ctf_update(void)
{
	long now = game_time_get();
	long team_index;

	for (team_index = 0; team_index < 2; team_index++)
	{
		bots_team_globals.holder[team_index] = NONE;
		bots_team_globals.at_home[team_index] = TRUE;
		bots_team_globals.present[team_index] = game_engine_ctf_flag_state(team_index,
			&bots_team_globals.position[team_index], &bots_team_globals.holder[team_index],
			&bots_team_globals.at_home[team_index], &bots_team_globals.home[team_index]);
	}
	for (team_index = 0; team_index < 2; team_index++)
	{
		struct bots_team_ctf *team = &bots_team_globals.teams[team_index];
		long flag = 1 - team_index;
		long holder = bots_team_globals.holder[flag];
		struct player_datum *holder_player = holder != NONE ? player_try_and_get(holder) : NULL;
		real_point3d position;

		if (!bots_team_globals.present[flag] || !bots_team_globals.present[team_index])
		{
			team->carrier = NONE;
			team->lying_since_tick = NONE;
			team->last_holder = NONE;
			continue;
		}
		if (holder_player && holder_player->team_index == team_index)
		{
			team->carrier = holder;
			team->last_holder = holder;
			team->lying_since_tick = NONE;
		}
		else if (holder != NONE || bots_team_globals.at_home[flag])
		{
			/* (theirs back home, or an enemy holds it: no carrier) */
			team->carrier = NONE;
			team->last_holder = NONE;
			team->lying_since_tick = NONE;
		}
		else
		{
			/* lying: who let it go, while alive and near it, else the nearest bot */
			if (team->lying_since_tick == NONE)
				team->lying_since_tick = now;
			if (team->carrier == NONE)
				team->carrier = team->last_holder;
			if (team->carrier != NONE &&
				(!bots_team_player_position(team->carrier, team_index, &position) ||
					bots_team_distance(&position, &bots_team_globals.position[flag]) > BOTS_TEAM_CARRIER_LEASH))
			{
				team->carrier = NONE;
			}
			if (team->carrier == NONE)
				team->carrier = bots_team_nearest_bot(team_index, &bots_team_globals.position[flag], NONE, NULL);
			team->last_holder = team->carrier;
		}
		/* the hiding carrier's guard: the team's bot nearest the hiding place */
		if (team->carrier != NONE && bots_team_globals.home_to_score && !bots_team_globals.at_home[team_index])
		{
			if (!team->hide_found || team->hide_generation != bots_common_map_generation())
				bots_team_find_hide(team_index);
			if (now - team->escort_tick >= TICKS_PER_SECOND || team->escort == team->carrier)
			{
				team->escort_tick = now;
				team->escort = bots_team_nearest_bot(team_index, &team->hide, team->carrier, NULL);
			}
		}
		else
			team->escort = NONE;
	}
}

void bots_team_reset(void)
{
	long team_index;

	csmemset(&bots_team_globals, 0, sizeof(bots_team_globals));
	bots_team_globals.tick = NONE;
	for (team_index = 0; team_index < 2; team_index++)
	{
		bots_team_globals.teams[team_index].carrier = NONE;
		bots_team_globals.teams[team_index].lying_since_tick = NONE;
		bots_team_globals.teams[team_index].last_holder = NONE;
		bots_team_globals.teams[team_index].escort = NONE;
	}
}

void bots_team_update(void)
{
	long now = game_time_get();

	if (bots_team_globals.tick == now)
		return;
	bots_team_globals.tick = now;
	bots_team_globals.ctf = game_engine_running() &&
		game_engine_get_variant()->game_engine_index == game_engine_ctf;
	if (bots_team_globals.ctf)
	{
		bots_team_globals.home_to_score = game_engine_ctf_home_to_score();
		bots_team_ctf_update();
	}
}

/* the player's team, 0 or 1, NONE */
static long bots_team_of(long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	if (!player || (player->team_index != 0 && player->team_index != 1))
		return NONE;
	return player->team_index;
}

long bots_team_ctf_carrier(long team_index)
{
	if (!bots_team_globals.ctf || (team_index != 0 && team_index != 1))
		return NONE;
	return bots_team_globals.teams[team_index].carrier;
}

short bots_team_ctf_carry(long player_index, real_point3d *point)
{
	long team_index = bots_team_of(player_index);
	struct bots_team_ctf *team;

	if (!bots_team_globals.ctf || team_index == NONE)
		return _bots_carry_none;
	team = &bots_team_globals.teams[team_index];
	if (team->carrier != player_index)
		return _bots_carry_none;
	if (bots_team_globals.holder[1 - team_index] != player_index)
	{
		*point = bots_team_globals.position[1 - team_index];
		return _bots_carry_regrab;
	}
	if (!bots_team_globals.home_to_score || bots_team_globals.at_home[team_index])
	{
		*point = bots_team_globals.home[team_index];
		return _bots_carry_home;
	}
	if (!team->hide_found || team->hide_generation != bots_common_map_generation())
		bots_team_find_hide(team_index);
	*point = team->hide;
	return _bots_carry_hide;
}

boolean bots_team_ctf_drop(long player_index, boolean enemy_visible, real enemy_distance)
{
	long team_index = bots_team_of(player_index);
	real_point3d point, position;

	if (!enemy_visible || enemy_distance > 20.0f || team_index == NONE)
		return FALSE;
	if (bots_team_ctf_carry(player_index, &point) != _bots_carry_hide)
		return FALSE;
	/* (settled in its hiding place, or nearly) */
	return bots_team_player_position(player_index, team_index, &position) &&
		bots_team_distance(&position, &point) < 4.0f;
}

boolean bots_team_ctf_regrab_now(long player_index, boolean fighting)
{
	long team_index = bots_team_of(player_index);
	struct bots_team_ctf *team;
	real_point3d point;

	if (team_index == NONE || bots_team_ctf_carry(player_index, &point) != _bots_carry_regrab)
		return FALSE;
	team = &bots_team_globals.teams[team_index];
	if (!fighting)
		return TRUE;
	return team->lying_since_tick != NONE &&
		game_time_get() - team->lying_since_tick >= BOTS_TEAM_REGRAB_SECONDS * TICKS_PER_SECOND;
}

boolean bots_team_ctf_flag_left(long player_index)
{
	long team_index = bots_team_of(player_index);
	struct bots_team_ctf *team;

	if (!bots_team_globals.ctf || team_index == NONE)
		return FALSE;
	team = &bots_team_globals.teams[team_index];
	return team->carrier != NONE && team->carrier != player_index &&
		bots_team_globals.holder[1 - team_index] == NONE && !bots_team_globals.at_home[1 - team_index];
}

boolean bots_team_ctf_escort(long player_index, real_point3d *point)
{
	long team_index = bots_team_of(player_index);
	struct bots_team_ctf *team;
	real_point3d position;

	if (!bots_team_globals.ctf || team_index == NONE)
		return FALSE;
	team = &bots_team_globals.teams[team_index];
	if (team->escort != player_index || team->carrier == NONE)
		return FALSE;
	/* (by the carrier where it is, else its hiding place) */
	if (bots_team_player_position(team->carrier, team_index, &position) &&
		bots_team_distance(&position, &team->hide) < 6.0f)
	{
		*point = position;
	}
	else
		*point = team->hide;
	return TRUE;
}
