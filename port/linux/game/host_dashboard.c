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

Multiplayer's HOST SERVER begins it from the main menu with a playlist's
rotation (menu_functions.c's "port host" functions). It shows the server's state, the game being played (its map,
gametype, round of the rotation, time and team scores) and the players
(their scores and pings). Up and down choose a player, A opens what can be
done to them (kick, ban for a day or for good, mute or unmute), Y kicks
them at once, X ends the game and plays the rotation's next, START opens
the notices the host can show every player, and B (twice) stops hosting
and goes back to the main menu.
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

#include <math.h>
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
	/* a menu over the screen: none, a player's, or the notices */
	_host_menu_none = 0,
	_host_menu_player,
	_host_menu_notice,
	HOST_MENU_MAXIMUM_ROWS = 8,
};

/* a player's menu's rows: their label, and the command run on the player's
slot (%ld) */
static struct
{
	char const *label;
	char const *command;
	char const *done;
} const host_player_actions[] =
{
	{ "KICK", "kick %ld", "kicked" },
	{ "BAN FOR A DAY", "ban %ld 1d", "banned for a day" },
	{ "BAN", "ban %ld", "banned" },
	{ "MUTE", "mute %ld", "muted" },
	{ "UNMUTE", "unmute %ld", "unmuted" },
	{ "CANCEL", NULL, NULL },
};

/* the notices (say) */
static char const *const host_notices[] =
{
	"Welcome to the server!",
	"The next round starts soon.",
	"The server restarts after this round.",
	"Play fair, and have fun.",
	"The server is closing soon.",
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
	/* the menu open over the screen, its row chosen, and its player's slot */
	short menu;
	short menu_row;
	long menu_slot;
} host;

static void host_run(char const *line, char const *done);

/* the open menu's rows */
static short host_menu_rows(
	void)
{
	return host.menu == _host_menu_player ? (short)NUMBEROF(host_player_actions) :
		host.menu == _host_menu_notice ? (short)(NUMBEROF(host_notices) + 1) : 0;
}

/* the menu's row chosen: its command run, and the menu closed */
static void host_menu_choose(
	void)
{
	char line[192];
	char done[96];
	char name[16];
	long score, ping;

	if (host.menu == _host_menu_player && host.menu_row < (short)NUMBEROF(host_player_actions) &&
		host_player_actions[host.menu_row].command &&
		server_status_player_row(host.menu_slot, name, sizeof(name), &score, &ping))
	{
		snprintf(line, sizeof(line), host_player_actions[host.menu_row].command, host.menu_slot);
		snprintf(done, sizeof(done), "%s %s", name, host_player_actions[host.menu_row].done);
		host_run(line, done);
	}
	else if (host.menu == _host_menu_notice && host.menu_row < (short)NUMBEROF(host_notices))
	{
		snprintf(line, sizeof(line), "say \"%s\"", host_notices[host.menu_row]);
		host_run(line, "notice shown");
	}
	host.menu = _host_menu_none;
}

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
	char const *rotation,
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
	if (!server_director_host_begin(rotation, error_text, (long)error_size))
		return FALSE;
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
	/* (a menu open: its rows alone, B closing it) */
	if (host.menu != _host_menu_none)
	{
		short rows = host_menu_rows();

		if (pressed & (1UL << HALO_OVERLAY_PAD_UP))
			host.menu_row = (short)((host.menu_row + rows - 1) % rows);
		if (pressed & (1UL << HALO_OVERLAY_PAD_DOWN))
			host.menu_row = (short)((host.menu_row + 1) % rows);
		if (pressed & (1UL << HALO_OVERLAY_PAD_A))
			host_menu_choose();
		else if (pressed & ((1UL << HALO_OVERLAY_PAD_B) | (1UL << HALO_OVERLAY_PAD_START)))
			host.menu = _host_menu_none;
		return;
	}
	if (pressed & (1UL << HALO_OVERLAY_PAD_UP))
		host_move(-1);
	if (pressed & (1UL << HALO_OVERLAY_PAD_DOWN))
		host_move(1);
	if (pressed & (1UL << HALO_OVERLAY_PAD_A))
	{
		if (host.chosen_slot != NONE)
		{
			host.menu = _host_menu_player;
			host.menu_row = 0;
			host.menu_slot = host.chosen_slot;
		}
		else
		{
			host_set_status("no player chosen");
		}
	}
	if (pressed & (1UL << HALO_OVERLAY_PAD_START))
	{
		host.menu = _host_menu_notice;
		host.menu_row = 0;
	}
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

/* ---------- drawing: the menus' look (the Host Server screens', tools/
port_settings.py's host_pictures): a dark blue gradient, a title, two
columns of a tab over a rounded panel outlined in blue, rows on bands, and
the buttons along the foot */

#define HOST_COLOR(a, r, g, b) ((pixel32)(((unsigned long)(a) << 24) | ((unsigned long)(r) << 16) | \
	((unsigned long)(g) << 8) | (unsigned long)(b)))

static real_argb_color const host_blue = { 1.0f, 0x28 / 255.0f, 0x96 / 255.0f, 1.0f };
static real_argb_color const host_lavender = { 1.0f, 0x8C / 255.0f, 0xA8 / 255.0f, 0xE8 / 255.0f };
static real_argb_color const host_red = { 1.0f, 1.0f, 0.35f, 0.35f };
static real_argb_color const host_green = { 1.0f, 0.4f, 0.9f, 0.45f };

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

/* how wide the text is in the font */
static short host_text_width(
	long font_index,
	char const *text)
{
	static real_argb_color const white = { 1.0f, 1.0f, 1.0f, 1.0f };
	wchar_t wide[128];
	rectangle2d bounds, text_bounds, cursor;
	short index;

	for (index = 0; text[index] && index < (short)NUMBEROF(wide) - 1; index++)
		wide[index] = (wchar_t)(unsigned char)text[index];
	wide[index] = 0;
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 0x3FFF;
	bounds.y1 = 480;
	draw_string_set_draw_mode(font_index, NONE, _host_text_left, 0, &white);
	draw_unicode_string_compute_bounds(&bounds, wide, &text_bounds, &cursor);
	return (short)MAX(text_bounds.x1, cursor.x0);
}

/* a rounded rectangle, of strips a unit tall at its top and bottom
(draw_quad draws none other), none of them over another */
static void host_rounded(
	rectangle2d const *rectangle,
	short radius,
	pixel32 color)
{
	short height = (short)(rectangle->y1 - rectangle->y0);
	short row;
	rectangle2d strip;

	radius = (short)MIN(radius, height / 2);
	for (row = 0; row < radius; row++)
	{
		real dy = (real)(radius - row) - 0.5f;
		short inset = (short)((real)radius - (real)sqrt((real)(radius * radius) - dy * dy) + 0.5f);

		strip.x0 = (short)(rectangle->x0 + inset);
		strip.x1 = (short)(rectangle->x1 - inset);
		strip.y0 = (short)(rectangle->y0 + row);
		strip.y1 = (short)(strip.y0 + 1);
		draw_quad(&strip, color);
		strip.y0 = (short)(rectangle->y1 - row - 1);
		strip.y1 = (short)(strip.y0 + 1);
		draw_quad(&strip, color);
	}
	strip = *rectangle;
	strip.y0 = (short)(rectangle->y0 + radius);
	strip.y1 = (short)(rectangle->y1 - radius);
	if (strip.y1 > strip.y0)
		draw_quad(&strip, color);
}

/* a box: its blue outline (2 units) round its fill */
static void host_box(
	rectangle2d const *rectangle,
	short radius,
	pixel32 fill)
{
	rectangle2d inside = *rectangle;

	host_rounded(rectangle, radius, HOST_COLOR(0xFF, 0x28, 0x96, 0xFF));
	inside.x0 += 2;
	inside.y0 += 2;
	inside.x1 -= 2;
	inside.y1 -= 2;
	host_rounded(&inside, (short)MAX(radius - 2, 1), fill);
}

/* a small triangle pointing right, its middle at x, y */
static void host_arrow(
	short x,
	short y,
	short size,
	pixel32 color)
{
	short column;
	rectangle2d strip;

	for (column = 0; column < size; column++)
	{
		strip.x0 = (short)(x + column);
		strip.x1 = (short)(strip.x0 + 1);
		strip.y0 = (short)(y - (size - column));
		strip.y1 = (short)(y + (size - column));
		draw_quad(&strip, color);
	}
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

/* a map's name as the menus show it (BLOOD GULCH), and a gametype's (TEAM
SLAYER, CAPTURE THE FLAG) */
static void host_map_name(
	char const *map,
	char *text,
	size_t size)
{
	char const *name = strrchr(map, '\\');

	snprintf(text, size, "%s", server_map_title(name ? name + 1 : map));
	host_capitals(text);
}

static void host_gametype_name(
	char const *gametype,
	char *text,
	size_t size)
{
	size_t index;

	if (!strcmp(gametype, "ctf"))
	{
		snprintf(text, size, "CAPTURE THE FLAG");
		return;
	}
	if (!strcmp(gametype, "king") || !strcmp(gametype, "koth"))
	{
		snprintf(text, size, "KING OF THE HILL");
		return;
	}
	for (index = 0; gametype[index] && gametype[index] != '+' && index + 1 < size; index++)
		text[index] = gametype[index] == '_' ? ' ' : gametype[index];
	text[index] = 0;
	host_capitals(text);
}

/* a column: its tab with its title, and its panel; the panel's inside */
static void host_column(
	short x0,
	short x1,
	short y0,
	short y1,
	long font_index,
	short line_height,
	char const *title,
	rectangle2d *inside)
{
	rectangle2d tab;
	rectangle2d panel;
	short tab_height = (short)(line_height + 12);

	tab.x0 = x0;
	tab.x1 = x1;
	tab.y0 = y0;
	tab.y1 = (short)(y0 + tab_height);
	host_box(&tab, 10, HOST_COLOR(0xE6, 0x08, 0x1C, 0x38));
	{
		real_argb_color white = get_ui_argb_white();

		host_text(font_index, x0, (short)(y0 + 6), x1, line_height, _host_text_centre, &white, title);
	}
	panel.x0 = x0;
	panel.x1 = x1;
	panel.y0 = (short)(tab.y1 + 6);
	panel.y1 = y1;
	host_box(&panel, 12, HOST_COLOR(0xC8, 0x08, 0x1A, 0x34));
	inside->x0 = (short)(panel.x0 + HOST_PADDING * 3);
	inside->x1 = (short)(panel.x1 - HOST_PADDING * 3);
	inside->y0 = (short)(panel.y0 + HOST_PADDING * 2);
	inside->y1 = (short)(panel.y1 - HOST_PADDING * 2);
}

/* a label and its value on a line of the match's panel */
static short host_field(
	long font_index,
	short line_height,
	short x0,
	short x1,
	short y,
	char const *label,
	char const *value)
{
	real_argb_color white = get_ui_argb_white();
	short middle = (short)(x0 + (x1 - x0) * 2 / 5);

	host_text(font_index, x0, y, middle, line_height, _host_text_left, &host_lavender, label);
	host_text(font_index, middle, y, x1, line_height, _host_text_left, &white, value);
	return (short)(y + line_height + HOST_PADDING);
}

/* the buttons along the foot, the last ending at right: each its picture
and its words, left-justified (the picture stays beside them) */
static void host_hints(
	long font_index,
	short line_height,
	short right,
	short y,
	wchar_t const *const *hints,
	char const *const *words,
	short count)
{
	real_argb_color white = get_ui_argb_white();
	short widths[8];
	short total = 0;
	short index;
	short x;

	for (index = 0; index < count && index < (short)NUMBEROF(widths); index++)
	{
		/* (the picture is about a line high, and a space after it) */
		widths[index] = (short)(line_height + 6 + host_text_width(font_index, words[index]));
		total = (short)(total + widths[index] + (index ? 18 : 0));
	}
	x = (short)(right - total);
	for (index = 0; index < count && index < (short)NUMBEROF(widths); index++)
	{
		rectangle2d bounds;

		bounds.x0 = x;
		bounds.x1 = (short)(x + widths[index] + 12);
		bounds.y0 = y;
		bounds.y1 = (short)(y + line_height);
		draw_string_set_draw_mode(font_index, NONE, _host_text_left, 0, &white);
		draw_string_and_hack_in_icons(&bounds, NULL, NULL, 0, hints[index], FALSE);
		x = (short)(x + widths[index] + 18);
	}
}

void host_dashboard_draw(
	void)
{
	static pixel32 const BAND = HOST_COLOR(0x38, 0x28, 0x96, 0xFF);
	static pixel32 const CHOSEN_BAND = HOST_COLOR(0x70, 0x28, 0x96, 0xFF);
	static wchar_t const *const hints[] = { L"%a-button Player", L"%y-button Kick", L"%x-button Next Round",
		L"%start-button Notices", L"%b-button Stop Hosting" };
	static char const *const hint_words[] = { "Player", "Kick", "Next Round", "Notices", "Stop Hosting" };
	real_argb_color white = get_ui_argb_white();
	struct server_entry const *current;
	struct server_rotation const *rotation;
	long font_index, small_font_index;
	short line_height, small_line_height;
	short width, height, y, middle, column, margin, foot;
	unsigned long now = system_milliseconds();
	short state;
	rectangle2d rectangle, inside;
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
	margin = (short)MAX(HOST_MARGIN, width / 16);

	/* the whole screen, the game's under it unseen: the menus' gradient,
	in bands from their blue down to their dark */
	for (y = 0; y < height; y = (short)(y + 8))
	{
		long t = (long)y * 255 / height;

		rectangle.x0 = 0;
		rectangle.x1 = width;
		rectangle.y0 = y;
		rectangle.y1 = (short)MIN(height, y + 8);
		draw_quad(&rectangle, HOST_COLOR(0xFF, 0x0A - (0x0A - 0x03) * t / 255, 0x26 - (0x26 - 0x08) * t / 255,
			0x49 - (0x49 - 0x14) * t / 255));
	}

	/* the title, its server's name and state under it */
	y = (short)(height / 24);
	host_text(font_index, 0, y, width, line_height, _host_text_centre, &host_blue, "HOST SERVER");
	y = (short)(y + line_height + 2);
	snprintf(text, sizeof(text), "%s", config_string("server.name"));
	host_capitals(text);
	host_text(small_font_index, margin, y, width - margin, small_line_height, _host_text_left, &host_lavender, text);
	host_text(small_font_index, margin, y, width - margin, small_line_height, _host_text_right,
		state == _server_state_stopping ? &host_red : &host_green, host_state_text(state));
	y = (short)(y + small_line_height + HOST_PADDING * 2);

	/* the two columns: the match at the left, the players at the right */
	foot = (short)(height - margin / 2 - small_line_height);
	middle = (short)(width / 2);
	host_column(margin, (short)(middle - margin / 4), y, (short)(foot - HOST_PADDING * 3), font_index, line_height,
		"CURRENT MATCH", &inside);
	{
		short line = inside.y0;
		char value[96];
		long red_score, blue_score;

		if (current && current->map_name[0])
		{
			host_map_name(current->map_name, value, sizeof(value));
			line = host_field(small_font_index, small_line_height, inside.x0, inside.x1, line, "MAP", value);
			host_gametype_name(current->gametype, value, sizeof(value));
			line = host_field(small_font_index, small_line_height, inside.x0, inside.x1, line, "GAME TYPE", value);
		}
		else
		{
			line = host_field(small_font_index, small_line_height, inside.x0, inside.x1, line, "MAP", "-");
		}
		if (rotation && rotation->count > 0)
		{
			long index = server_director_index();

			if (index >= 0)
				snprintf(value, sizeof(value), "%ld OF %d", index + 1, rotation->count);
			else
				snprintf(value, sizeof(value), "- OF %d", rotation->count);
			line = host_field(small_font_index, small_line_height, inside.x0, inside.x1, line, "ROUND", value);
		}
		if (state == _server_state_playing)
		{
			long seconds = (long)server_director_game_seconds();

			snprintf(value, sizeof(value), "%02ld:%02ld", seconds / 60, seconds % 60);
			line = host_field(small_font_index, small_line_height, inside.x0, inside.x1, line, "TIME", value);
			if (server_status_team_scores(&red_score, &blue_score))
			{
				snprintf(value, sizeof(value), "RED %ld - BLUE %ld", red_score, blue_score);
				line = host_field(small_font_index, small_line_height, inside.x0, inside.x1, line, "SCORE", value);
			}
		}
		else if (state == _server_state_lobby)
		{
			snprintf(value, sizeof(value), "%ld TO START", config_integer("server.min_players"));
			line = host_field(small_font_index, small_line_height, inside.x0, inside.x1, line, "PLAYERS", value);
		}
		{
			struct server_entry const *next = server_director_next();

			if (next && next->map_name[0])
			{
				char map[48];
				char gametype[48];

				line = (short)(line + HOST_PADDING * 2);
				host_text(small_font_index, inside.x0, line, inside.x1, small_line_height, _host_text_left,
					&host_lavender, "NEXT ROUND");
				line = (short)(line + small_line_height + 2);
				host_map_name(next->map_name, map, sizeof(map));
				host_text(font_index, inside.x0, line, inside.x1, line_height, _host_text_left, &host_blue, map);
				line = (short)(line + line_height);
				host_gametype_name(next->gametype, gametype, sizeof(gametype));
				host_text(small_font_index, (short)(inside.x0 + 16), line, inside.x1, small_line_height,
					_host_text_left, &white, gametype);
			}
		}
	}

	/* the players: each on a band, the chosen one's brighter, with its arrow */
	{
		short line;
		short row_height = (short)(small_line_height + 2 * HOST_PADDING);
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
		host_column((short)(middle + margin / 4), (short)(width - margin), y, (short)(foot - HOST_PADDING * 3),
			font_index, line_height, text, &inside);
		line = inside.y0;
		if (host.slot_count == 0)
		{
			host_text(small_font_index, inside.x0, line, inside.x1, small_line_height, _host_text_left,
				&host_lavender, "WAITING FOR PLAYERS TO JOIN");
		}
		for (column = 0; column < host.slot_count && line + row_height <= inside.y1; column++)
		{
			char name[16];
			char value[32];
			long score, ping;
			boolean chosen = host.slots[column] == host.chosen_slot;
			rectangle2d band;

			if (!server_status_player_row(host.slots[column], name, sizeof(name), &score, &ping))
				continue;
			band.x0 = (short)(inside.x0 - HOST_PADDING);
			band.x1 = (short)(inside.x1 + HOST_PADDING);
			band.y0 = line;
			band.y1 = (short)(line + row_height);
			if (chosen)
			{
				host_rounded(&band, 4, CHOSEN_BAND);
				host_arrow((short)(band.x0 + 4), (short)(line + row_height / 2), 5, HOST_COLOR(0xFF, 0x5F, 0xB4, 0xFF));
			}
			else
			{
				host_rounded(&band, 4, BAND);
			}
			if (server_status_player_muted(host.slots[column]))
				snprintf(text, sizeof(text), "%s (MUTED)", name);
			else
				snprintf(text, sizeof(text), "%s", name);
			host_text(small_font_index, (short)(inside.x0 + 10), (short)(line + HOST_PADDING), inside.x1,
				small_line_height, _host_text_left, chosen ? &white : &host_lavender, text);
			if (ping == NONE)
				snprintf(value, sizeof(value), "%ld", score);
			else
				snprintf(value, sizeof(value), "%ld   %ld MS", score, ping);
			host_text(small_font_index, inside.x0, (short)(line + HOST_PADDING), inside.x1, small_line_height,
				_host_text_right, &white, value);
			line = (short)(line + row_height + HOST_ROW_GAP);
		}
	}

	/* the foot: what the last command did, or the second B's question,
	else the buttons */
	if (host.confirm_until && (long)(now - host.confirm_until) < 0)
	{
		rectangle2d bounds;

		bounds.x0 = margin;
		bounds.x1 = (short)(width - margin);
		bounds.y0 = foot;
		bounds.y1 = (short)(foot + small_line_height);
		draw_string_set_draw_mode(small_font_index, NONE, _host_text_left, 0, &host_red);
		draw_string_and_hack_in_icons(&bounds, NULL, NULL, 0, L"Press %b-button again to stop hosting", FALSE);
	}
	else if (host.status[0] && (long)(now - host.status_until) < 0)
	{
		snprintf(text, sizeof(text), "%s", host.status);
		host_capitals(text);
		host_text(small_font_index, margin, foot, (short)(width - margin), small_line_height, _host_text_left, &white,
			text);
	}
	else
	{
		host_hints(small_font_index, small_line_height, (short)(width - margin), foot, hints, hint_words,
			(short)NUMBEROF(hints));
	}

	/* the menu open, over the middle of the screen */
	if (host.menu != _host_menu_none)
	{
		short rows = host_menu_rows();
		short box_width = (short)MIN(width - 4 * margin, 380);
		short row_height = (short)(small_line_height + 2 * HOST_PADDING);
		short box_height = (short)(line_height + 4 * HOST_PADDING + rows * (row_height + HOST_ROW_GAP) + HOST_PADDING);
		short x0 = (short)((width - box_width) / 2);
		short y0 = (short)((height - box_height) / 2);
		short row;
		char title[64];

		rectangle.x0 = 0;
		rectangle.y0 = 0;
		rectangle.x1 = width;
		rectangle.y1 = height;
		draw_quad(&rectangle, HOST_COLOR(0x90, 0, 0, 0));
		rectangle.x0 = x0;
		rectangle.y0 = y0;
		rectangle.x1 = (short)(x0 + box_width);
		rectangle.y1 = (short)(y0 + box_height);
		host_box(&rectangle, 12, HOST_COLOR(0xF4, 0x08, 0x1A, 0x34));
		if (host.menu == _host_menu_player)
		{
			char name[16];
			long score, ping;

			if (!server_status_player_row(host.menu_slot, name, sizeof(name), &score, &ping))
				snprintf(name, sizeof(name), "PLAYER");
			snprintf(title, sizeof(title), "%s%s", name, server_status_player_muted(host.menu_slot) ? " (MUTED)" : "");
		}
		else
		{
			snprintf(title, sizeof(title), "SHOW EVERY PLAYER");
		}
		host_text(font_index, x0, (short)(y0 + HOST_PADDING * 2), (short)(x0 + box_width), line_height,
			_host_text_centre, &host_blue, title);
		y = (short)(y0 + line_height + 3 * HOST_PADDING);
		for (row = 0; row < rows; row++)
		{
			char const *label = host.menu == _host_menu_player ? host_player_actions[row].label :
				row < (short)NUMBEROF(host_notices) ? host_notices[row] : "Cancel";
			boolean chosen = row == host.menu_row;

			rectangle.x0 = (short)(x0 + HOST_PADDING * 2);
			rectangle.x1 = (short)(x0 + box_width - HOST_PADDING * 2);
			rectangle.y0 = y;
			rectangle.y1 = (short)(y + row_height);
			host_rounded(&rectangle, 4, chosen ? CHOSEN_BAND : BAND);
			if (chosen)
				host_arrow((short)(rectangle.x0 + 4), (short)(y + row_height / 2), 5, HOST_COLOR(0xFF, 0x5F, 0xB4, 0xFF));
			host_text(small_font_index, (short)(x0 + HOST_PADDING * 2 + 14), (short)(y + HOST_PADDING),
				(short)(x0 + box_width - HOST_PADDING * 3), small_line_height, _host_text_left,
				chosen ? &white : &host_lavender, label);
			y = (short)(y + row_height + HOST_ROW_GAP);
		}
	}
}

#else

int host_dashboard_start(char const *rotation, char *error_text, size_t error_size)
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
