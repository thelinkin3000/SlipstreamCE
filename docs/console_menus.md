# Console menus: lists, choosers, hints

What the menus should be on the Switch and the PS5, the pieces that build it,
and where each screen stands (audit of 2026-11-17).

## The target

- On a screen with a list, the list's rows are the only things the d-pad
  moves through.
- A list's chooser (a spinner of categories: map kind, gametype bank) sits
  beside the list, not in it, and the shoulders turn it on every platform
  (LB/RB; L/R on the Switch, L1/R1 on the PS5). A tap or click on its arrows
  still turns it.
- On the consoles, what a button bar did is a button press, and the bar's
  buttons give way to a hint bar along the screen's foot. A confirms (the
  Switch's A and B swapped as the menus swap them; Cross on the PS5), B
  cancels or goes back (Circle).

## The pieces

### Choosers beside the list

`tools/port_settings.py`: a list patch's `"drop"` leaves a child out of the
PC version's list (`tools/ce_menus.py`); the screen's `"children"` adds the
chooser at `LIST_CHOOSER_PLACE`; `LIST_CHOOSER_HANDLERS` puts
`port chooser previous` / `port chooser next` on the list's `white` and
`black` (LB, RB).

`port/linux/game/menu_functions.c`: `list_chooser()` finds the spinner by
name anywhere in the list's screen, `list_chooser_turn()` steps it (wrapping)
for the shoulders, and the list's update notices the new value and starts
the new category's list on its first row. The rows are the list's children
from 0.

### Hint bars

`tools/port_settings.py`'s `hint_bar(base, left, right, y=...)` returns the
widgets and the `<child>` to put in the screen. Each hint is
`(button, words)`:

| `button` | Drawn as |
| --- | --- |
| `"a"`, `"b"`, `"x"`, `"y"` | the menus' own button picture in the text (`%a-button`), each console's |
| `"lb"`, `"rb"`, `"dpad"`, `"start"` (`HINT_PICTURES`) | Kenney's picture for each console before the words |
| a tuple of those, `("lb", "rb")` | each picture in turn |
| any other string, `"LB"` | written before the words |

```python
LIST_HINTS = hint_bar("main_menu/gametype_select",
                      [("a", "SELECT"), (("lb", "rb"), "CATEGORY")],   # from the left edge
                      [("b", "BACK")],                                # ending at the right edge
                      y=415)
# LIST_HINTS[0]: widget lines for a port file; LIST_HINTS[1]: the screen's <child>
```

No x is given anywhere. Every frame the bar's `port hint bar` input
(`menu_functions.c`'s `hint_bar_update`) measures each hint as it is drawn
(`ui_widget.c`'s `ui_widget_port_text_width`: the font, and the button
pictures as `draw_string_and_hack_in_icons` advances over them) and lays the
left hints out from x 24 and the right ones back from x 616, 18 apart. A
screen's update can hide a hint (`base/hint_bar/left_N` / `right_N`) and the
rest close up.

A new picture for `HINT_PICTURES` needs its PNG in
`port/assets/menus/ce/port/hints` (`browser_hint_pictures`, written by
`tools/ce_menus.py`).

### Regenerating the XML

`tools/ce_menus.py` needs the PC version's tags. Without them, port files
(`multiplayer_files()`, `settings_files()`) can be written from
`port_settings.py` alone, and a WIDGET_PATCHES change to a PC screen has to
be made by hand in its XML too, the same way the generator would.

## Done so far

The consoles' button rules: A accepts (or opens the row), B goes back, X
(west) saves, Y (north) restores the defaults; hints in that order (A at the
right edge, B left of it, X, Y and the rest from the left edge).

- Lists: New Game, Map and Gametype (choosers on LB/RB), Load Game, Edit
  Gametypes (Y NEW), the gametype editor (X SAVE) and its game type, Change
  Color, Multiplayer, co-op's player 2, Difficulty, Select Profile, Mods (A
  ON / OFF, X SAVE), Theater, the server browser (rows alone take the
  focus), Host Server's screens, the lobby (A START NOW, X ADD PLAYER, Y MAP
  VOTE) and a game in progress's lobby (A JOIN GAME, X ADD PLAYER), ADD
  PLAYER (join4).
- Settings pages and Host Server's settings: X SAVE, Y DEFAULTS, B BACK
  (`_screen`'s `console`); the profile settings: X SAVE.
- Server Setup: X START GAME; its rows laid out from the visible ones,
  24 apart (menu_functions.c's server_settings_update), and its sub-pages
  (co-op, voice, map sharing, every gametype option page) X SAVE.
- The map import page: two columns, its QR code 200 across, B BACK a hint.
- Names typed on the menus' keyboard (virtual_keyboard_launch_name), a
  name's rules kept.
- Lists wrap round (rows_track), and a list given the focus by Back gets its
  row again.
- The pause menus: the box as high as its rows (a frame for 3 to 7 buttons,
  the campaign's for 5 to 8 rows), the consoles' hints in its footer in the
  place of the map's key.

Still to do: the remaining dialogs (confirm delete, errors), MCC's pause
menu, Custom Edition maps whose pause screens are rebuilt (four buttons at
most).

## Audit: every screen on the consoles

Read from the XML (children for `switch`/`ps5` only) and the handlers in
`menu_functions.c`, not tried on a device. An empty button bar cannot take
the focus (`ui_widget.c`'s `widget_instance_port_is_label`), and an
unhandled B goes back (`widget_instance_go_back_to_previous`), so a screen
with no B handler still goes back. Verdicts are from before the changes
above (2026-11-17's first pass).

**Conforms already:** main menu, Select Profile, Mods, Theater, Host
Server's hub and gametype screens; join4 and in-game join (no list, own
hints); news, credits, invite QR (no list, B closes).

**Partial:**

| Screen | What's left |
| --- | --- |
| Server browser (`join_game_screen`) | In Server Browser mode the mode tab row and the column header row can take focus (LAN and Direct Link are fine) |
| Lobby (`lobby_screen`) | Bar TEAM / START / ADD / LEAVE focusable; Start, Y, X, B bound; help text, no hints |
| Host builder (`builder_screen`) | Name row and REMOVE ALL / REORDER pills in the rounds list |
| Host round (`round_screen`) | ADD ROUND pill row, which repeats X |
| Campaign menu (`campaign_menu`) | A BACK row in the list |

**Lists with a focusable bar (no hints):**

| Screen | Bar |
| --- | --- |
| Load Game (`load_level_select_screen`) | DELETE / OK / BACK (X deletes already) |
| Difficulty (`difficulty_select_list_screen`) | OK / BACK |
| Multiplayer (`multiplayer_type_select_screen`) | BACK only |
| Co-op player 2 profile (`player_2_profile_screen`) | OK / BACK |
| Lobby preview (`preview_screen`) | JOIN / ADD / BACK, the only focusable thing |
| Map vote (`map_vote_screen`) | LOBBY |
| Add player (`player_profile_screen`) | OK / BACK |
| Edit Gametypes (`playlist_select/gametype_select_screen`) | NEW / DELETE / OK / BACK (X deletes already) |
| Gametype editor (`gametype_edit_screen`, `create_and_edit_new_gametype_screen`) | OK (save) / CANCEL |
| Gametype type (`playlist_edit/game_type_select`) | BACK only |
| Profile settings (`player_profile_edit_screen`, also the pause menu's SETTINGS) | OK (save) / CANCEL |
| Change Color (`color_select_screen`) | OK / BACK |
| Controls (`controls_settings_screen`) | DEFAULTS / OK / CANCEL |
| Steam Workshop, Map Archive | BACK only |
| MCC downloads (`mcc_downloads/screen`) | PREVIOUS / NEXT / REMOVE / BACK as rows (paging should be LB/RB) |

**No list, bar focusable** (spinner pages; each needs its own design, e.g.
A or Start to save, Y for defaults, B to cancel): Server Setup and its co-op
/ voice / map share pages, server browser filters, password, direct IP, Host
settings, every gametype option page (CTF, KOTH, Oddball, Race, Slayer;
player, item, vehicle, indicator, teamplay, OpenCE options), every settings
page `port_settings.py` writes (video, gamepad, network, audio, etc.: DEFAULTS
/ OK / CANCEL), and the dialogs (confirm delete, gamespy error,
`pm_empty_selected`). The game files, MCC import page, MCC import and MCC
download screens are dialogs whose rows are their buttons, a BACK row among
them.

### Odd things found

- Teamplay Options OK: A saves, Start only goes back without saving.
- Vehicle Options OK: A saves the vehicle options, Start runs "mp profile set
  item options" (probably the PC version's own slip).
- Main menu PROFILE: A opens the profile manager, Start opens Multiplayer.
- Create Internet / LAN items: two `a` handlers and no `start`.
- Gyro Aim: B saves (`save_on_leave`); every other settings page's B
  discards.
- Main menu: B does nothing on the consoles (its quit is desktop, Android and
  web only), probably on purpose.
- `invite_qr_screen` handles `b` only; Back falls to the engine's default.
- `pm_empty_selected`: its OK button has no handler; A works through the
  screen's.
