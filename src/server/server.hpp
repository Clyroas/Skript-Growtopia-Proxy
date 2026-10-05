#pragma once
#include <enet/enet.h>

#include <cstdint>
#include <deque>

#include "../core/core.hpp"
#include "../player/player.hpp"
#include "../utils/byte_stream.hpp"

namespace server {
class Server final {
public:
    explicit Server(core::Core* core);
    ~Server();

    void process();

    void on_connect(ENetPeer* peer);
    void on_receive(ENetPeer* peer, ENetPacket* packet);
    void on_disconnect(ENetPeer* peer);

    // Replays packets the client sent while the upstream handshake was still in
    // flight. Called by Client::on_connect once the upstream peer exists.
    void flush_pending_from_client();

    [[nodiscard]] player::Player* get_player() const { return player_; }

private:
    void handle_redirected_packet(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player);
    void clear_pending();

    ENetHost* host_;
    core::Core* core_;
    player::Player* player_;

    // connectID of the client-facing peer the current session runs on. ENet recycles
    // peer objects once their slot is freed, so pointer identity alone cannot tell a
    // live session from a stale event delivered on a recycled peer.
    std::uint32_t active_client_connect_id_ = 0;

    // Client packets that arrived before the upstream link was ready. All access
    // happens under the shared ENet traffic lock (on_receive, flush, clear).
    std::deque<ENetPacket*> pending_from_client_;
};
}