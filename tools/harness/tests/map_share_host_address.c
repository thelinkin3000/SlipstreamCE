/*
MAP_SHARE_HOST_ADDRESS.C (test)

The address a map sharing download (port/linux/game/map_share.c) reaches its
host at: its client connection's, which has none while it is being made
(asked during the game search, and the join's first frames), and is another
host's on a connection used again. Kept a second for each frame's asks, but
not for the download's: on a LAN the host's offer and sources come within that
second, and a download that took none, or the last host's, never reached the
host and failed a minute later, "no player sent any of the map".
test_map_share_host_address.py takes the constants and structures (config.inc)
and the code (under_test.inc).
*/

#include "harness.h"

typedef unsigned short word;
/* (a connection's socket: xnet.c's Winsock) */
typedef int SOCKET;

#include "config.inc"

#define csmemset(destination, value, size) memset(destination, value, size)
#define csmemcpy(destination, source, size) memcpy(destination, source, size)
#define csmemcmp(first, second, size) memcmp(first, second, size)
#define NUMBEROF(array) (sizeof(array) / sizeof((array)[0]))

#define ADDRESS(a, b, c, d) \
	((unsigned long)(a) << 24 | (unsigned long)(b) << 16 | (unsigned long)(c) << 8 | (unsigned long)(d))

/* the fake world: a clock, the client's connection (the host's address once
connected: 0.0.0.0 before, as network_connection_get_address says when the
socket has no peer), and the connects the code under test made */
enum
{
	IPV4_ADDRESS_LENGTH = 4,
	LOCAL_MACHINE = 2,
};

struct transport_address
{
	union
	{
		unsigned long long_words[4];
		byte bytes[16];
	} address;
	short address_length;
	word port;
};

struct network_game_client
{
	int unused;
};

struct network_connection
{
	boolean connected;
	unsigned long address;
};

static unsigned long fake_now = 1000;
static struct network_game_client fake_client;
static struct network_connection fake_connection;
static int fake_connects;
static unsigned long fake_connected_to;

static struct map_share_globals map_share_globals;

static unsigned long system_milliseconds(void)
{
	return fake_now;
}

static struct network_game_client *global_network_game_client_get(void)
{
	return &fake_client;
}

static struct network_connection *network_game_client_get_connection(struct network_game_client *client)
{
	(void)client;
	return &fake_connection;
}

static void network_connection_get_address(struct network_connection *connection,
	struct transport_address *reliable_address, struct transport_address *unreliable_address)
{
	memset(reliable_address, 0, sizeof(*reliable_address));
	reliable_address->address_length = IPV4_ADDRESS_LENGTH;
	if (connection->connected)
		reliable_address->address.long_words[0] = connection->address;
	if (unreliable_address)
		*unreliable_address = *reliable_address;
}

static short network_game_client_get_local_machine_index(void)
{
	return LOCAL_MACHINE;
}

static unsigned long host_order(unsigned long address)
{
	return address;
}

static boolean p2p_peer_reached(byte const *identifier, unsigned long *address)
{
	(void)identifier;
	(void)address;
	return FALSE;
}

static void p2p_reach_player(byte const *key_hash)
{
	(void)key_hash;
}

static struct connection *connection_new(void)
{
	short index;

	for (index = 0; index < MAXIMUM_CONNECTIONS; index++)
	{
		struct connection *connection = &map_share_globals.connections[index];

		if (!connection->used)
		{
			memset(connection, 0, sizeof(*connection));
			connection->used = TRUE;
			connection->source = NONE;
			return connection;
		}
	}
	return NULL;
}

static void connection_set_files(struct connection *connection)
{
	(void)connection;
}

static void connection_connect(struct connection *connection, unsigned long address)
{
	connection->address = address;
	connection->state = _connection_connecting;
	fake_connects++;
	fake_connected_to = address;
}

#include "under_test.inc"

/* a fresh client, its connection not made yet, and no download */
static void client_setup(void)
{
	memset(&map_share_globals, 0, sizeof(map_share_globals));
	memset(&fake_connection, 0, sizeof(fake_connection));
	fake_connects = 0;
	fake_connected_to = 0;
	map_share_globals.download.state = _download_transferring;
}

/* the host's sources message: no other machine (the host itself is the
client's connection's) */
static void sources_come(void)
{
	struct distributed_map_share_sources sources;
	struct distributed_map_share_source entries[1];

	memset(&sources, 0, sizeof(sources));
	memset(entries, 0, sizeof(entries));
	download_sources(&sources, entries);
}

/* the game's frames for a while: each asks the host's address (map_share_update), and
the download reaches its sources (download_update) */
static void frames(int count)
{
	int frame;

	for (frame = 0; frame < count; frame++)
	{
		(void)map_share_host_address();
		download_reach_sources();
		fake_now += 8;
	}
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* the address asked before the connection was made is none: not kept */
	CASE("none-not-kept")
	{
		client_setup();
		CHECK(map_share_host_address() == 0, "an address before the connection was made");
		fake_now += 10;
		fake_connection.connected = TRUE;
		fake_connection.address = ADDRESS(100, 73, 99, 56);
		CHECK(map_share_host_address() == ADDRESS(100, 73, 99, 56),
			"the address asked just before the connection was made kept for a second: %08lx",
			map_share_host_address());
		return 0;
	}
	/* the join, the request and the host's answer within the second after the
	game search asked the address: the download reaches the host */
	CASE("sources-within-the-second")
	{
		client_setup();
		frames(10);
		fake_connection.connected = TRUE;
		fake_connection.address = ADDRESS(192, 168, 0, 52);
		frames(2);
		sources_come();
		frames(2);
		CHECK(fake_connects == 1 && fake_connected_to == ADDRESS(192, 168, 0, 52),
			"the download did not reach its host (%d connects, the last to %08lx)", fake_connects,
			fake_connected_to);
		return 0;
	}
	/* the same connection used again for another host, its sources within
	the second: the new host is reached, not the last */
	CASE("sources-of-another-host")
	{
		client_setup();
		fake_connection.connected = TRUE;
		fake_connection.address = ADDRESS(192, 168, 0, 52);
		frames(3);
		fake_connection.address = ADDRESS(192, 168, 0, 35);
		sources_come();
		frames(2);
		CHECK(fake_connects == 1 && fake_connected_to == ADDRESS(192, 168, 0, 35),
			"the download reached %08lx, not the game's host (%d connects)", fake_connected_to, fake_connects);
		return 0;
	}
	/* the sources before the connection has an address: the host reached
	once it has one, not never */
	CASE("address-later")
	{
		client_setup();
		sources_come();
		frames(5);
		CHECK(fake_connects == 0, "a connect to no address");
		fake_connection.connected = TRUE;
		fake_connection.address = ADDRESS(100, 73, 99, 56);
		frames(2);
		CHECK(fake_connects == 1 && fake_connected_to == ADDRESS(100, 73, 99, 56),
			"the host never reached once the connection had its address (%d connects)", fake_connects);
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
