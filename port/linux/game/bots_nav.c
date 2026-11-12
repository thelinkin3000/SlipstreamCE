/*
BOTS_NAV.C

(port) The ground the bots' searches walk, mended and mapped (bots_nav.h).
*/

#include "cseries.h"
#include "tag_files/tag_groups.h"
#include "physics/collision_bsp_definitions.h"

#include "bots_nav.h"

#include <math.h>
#include <stdlib.h>

/* collision_surface's flags (collision_bsp.c's) */
#define BOTS_NAV_TWO_SIDED_BIT 0
#define BOTS_NAV_CLIMBABLE_BIT 2
#define BOTS_NAV_BREAKABLE_BIT 3
#define BOTS_NAV_MAXIMUM_NEIGHBOURS 16

/* the surfaces across a surface's edges */
static long bots_nav_neighbours(struct collision_bsp const *bsp, long surface_index, long *neighbours)
{
	struct collision_surface const *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index,
		struct collision_surface);
	long edge_index = surface->first_edge_index, count = 0, steps = 0;

	do
	{
		struct collision_edge const *edge;
		boolean right;
		long other;

		if (edge_index < 0 || edge_index >= bsp->edges.count)
			break;
		edge = TAG_BLOCK_GET_ELEMENT(&bsp->edges, edge_index, struct collision_edge);
		right = edge->surface_indices[1] == surface_index;
		other = edge->surface_indices[!right];
		if (other != NONE && other >= 0 && other < bsp->surfaces.count && count < BOTS_NAV_MAXIMUM_NEIGHBOURS)
			neighbours[count++] = other;
		edge_index = edge->edge_indices[right];
	} while (edge_index != surface->first_edge_index && ++steps < 64);
	return count;
}

/* how upright the surface's face is (its normal's upward part; a two-sided
one either face's) */
static real bots_nav_surface_k(struct collision_bsp const *bsp, long surface_index)
{
	struct collision_surface const *surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, surface_index,
		struct collision_surface);
	long plane_index = surface->plane_designator & LONG_MAX;
	real k;

	if (plane_index < 0 || plane_index >= bsp->bsp3d.planes.count)
		return -1.0f;
	k = TAG_BLOCK_GET_ELEMENT(&bsp->bsp3d.planes, plane_index, real_plane3d)->n.k;
	if (surface->plane_designator < 0)
		k = -k;
	if (TEST_FLAG(surface->flags, BOTS_NAV_TWO_SIDED_BIT))
		k = (real)fabs(k);
	return k;
}

long bots_nav_open_ramps(struct collision_bsp const *bsp, byte *walkable, byte *vehicle_walkable, long count,
	short walkable_bit, real minimum_k, real vehicle_minimum_k, long *vehicle_opened)
{
	long *queue;
	long head = 0, tail = 0, opened = 0, surface_index;

	*vehicle_opened = 0;
	if (!bsp || count <= 0)
		return 0;
	count = MIN(count, bsp->surfaces.count);
	queue = (long *)malloc((size_t)count * sizeof(long));
	if (!queue)
		return 0;
	for (surface_index = 0; surface_index < count; surface_index++)
	{
		if (TEST_FLAG(walkable[surface_index], walkable_bit))
			queue[tail++] = surface_index;
	}
	while (head < tail)
	{
		long neighbours[BOTS_NAV_MAXIMUM_NEIGHBOURS];
		long neighbour_count = bots_nav_neighbours(bsp, queue[head++], neighbours), index;

		for (index = 0; index < neighbour_count; index++)
		{
			long other = neighbours[index];
			struct collision_surface const *surface;
			real k;

			if (other >= count || TEST_FLAG(walkable[other], walkable_bit))
				continue;
			surface = TAG_BLOCK_GET_ELEMENT(&bsp->surfaces, other, struct collision_surface);
			if (TEST_FLAG(surface->flags, BOTS_NAV_CLIMBABLE_BIT) || TEST_FLAG(surface->flags, BOTS_NAV_BREAKABLE_BIT))
				continue;
			k = bots_nav_surface_k(bsp, other);
			if (k < minimum_k)
				continue;
			walkable[other] |= (byte)FLAG(walkable_bit);
			opened++;
			if (vehicle_walkable && k >= vehicle_minimum_k)
			{
				vehicle_walkable[other] |= (byte)FLAG(walkable_bit);
				(*vehicle_opened)++;
			}
			if (tail < count)
				queue[tail++] = other;
		}
	}
	free(queue);
	return opened;
}

long bots_nav_components(struct collision_bsp const *bsp, byte const *walkable, long count, short walkable_bit,
	short *component)
{
	long *queue;
	long pieces = 0, surface_index;

	for (surface_index = 0; surface_index < count; surface_index++)
		component[surface_index] = NONE;
	if (!bsp || count <= 0)
		return 0;
	queue = (long *)malloc((size_t)count * sizeof(long));
	if (!queue)
		return 0;
	for (surface_index = 0; surface_index < count && surface_index < bsp->surfaces.count; surface_index++)
	{
		long head = 0, tail = 0;

		if (component[surface_index] != NONE || !TEST_FLAG(walkable[surface_index], walkable_bit))
			continue;
		/* (more pieces than a short holds: the rest one piece, which keeps
		every way through portals open) */
		component[surface_index] = (short)MIN(pieces, 32766);
		queue[tail++] = surface_index;
		while (head < tail)
		{
			long neighbours[BOTS_NAV_MAXIMUM_NEIGHBOURS];
			long neighbour_count = bots_nav_neighbours(bsp, queue[head++], neighbours), index;

			for (index = 0; index < neighbour_count; index++)
			{
				long other = neighbours[index];

				if (other >= count || component[other] != NONE || !TEST_FLAG(walkable[other], walkable_bit))
					continue;
				component[other] = component[surface_index];
				if (tail < count)
					queue[tail++] = other;
			}
		}
		pieces++;
	}
	free(queue);
	return pieces;
}

static short bots_nav_component_of(short const *component, long count, long surface_index)
{
	return surface_index >= 0 && surface_index < count ? component[surface_index] : NONE;
}

static real bots_nav_distance(real_point3d const *a, real_point3d const *b)
{
	real dx = a->x - b->x, dy = a->y - b->y, dz = a->z - b->z;

	return (real)sqrt(dx * dx + dy * dy + dz * dz);
}

#define BOTS_NAV_MAXIMUM_PORTALS 64

long bots_nav_route(short const *component, long count, struct bots_nav_portal const *portals, long portal_count,
	long start_surface_index, real_point3d const *start, long goal_surface_index, real_point3d const *goal)
{
	real cost[BOTS_NAV_MAXIMUM_PORTALS];
	long first[BOTS_NAV_MAXIMUM_PORTALS];
	boolean done[BOTS_NAV_MAXIMUM_PORTALS];
	short start_piece = bots_nav_component_of(component, count, start_surface_index);
	short goal_piece = bots_nav_component_of(component, count, goal_surface_index);
	real best_cost = 1e30f;
	long best_first = NONE, index;

	if (!component || start_piece == NONE || goal_piece == NONE || start_piece == goal_piece)
		return NONE;
	portal_count = MIN(portal_count, BOTS_NAV_MAXIMUM_PORTALS);
	for (index = 0; index < portal_count; index++)
	{
		cost[index] = 1e30f;
		first[index] = NONE;
		done[index] = FALSE;
		if (portals[index].usable &&
			bots_nav_component_of(component, count, portals[index].entrance_surface_index) == start_piece &&
			bots_nav_component_of(component, count, portals[index].exit_surface_index) != NONE &&
			bots_nav_component_of(component, count, portals[index].exit_surface_index) != start_piece)
		{
			cost[index] = bots_nav_distance(start, &portals[index].entrance);
			first[index] = index;
		}
	}
	/* (Dijkstra's over the portals: from one's exit to another's entrance on
	the same piece, by straight lines) */
	for (;;)
	{
		long current = NONE, next;
		short exit_piece;

		for (index = 0; index < portal_count; index++)
		{
			if (!done[index] && cost[index] < 1e29f && (current == NONE || cost[index] < cost[current]))
				current = index;
		}
		if (current == NONE || cost[current] >= best_cost)
			break;
		done[current] = TRUE;
		exit_piece = bots_nav_component_of(component, count, portals[current].exit_surface_index);
		if (exit_piece == goal_piece)
		{
			real total = cost[current] + bots_nav_distance(&portals[current].exit, goal);

			if (total < best_cost)
			{
				best_cost = total;
				best_first = first[current];
			}
			continue;
		}
		for (next = 0; next < portal_count; next++)
		{
			real through;

			if (done[next] || !portals[next].usable ||
				bots_nav_component_of(component, count, portals[next].entrance_surface_index) != exit_piece)
			{
				continue;
			}
			through = cost[current] + bots_nav_distance(&portals[current].exit, &portals[next].entrance);
			if (through < cost[next])
			{
				cost[next] = through;
				first[next] = first[current];
			}
		}
	}
	return best_first;
}
