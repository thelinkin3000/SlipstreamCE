"""The address a map sharing download (map_share.c) reaches its host at: its connection's, none while it is being made
and another host's on a connection used again, so not the second-old one each frame's asks keep: the host's offer and
sources come within that second on a LAN, and a download that took either never reached the host."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["none-not-kept", "sources-within-the-second", "sources-of-another-host", "address-later"]

# a fault in map_share.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    "none-kept": (("cached_connection = cached_address ? connection : NULL;", "cached_connection = connection;"),
                  "map_share_host_address_fresh", "none-not-kept"),
    "kept-for-the-download": (("if (!fresh && connection == cached_connection",
                               "if (connection == cached_connection"),
                              "map_share_host_address_fresh", "sources-of-another-host"),
    "host-not-asked-again": (("\t\tif (!source->address)\n\t\t\tsource->address = map_share_host_address_fresh(TRUE);\n",
                              ""), "source_address", "address-later"),
}

FUNCTIONS = ("elapsed", "address_internet", "address_private", "source_address_allowed", "map_share_host",
             "map_share_host_address_now", "map_share_host_address_fresh", "map_share_host_address",
             "download_sources", "source_address", "download_reach_sources")


def generated(fault=None, faulty_function=None):
    header = read("port/linux/game/map_share.h")
    source = read("port/linux/game/map_share.c")
    config = "#define P2P_MAP_SHARE_PORT %d\n" % constant(read("port/linux/src/p2p.h"), "P2P_MAP_SHARE_PORT")
    config += "#define P2P_PLAYER_KEY_HASH_SIZE %d\n" % constant(read("port/linux/src/p2p.h"),
                                                                 "P2P_PLAYER_KEY_HASH_SIZE")
    config += "#define HALO_PORT_MAXIMUM_NETWORK_MACHINES %d\n" % constant(
        read("port/linux/include/halo_port_limits.h"), "HALO_PORT_MAXIMUM_NETWORK_MACHINES")
    config += enum_with(header, "MAP_SHARE_HASH_BYTES") + "\n"
    for member in ("OFFER_TIMEOUT", "_offer_in_progress_bit", "_source_internet_bit", "_download_none",
                   "_connection_connecting"):
        config += enum_with(source, member) + "\n"
    for name in ("distributed_map_share_request", "distributed_map_share_sources", "distributed_map_share_source",
                 "download_file", "source", "download", "request", "serving", "connection", "host_machine",
                 "map_share_globals"):
        config += structure(source, name) + "\n"
    text = ""
    for name in FUNCTIONS:
        code = function(source, name)
        if fault and name == faulty_function:
            code = mutated(code, *fault)
        text += code + "\n"
    return (("config.inc", config), ("under_test.inc", text))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("map_share_host_address", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty_function, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("map_share_host_address", generated(fault, faulty_function)), case)
    assert status == CHECK_FAILED, f"map_share.c with a fault ({control}) passed '{case}': the test cannot see it"
