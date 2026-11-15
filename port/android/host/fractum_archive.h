/*
FRACTUM_ARCHIVE.H

FRACTUM SERAPH'S MAP ARCHIVE (Settings > Manage Content): the Custom
Edition maps of FractumSeraph's archive, browsed and downloaded into
custom_maps. The Steam Workshop screen shows it (steam_downloads.c's
archive section: the same rows, details and search keyboard), and this
module is its model, in the host of every build (Android, Switch, PS5).

- The list: FractumSeraph's approved map list (map_torrents.txt, his fork's
  map-index branch, docs/fractum_map_archive.md), fetched from GitHub over
  HTTPS with its signature (map_torrents.txt.sig: Ed25519, over
  FRACTUM_LIST_CONTEXT and the list's exact bytes) and taken only when one
  of FRACTUM_LIST_KEYS signs it. It is kept in <data root>/map_archive and
  taken from there at the next start, checked again; a fetched one replaces
  it when its serial (the "# serial" line) is newer. Each line is a map:
  file name, header checksum, size, piece length, info hash, blake2b:<hash>,
  share:<hash>, then its sources; the resource maps (checksum 0) are not
  listed.
- A download: http://halomaps.fractumseraph.net/maps/<file name,
  percent-encoded>, written to custom_maps/<name>.part while its BLAKE2b is
  taken, then checked against the list's size and BLAKE2b, renamed into
  place (replacing another version of that name only after a confirmation)
  and named in custom_maps/downloaded_maps.txt, as map sharing's downloads
  are (map_share.c: their scripts held to tighter rules). One download at a
  time, the rest queued.

Its own lock and thread: the screen's calls never wait on the network.
*/

#ifndef FRACTUM_ARCHIVE_H
#define FRACTUM_ARCHIVE_H

/* the screen opened (data_root: where the list is kept; custom_maps: the
folder the maps go into, the guest's d:\custom_maps on this machine) and
closed */
void fractum_archive_open(char const *data_root, char const *custom_maps);
void fractum_archive_close(void);

/* the rows, built for the frame: 0 when the lock is held elsewhere (the
last frame's then stand); count_out the rows there are */
int fractum_archive_try_lock(void);
void fractum_archive_unlock(void);
/* (locked) */
int fractum_archive_count(void);
void fractum_archive_row(int place, char *name, int name_size, char *state, int state_size);
void fractum_archive_details(int place, char *title, int title_size, char *text, int text_size);
/* (locked) the search's text, once, when the keyboard is wanted: 1 then */
int fractum_archive_take_search(char *initial, int initial_size);
/* (locked) whether a map was put in custom_maps since the last call */
int fractum_archive_take_rescan(void);
/* (locked) the row the list wants chosen (a page turned), else -1 */
int fractum_archive_take_chosen(void);

/* A on a row; B (1 if it was taken: the screen stays); the search typed */
void fractum_archive_choose(int place);
int fractum_archive_back(void);
void fractum_archive_search(char const *text);

#endif
