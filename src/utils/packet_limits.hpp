#pragma once
#include <cstddef>

// Shared packet-size limits.
//
// These exist so the receive cap, the send cap and the per-direction caps can never
// drift apart again. A packet that is accepted on one side but refused on the other is
// silently discarded, which surfaces to the player as a random disconnect (typically a
// world that never finishes loading, or an item database that never arrives).
//
// ENet itself permits ENET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE (32 MiB) per packet and
// fragments larger payloads transparently, so this ceiling is ENet's, not ours.
namespace packet {
inline constexpr std::size_t kMaxPacketSize = 32u * 1024u * 1024u;

// Smallest meaningful message: a 4-byte NetMessageType and nothing else.
inline constexpr std::size_t kMinPacketSize = 4u;
}
