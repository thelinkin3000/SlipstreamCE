# FractumSeraph's map archive

Settings > Manage Content > **FRACTUM SERAPH'S MAP ARCHIVE** browses the
Custom Edition maps of FractumSeraph's archive and downloads them into
`custom_maps`, on Android, the Switch and the PS5. The screen is the Steam
Workshop's (`tools/port_settings.py`'s `_steam_workshop`, opened by
`port steam init archive`); its model is the host's
`port/android/host/fractum_archive.c`, which `steam_downloads.c` hands the
screen's calls to in its archive section.

## The list

FractumSeraph's approved map list, `map_torrents.txt`, from the `map-index`
branch of his fork (github.com/FractumSeraph/OpenCE), with its signature,
`map_torrents.txt.sig`:

- Comment lines first: `# halo map torrents v1`, then `# serial <n>` (a Unix
  time, raised whenever the list changes), then the format.
- Then a map a line, separated by tabs: the file name, the map header's
  checksum (8 hex digits; `00000000` for the resource maps, matched by name
  alone), the size in bytes, the BitTorrent piece length and v1 info hash,
  `blake2b:<BLAKE2b-256 of the file>`, `share:<his map sharing hash>`, then
  where the map archives hold it (`halomaps:<file id>`, `halonet:<zip>`).
- The signature is 128 hex digits: Ed25519, by his fork's release key (or its
  recovery key, `map_index_key.h` there), over `fractum-halo-map-index 1\n`
  and then the list's exact bytes.

On 2026-11-15 it had 5,834 maps (474 GB), 77 of them resource maps.

The game fetches both from `raw.githubusercontent.com` over HTTPS when the
screen first opens, and takes the list only if the signature checks. A good
list is kept in `<data root>/map_archive/`, checked again and used at the next
start until GitHub's is newer (a higher serial). The resource maps aren't
listed: they aren't maps to play.

## A download

A map comes from his web seed, `http://halomaps.fractumseraph.net/maps/<file
name>` (percent-encoded; plain HTTP), into `custom_maps/<name>.part`. It's
taken only when its size and BLAKE2b are the list's; then it's renamed into
place and named in `custom_maps/downloaded_maps.txt`, as map sharing's
downloads are, so its scripts are held to the same tighter rules
(`map_share.c`). One map downloads at a time; the rest wait in a queue of up
to 64. The screen can't be left while maps are coming.

A map whose file name is in `custom_maps` with the list's size shows as
**INSTALLED** (A, twice, removes it); one of another size as **OTHER
VERSION** (A, twice, replaces it). The map lists look at `custom_maps` again
after each download or removal.

## Rows

**SEARCH** (a part of the name), **SORT** (name, largest, smallest), **SHOW**
(all, installed, not installed), then the maps 20 a page, and **PREVIOUS
PAGE** and **NEXT PAGE**.
