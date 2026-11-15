/*
EMBEDDED_ASSETS.H

The files built into the game (tools/embed_assets.py): the high-res HUD's
PNGs (hud_hires.h), the fonts (text_hires.h) and the menus' files
(menu_files.h), each by its number in their tables.

Where the game is a guest image (Android, the Switch, the PS5, the desktop
guest builds: tools/android_build.py's generate_guest_image), the files are
the host's, built into it, and not in the image, whose 64 MB window has
little room (port/android/guest/guest.ld): a file is copied from the host
into the guest's own memory (its malloc's, outside that window) the first
time it is asked for (host_embedded_asset_read), and kept there for the
run, so the same bytes stay at the same address. Elsewhere (the native
desktop builds) they are in the program itself, as C data.
*/

#ifndef EMBEDDED_ASSETS_H
#define EMBEDDED_ASSETS_H

/* the file's bytes (with a NUL after them), or NULL: no such file, or it
could not be had */
const void *embedded_asset(unsigned int asset);

#endif
