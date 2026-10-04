#pragma once
#include <enet/enet.h>
#include <chrono>
#include <cstdint>
#include <string>

#include "../core/core.hpp"
#include "../player/player.hpp"
#include "../utils/byte_stream.hpp"

namespace client {
class Client final {
public:
    explicit Client(core::Core* core);
    ~Client();

    [[nodiscard]] ENetPeer* connect(const std::string& host, enet_uint16 port);
    void process();

    void on_connect(ENetPeer* peer);
    void on_receive(ENetPeer* peer, ENetPacket* packet);
    void on_disconnect(ENetPeer* peer);

    // Tears the upstream session down because the local client went away (as opposed
    // to the server dropping us). Without this the upstream player object survived
    // the quit still wrapping a dead peer; lingering peers from previous sessions
    // then fired disconnect events that matched the stale object and killed the
    // next live session the moment it connected.
    void on_local_disconnect();

    [[nodiscard]] player::Player* get_player() const { return player_; }
    [[nodiscard]] bool is_connected() const { return is_connected_; }

private:

    void handle_server_hello(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player);
    void handle_text_message(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player, ENetPeer* peer);
    void handle_game_packet(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player, ENetPeer* peer);
    void handle_unknown_packet(std::uint32_t raw_type, ByteStream<std::uint16_t>& byte_stream, player::Player* to_player, ENetPeer* peer);
    void handle_redirected_packet(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player);

    ENetHost* host_;
    core::Core* core_;
    player::Player* player_;
    bool is_connected_;

    // connectID of the peer the current upstream session runs on. Peer objects are
    // reused by ENet once their slot is freed, so a bare pointer comparison cannot
    // tell a live session from a stale event delivered on a recycled peer.
    std::uint32_t active_upstream_connect_id_ = 0;

    // In-flight upstream handshake, watched by process(): the old watchdog only ran
    // in the non-HTTPS flow, so a handshake the game server never answered (usually
    // because the previous session was still open) hung silently - the game client
    // timed out, retried, and tripped the server's login rate limiter.
    ENetPeer* pending_connect_peer_ = nullptr;
    std::chrono::steady_clock::time_point pending_connect_since_{};
    std::string connect_target_host_;
    enet_uint16 connect_target_port_ = 0;
    int connect_attempts_ = 0;

    void retry_or_fail_pending_connect(const char* reason);
};
}
