/*
HOST_DASHBOARD.C

The host's screen (halo_host_dashboard.h), on every build but the web's:
hosting from the menus, the app is a dedicated server with no player of its
own (server_mode.h's halo_server_host_begin; the director that hosts the
rotation one game after another, port/linux/game/server_director.c), and
this screen takes the whole of it in place of the game. With no local
player, the game draws no view of its world (main.c's main_game_render:
its window is a non-player's), and this is drawn over everything else, the
lobby's screens and the menus too, which get none of the controllers.

A first version, for trying hosting on a device: the overlay menu's
"Host Server" begins it from the main menu, with the rotation server.rotation
says. It shows the server's state, the game being played (its map,
gametype, round of the rotation, time and team scores) and the players
(their scores and pings). Up and down choose a player, Y kicks the chosen
one, X ends the game and plays the rotation's next, and B (twice) stops
hosting and goes back to the main menu.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "interface/interface.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "halo_host_dashboard.h"
#include "halo_overlay_menu.h"
#include "server_director.h"

#include "../src/server_console.h" /* port: port/linux/src/server_console.c */
#include "../src/server_rotation.h" /* port: port/linux/src/server_rotation.c */

#include <stdio.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c, sdl_platform.c) */
const char *config_string(const char *name);
long config_integer(const char *name);
unsigned long system_milliseconds(void);
void draw_quad(rectangle2d *rectangle, pixel32 color);
/* (main.c's) */
boolean main_menu_scenario_is_loaded(void);

#if HALO_HOST_BUILD

enum
{
	HOST_PORTS = 4,
	/* the margins and the room around a row's text (the screen's 480
	lines), and the players' rows' gap */
	HOST_MARGIN = 24,
	HOST_PADDING = 6,
	HOST_ROW_GAP = 2,
	/* the second B's time to stop hosting */
	HOST_CONFIRM_MILLISECONDS = 3000,
	/* what the last command did, shown this long */
	HOST_STATUS_MILLISECONDS = 3000,
	HOST_REPEAT_DELAY_MILLISECONDS = 400,
	HOST_REPEAT_MILLISECONDS = 120,
	MAXIMUM_HOST_ROWS = HALO_PORT_MAXIMUM_NETWORK_PLAYERS,
	_host_text_left = 0,
	_host_text_right = 1,
	_host_text_centre = 2,
};

static struct
{
	unsigned long port_buttons[HOST_PORTS];
	unsigned long previous_buttons;
	unsigned long repeat_at;
	/* the players' slots, as last drawn, and the chosen one's (NONE: none) */
	long slots[MAXIMUM_HOST_ROWS];
	long slot_count;
	long chosen_slot;
	/* B pressed once: until when a second stops hosting */
	unsigned long confirm_until;
	char status[96];
	unsigned long status_until;
	boolean logged_font;
} host;

static void host_set_status(
	char const *text)
{
	snprintf(host.status, sizeof(host.status), "%s", text);
	host.status_until = system_milliseconds() + HOST_STATUS_MILLISECONDS;
}

/* a command of the server's, as its owner (the console's) */
static void host_run(
	char const *line,
	char const *done)
{
	boolean succeeded = FALSE;
	char *output = server_commands_run(line, _server_role_owner, _server_origin_standard_input, &succeeded);

	if (output)
		server_console_free(output);
	host_set_status(succeeded ? done : "not done");
	error(_error_silent, "host: %s (%s)", line, succeeded ? "done" : "not done");
}

int host_dashboard_start(
	char *error_text,
	size_t error_size)
{
	error_text[0] = 0;
	if (halo_server_hosting())
	{
		snprintf(error_text, error_size, "already hosting");
		return FALSE;
	}
	if (global_network_game_server_get() || global_network_game_client_get())
	{
		snprintf(error_text, error_size, "leave the network game first");
		return FALSE;
	}
	if (!main_menu_scenario_is_loaded())
	{
		snprintf(error_text, error_size, "only from the main menu");
		return FALSE;
	}
	if (!server_director_host_begin())
	{
		snprintf(error_text, error_size, "could not host");
		return FALSE;
	}
	memset(&host, 0, sizeof(host));
	host.chosen_slot = NONE;
	return TRUE;
}

unsigned char host_dashboard_take_pad(
	short port,
	unsigned long buttons)
{
	if (!halo_server_hosting())
		return FALSE;
	if (port >= 0 && port < HOST_PORTS)
		host.port_buttons[port] = buttons;
	return TRUE;
}

/* the chosen player moved by a row */
static void host_move(
	long by)
{
	long index;
	long row = NONE;

	if (host.slot_count <= 0)
	{
		host.chosen_slot = NONE;
		return;
	}
	for (index = 0; index < host.slot_count; index++)
	{
		if (host.slots[index] == host.chosen_slot)
			row = index;
	}
	row = row == NONE ? 0 : PIN(row + by, 0, host.slot_count - 1);
	host.chosen_slot = host.slots[row];
}

void host_dashboard_update(
	void)
{
	enum
	{
		DIRECTIONS = (1UL << HALO_OVERLAY_PAD_UP) | (1UL << HALO_OVERLAY_PAD_DOWN),
	};
	unsigned long buttons = 0;
	unsigned long pressed;
	unsigned long now = system_milliseconds();
	short port;

	if (!halo_server_hosting() || overlay_menu_open())
	{
		host.previous_buttons = (unsigned long)-1;
		return;
	}
	for (port = 0; port < HOST_PORTS; port++)
		buttons |= host.port_buttons[port];
	/* (a button held as the screen opened, or as the overlay closed, is
	no press) */
	if (host.previous_buttons == (unsigned long)-1)
		host.previous_buttons = buttons;
	pressed = buttons & ~host.previous_buttons;
	host.previous_buttons = buttons;
	if (pressed & DIRECTIONS)
	{
		host.repeat_at = now + HOST_REPEAT_DELAY_MILLISECONDS;
	}
	else if ((buttons & DIRECTIONS) && host.repeat_at && (long)(now - host.repeat_at) >= 0)
	{
		pressed |= buttons & DIRECTIONS;
		host.repeat_at = now + HOST_REPEAT_MILLISECONDS;
	}
	if (pressed & (1UL << HALO_OVERLAY_PAD_UP))
		host_move(-1);
	if (pressed & (1UL << HALO_OVERLAY_PAD_DOWN))
		host_move(1);
	if (pressed & (1UL << HALO_OVERLAY_PAD_X))
	{
		if (server_director_state() == _server_state_playing)
			host_run("sv_map_next", "next round");
		else
			host_set_status("no game to end");
	}
	if (pressed & (1UL << HALO_OVERLAY_PAD_Y))
	{
		char name[16];
		char line[64];
		long score, ping;

		if (host.chosen_slot != NONE &&
			server_status_player_row(host.chosen_slot, name, sizeof(name), &score, &ping))
		{
			char done[64];

			snprintf(line, sizeof(line), "kick %ld", host.chosen_slot);
			snprintf(done, sizeof(done), "%s kicked", name);
			host_run(line, done);
		}
		else
		{
			host_set_status("no player chosen");
		}
	}
	if (pressed & (1UL << HALO_OVERLAY_PAD_B))
	{
		if (host.confirm_until && (long)(now - host.confirm_until) < 0)
		{
			host.confirm_until = 0;
			server_director_host_stop();
			host_set_status("stopping");
		}
		else
		{
			host.confirm_until = now + HOST_CONFIRM_MILLISECONDS;
		}
	}
}

/* ---------- drawing */

/* a font of the map's (the menus' ui\large_ui, ui\small_ui, which every map
has for its pause menu), else the console's; and its lines' height */
static boolean host_font(
	char const *tag_name,
	long *font_index,
	short *line_height)
{
	struct font_header *font;

	*font_index = tag_loaded(FONT_GROUP_TAG, tag_name);
	if (*font_index == NONE)
		*font_index = interface_get_tag_index(_interface_font_terminal);
	if (*font_index == NONE)
		return FALSE;
	font = font_definition_get(*font_index);
	*line_height = (short)(font->ascending_height + font->descending_height + font->leading_height);
	return *line_height > 0;
}

static void host_text(
	long font_index,
	short x0,
	short y0,
	short x1,
	short height,
	short justification,
	real_argb_color const *color,
	char const *text)
{
	rectangle2d bounds;

	bounds.x0 = x0;
	bounds.y0 = y0;
	bounds.x1 = x1;
	bounds.y1 = (short)(y0 + height);
	draw_string_set_draw_mode(font_index, NONE, justification, 0, color);
	rasterizer_draw_string(&bounds, &bounds, NULL, 0, text);
}

static void host_rule(
	short x0,
	short x1,
	short y,
	pixel32 color)
{
	rectangle2d rule;

	rule.x0 = x0;
	rule.x1 = x1;
	rule.y0 = y;
	rule.y1 = (short)(y + 1);
	draw_quad(&rule, color);
}

static char const *host_state_text(
	short state)
{
	switch (state)
	{
	case _server_state_lobby:
		return "LOBBY";
	case _server_state_loading:
		return "LOADING";
	case _server_state_playing:
		return "PLAYING";
	case _server_state_postgame:
		return "POSTGAME";
	case _server_state_stopping:
		return "STOPPING";
	default:
		return "STARTING";
	}
}

static void host_capitals(
	char *text)
{
	for (; *text; text++)
	{
		if (*text >= 'a' && *text <= 'z')
			*text = (char)(*text - 'a' + 'A');
	}
}

/* a label and its value on a line of the left panel */
static short host_field(
	long font_index,
	short line_height,
	short x0,
	short x1,
	short y,
	char const *label,
	char const *value,
	real_argb_color const *label_color,
	real_argb_color const *value_color)
{
	short middle = (short)(x0 + (x1 - x0) * 2 / 5);

	host_text(font_index, x0, y, middle, line_height, _host_text_left, label_color, label);
	host_text(font_index, middle, y, x1, line_height, _host_text_left, value_color, value);
	return (short)(y + line_height + HOST_PADDING);
}

void host_dashboard_draw(
	void)
{
	/* the menus' colours, as the overlay's (overlay_menu.c): their blue
	(#2896FF), white, the rows' dark bands and the chosen one's outline */
	static real_argb_color const blue = { 1.0f, 0x28 / 255.0f, 0x96 / 255.0f, 1.0f };
	static real_argb_color const grey = { 1.0f, 0.6f, 0.65f, 0.7f };
	static real_argb_color const red = { 1.0f, 1.0f, 0.35f, 0.35f };
	static real_argb_color const green = { 1.0f, 0.4f, 0.9f, 0.45f };
	static pixel32 const BACKGROUND = 0xFF000A16UL;
	static pixel32 const PANEL = 0xFF001226UL;
	static pixel32 const BAND = 0xFF001A33UL;
	static pixel32 const CHOSEN_BAND = 0xFF002850UL;
	static pixel32 const OUTLINE = 0xFF2299FFUL;
	static pixel32 const RULE = 0xC02896FFUL;
	real_argb_color white = get_ui_argb_white();
	struct server_entry const *current;
	struct server_rotation const *rotation;
	long font_index, small_font_index;
	short line_height, small_line_height;
	short width, height, y, middle, column;
	unsigned long now = system_milliseconds();
	short state;
	rectangle2d rectangle;
	char text[128];

	if (!halo_server_hosting())
		return;
	if (!host_font("ui\\large_ui", &font_index, &line_height) ||
		!host_font("ui\\small_ui", &small_font_index, &small_line_height))
	{
		if (!host.logged_font)
		{
			host.logged_font = TRUE;
			error(_error_silent, "host screen: not drawn: no font");
		}
		return;
	}
	width = (short)(render.camera.viewport_bounds.x1 - render.camera.viewport_bounds.x0);
	height = (short)(render.camera.viewport_bounds.y1 - render.camera.viewport_bounds.y0);
	if (width <= 0 || height <= 0)
		return;
	state = server_director_state();
	current = server_director_current();
	rotation = server_director_rotation();

	/* the whole screen, the game's under it unseen */
	rectangle.x0 = 0;
	rectangle.y0 = 0;
	rectangle.x1 = width;
	rectangle.y1 = height;
	draw_quad(&rectangle, BACKGROUND);

	/* the title: the server's name and its state */
	y = HOST_MARGIN;
	snprintf(text, sizeof(text), "HOST SERVER: %s", config_string("server.name"));
	host_capitals(text);
	host_text(font_index, HOST_MARGIN, y, (short)(width - HOST_MARGIN), line_height, _host_text_left, &blue, text);
	host_text(font_index, HOST_MARGIN, y, (short)(width - HOST_MARGIN), line_height, _host_text_right,
		state == _server_state_stopping ? &red : &green, host_state_text(state));
	y = (short)(y + line_height + HOST_PADDING);
	host_rule(HOST_MARGIN, (short)(width - HOST_MARGIN), y, RULE);
	y = (short)(y + 2 * HOST_PADDING);

	/* the two panels: the game at the left, the players at the right */
	middle = (short)(width * 9 / 20);
	rectangle.x0 = HOST_MARGIN;
	rectangle.x1 = (short)(middle - HOST_MARGIN / 2);
	rectangle.y0 = y;
	rectangle.y1 = (short)(height - HOST_MARGIN - 2 * small_line_height - 2 * HOST_PADDING);
	draw_quad(&rectangle, PANEL);
	rectangle.x0 = (short)(middle + HOST_MARGIN / 2);
	rectangle.x1 = (short)(width - HOST_MARGIN);
	draw_quad(&rectangle, PANEL);

	/* the game */
	{
		short x0 = (short)(HOST_MARGIN + HOST_PADDING * 2);
		short x1 = (short)(middle - HOST_MARGIN / 2 - HOST_PADDING * 2);
		short line = (short)(y + HOST_PADDING * 2);
		char value[96];
		long red_score, blue_score;

		host_text(font_index, x0, line, x1, line_height, _host_text_left, &blue, "CURRENT MATCH");
		line = (short)(line + line_height + 2 * HOST_PADDING);
		if (current && current->map_name[0])
		{
			snprintf(value, sizeof(value), "%s", current->map_name);
			host_capitals(value);
			line = host_field(small_font_index, small_line_height, x0, x1, line, "MAP", value, &grey, &white);
			snprintf(value, sizeof(value), "%s", current->gametype);
			host_capitals(value);
			line = host_field(small_font_index, small_line_height, x0, x1, line, "GAME TYPE", value, &grey, &white);
		}
		else
		{
			line = host_field(small_font_index, small_line_height, x0, x1, line, "MAP", "-", &grey, &white);
		}
		if (rotation && rotation->count > 0)
		{
			long index = server_director_index();

			if (index >= 0)
				snprintf(value, sizeof(value), "%ld OF %d", index + 1, rotation->count);
			else
				snprintf(value, sizeof(value), "- OF %d", rotation->count);
			line = host_field(small_font_index, small_line_height, x0, x1, line, "ROUND", value, &grey, &white);
		}
		if (state == _server_state_playing)
		{
			long seconds = (long)server_director_game_seconds();

			snprintf(value, sizeof(value), "%02ld:%02ld", seconds / 60, seconds % 60);
			line = host_field(small_font_index, small_line_height, x0, x1, line, "TIME", value, &grey, &white);
			if (server_status_team_scores(&red_score, &blue_score))
			{
				snprintf(value, sizeof(value), "RED %ld - BLUE %ld", red_score, blue_score);
				line = host_field(small_font_index, small_line_height, x0, x1, line, "SCORE", value, &grey, &white);
			}
		}
		else if (state == _server_state_lobby)
		{
			snprintf(value, sizeof(value), "%ld TO START", config_integer("server.min_players"));
			line = host_field(small_font_index, small_line_height, x0, x1, line, "PLAYERS", value, &grey, &white);
		}
		{
			struct server_entry const *next = server_director_next();

			if (next && next->map_name[0])
			{
				snprintf(value, sizeof(value), "%s / %s", next->map_name, next->gametype);
				host_capitals(value);
				line = host_field(small_font_index, small_line_height, x0, x1, line, "NEXT", value, &grey, &white);
			}
		}
	}

	/* the players: each on a band, the chosen one's outlined */
	{
		short x0 = (short)(middle + HOST_MARGIN / 2 + HOST_PADDING * 2);
		short x1 = (short)(width - HOST_MARGIN - HOST_PADDING * 2);
		short line = (short)(y + HOST_PADDING * 2);
		short row_height = (short)(small_line_height + 2 * HOST_PADDING);
		short bottom = (short)(rectangle.y1 - HOST_PADDING);
		long slot;
		boolean chosen_found = FALSE;

		host.slot_count = 0;
		for (slot = 0; slot < MAXIMUM_HOST_ROWS; slot++)
		{
			char name[16];
			long score, ping;

			if (server_status_player_row(slot, name, sizeof(name), &score, &ping))
			{
				host.slots[host.slot_count++] = slot;
				if (slot == host.chosen_slot)
					chosen_found = TRUE;
			}
		}
		if (!chosen_found)
			host.chosen_slot = host.slot_count > 0 ? host.slots[0] : NONE;
		snprintf(text, sizeof(text), "PLAYERS (%ld/%ld)", host.slot_count, config_integer("server.max_players"));
		host_text(font_index, x0, line, x1, line_height, _host_text_left, &blue, text);
		line = (short)(line + line_height + 2 * HOST_PADDING);
		if (host.slot_count == 0)
		{
			host_text(small_font_index, x0, line, x1, small_line_height, _host_text_left, &grey,
				"WAITING FOR PLAYERS TO JOIN");
		}
		for (column = 0; column < host.slot_count && line + row_height <= bottom; column++)
		{
			char name[16];
			char value[32];
			long score, ping;
			boolean chosen = host.slots[column] == host.chosen_slot;
			rectangle2d band;

			if (!server_status_player_row(host.slots[column], name, sizeof(name), &score, &ping))
				continue;
			band.x0 = (short)(x0 - HOST_PADDING);
			band.x1 = (short)(x1 + HOST_PADDING);
			band.y0 = line;
			band.y1 = (short)(line + row_height);
			if (chosen)
			{
				draw_quad(&band, OUTLINE);
				band.x0 += 2;
				band.x1 -= 2;
				band.y0 += 2;
				band.y1 -= 2;
				draw_quad(&band, CHOSEN_BAND);
			}
			else
			{
				draw_quad(&band, BAND);
			}
			host_text(small_font_index, x0, (short)(line + HOST_PADDING), x1, small_line_height, _host_text_left,
				chosen ? &white : &blue, name);
			if (ping == NONE)
				snprintf(value, sizeof(value), "%ld", score);
			else
				snprintf(value, sizeof(value), "%ld   %ld MS", score, ping);
			host_text(small_font_index, x0, (short)(line + HOST_PADDING), x1, small_line_height, _host_text_right,
				&white, value);
			line = (short)(line + row_height + HOST_ROW_GAP);
		}
	}

	/* the foot: what the last command did, or the second B's question,
	else the buttons */
	{
		short line = (short)(height - HOST_MARGIN - small_line_height);
		rectangle2d bounds;

		bounds.x0 = HOST_MARGIN;
		bounds.x1 = (short)(width - HOST_MARGIN);
		bounds.y0 = line;
		bounds.y1 = (short)(line + small_line_height);
		if (host.confirm_until && (long)(now - host.confirm_until) < 0)
		{
			draw_string_set_draw_mode(small_font_index, NONE, _host_text_centre, 0, &red);
			draw_string_and_hack_in_icons(&bounds, NULL, NULL, 0, L"Press %b-button again to stop hosting", FALSE);
		}
		else if (host.status[0] && (long)(now - host.status_until) < 0)
		{
			snprintf(text, sizeof(text), "%s", host.status);
			host_capitals(text);
			host_text(small_font_index, bounds.x0, bounds.y0, bounds.x1, small_line_height, _host_text_centre, &white,
				text);
		}
		else
		{
			draw_string_set_draw_mode(small_font_index, NONE, _host_text_centre, 0, &white);
			draw_string_and_hack_in_icons(&bounds, NULL, NULL, 0,
				L"%y-button Kick Player   %x-button Next Round   %b-button Stop Hosting", FALSE);
		}
	}
}

#else

int host_dashboard_start(char *error_text, size_t error_size)
{
	snprintf(error_text, error_size, "not in this build");
	return FALSE;
}

unsigned char host_dashboard_take_pad(short port, unsigned long buttons)
{
	return FALSE;
}

void host_dashboard_update(void)
{
}

void host_dashboard_draw(void)
{
}

#endif
