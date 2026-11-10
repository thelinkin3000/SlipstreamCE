/*
ONSCREEN_KEYBOARD.C

The on-screen keyboard (halo_onscreen_keyboard.h): OpenCE's text chat's
keyboard (network_chat.c had it to itself), its keys a QWERTY page and a
symbols page, drawn in the menus' look, the Host Server screens' (tools/
port_settings.py's host_pictures): a dark panel outlined in their blue,
rounded keys with a faint outline, the key picked lit in blue, capitals
and the symbols' page lit while on.
*/

#include "cseries.h"
#include "game/game.h"
#include "interface/ui_widget.h"
#include "rasterizer/rasterizer.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "halo_onscreen_keyboard.h"

#include <math.h>
#include <string.h>

void draw_quad(rectangle2d *rectangle, pixel32 color);
unsigned long system_milliseconds(void);

#define OSK_COLOR(a, r, g, b) ((pixel32)(((unsigned long)(a) << 24) | ((unsigned long)(r) << 16) | \
	((unsigned long)(g) << 8) | (unsigned long)(b)))

/* its keys' characters but the last row's, the letters' and the symbols'
(no "|", which the game's text drawing takes for a code's start) */
static char const *const onscreen_letters[ONSCREEN_KEYBOARD_ROWS - 1] =
	{ "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?" };
static char const *const onscreen_symbols[ONSCREEN_KEYBOARD_ROWS - 1] =
	{ "1234567890", "!@#$%^&*()", "-_=+[]{};:", "\"'<>/\\?~,." };

void onscreen_keyboard_reset(
	struct onscreen_keyboard *keyboard)
{
	memset(keyboard, 0, sizeof(*keyboard));
	keyboard->row = 1;
}

void onscreen_keyboard_move(
	struct onscreen_keyboard *keyboard,
	short rows_down,
	short columns_right)
{
	short width;

	keyboard->row = (short)((keyboard->row + rows_down % ONSCREEN_KEYBOARD_ROWS + ONSCREEN_KEYBOARD_ROWS) %
		ONSCREEN_KEYBOARD_ROWS);
	width = keyboard->row == ONSCREEN_KEYBOARD_ROWS - 1 ? ONSCREEN_KEYBOARD_WIDE_COLUMNS : 1;
	keyboard->column = (short)(keyboard->column - keyboard->column % width);
	keyboard->column = (short)((keyboard->column + columns_right * width % ONSCREEN_KEYBOARD_COLUMNS +
		ONSCREEN_KEYBOARD_COLUMNS) % ONSCREEN_KEYBOARD_COLUMNS);
}

void onscreen_keyboard_steer(
	struct onscreen_keyboard *keyboard,
	short rows_down,
	short columns_right,
	unsigned long now)
{
	if (!rows_down && !columns_right)
	{
		keyboard->repeat_at = 0;
	}
	else if (!keyboard->repeat_at || (long)(now - keyboard->repeat_at) >= 0)
	{
		onscreen_keyboard_move(keyboard, rows_down, columns_right);
		keyboard->repeat_at = now + (keyboard->repeat_at ? ONSCREEN_KEYBOARD_REPEAT_MILLISECONDS :
			ONSCREEN_KEYBOARD_REPEAT_FIRST_MILLISECONDS);
	}
}

/* the character of a key but the last row's, a letter in capitals with
shift; 0 for none */
static char onscreen_character(
	struct onscreen_keyboard const *keyboard,
	short row,
	short column)
{
	char character;

	if (row < 0 || row >= ONSCREEN_KEYBOARD_ROWS - 1 || column < 0 || column >= ONSCREEN_KEYBOARD_COLUMNS)
		return 0;
	character = (keyboard->symbols ? onscreen_symbols : onscreen_letters)[row][column];
	if (keyboard->shift && character >= 'a' && character <= 'z')
		character = (char)(character - 'a' + 'A');
	return character;
}

short onscreen_keyboard_press(
	struct onscreen_keyboard *keyboard,
	char *typed)
{
	*typed = 0;
	if (keyboard->row < ONSCREEN_KEYBOARD_ROWS - 1)
	{
		*typed = onscreen_character(keyboard, keyboard->row, keyboard->column);
		return *typed ? _onscreen_press_character : _onscreen_press_none;
	}
	switch (keyboard->column / ONSCREEN_KEYBOARD_WIDE_COLUMNS)
	{
	case _onscreen_key_shift:
		keyboard->shift = !keyboard->shift;
		return _onscreen_press_none;
	case _onscreen_key_symbols:
		keyboard->symbols = !keyboard->symbols;
		return _onscreen_press_none;
	case _onscreen_key_space:
		*typed = ' ';
		return _onscreen_press_character;
	case _onscreen_key_delete:
		return _onscreen_press_delete;
	default:
		return _onscreen_press_done;
	}
}

boolean onscreen_keyboard_pick_at(
	struct onscreen_keyboard *keyboard,
	short x,
	short y)
{
	short width = (short)(keyboard->keys.x1 - keyboard->keys.x0);
	short height = (short)(keyboard->keys.y1 - keyboard->keys.y0);
	short row, column;

	if (!keyboard->drawn || width <= 0 || height <= 0 || x < keyboard->keys.x0 || x >= keyboard->keys.x1 ||
		y < keyboard->keys.y0 || y >= keyboard->keys.y1)
	{
		return FALSE;
	}
	row = (short)((y - keyboard->keys.y0) * ONSCREEN_KEYBOARD_ROWS / height);
	column = (short)((x - keyboard->keys.x0) * ONSCREEN_KEYBOARD_COLUMNS / width);
	if (row == ONSCREEN_KEYBOARD_ROWS - 1)
		column = (short)(column - column % ONSCREEN_KEYBOARD_WIDE_COLUMNS);
	keyboard->row = (short)PIN(row, 0, ONSCREEN_KEYBOARD_ROWS - 1);
	keyboard->column = (short)PIN(column, 0, ONSCREEN_KEYBOARD_COLUMNS - 1);
	return TRUE;
}

/* ---------- drawing */

/* a rounded rectangle, of strips a unit tall at its top and bottom (draw_quad
draws none other), none of them over another */
static void onscreen_rounded(
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

/* a box: its outline (thickness units) round its fill */
static void onscreen_box(
	rectangle2d const *rectangle,
	short radius,
	short thickness,
	pixel32 outline,
	pixel32 fill)
{
	rectangle2d inside = *rectangle;

	onscreen_rounded(rectangle, radius, outline);
	inside.x0 = (short)(inside.x0 + thickness);
	inside.y0 = (short)(inside.y0 + thickness);
	inside.x1 = (short)(inside.x1 - thickness);
	inside.y1 = (short)(inside.y1 - thickness);
	onscreen_rounded(&inside, (short)MAX(radius - thickness, 1), fill);
}

void onscreen_keyboard_draw(
	struct onscreen_keyboard *keyboard,
	rectangle2d const *keys,
	long font_index,
	short line_height,
	char const *done_label)
{
	static pixel32 const BLUE = OSK_COLOR(0xFF, 0x28, 0x96, 0xFF);
	static pixel32 const PANEL = OSK_COLOR(0xE8, 0x06, 0x14, 0x2A);
	static pixel32 const KEY = OSK_COLOR(0xD8, 0x0A, 0x26, 0x49);
	static pixel32 const WIDE_KEY = OSK_COLOR(0xD8, 0x10, 0x30, 0x58);
	static pixel32 const KEY_OUTLINE = OSK_COLOR(0x70, 0x28, 0x96, 0xFF);
	static pixel32 const PICKED = OSK_COLOR(0xF0, 0x1E, 0x64, 0xB4);
	static pixel32 const PICKED_OUTLINE = OSK_COLOR(0xFF, 0xA8, 0xD8, 0xFF);
	static real_argb_color const lavender = { 1.0f, 0x8C / 255.0f, 0xA8 / 255.0f, 0xE8 / 255.0f };
	static real_argb_color const blue = { 1.0f, 0x28 / 255.0f, 0x96 / 255.0f, 1.0f };
	real_argb_color white = get_ui_argb_white();
	short key_width = (short)((keys->x1 - keys->x0) / ONSCREEN_KEYBOARD_COLUMNS);
	short key_height = (short)((keys->y1 - keys->y0) / ONSCREEN_KEYBOARD_ROWS);
	short gap = (short)MAX(2, key_width / 14);
	short radius = (short)MAX(3, key_height / 5);
	rectangle2d panel;
	short row;

	if (key_width <= 4 || key_height <= 4)
		return;
	panel = *keys;
	panel.x0 = (short)(panel.x0 - gap * 3);
	panel.y0 = (short)(panel.y0 - gap * 3);
	panel.x1 = (short)(panel.x1 + gap * 3);
	panel.y1 = (short)(panel.y1 + gap * 3);
	onscreen_box(&panel, (short)(radius * 2), 2, BLUE, PANEL);
	for (row = 0; row < ONSCREEN_KEYBOARD_ROWS; row++)
	{
		boolean wide = row == ONSCREEN_KEYBOARD_ROWS - 1;
		short span = wide ? ONSCREEN_KEYBOARD_WIDE_COLUMNS : 1;
		short column;

		for (column = 0; column < ONSCREEN_KEYBOARD_COLUMNS; column += span)
		{
			short wide_key = (short)(column / span);
			boolean picked = row == keyboard->row && column == keyboard->column;
			boolean lit = wide && ((wide_key == _onscreen_key_shift && keyboard->shift) ||
				(wide_key == _onscreen_key_symbols && keyboard->symbols));
			rectangle2d key;
			rectangle2d bounds;
			char label[16];

			key.x0 = (short)(keys->x0 + column * key_width + gap);
			key.x1 = (short)(keys->x0 + (column + span) * key_width - gap);
			key.y0 = (short)(keys->y0 + row * key_height + gap);
			key.y1 = (short)(keys->y0 + (row + 1) * key_height - gap);
			if (picked)
				onscreen_box(&key, radius, 2, PICKED_OUTLINE, PICKED);
			else if (lit)
				onscreen_box(&key, radius, 2, BLUE, WIDE_KEY);
			else
				onscreen_box(&key, radius, 1, KEY_OUTLINE, wide ? WIDE_KEY : KEY);
			if (!wide)
			{
				label[0] = onscreen_character(keyboard, row, column);
				label[1] = 0;
			}
			else
			{
				static char const *const names[NUMBER_OF_ONSCREEN_WIDE_KEYS] =
					{ "SHIFT", "#+=", "SPACE", "DELETE", "DONE" };
				char const *name = wide_key == _onscreen_key_symbols && keyboard->symbols ? "ABC" :
					wide_key == _onscreen_key_done && done_label ? done_label : names[wide_key];

				snprintf(label, sizeof(label), "%s", name);
			}
			bounds = key;
			bounds.y0 = (short)(key.y0 + (key.y1 - key.y0 - line_height) / 2);
			bounds.y1 = (short)(bounds.y0 + line_height);
			draw_string_set_draw_mode(font_index, NONE, 2 /* centred */, 0,
				picked ? &white : lit ? &blue : wide ? &lavender : &white);
			rasterizer_draw_string(&bounds, NULL, NULL, 0, label);
		}
	}
	keyboard->drawn = TRUE;
	keyboard->keys = *keys;
}

void onscreen_keyboard_draw_field(
	rectangle2d const *box,
	long font_index,
	short line_height,
	char const *text,
	boolean masked)
{
	static pixel32 const BLUE = OSK_COLOR(0xFF, 0x28, 0x96, 0xFF);
	static pixel32 const FIELD = OSK_COLOR(0xF0, 0x04, 0x0E, 0x1E);
	real_argb_color white = get_ui_argb_white();
	char shown[160];
	wchar_t wide[160];
	rectangle2d bounds;
	rectangle2d area, measured, cursor;
	short length, first, index;
	short room;

	onscreen_box(box, (short)MAX(4, (box->y1 - box->y0) / 4), 2, BLUE, FIELD);
	for (length = 0; text[length] && length < (short)sizeof(shown) - 2; length++)
		shown[length] = masked ? '*' : text[length];
	/* (the caret, blinking) */
	shown[length] = (system_milliseconds() / 500) % 2 ? '_' : ' ';
	shown[length + 1] = 0;
	bounds.x0 = (short)(box->x0 + 10);
	bounds.x1 = (short)(box->x1 - 10);
	bounds.y0 = (short)(box->y0 + (box->y1 - box->y0 - line_height) / 2);
	bounds.y1 = (short)(bounds.y0 + line_height);
	room = (short)(bounds.x1 - bounds.x0);
	/* (its end, where the whole is too long for the box) */
	draw_string_set_draw_mode(font_index, NONE, 0, 0, &white);
	for (first = 0; first < length; first++)
	{
		for (index = 0; shown[first + index] && index < (short)NUMBEROF(wide) - 1; index++)
			wide[index] = (wchar_t)(unsigned char)shown[first + index];
		wide[index] = 0;
		area = bounds;
		area.x1 = 0x3FFF;
		draw_unicode_string_compute_bounds(&area, wide, &measured, &cursor);
		if (MAX(measured.x1, cursor.x0) - bounds.x0 <= room)
			break;
	}
	rasterizer_draw_string(&bounds, &bounds, NULL, 0, shown + first);
}
