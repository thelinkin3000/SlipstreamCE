/*
HOST_EMBEDDED_ASSETS.H

The files built into the game that a guest image's host keeps for it
(tools/embed_assets.py --host: the high-res HUD's PNGs, the fonts, the
menus' files), by their numbers (port/linux/src/embedded_assets.h). The
guest asks for a copy of one (host_embedded_asset_read, in its imports);
the host reads its own, the HUD's PNGs it decodes (host_vk_hud.c, the
Switch's host_dk_hud.c), by number.
*/

#ifndef HOST_EMBEDDED_ASSETS_H
#define HOST_EMBEDDED_ASSETS_H

/* the asset's bytes when it is of that size, else NULL */
const unsigned char *host_embedded_asset_bytes(unsigned int asset, unsigned int size);
/* the asset's bytes into the guest's memory at destination: 1 if they are */
int host_embedded_asset_read(unsigned int asset, unsigned int destination, unsigned int size);

#endif
