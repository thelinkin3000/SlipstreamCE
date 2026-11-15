/*
FRACTUM_ARCHIVE.C

fractum_archive.h's: FRACTUM SERAPH'S MAP ARCHIVE's model. A thread of its
own loads the list (the kept copy, then GitHub's, each checked against its
signature), lists custom_maps to tell what is installed, and downloads the
maps queued, one at a time; the screen's calls take the lock briefly and
never wait on the network.
*/

#include "fractum_archive.h"
#include "steam_net.h"
#include "posix.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the list's source and its keys */

#define LIST_URL "https://raw.githubusercontent.com/FractumSeraph/OpenCE/map-index/map_torrents.txt"
#define SIGNATURE_URL LIST_URL ".sig"
/* the list's first line, and what its signature covers before its bytes
(FractumSeraph's map_list_index.h) */
#define LIST_FORMAT "# halo map torrents v1"
#define LIST_CONTEXT "fractum-halo-map-index 1\n"
#define LIST_MAXIMUM_SIZE (16UL * 1024 * 1024)
/* the maps, by file name: plain HTTP, as his fork's web seed is
(docs/map_torrents.md there) */
#define MAPS_HOST "halomaps.fractumseraph.net"
#define MAPS_PATH "/maps/"
#define USER_AGENT "SlipstreamCE (Halo; Custom Edition map downloads)"

/* the keys the list may be signed by: his fork's release key and its
recovery key (his port/linux/src/map_index_key.h) */
static const uint8_t list_keys[][32] =
{
	{
		0xc1, 0x20, 0x6c, 0xa2, 0xbe, 0xca, 0x78, 0xe1, 0x1c, 0xb1, 0x05, 0x30, 0xdd, 0x6e, 0x73, 0x3c,
		0x45, 0x61, 0xa6, 0xf1, 0x76, 0x2d, 0x49, 0x5d, 0xb1, 0xa6, 0x62, 0x53, 0xf3, 0x5a, 0x24, 0x3e,
	},
	{
		0xbb, 0x14, 0x21, 0xe0, 0x14, 0x7f, 0x12, 0x10, 0xa8, 0x44, 0x65, 0x32, 0xf6, 0x70, 0x96, 0xfc,
		0xe8, 0x87, 0xc7, 0xb5, 0x97, 0x87, 0x5c, 0x55, 0x27, 0x12, 0x98, 0xe4, 0x71, 0x88, 0xf8, 0xd3,
	},
};

enum
{
	/* a map's file name: the game's 31 characters and ".map" */
	NAME_SIZE = 40,
	PAGE_MAPS = 20,
	/* SEARCH, SORT and SHOW */
	FILTER_ROWS = 3,
	QUEUE_SIZE = 64,
	SEARCH_SIZE = 64,
	/* (a map's name and why its download failed) */
	MESSAGE_SIZE = 320,
	PATH_SIZE = 1024
};

enum sort
{
	SORT_NAME,
	SORT_LARGEST,
	SORT_SMALLEST,
	SORT_COUNT
};
static char const *const sort_names[SORT_COUNT] = { "NAME", "LARGEST", "SMALLEST" };

enum show
{
	SHOW_ALL,
	SHOW_INSTALLED,
	SHOW_MISSING,
	SHOW_COUNT
};
static char const *const show_names[SHOW_COUNT] = { "ALL", "INSTALLED", "NOT INSTALLED" };

/* what custom_maps has of a map's name */
enum installed
{
	INSTALLED_NONE,
	INSTALLED_SAME,
	/* a file of its name, of another size: another version */
	INSTALLED_OTHER
};

struct map
{
	char name[NAME_SIZE];
	unsigned long checksum;
	unsigned long long size;
	uint8_t blake2b[32];
	unsigned char installed;
};

/* the list's state */
enum list_state
{
	LIST_LOADING,
	LIST_READY,
	LIST_FAILED
};

/* what a row is */
enum row_kind
{
	ROW_NONE,
	ROW_LIST,
	ROW_SEARCH,
	ROW_SORT,
	ROW_SHOW,
	ROW_MAP,
	ROW_PREVIOUS,
	ROW_NEXT
};

/* the model's lock, and its thread's wake */
static pthread_mutex_t archive_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t archive_wake = PTHREAD_COND_INITIALIZER;

static struct
{
	int started;
	char data_root[PATH_SIZE];
	char custom_maps[PATH_SIZE];

	enum list_state list_state;
	char list_error[MESSAGE_SIZE];
	unsigned long long serial;
	struct map *maps;
	int map_count;
	/* the maps shown, by search, show and sort: indices into maps */
	int *shown;
	int shown_count;
	int page;

	char search[SEARCH_SIZE];
	enum sort sort;
	enum show show;
	int want_search;
	int want_chosen;

	/* the downloads: the one under way (-1: none), its bytes so far, and
	those waiting */
	int downloading;
	unsigned long long downloaded;
	int cancel;
	int queue[QUEUE_SIZE];
	int queue_count;
	/* a row waiting for its second A (REPLACE? REMOVE?), and the map */
	int confirm;
	int confirm_map;
	char message[MESSAGE_SIZE];
	/* the threads' asks */
	int want_list;
	int want_scan;
	char want_remove[NAME_SIZE];
	int rescan;
} archive;

/* ---------- small helpers */

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int names_equal(char const *a, char const *b)
{
	while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
	{
		a++;
		b++;
	}
	return !*a && !*b;
}

static int name_compare(char const *a, char const *b)
{
	while (*a && lower((unsigned char)*a) == lower((unsigned char)*b))
	{
		a++;
		b++;
	}
	return lower((unsigned char)*a) - lower((unsigned char)*b);
}

/* whether text holds part, whatever their case */
static int contains(char const *text, char const *part)
{
	size_t length = strlen(part);

	if (!length)
		return 1;
	for (; *text; text++)
	{
		size_t i = 0;

		while (i < length && text[i] && lower((unsigned char)text[i]) == lower((unsigned char)part[i]))
			i++;
		if (i == length)
			return 1;
	}
	return 0;
}

static int hex_value(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
		-1;
}

/* count bytes of hex digits, exactly: 1 if they are */
static int hex_bytes(char const *text, size_t length, uint8_t *bytes, size_t count)
{
	size_t i;

	if (length != count * 2)
		return 0;
	for (i = 0; i < count; i++)
	{
		int high = hex_value((unsigned char)text[2 * i]);
		int low = hex_value((unsigned char)text[2 * i + 1]);

		if (high < 0 || low < 0)
			return 0;
		bytes[i] = (uint8_t)(high * 16 + low);
	}
	return 1;
}

static void size_text(unsigned long long bytes, char *text, int size)
{
	if (bytes >= 1024ULL * 1024 * 1024)
		snprintf(text, (size_t)size, "%.2f GB", (double)bytes / (1024.0 * 1024 * 1024));
	else if (bytes >= 1024 * 1024)
		snprintf(text, (size_t)size, "%.1f MB", (double)bytes / (1024.0 * 1024));
	else
		snprintf(text, (size_t)size, "%llu KB", (bytes + 1023) / 1024);
}

/* a map's name as the game lists it: its file's, less ".map" */
static void display_name(struct map const *map, char *text, int size)
{
	size_t length = strlen(map->name);

	if (length > 4 && names_equal(map->name + length - 4, ".map"))
		length -= 4;
	snprintf(text, (size_t)size, "%.*s", (int)length, map->name);
}

static void path_join(char *path, size_t size, char const *folder, char const *name)
{
	snprintf(path, size, "%s/%s", folder, name);
}

/* ---------- the list */

/* whether the signature (its 128 hex digits, white space around them)
signs the list */
static int list_signed(char const *signature_text, size_t signature_size, unsigned char const *list, size_t list_size)
{
	uint8_t signature[64];
	size_t start = 0, end = signature_size, context = strlen(LIST_CONTEXT), i;
	uint8_t *message;
	int good = 0;

	while (start < end && (signature_text[start] == ' ' || signature_text[start] == '\n' ||
		signature_text[start] == '\r' || signature_text[start] == '\t'))
		start++;
	while (end > start && (signature_text[end - 1] == ' ' || signature_text[end - 1] == '\n' ||
		signature_text[end - 1] == '\r' || signature_text[end - 1] == '\t'))
		end--;
	if (!hex_bytes(signature_text + start, end - start, signature, sizeof(signature)))
		return 0;
	message = malloc(context + list_size);
	if (!message)
		return 0;
	memcpy(message, LIST_CONTEXT, context);
	memcpy(message + context, list, list_size);
	for (i = 0; i < sizeof(list_keys) / sizeof(list_keys[0]) && !good; i++)
		good = crypto_ed25519_check(signature, list_keys[i], message, context + list_size) == 0;
	free(message);
	return good;
}

/* the list's serial (its "# serial" line), 0 for none */
static unsigned long long list_serial(unsigned char const *list, size_t size)
{
	static char const key[] = "# serial ";
	size_t at = 0;

	while (at < size)
	{
		size_t end = at;

		while (end < size && list[end] != '\n')
			end++;
		if (list[at] != '#')
			break;
		if (end - at > sizeof(key) - 1 && !memcmp(list + at, key, sizeof(key) - 1))
			return strtoull((char const *)list + at + sizeof(key) - 1, NULL, 10);
		at = end + 1;
	}
	return 0;
}

/* the list's maps (its resource maps left out): their count, -1 if it is
not the list's format */
static int list_parse(unsigned char const *list, size_t size, struct map **maps_out)
{
	size_t at = 0, lines = 0, i;
	struct map *maps;
	int count = 0;

	*maps_out = NULL;
	if (size < sizeof(LIST_FORMAT) - 1 || memcmp(list, LIST_FORMAT, sizeof(LIST_FORMAT) - 1))
		return -1;
	for (i = 0; i < size; i++)
		lines += list[i] == '\n';
	maps = calloc(lines + 1, sizeof(*maps));
	if (!maps)
		return -1;
	while (at < size)
	{
		char const *fields[8];
		size_t lengths[8];
		size_t end = at, field = 0, start;
		struct map map;

		while (end < size && list[end] != '\n')
			end++;
		start = at;
		at = end + 1;
		if (end > start && list[end - 1] == '\r')
			end--;
		if (end == start || list[start] == '#')
			continue;
		/* (the fields up to blake2b's: name, checksum, size, piece length,
		info hash, blake2b:<hash>) */
		while (field < 6 && start <= end)
		{
			size_t stop = start;

			while (stop < end && list[stop] != '\t')
				stop++;
			fields[field] = (char const *)list + start;
			lengths[field] = stop - start;
			field++;
			start = stop + 1;
		}
		if (field < 6 || !lengths[0] || lengths[0] >= NAME_SIZE || lengths[1] != 8 || lengths[5] != 8 + 64 ||
			memcmp(fields[5], "blake2b:", 8))
			continue;
		memset(&map, 0, sizeof(map));
		memcpy(map.name, fields[0], lengths[0]);
		/* (a name with a folder in it is no file of custom_maps) */
		if (strchr(map.name, '/') || strchr(map.name, '\\') || map.name[0] == '.')
			continue;
		map.checksum = strtoul(fields[1], NULL, 16);
		map.size = strtoull(fields[2], NULL, 10);
		if (!hex_bytes(fields[5] + 8, 64, map.blake2b, 32) || !map.size)
			continue;
		/* (the resource maps, matched by name alone: no map to play) */
		if (!map.checksum)
			continue;
		maps[count++] = map;
	}
	*maps_out = maps;
	return count;
}

static int compare_by_name(void const *a, void const *b)
{
	int x = *(int const *)a, y = *(int const *)b;
	int order = name_compare(archive.maps[x].name, archive.maps[y].name);

	return order ? order : x - y;
}

static int compare_by_size(void const *a, void const *b)
{
	int x = *(int const *)a, y = *(int const *)b;
	unsigned long long p = archive.maps[x].size, q = archive.maps[y].size;

	if (p != q)
		return archive.sort == SORT_SMALLEST ? (p < q ? -1 : 1) : (p > q ? -1 : 1);
	return compare_by_name(a, b);
}

/* (locked) the maps shown again, by the search, show and sort */
static void shown_build(void)
{
	int i;

	archive.shown_count = 0;
	if (!archive.shown)
		return;
	for (i = 0; i < archive.map_count; i++)
	{
		struct map const *map = &archive.maps[i];

		if (!contains(map->name, archive.search))
			continue;
		if (archive.show == SHOW_INSTALLED && map->installed == INSTALLED_NONE)
			continue;
		if (archive.show == SHOW_MISSING && map->installed == INSTALLED_SAME)
			continue;
		archive.shown[archive.shown_count++] = i;
	}
	qsort(archive.shown, (size_t)archive.shown_count, sizeof(int),
		archive.sort == SORT_NAME ? compare_by_name : compare_by_size);
	if ((archive.page - 1) * PAGE_MAPS >= archive.shown_count)
		archive.page = 1;
}

/* a file whole, at most maximum bytes (malloc'd): 1 with its size */
static int file_read(char const *path, unsigned long maximum, unsigned char **data, size_t *size)
{
	FILE *file = fopen(path, "rb");
	long length;

	*data = NULL;
	*size = 0;
	if (!file)
		return 0;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || (unsigned long)length > maximum ||
		fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return 0;
	}
	*data = malloc((size_t)length + 1);
	if (!*data || fread(*data, 1, (size_t)length, file) != (size_t)length)
	{
		free(*data);
		*data = NULL;
		fclose(file);
		return 0;
	}
	fclose(file);
	(*data)[length] = 0;
	*size = (size_t)length;
	return 1;
}

static int file_write(char const *path, unsigned char const *data, size_t size)
{
	char part[PATH_SIZE + 8];
	FILE *file;
	int good;

	snprintf(part, sizeof(part), "%s.part", path);
	file = fopen(part, "wb");
	if (!file)
		return 0;
	good = fwrite(data, 1, size, file) == size;
	good = fclose(file) == 0 && good;
	if (good)
	{
		remove(path);
		good = rename(part, path) == 0;
	}
	if (!good)
		remove(part);
	return good;
}

/* (thread) the list taken in place of the one in use, if it is signed and
newer: 1 if taken */
static int list_take(unsigned char const *list, size_t list_size, char const *signature, size_t signature_size,
	char const *source, char *error, int error_size)
{
	struct map *maps;
	int count;
	unsigned long long serial;

	if (!list_signed(signature, signature_size, list, list_size))
	{
		snprintf(error, (size_t)error_size, "the list from %s is not signed by FractumSeraph's key", source);
		return 0;
	}
	serial = list_serial(list, list_size);
	pthread_mutex_lock(&archive_lock);
	if (archive.list_state == LIST_READY && serial <= archive.serial)
	{
		pthread_mutex_unlock(&archive_lock);
		return 0;
	}
	pthread_mutex_unlock(&archive_lock);
	count = list_parse(list, list_size, &maps);
	if (count <= 0)
	{
		free(maps);
		snprintf(error, (size_t)error_size, "the list from %s is not one of maps", source);
		return 0;
	}
	pthread_mutex_lock(&archive_lock);
	/* (no download under way or waiting holds an index into the old list:
	the thread taking this is the one that downloads) */
	free(archive.maps);
	free(archive.shown);
	archive.maps = maps;
	archive.map_count = count;
	archive.shown = malloc((size_t)count * sizeof(int));
	archive.serial = serial;
	archive.list_state = LIST_READY;
	archive.queue_count = 0;
	archive.confirm = -1;
	archive.want_scan = 1;
	shown_build();
	pthread_mutex_unlock(&archive_lock);
	steam_log("map archive: the list from %s is in use: serial %llu, %d maps", source, serial, count);
	return 1;
}

/* (thread) the list kept, then GitHub's */
static void list_load(void)
{
	char list_path[PATH_SIZE], signature_path[PATH_SIZE], folder[PATH_SIZE], error[MESSAGE_SIZE];
	unsigned char *list = NULL, *signature = NULL;
	unsigned long list_size = 0, signature_size = 0;
	size_t kept_list_size, kept_signature_size;
	int status = 0, kept = 0;

	error[0] = 0;
	path_join(folder, sizeof(folder), archive.data_root, "map_archive");
	path_join(list_path, sizeof(list_path), folder, "map_torrents.txt");
	path_join(signature_path, sizeof(signature_path), folder, "map_torrents.txt.sig");
	if (file_read(list_path, LIST_MAXIMUM_SIZE, &list, &kept_list_size) &&
		file_read(signature_path, 4096, &signature, &kept_signature_size))
	{
		kept = list_take(list, kept_list_size, (char const *)signature, kept_signature_size, "this device", error,
			(int)sizeof(error));
	}
	free(list);
	free(signature);
	list = signature = NULL;
	if (steam_https_get(LIST_URL, LIST_MAXIMUM_SIZE, &list, &list_size, &status, error, (int)sizeof(error)) &&
		status == 200 &&
		steam_https_get(SIGNATURE_URL, 4096, &signature, &signature_size, &status, error, (int)sizeof(error)) &&
		status == 200)
	{
		if (list_take(list, list_size, (char const *)signature, signature_size, "GitHub", error, (int)sizeof(error)))
		{
			posix_make_directory(folder);
			if (!file_write(list_path, list, list_size) ||
				!file_write(signature_path, signature, signature_size))
				steam_log("map archive: the list could not be kept in %s", folder);
		}
	}
	else if (!error[0])
		snprintf(error, sizeof(error), "GitHub answered %d", status);
	free(list);
	free(signature);
	pthread_mutex_lock(&archive_lock);
	if (archive.list_state != LIST_READY)
	{
		archive.list_state = LIST_FAILED;
		snprintf(archive.list_error, sizeof(archive.list_error), "%s", error[0] ? error : "no list");
		steam_log("map archive: no list: %s", archive.list_error);
	}
	else if (error[0] && !kept)
		steam_log("map archive: kept the list in use (%s)", error);
	pthread_mutex_unlock(&archive_lock);
}

/* ---------- what is installed */

struct file_entry
{
	char name[NAME_SIZE];
	unsigned long long size;
};

static int compare_files(void const *a, void const *b)
{
	return name_compare(((struct file_entry const *)a)->name, ((struct file_entry const *)b)->name);
}

/* (thread) custom_maps listed: each map's installed */
static void installed_scan(void)
{
	struct file_entry *files = NULL;
	int count = 0, capacity = 0, i;
	char name[260];
	void *directory = posix_directory_open(archive.custom_maps);

	while (directory && posix_directory_next(directory, name, sizeof(name)))
	{
		char path[PATH_SIZE];
		struct posix_file_information information;
		size_t length = strlen(name);

		if (length < 5 || length >= NAME_SIZE || !names_equal(name + length - 4, ".map"))
			continue;
		path_join(path, sizeof(path), archive.custom_maps, name);
		if (posix_stat(path, &information) != 0 || (information.flags & _posix_file_is_directory))
			continue;
		if (count == capacity)
		{
			struct file_entry *grown = realloc(files, (size_t)(capacity ? capacity * 2 : 256) * sizeof(*files));

			if (!grown)
				break;
			files = grown;
			capacity = capacity ? capacity * 2 : 256;
		}
		snprintf(files[count].name, sizeof(files[count].name), "%s", name);
		files[count].size = ((unsigned long long)information.size_high << 32) | information.size_low;
		count++;
	}
	if (directory)
		posix_directory_close(directory);
	if (count)
		qsort(files, (size_t)count, sizeof(*files), compare_files);
	pthread_mutex_lock(&archive_lock);
	for (i = 0; i < archive.map_count; i++)
	{
		struct file_entry key;
		struct file_entry const *found;

		snprintf(key.name, sizeof(key.name), "%s", archive.maps[i].name);
		found = count ? bsearch(&key, files, (size_t)count, sizeof(*files), compare_files) : NULL;
		archive.maps[i].installed = !found ? INSTALLED_NONE :
			found->size == archive.maps[i].size ? INSTALLED_SAME : INSTALLED_OTHER;
	}
	shown_build();
	pthread_mutex_unlock(&archive_lock);
	free(files);
}

/* (thread) the map's file named in custom_maps/downloaded_maps.txt, if it
is not yet (map_share.c's downloaded_maps_mark) */
static void downloaded_mark(char const *name)
{
	char path[PATH_SIZE], line[NAME_SIZE + 8];
	FILE *file;

	path_join(path, sizeof(path), archive.custom_maps, "downloaded_maps.txt");
	file = fopen(path, "rb");
	while (file && fgets(line, sizeof(line), file))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && names_equal(line, name))
		{
			fclose(file);
			return;
		}
	}
	if (file)
		fclose(file);
	file = fopen(path, "ab");
	if (!file)
	{
		steam_log("map archive: could not name %s in %s", name, path);
		return;
	}
	fprintf(file, "%s\r\n", name);
	fclose(file);
}

/* ---------- a download */

static int send_all(int socket, char const *data, size_t size)
{
	while (size)
	{
		int sent;

		if (steam_socket_wait(socket, 20000, 1) <= 0)
			return 0;
		sent = posix_socket_send(socket, data, (int)size, 0);
		if (sent <= 0)
			return 0;
		data += sent;
		size -= (size_t)sent;
	}
	return 1;
}

static int receive(int socket, char *buffer, size_t size)
{
	if (steam_socket_wait(socket, 20000, 0) <= 0)
		return -1;
	return posix_socket_recv(socket, buffer, (int)size, 0);
}

/* (thread) the map downloaded into custom_maps: 1, else 0 and why */
static int download(struct map const *map, char *error, int error_size)
{
	enum { BLOCK = 64 * 1024 };
	char request[1024], path[PATH_SIZE], part[PATH_SIZE + 8];
	char *buffer = malloc(BLOCK + 1);
	char const *at;
	size_t have = 0, i, used;
	char *end = NULL;
	unsigned long long written = 0;
	crypto_blake2b_ctx hash;
	uint8_t digest[32];
	FILE *file = NULL;
	int socket = -1, good = 0, status;
	long long length = -1;

	if (!buffer)
	{
		snprintf(error, (size_t)error_size, "out of memory");
		return 0;
	}
	/* (the name percent-encoded: spaces, brackets and letters past ASCII
	are in some) */
	used = (size_t)snprintf(request, sizeof(request), "GET " MAPS_PATH);
	for (at = map->name; *at && used + 4 < sizeof(request); at++)
	{
		unsigned char c = (unsigned char)*at;

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
			c == '.' || c == '~')
			request[used++] = (char)c;
		else
			used += (size_t)snprintf(request + used, sizeof(request) - used, "%%%02X", c);
	}
	snprintf(request + used, sizeof(request) - used, " HTTP/1.1\r\nHost: " MAPS_HOST "\r\nUser-Agent: " USER_AGENT
		"\r\nAccept: */*\r\nConnection: close\r\n\r\n");
	path_join(path, sizeof(path), archive.custom_maps, map->name);
	snprintf(part, sizeof(part), "%s.part", path);
	socket = steam_tcp_connect(MAPS_HOST, 80, 10000);
	if (socket < 0)
	{
		snprintf(error, (size_t)error_size, "the archive (" MAPS_HOST ") could not be reached");
		goto done;
	}
	if (!send_all(socket, request, strlen(request)))
	{
		snprintf(error, (size_t)error_size, "the archive took no request");
		goto done;
	}
	/* (the head, up to its blank line) */
	while (!end)
	{
		int got;

		if (have >= BLOCK)
		{
			snprintf(error, (size_t)error_size, "the archive's answer is not HTTP");
			goto done;
		}
		got = receive(socket, buffer + have, BLOCK - have);
		if (got <= 0)
		{
			snprintf(error, (size_t)error_size, "the archive closed the connection");
			goto done;
		}
		have += (size_t)got;
		buffer[have] = 0;
		end = strstr(buffer, "\r\n\r\n");
	}
	status = !strncmp(buffer, "HTTP/1.", 7) && have > 12 ? atoi(buffer + 9) : 0;
	*end = 0;
	for (at = buffer; (at = strchr(at, '\n')) != NULL;)
	{
		static char const name[] = "content-length:";

		at++;
		for (i = 0; i < sizeof(name) - 1 && lower((unsigned char)at[i]) == name[i]; i++)
			;
		if (i == sizeof(name) - 1)
			length = strtoll(at + i, NULL, 10);
	}
	if (status != 200)
	{
		snprintf(error, (size_t)error_size, status == 404 ? "the archive does not have it" : "the archive answered %d",
			status);
		goto done;
	}
	if (length >= 0 && (unsigned long long)length != map->size)
	{
		snprintf(error, (size_t)error_size, "the archive's file is not the list's size");
		goto done;
	}
	file = fopen(part, "wb");
	if (!file)
	{
		snprintf(error, (size_t)error_size, "custom_maps could not be written to");
		goto done;
	}
	crypto_blake2b_init(&hash, 32);
	/* (what came with the head first) */
	have -= (size_t)(end + 4 - buffer);
	memmove(buffer, end + 4, have);
	for (;;)
	{
		int cancel;

		if (have)
		{
			if (written + have > map->size)
			{
				snprintf(error, (size_t)error_size, "the archive's file is larger than the list's");
				goto done;
			}
			if (fwrite(buffer, 1, have, file) != have)
			{
				snprintf(error, (size_t)error_size, "custom_maps is full, or could not be written to");
				goto done;
			}
			crypto_blake2b_update(&hash, (uint8_t const *)buffer, have);
			written += have;
			have = 0;
		}
		pthread_mutex_lock(&archive_lock);
		archive.downloaded = written;
		cancel = archive.cancel;
		pthread_mutex_unlock(&archive_lock);
		if (cancel)
		{
			snprintf(error, (size_t)error_size, "cancelled");
			goto done;
		}
		if (written == map->size)
			break;
		{
			int got = receive(socket, buffer, BLOCK);

			if (got <= 0)
			{
				snprintf(error, (size_t)error_size, "the download was cut short");
				goto done;
			}
			have = (size_t)got;
		}
	}
	if (fclose(file) != 0)
	{
		file = NULL;
		snprintf(error, (size_t)error_size, "custom_maps is full, or could not be written to");
		goto done;
	}
	file = NULL;
	crypto_blake2b_final(&hash, digest);
	if (crypto_verify32(digest, map->blake2b) != 0)
	{
		snprintf(error, (size_t)error_size, "the file is not the list's (its BLAKE2b differs)");
		goto done;
	}
	/* (another version of its name, if one is there: replaced, as asked) */
	remove(path);
	if (rename(part, path) != 0)
	{
		snprintf(error, (size_t)error_size, "the map could not be put in custom_maps");
		goto done;
	}
	downloaded_mark(map->name);
	good = 1;
done:
	if (file)
		fclose(file);
	if (!good)
		remove(part);
	if (socket >= 0)
		posix_socket_close(socket);
	free(buffer);
	return good;
}

/* ---------- the thread */

static void *archive_thread(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&archive_lock);
	for (;;)
	{
		if (archive.want_list)
		{
			archive.want_list = 0;
			archive.list_state = archive.list_state == LIST_READY ? LIST_READY : LIST_LOADING;
			pthread_mutex_unlock(&archive_lock);
			list_load();
			pthread_mutex_lock(&archive_lock);
			continue;
		}
		if (archive.want_remove[0])
		{
			char name[NAME_SIZE], path[PATH_SIZE];

			snprintf(name, sizeof(name), "%s", archive.want_remove);
			archive.want_remove[0] = 0;
			pthread_mutex_unlock(&archive_lock);
			path_join(path, sizeof(path), archive.custom_maps, name);
			if (remove(path) == 0)
			{
				steam_log("map archive: removed %s", path);
				pthread_mutex_lock(&archive_lock);
				snprintf(archive.message, sizeof(archive.message), "%s removed.", name);
				archive.rescan = 1;
			}
			else
			{
				pthread_mutex_lock(&archive_lock);
				snprintf(archive.message, sizeof(archive.message), "%s could not be removed.", name);
			}
			archive.want_scan = 1;
			continue;
		}
		if (archive.want_scan)
		{
			archive.want_scan = 0;
			pthread_mutex_unlock(&archive_lock);
			installed_scan();
			pthread_mutex_lock(&archive_lock);
			continue;
		}
		if (archive.queue_count && archive.list_state == LIST_READY)
		{
			struct map map;
			char error[200];
			int good;

			archive.downloading = archive.queue[0];
			memmove(archive.queue, archive.queue + 1, (size_t)(--archive.queue_count) * sizeof(int));
			archive.downloaded = 0;
			archive.cancel = 0;
			map = archive.maps[archive.downloading];
			pthread_mutex_unlock(&archive_lock);
			steam_log("map archive: downloading %s (%llu bytes)", map.name, map.size);
			good = download(&map, error, (int)sizeof(error));
			steam_log("map archive: %s: %s", map.name, good ? "installed" : error);
			pthread_mutex_lock(&archive_lock);
			if (good)
			{
				snprintf(archive.message, sizeof(archive.message), "%s installed.", map.name);
				archive.rescan = 1;
			}
			else
				snprintf(archive.message, sizeof(archive.message), "%s: %s.", map.name, error);
			archive.downloading = -1;
			archive.want_scan = 1;
			continue;
		}
		pthread_cond_wait(&archive_wake, &archive_lock);
	}
	return NULL;
}

/* ---------- the screen's calls */

void fractum_archive_open(char const *data_root, char const *custom_maps)
{
	pthread_t thread;
	int start;

	pthread_mutex_lock(&archive_lock);
	start = !archive.started;
	if (start)
	{
		archive.started = 1;
		archive.downloading = -1;
		archive.confirm = -1;
		archive.page = 1;
		archive.want_list = 1;
		snprintf(archive.data_root, sizeof(archive.data_root), "%s", data_root);
	}
	/* (the folder may have moved since: GAME FILES) */
	snprintf(archive.custom_maps, sizeof(archive.custom_maps), "%s", custom_maps);
	posix_make_directory(archive.custom_maps);
	/* (a list that could not be had: tried again) */
	if (archive.list_state == LIST_FAILED)
		archive.want_list = 1;
	archive.want_scan = 1;
	archive.message[0] = 0;
	archive.confirm = -1;
	archive.want_chosen = -1;
	pthread_cond_signal(&archive_wake);
	pthread_mutex_unlock(&archive_lock);
	if (start && pthread_create(&thread, NULL, archive_thread, NULL) == 0)
		pthread_detach(thread);
}

void fractum_archive_close(void)
{
	pthread_mutex_lock(&archive_lock);
	archive.confirm = -1;
	archive.want_search = 0;
	pthread_mutex_unlock(&archive_lock);
}

int fractum_archive_try_lock(void)
{
	return pthread_mutex_trylock(&archive_lock) == 0;
}

void fractum_archive_unlock(void)
{
	pthread_mutex_unlock(&archive_lock);
}

/* (locked) the maps on the page shown */
static int page_count(void)
{
	int from = (archive.page - 1) * PAGE_MAPS;
	int left = archive.shown_count - from;

	return left < 0 ? 0 : left > PAGE_MAPS ? PAGE_MAPS : left;
}

/* (locked) the row at a place; *map the map's index */
static enum row_kind row_kind(int place, int *map)
{
	int on_page;

	*map = -1;
	if (place < 0)
		return ROW_NONE;
	if (archive.list_state != LIST_READY)
		return place == 0 ? ROW_LIST : ROW_NONE;
	if (place == 0)
		return ROW_SEARCH;
	if (place == 1)
		return ROW_SORT;
	if (place == 2)
		return ROW_SHOW;
	place -= FILTER_ROWS;
	on_page = page_count();
	if (place < on_page)
	{
		*map = archive.shown[(archive.page - 1) * PAGE_MAPS + place];
		return ROW_MAP;
	}
	place -= on_page;
	if (archive.page > 1 && place-- == 0)
		return ROW_PREVIOUS;
	if (archive.page * PAGE_MAPS < archive.shown_count && place == 0)
		return ROW_NEXT;
	return ROW_NONE;
}

int fractum_archive_count(void)
{
	if (archive.list_state != LIST_READY)
		return 1;
	return FILTER_ROWS + page_count() + (archive.page > 1) + (archive.page * PAGE_MAPS < archive.shown_count);
}

/* (locked) where the map is in the queue, else -1 */
static int queued_at(int map)
{
	int i;

	for (i = 0; i < archive.queue_count; i++)
		if (archive.queue[i] == map)
			return i;
	return -1;
}

static void percent_text(char *text, int size)
{
	unsigned long long total = archive.downloading >= 0 ? archive.maps[archive.downloading].size : 0;

	snprintf(text, (size_t)size, "%d%%", total ? (int)(archive.downloaded * 100 / total) : 0);
}

void fractum_archive_row(int place, char *name, int name_size, char *state, int state_size)
{
	int map;

	name[0] = state[0] = 0;
	switch (row_kind(place, &map))
	{
	case ROW_LIST:
		snprintf(name, (size_t)name_size, "MAP LIST");
		snprintf(state, (size_t)state_size, "%s", archive.list_state == LIST_FAILED ? "TRY AGAIN" : "LOADING");
		break;
	case ROW_SEARCH:
		snprintf(name, (size_t)name_size, "SEARCH");
		snprintf(state, (size_t)state_size, "%.15s", archive.search[0] ? archive.search : "ANY");
		break;
	case ROW_SORT:
		snprintf(name, (size_t)name_size, "SORT");
		snprintf(state, (size_t)state_size, "%s", sort_names[archive.sort]);
		break;
	case ROW_SHOW:
		snprintf(name, (size_t)name_size, "SHOW");
		snprintf(state, (size_t)state_size, "%s", show_names[archive.show]);
		break;
	case ROW_MAP:
	{
		struct map const *entry = &archive.maps[map];

		display_name(entry, name, name_size);
		if (archive.downloading == map)
			percent_text(state, state_size);
		else if (queued_at(map) >= 0)
			snprintf(state, (size_t)state_size, "QUEUED");
		else if (archive.confirm == place)
			snprintf(state, (size_t)state_size, "%s", entry->installed == INSTALLED_SAME ? "REMOVE?" : "REPLACE?");
		else
		{
			char size[24];

			size_text(entry->size, size, (int)sizeof(size));
			snprintf(state, (size_t)state_size, "%s", entry->installed == INSTALLED_SAME ? "INSTALLED" :
				entry->installed == INSTALLED_OTHER ? "OTHER VERSION" : size);
		}
		break;
	}
	case ROW_PREVIOUS:
		snprintf(name, (size_t)name_size, "PREVIOUS PAGE");
		snprintf(state, (size_t)state_size, "%d", archive.page - 1);
		break;
	case ROW_NEXT:
		snprintf(name, (size_t)name_size, "NEXT PAGE");
		snprintf(state, (size_t)state_size, "%d OF %d", archive.page + 1,
			(archive.shown_count + PAGE_MAPS - 1) / PAGE_MAPS);
		break;
	default:
		break;
	}
}

void fractum_archive_details(int place, char *title, int title_size, char *text, int text_size)
{
	char body[600];
	int map;
	size_t used;

	title[0] = body[0] = 0;
	switch (row_kind(place, &map))
	{
	case ROW_LIST:
		snprintf(title, (size_t)title_size, "Map list");
		if (archive.list_state == LIST_FAILED)
			snprintf(body, sizeof(body), "The list of maps could not be had: %s. A tries again.", archive.list_error);
		else
			snprintf(body, sizeof(body), "Getting the list of maps from FractumSeraph's archive...");
		break;
	case ROW_SEARCH:
		snprintf(title, (size_t)title_size, "Search");
		snprintf(body, sizeof(body), "A types a part of a map's name. %d of %d maps match.", archive.shown_count,
			archive.map_count);
		break;
	case ROW_SORT:
		snprintf(title, (size_t)title_size, "Sort");
		snprintf(body, sizeof(body), "A sorts the maps by name, or by size.");
		break;
	case ROW_SHOW:
		snprintf(title, (size_t)title_size, "Show");
		snprintf(body, sizeof(body), "A shows every map, those in custom_maps, or those not there.");
		break;
	case ROW_MAP:
	{
		struct map const *entry = &archive.maps[map];
		char size[24];

		display_name(entry, title, title_size);
		size_text(entry->size, size, (int)sizeof(size));
		used = (size_t)snprintf(body, sizeof(body), "%s, version %08lX.\n", size, entry->checksum);
		if (archive.downloading == map)
			snprintf(body + used, sizeof(body) - used, "Downloading: A cancels.");
		else if (queued_at(map) >= 0)
			snprintf(body + used, sizeof(body) - used, "Queued: A takes it out.");
		else if (entry->installed == INSTALLED_SAME)
			snprintf(body + used, sizeof(body) - used, "In custom_maps. A, twice, removes it.");
		else if (entry->installed == INSTALLED_OTHER)
			snprintf(body + used, sizeof(body) - used, "Another version of it is in custom_maps. A, twice, "
				"replaces it with this one.");
		else
			snprintf(body + used, sizeof(body) - used, "A downloads it into custom_maps.");
		break;
	}
	case ROW_PREVIOUS:
	case ROW_NEXT:
		snprintf(title, (size_t)title_size, "%d maps", archive.shown_count);
		snprintf(body, sizeof(body), "Page %d of %d.", archive.page,
			(archive.shown_count + PAGE_MAPS - 1) / PAGE_MAPS);
		break;
	default:
		break;
	}
	text[0] = 0;
	if (archive.message[0])
		snprintf(text, (size_t)text_size, "%s\n", archive.message);
	used = strlen(text);
	snprintf(text + used, (size_t)text_size - used, "%s", body);
	if (archive.downloading >= 0 && map != archive.downloading)
	{
		char shown[NAME_SIZE], percent[8];

		display_name(&archive.maps[archive.downloading], shown, (int)sizeof(shown));
		percent_text(percent, (int)sizeof(percent));
		used = strlen(text);
		snprintf(text + used, (size_t)text_size - used, "\nDownloading %s: %s%s", shown, percent,
			archive.queue_count ? "" : ".");
		if (archive.queue_count)
		{
			used = strlen(text);
			snprintf(text + used, (size_t)text_size - used, ", %d more queued.", archive.queue_count);
		}
	}
}

int fractum_archive_take_search(char *initial, int initial_size)
{
	if (!archive.want_search)
		return 0;
	archive.want_search = 0;
	snprintf(initial, (size_t)initial_size, "%s", archive.search);
	return 1;
}

int fractum_archive_take_rescan(void)
{
	int rescan = archive.rescan;

	archive.rescan = 0;
	return rescan;
}

int fractum_archive_take_chosen(void)
{
	int chosen = archive.want_chosen;

	archive.want_chosen = -1;
	return chosen;
}

void fractum_archive_choose(int place)
{
	int map, confirm;

	pthread_mutex_lock(&archive_lock);
	confirm = archive.confirm;
	archive.confirm = -1;
	archive.message[0] = 0;
	switch (row_kind(place, &map))
	{
	case ROW_LIST:
		if (archive.list_state == LIST_FAILED)
			archive.want_list = 1;
		break;
	case ROW_SEARCH:
		archive.want_search = 1;
		break;
	case ROW_SORT:
		archive.sort = (enum sort)((archive.sort + 1) % SORT_COUNT);
		archive.page = 1;
		shown_build();
		break;
	case ROW_SHOW:
		archive.show = (enum show)((archive.show + 1) % SHOW_COUNT);
		archive.page = 1;
		shown_build();
		break;
	case ROW_MAP:
	{
		int at = queued_at(map);

		if (archive.downloading == map)
			archive.cancel = 1;
		else if (at >= 0)
		{
			memmove(archive.queue + at, archive.queue + at + 1, (size_t)(archive.queue_count - at - 1) * sizeof(int));
			archive.queue_count--;
			snprintf(archive.message, sizeof(archive.message), "Taken out of the queue.");
		}
		else if (archive.maps[map].installed != INSTALLED_NONE && confirm != place)
			archive.confirm = place;
		else if (archive.maps[map].installed == INSTALLED_SAME)
			snprintf(archive.want_remove, sizeof(archive.want_remove), "%s", archive.maps[map].name);
		else if (archive.queue_count == QUEUE_SIZE)
			snprintf(archive.message, sizeof(archive.message), "The queue is full: wait for some to finish.");
		else
		{
			archive.queue[archive.queue_count++] = map;
			if (archive.downloading >= 0)
				snprintf(archive.message, sizeof(archive.message), "Queued (%d waiting).", archive.queue_count);
		}
		break;
	}
	case ROW_PREVIOUS:
		archive.page--;
		archive.want_chosen = FILTER_ROWS;
		break;
	case ROW_NEXT:
		archive.page++;
		archive.want_chosen = FILTER_ROWS;
		break;
	default:
		break;
	}
	pthread_cond_signal(&archive_wake);
	pthread_mutex_unlock(&archive_lock);
}

int fractum_archive_back(void)
{
	int taken = 0;

	pthread_mutex_lock(&archive_lock);
	if (archive.confirm >= 0)
	{
		archive.confirm = -1;
		taken = 1;
	}
	/* (the screen is not left while maps are coming, as the Workshop's: the
	queue would go on with no screen to show it) */
	else if (archive.downloading >= 0 || archive.queue_count)
	{
		int going = (archive.downloading >= 0) + archive.queue_count;

		snprintf(archive.message, sizeof(archive.message), "%d download%s going: wait, or cancel %s, to leave.",
			going, going == 1 ? "" : "s", going == 1 ? "it" : "them");
		taken = 1;
	}
	pthread_mutex_unlock(&archive_lock);
	return taken;
}

void fractum_archive_search(char const *text)
{
	pthread_mutex_lock(&archive_lock);
	snprintf(archive.search, sizeof(archive.search), "%s", text);
	archive.page = 1;
	shown_build();
	archive.want_chosen = archive.shown_count ? FILTER_ROWS : 0;
	pthread_mutex_unlock(&archive_lock);
}
