/* bots_nav.h: the ground the bots' searches walk, mended and mapped (port/linux/game/bots_nav.c)

The level's walkable surfaces (the pathfinding surfaces tool builds with the
structure BSP, a byte a collision surface) were made for the campaign's AI,
and the multiplayer maps' leave out much a player walks: ramps steeper than
the AI's limit, the invisible ramps laid over stairs (Wizard's, Prisoner's,
Longest's, Derelict's upper floors), so that a search from below never
reaches an upper floor. bots_nav_open_ramps opens, in the bots' own copies,
the surfaces a biped stands on (no steeper than its slope limit, upright, not
a ladder nor glass) that join the walkable ground edge to edge, those joined
to them and so on; a vehicle's copy takes the flatter of them alone.

bots_nav_components numbers the ground's pieces (surfaces joined edge to
edge, walkable), and bots_nav_route finds, for a goal on another piece, the
first of the ladders and teleporters (portals: entered on one piece, left on
another) that lead there, the shortest way by straight lines. */
#ifndef HALO_BOTS_NAV_H
#define HALO_BOTS_NAV_H

struct collision_bsp;

struct bots_nav_portal
{
	long entrance_surface_index;
	long exit_surface_index;
	real_point3d entrance;
	real_point3d exit;
	/* (left out: a ladder no bot gets up) */
	boolean usable;
};

/* the surfaces opened in the copies (each `walkable_bit` of `walkable` and
`vehicle_walkable`, `count` of them): the biped's walkable no steeper than
`minimum_k` (its ground normal's least upward part), the vehicles' no steeper
than `vehicle_minimum_k`; the vehicles' count in *vehicle_opened */
long bots_nav_open_ramps(struct collision_bsp const *bsp, byte *walkable, byte *vehicle_walkable, long count,
	short walkable_bit, real minimum_k, real vehicle_minimum_k, long *vehicle_opened);

/* each surface's piece (NONE for one not walkable) into `component`; the
pieces' count */
long bots_nav_components(struct collision_bsp const *bsp, byte const *walkable, long count, short walkable_bit,
	short *component);

/* the first portal on the way from the start's surface to the goal's,
on different pieces (NONE: the same piece, or no way through portals) */
long bots_nav_route(short const *component, long count, struct bots_nav_portal const *portals, long portal_count,
	long start_surface_index, real_point3d const *start, long goal_surface_index, real_point3d const *goal);

#endif
