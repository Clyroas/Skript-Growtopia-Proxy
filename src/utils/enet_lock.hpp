#pragma once
#include <mutex>

// Global serialisation for ENet access.
//
// ENet is not thread-safe: enet_host_service() walks and reorders the same intrusive
// command/reliable lists that enet_peer_send() inserts into. In this proxy those two
// operations ran on different threads -- the relay thread serviced both hosts while
// detached automation workers (dropat, autocollect, autocomp, packet_utils, ...) and the
// ImGui command thread called send_packet() directly. Concurrent list mutation corrupts
// ENet's reliable-sequence bookkeeping, which does not fail immediately: the peer simply
// stops advancing and times out 5-20 seconds later, i.e. an apparently random disconnect.
//
// Every enet_host_service / enet_peer_send / enet_peer_disconnect / enet_host_flush call
// must hold this lock.
//
// A recursive mutex is used because enet_host_service() dispatches events whose listeners
// may send packets (and enet_peer_disconnect internally queues commands), so the same
// thread can legitimately re-enter.
namespace net {
inline std::recursive_mutex& enet_traffic_mutex()
{
    static std::recursive_mutex mutex{};
    return mutex;
}

using ENetLock = std::lock_guard<std::recursive_mutex>;

inline ENetLock lock_enet()
{
    return ENetLock{ enet_traffic_mutex() };
}
}
