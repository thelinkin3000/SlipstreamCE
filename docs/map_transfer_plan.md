# Map transfers: Workshop + host for MCC, CE sharing on the native host

Plan, 2026-11-13. Branch to be decided (not `bots-v2`). Nothing here changes
the wire: every stage must keep playing with upstream OpenCE builds at
network version 44.

## Goals

1. A joiner on an MCC map it lacks downloads the package from the host and
   from Steam's Workshop at once, automatically, when both hold the same
   version, so the host's upload carries only part of it. Otherwise from the
   host alone, as now.
2. Custom Edition map sharing (its own connections, port 5153) runs on the
   native host's threads: no frame rate, no stop while a map loads, uploads at
   the player's cap, downloads at whatever the line gives.
3. The internet tunnel does not cap a map-sharing stream below the uploader's
   cap.

## Caps

- `network.map_share_upload` (host, default 1024 KB/s, 16-20480) and
  `network.map_seed_upload` (a player seeding CE pieces, default 256 KB/s,
  16-10240) cap uploads only.
- Downloads have no cap (`network.map_download` is on/off). They take what the
  sources and the line give. The plan keeps it so.

## What OpenCE pins down (do not change)

- Network version 44 (`HALO_PORT_NETWORK_VERSION`).
- CE sharing: port 5153, the frames, `FRAME_MAGIC`/`FRAME_VERSION` (a hello
  of another version is closed: `frame_hello`), 256 KB pieces, SHA-256.
- MCC sharing: messages on the game's reliable connection
  (`_distributed_message_mcc_share_request/response`); chunks of at most 3072
  bytes; the host serves each file in order (an out-of-order request is
  ignored: `host_request_chunk`) and queues at most 32 requests a transfer
  (`MCC_SHARE_WINDOW * 2`; more stalls the transfer); `reserved` and unused
  `flags` must be zero.
- So MCC host uploads stay in the guest, on the game connection, a frame at a
  time. Moving them, larger chunks, or several MCC sources are protocol
  changes: an OpenCE PR for network version 45, not this plan.

## Part A: MCC from the host and the Workshop at once (joiner side)

The host's manifest already gives each file's kind, size, leaf name and
SHA-256 (`mcc_share_entry`). Steam's manifest gives each file's path, size,
whole-file SHA-1 and per-chunk SHA-1s. They cannot be compared directly, so a
match is decided in three steps: same leaf names and sizes before downloading;
the host's first bytes of each file hashed with SHA-1 along Steam's chunk
boundaries and compared with Steam's chunk hashes; every finished file checked
against the host's SHA-256 whatever its bytes came from (the check that
exists).

The host only serves front to back, so the split is: the host fills from the
front of the package (as now), Steam from the back, a chunk at a time; where
they meet the joiner ends the host's transfer (`MCC_SHARE_OP_DONE`, flags 0,
which every host accepts as a cancel). Steam's servers are usually faster than
one player's upload, so most of a package comes from Steam.

### A1. Native host: a Workshop item by id

- `steam_downloads.c`: look an item up by id on the session thread
  (`steam_cm_workshop_details` exists), then its manifest
  (`steam_depot_manifest`), without downloading.
- `steam_depot.c`: keep the whole-file SHA-1 (`FileMapping` field 5,
  `sha_content`) in `steam_depot_file`.
- Host imports (`host_imports.list`, `guest_host.h`, the three hosts):
  `host_steam_item_lookup(id)`, `host_steam_item_poll(...)` (state, file list:
  path, size, SHA-1, chunk offsets/sizes/SHA-1s), `host_steam_signed_in()`.
- Needs a signed-in account owning MCC and Halo: CE Anniversary; otherwise the
  lookup says so and the joiner goes host-only.

### A2. Native host: ranges of a file from the back

- A job: item, depot key, the file's chunks, the `.part` path the guest
  writes, and a fence (the offset below which the guest's host transfer has
  or will have the bytes). The files thread fetches chunks from the last down,
  writes each at its offset (pwrite), never at or below the fence, and reports
  the lowest offset it holds.
- Imports: `host_steam_range_start`, `host_steam_range_fence` (the guest moves
  the fence up as host bytes arrive), `host_steam_range_poll` (lowest held
  offset, bytes, speed, error), `host_steam_range_cancel`.
- Each chunk is checked against its SHA-1 as it is unpacked (exists).

### A3. Guest MCC client: the split

- `mcc_share_session.c` / `mcc_share_stage.c`: when the offer arrives and the
  package carries a Workshop id (`mcc_workshop_wire_unpack`), look the item up
  (A1). Match the host's files to the item's by leaf name (case aside) and
  size; any difference, or a file the item lacks: host-only.
- Write host bytes into the same `.part` files the Steam job writes. Move the
  fence up as they land. When the host's front reaches the Steam job's lowest
  offset in the last file it needs, send DONE(0) and stop requesting.
- SHA-1 the host's bytes per Steam chunk as they arrive; the first complete
  chunk of each file decides. A mismatch cancels the Steam job and discards
  its ranges (tracked per file), and the host transfer goes on to the end.
- Verify each file against the host's SHA-256 (exists). A file that fails is
  fetched again from the host alone.
- To check first: after DONE(0) the host does not call `host_finished`. Find
  how the joiner is admitted then (its HAVE, `host_request_have`, and the
  ordinary admission of a local package), against an OpenCE host and ours,
  so the join in progress (#665) still happens.

### A4. MCC's shared resource sets

- The host never sends the base resources (bitmaps/sounds sets). A joiner
  without matching copies cannot play today. When the check says they are
  missing and the joiner is signed in, queue the shared sets on the existing
  downloader (it fetches them from depot 976733) before or with the item.

### A5. The prompt

- DOWNLOAD stays the one choice; it splits automatically when A1 found a
  matching item, else host-only. The Workshop page action stays for PCs.
- Progress shows both sources: bytes and speed from the host and from Steam.
- debug.txt: one line when the split is decided (or why not), one when the
  sources meet, totals per source at the end.

### A6. The host part, within what OpenCE allows

- Joiner: `MCC_SHARE_WINDOW` 16 -> 32 requests in flight (every host queues
  32; more would stall an OpenCE host). About twice the host part's speed.
- Our hosts: `host_serve` opens, seeks and closes the file for every 3 KB
  chunk on the game's thread. Keep the file open per transfer and read ahead
  on a thread into a buffer, so the frame only copies.

### A tests

- Harness: the split's bookkeeping (fence, meeting, discard on mismatch,
  re-fetch on a failed SHA-256), with negative controls; the joiner against
  the unmodified host session code (its in-order rule, the 32 queue, DONE(0)).
- `tools/test_host_mcc_hash.py`-style test for the SHA-1 along chunk
  boundaries.
- Device: a Workshop package joined from a signed-in Switch and Android, and
  from one not signed in; an item whose Workshop version differs from the
  host's.

## Part B: CE map sharing on the native host

Today `map_share.c` (guest, ~3600 lines) runs the connections, the torrent
logic, the pacing and the file I/O from `map_share_update`, once a frame. The
plan splits it: the transfer engine moves to the native host; the game's side
(offers, sources, admission, UI, catalog) stays in the guest and talks to the
engine through host imports. The wire stays byte for byte the same, so OpenCE
players still download from us, seed to us and take seeds from us.

### B1. Split the engine out, still in the guest

- A `map_share_engine.c` with no engine headers: connections, frames, hello,
  state/have/request/block/refuse, piece choice (rarest first), piece SHA-256
  checks, `.part` files, the upload token bucket. `map_share.c` keeps the
  distributed messages, the tracker, the catalog and the UI, and calls the
  engine through a small interface (catalog in, download job in, sources in,
  progress and completion out).
- Harness: the engine against the current `map_share.c` frames, byte for byte
  (a recorded session, both directions), before anything moves.

### B2. The engine on the native host

- `host_share.c`, shared by Android, Switch and PS5 as `host_mcc_hash.c` is:
  one thread, a listener on 5153, outgoing connections, poll/select, blocking
  sends paced by the token bucket at the cap (host or seed cap, as the guest
  says), receives unpaced. File reads and writes with pread/pwrite. Piece
  hashing with the host's SHA-256 (the CPU instructions `host_mcc_hash.c`
  uses).
- Imports: `host_share_start(cap, role)`, `host_share_catalog(files: path,
  size, hash)`, `host_share_download(job: files, hashes, sizes, destination)`,
  `host_share_source(address, port, identity)`, `host_share_poll(events)`,
  `host_share_set_cap`, `host_share_stop`.
- Platforms: Android (kernel sockets, same as the guest's), Switch (libnx
  sockets beside `host_net.c`; mind its socket buffer limits), PS5.

### B3. Addresses through the tunnel

- A native-host socket bypasses `xnet.c`, so the guest does the translation:
  - outgoing: the guest calls `p2p_outgoing(stream=1, ...)` for a peer's
    virtual address and hands the engine the local stand-in address/port;
  - incoming: the guest registers the engine's listener port with p2p
    (`p2p_socket_port`) so tunnel streams to 5153 land on it; the engine
    reports each accepted connection's local address/port, the guest maps it
    to the peer (`p2p_incoming`) and says which machine it is, or to close it.
- LAN machines are connected directly.

### B4. The guest's side

- `map_share.c` keeps offers, sources and the tracker on distributed
  messages, admission, the lobby hold, the UI and the catalog rescan; it feeds
  the engine and reads its events each frame (a copy under a lock, as
  `steam_downloads.c` does). A finished download is installed by the engine
  into custom_maps; the guest rescans and joins the game in progress.
- `map_share_files.c`'s hashing thread moves to the host with the engine.
- Guest image: the engine's code and buffers leave the guest (headroom).

### B tests

- Harness: engine-to-engine and engine-to-old-`map_share.c` transfers,
  several sources, a lying source, a `.part` resumed, cap pacing measured
  (sent bytes per second within a few % of the cap).
- Device: Switch host to Android joiner and back, over LAN and the internet;
  an OpenCE (upstream build) joiner taking a map from us and seeding to us.

## Part C: the tunnel does not cap map sharing

All local: KCP advertises its window in every packet and each side limits
only its own sending, so no OpenCE player sees a difference but speed.

### C1. Map-sharing streams get their own settings

- `p2p.c`: streams whose port is 5153 get a send window sized to the cap
  times the stream's smoothed round trip (`rx_srtt`) times 2, between 128 and
  1024 messages; a receive window and `STREAM_BUFFER_SIZE` to match; more than
  8 KB read a pass (`stream_readable`); larger messages. The game's streams
  keep today's settings (MCC chunks ride the game's stream: unchanged).
- An OpenCE receiver advertises 256 segments, so our sender reaches about
  300 KB in flight to it, our receivers more.
- Memory: a 1024-message window is about 1.2 MB a stream on the guest's heap:
  at most a few large streams at once (Switch first).

### C2. Back off before the line does

- In `p2p.c`, per map-sharing stream: keep the lowest round trip seen; when
  the smoothed one rises well above it (more than 50 ms or 2x) or resends
  climb, halve the stream's send rate and grow it back slowly. The uploader's
  cap stays the ceiling.

### C3. The game first

- While a game is being played on this machine, map uploads run at a part of
  the cap (setting, default 25%); in the lobby and postgame, the whole cap.
  The guest tells the engine (B4) and the MCC host serve (A6).

### C tests

- Harness: window sizing from cap and round trip, the back-off on a rising
  round trip, the game's streams unchanged.
- Device: a CE map over the internet between two phones with the cap at
  5 MB/s; the game's ping in a match while a joiner downloads.

## Order

1. A1-A6 (Workshop + host for MCC): the largest gain, all joiner side.
2. C1 (tunnel settings for 5153): small, lifts CE over the internet now.
3. B1 (engine split in the guest, byte-for-byte test), then B2-B4.
4. C2-C3 (back-off, game first), before raising the default caps.

Each stage: its own commit, Android/Switch/PS5 builds, the harness compared
with main, and the log lines above for the device test.

## Later, with OpenCE (network version 45)

- MCC transfers on the native host's connections (as CE's), out of the game's
  reliable stream; larger chunks; several sources.
- A capability field in both hellos, so later changes can fall back.
