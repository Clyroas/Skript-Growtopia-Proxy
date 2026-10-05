#pragma once
#include <vector>
#include <string>
#include <enet/enet.h>
#include "../utils/enet_lock.hpp"

namespace player {
class Player  {
public:
    Player() : peer_{ nullptr } {}
    Player(const Player& other) noexcept { peer_ = other.peer_; }
    explicit Player(ENetPeer* peer) : peer_{ peer } {}
    ~Player() = default;

    // peer_ is legitimately null for a default-constructed Player, so these must not
    // dereference it unconditionally (previously they did).
    [[nodiscard]] bool is_connected() const
    {
        return peer_ != nullptr && peer_->state == ENET_PEER_STATE_CONNECTED;
    }
    [[nodiscard]] bool is_disconnected() const
    {
        return peer_ == nullptr || peer_->state == ENET_PEER_STATE_DISCONNECTED;
    }

    void disconnect() const
    {
        if (!peer_) return;
        const net::ENetLock guard{ net::enet_traffic_mutex() };
        enet_peer_disconnect(peer_, 0);
    }
    void disconnect_now() const
    {
        if (!peer_) return;
        const net::ENetLock guard{ net::enet_traffic_mutex() };
        enet_peer_disconnect_now(peer_, 0);
    }
    void disconnect_later() const
    {
        if (!peer_) return;
        const net::ENetLock guard{ net::enet_traffic_mutex() };
        enet_peer_disconnect_later(peer_, 0);
    }

    [[nodiscard]] bool send_packet(const std::vector<std::byte>& data, int channel = 0) const;
    [[nodiscard]] bool send_packet_unreliable(const std::vector<std::byte>& data, int channel = 0) const;
    void send_log(const std::string& message) const;

    [[nodiscard]] ENetPeer* get_peer() const { return peer_; }

private:
    ENetPeer* peer_;
};
}
