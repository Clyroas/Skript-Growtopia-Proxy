#include <magic_enum/magic_enum.hpp>
#include "../utils/proxy_logger.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <chrono>

#include "client.hpp"
#include "../utils/strenc.hpp"
#include "../utils/packet_limits.hpp"
#include "../utils/enet_lock.hpp"
#include "../packet/packet_helper.hpp"
#include "../packet/message/core.hpp"
#include "../packet/packet_variant.hpp"
#include "../server/server.hpp"
#include "../utils/network.hpp"
#include "../utils/player_tracker.hpp"
#include "../packet/filter.hpp"
#include "../extension/command_handler/join_command.hpp"
#include "../extension/command_handler/banall_command.hpp"
#include "../extension/command_handler/doorid_command.hpp"
#include "../extension/command_handler/moddetect_command.hpp"

#include "../utils/socks5_tunnel.hpp"
#include "../utils/inventory_manager.hpp"
#include "../utils/world_manager.hpp"
#include "../utils/world_info.h"
#include <cmath>

namespace client {
namespace {

std::chrono::steady_clock::time_point g_connect_attempt_time{};
bool g_connect_pending = false;
constexpr int CONNECT_TIMEOUT_SEC = 10;


void send_connection_error_msg(player::Player* to_player) {
    if (!to_player) return;
    packet::Variant var{};
    var.add("OnConsoleMessage");
    var.add("`4[Connection Error]`` Can't reach the server. Try using a `$VPN``, or `4close GT and the proxy then relog``.");
    std::vector<std::byte> ext_data = var.serialize();
    packet::GameUpdatePacket pkt{};
    pkt.type = packet::PACKET_CALL_FUNCTION;
    pkt.net_id = -1;
    pkt.flags.extended = 1;
    pkt.data_size = static_cast<uint32_t>(ext_data.size());
    ByteStream<std::uint16_t> bs{};
    bs.write(packet::NET_MESSAGE_GAME_PACKET);
    bs.write(pkt);
    bs.write_data(ext_data.data(), ext_data.size());
    to_player->send_packet(bs.get_data(), 0);
}


} 

Client::Client(core::Core* core)
    : core_{ core }
    , player_{ nullptr }
{
    // 8 slots instead of 1: a reconnect before the old upstream peer has timed out
    // (sub-server switch, world change) would otherwise be refused for lack of a slot.
    constexpr std::size_t kPeerSlots = 8;
    host_ = enet_host_create(nullptr, kPeerSlots, 2, 0, 0);
    if (!host_) {
        return;
    }

    if (enet_host_compress_with_range_coder(host_) != 0) {
        return;
    }

    host_->checksum = enet_crc32;
    host_->usingNewPacket = 1;

    core_->get_event_dispatcher().appendListener(
        core::EventType::Connection,
        [&](const core::EventConnection& evt)
        {
            if (evt.from != core::EventFrom::FromClient) {
                return;
            }

            const core::Config& config{ core_->get_config() };
            player::Player* local = core_->get_server()->get_player();

            auto send_console = [&](const std::string& msg) {
                if (!local) return;
                packet::Variant var{};
                var.add("OnConsoleMessage");
                var.add(msg);
                std::vector<std::byte> ext_data = var.serialize();
                packet::GameUpdatePacket pkt{};
                pkt.type = packet::PACKET_CALL_FUNCTION;
                pkt.net_id = -1;
                pkt.flags.extended = 1;
                pkt.data_size = static_cast<uint32_t>(ext_data.size());
                ByteStream<std::uint16_t> bs{};
                bs.write(packet::NET_MESSAGE_GAME_PACKET);
                bs.write(pkt);
                bs.write_data(ext_data.data(), ext_data.size());
                local->send_packet(bs.get_data(), 0);
            };

            // SOCKS5 must be set up BEFORE any early return below.
            //
            // This block used to sit after `if (get_extension(0x153bd697)) return;`,
            // and IWebServerExtension carries exactly that UID and is registered
            // unconditionally in main.cpp, so the guard always fired and this code was
            // unreachable. ENet's relay in lib/enet/win32.c only engages when
            // g_socks5_active is set, which only happens here, so enabling proxy.enabled
            // silently did nothing and all traffic left on the real IP.
            //
            // The tunnel is a process-wide switch consumed by ENet's send path, so it
            // must be established whenever a client connection comes up, regardless of
            // which listener goes on to create the upstream peer.
            if (config.get<bool>("proxy.enabled")) {
                const std::string proxy_host = config.get<std::string>("proxy.host");
                const uint16_t    proxy_port = static_cast<uint16_t>(
                                                    config.get<unsigned int>("proxy.port"));
                const std::string proxy_user = config.get<std::string>("proxy.username");
                const std::string proxy_pass = config.get<std::string>("proxy.password");

                send_console("`5[SOCKS5]`` Connecting to proxy " + proxy_host
                             + ":" + std::to_string(proxy_port) + "...");
                spdlog::info("[SOCKS5] Connecting to proxy {}:{}", proxy_host, proxy_port);

                try {
                    socks5_tunnel::connect(proxy_host, proxy_port, proxy_user, proxy_pass);
                    send_console("`2[SOCKS5]`` Tunnel established — relaying game traffic via UDP ASSOCIATE");
                    spdlog::info("[SOCKS5] Tunnel established, g_socks5_active set");
                } catch (const std::exception& e) {
                    // Leave g_socks5_active at 0 so ENet falls back to a direct send
                    // rather than blackholing traffic at a relay that never answered.
                    send_console(std::string("`4[SOCKS5]`` Tunnel failed: ") + e.what()
                                 + " (falling back to a direct connection)");
                    spdlog::error("[SOCKS5] Tunnel failed: {}", e.what());
                }
            } else if (socks5_tunnel::is_active()) {
                socks5_tunnel::disconnect();
                spdlog::info("[SOCKS5] Tunnel closed (proxy.enabled is false)");
            }

            // When the HTTPS extension is present it owns the upstream connection:
            // its own Connection listener connects to the server_data.php target, and
            // cancels this event, so nothing below should run.
            if (const auto ext{ core_->get_extension(0x153bd697) }; ext) {
                return;
            }

            std::ignore = connect(
                config.get<std::string>("server.address"),
                config.get<unsigned int>("server.port")
            );
            g_connect_attempt_time = std::chrono::steady_clock::now();
            g_connect_pending = true;
        }
    );
}

Client::~Client()
{
    enet_host_destroy(host_);
    delete player_;
}

ENetPeer* Client::connect(const std::string& host, const enet_uint16 port)
{
    if (!host_) {
        spdlog::error("[ENET] connect('{}', {}) but the upstream host is null", host, port);
        return nullptr;
    }

    ENetAddress address{};
    // enet_address_set_host returns < 0 when the name cannot be resolved. Every caller
    // used to ignore this, which silently left address.host = 0 (0.0.0.0) and produced a
    // connection that could never complete.
    if (enet_address_set_host(&address, host.c_str()) < 0) {
        spdlog::error("[ENET] Could not resolve upstream host '{}'", host);
        return nullptr;
    }
    address.port = port;

    const net::ENetLock guard{ net::enet_traffic_mutex() };

    // A still-pending handshake from an earlier connect is dead weight: it never
    // completes (the server ignores it while the previous session is open) and it
    // occupies a peer slot. Reset it so only one handshake is ever in flight.
    if (pending_connect_peer_ && pending_connect_peer_->state == ENET_PEER_STATE_CONNECTING) {
        spdlog::warn("[ENET] dropping unanswered handshake to {}:{} before reconnecting",
                     connect_target_host_, connect_target_port_);
        enet_peer_reset(pending_connect_peer_);
    }
    pending_connect_peer_ = nullptr;

    ENetPeer* peer{ enet_host_connect(host_, &address, 2, 0) };
    if (!peer) {
        // Returns NULL when no peer slot is free (or on failure). Ignored at every call
        // site before, which left the client attached with no upstream link.
        spdlog::error("[ENET] enet_host_connect failed for '{}' -> {}:{} (no free peer slot?)",
                      host, network::format_ip_address(address.host), port);
        return nullptr;
    }

    connect_attempts_ = (host == connect_target_host_ && port == connect_target_port_)
                            ? connect_attempts_ + 1
                            : 1;
    connect_target_host_ = host;
    connect_target_port_ = port;
    pending_connect_peer_ = peer;
    pending_connect_since_ = std::chrono::steady_clock::now();

    spdlog::info("[ENET] connecting upstream to '{}' -> {}:{} (attempt {}, peer slots in use on this host)",
                 host, network::format_ip_address(address.host), port, connect_attempts_);
    return peer;
}

void Client::process()
{
    if (!host_) {
        return;
    }

    // Watchdog for the in-flight upstream handshake. The legacy watchdog below only
    // arms in the non-HTTPS flow; with the HTTPS extension (the normal setup) a
    // handshake the game server never answered hung forever - the game client timed
    // out, retried, and tripped the server's "try again in 30 seconds" rate limiter,
    // which the player experienced as logins getting harder and harder.
    if (pending_connect_peer_) {
        const auto pending_elapsed = std::chrono::steady_clock::now() - pending_connect_since_;
        if (pending_elapsed >= std::chrono::seconds(5)) {
            spdlog::warn("[CONNECTION] Upstream handshake to {}:{} got no answer within 5s (attempt {})",
                         connect_target_host_, connect_target_port_, connect_attempts_);
            bool still_connecting = false;
            {
                const net::ENetLock guard{ net::enet_traffic_mutex() };
                still_connecting = pending_connect_peer_->state == ENET_PEER_STATE_CONNECTING;
                if (still_connecting) {
                    enet_peer_reset(pending_connect_peer_);
                }
            }
            pending_connect_peer_ = nullptr;
            // If it stopped being CONNECTING, the handshake completed and its event
            // is about to be serviced below - leave the session alone.
            if (still_connecting) {
                retry_or_fail_pending_connect("upstream handshake timeout");
            }
        }
    }
    
    if (g_connect_pending) {
        const auto now = std::chrono::steady_clock::now();
        const auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - g_connect_attempt_time).count();
        if (sec >= CONNECT_TIMEOUT_SEC) {
            g_connect_pending = false;
            spdlog::warn("[CONNECTION] Server did not respond within {}s", CONNECT_TIMEOUT_SEC);
            const player::Player* local = core_->get_server()->get_player();
            send_connection_error_msg(const_cast<player::Player*>(local));
        }
    }

    ENetEvent ev{};
    // Poll without waiting so the other host is serviced promptly by Core::run().
    // Bound each pass: a continuously busy upstream must not starve the local-client
    // host, whose ACKs and keepalives are serviced by the next Core::run() iteration.
    constexpr std::size_t kMaxEventsPerPass = 128;
    {
        const net::ENetLock guard{ net::enet_traffic_mutex() };

        std::size_t events_processed = 0;
        while (events_processed < kMaxEventsPerPass && enet_host_service(host_, &ev, 0) > 0) {
            ++events_processed;
            switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                on_connect(ev.peer);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                // ENet calls enet_peer_reset() and forces event->data = 0 before this
                // event is returned, so ev.data is always 0 here. Log what is still
                // readable so a failed handshake can be told apart from a mid-session
                // timeout. state will be DISCONNECTED because of that reset.
                spdlog::warn("[ENET] upstream DISCONNECT event: peer={}:{} state={} (data={})",
                             network::format_ip_address(ev.peer->address.host),
                             ev.peer->address.port,
                             static_cast<int>(ev.peer->state),
                             static_cast<unsigned>(ev.data));
                on_disconnect(ev.peer);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                spdlog::trace("[ENET] upstream received {} bytes", ev.packet->dataLength);
                on_receive(ev.peer, ev.packet);
                break;
            default:
                break;
            }
        }

        // enet_peer_send() only queues; without this the upstream socket is not written
        // until the next service call, batching outbound traffic by up to the 16 ms
        // timeout above. Reliable packets are ACKed from the peer's inbound path, so
        // delaying them adds round-trip time and eats into ENet's timeout budget.
        enet_host_flush(host_);
    }
}

void Client::on_connect(ENetPeer* peer)
{
    g_connect_pending = false;
    pending_connect_peer_ = nullptr;
    connect_attempts_ = 0;
    spdlog::info(
        "Server connection established: {}:{}",
        network::format_ip_address(peer->address.host),
        peer->address.port
    );

    // The upstream peer used to keep ENet's defaults (limit 32, min 5000 ms, max 30000 ms),
    // which is stricter than the client-facing peer and left no headroom for the proxy's
    // own parsing and logging work. Match the client-facing settings so a brief stall on
    // either side does not tear the session down.
    enet_peer_timeout(peer, 10000, 15000, 20000);

    // Same reasoning as Server::on_connect: retire any previous upstream peer explicitly
    // so it cannot linger in the host's peer array and deliver stale data or keep
    // consuming bandwidth once its session has been replaced.
    if (player_) {
        if (ENetPeer* old_peer = player_->get_peer(); old_peer != nullptr && old_peer != peer) {
            spdlog::warn("Upstream reconnect: retiring previous server peer");
            enet_peer_disconnect_now(old_peer, 0);
        }
        delete player_;
        player_ = nullptr;
    }

    player_ = new player::Player{ peer };
    active_upstream_connect_id_ = peer->connectID;

    core::EventConnection event_connection{ *player_ };
    event_connection.from = core::EventFrom::FromServer;
    core_->get_event_dispatcher().dispatch(event_connection);

    // The client may have sent its login packets while this handshake was still
    // completing; the server side queued them. Replay them now, in order, so the
    // fresh session starts from its real handshake instead of a silent hole.
    core_->get_server()->flush_pending_from_client();
}

void Client::on_receive(ENetPeer* peer, ENetPacket* packet)
{
    if (!player_) {
        enet_peer_disconnect(peer, 0);
        return;
    }

    player::Player* to_player{ core_->get_server()->get_player() };
    if (!to_player) {
        player_->disconnect();
        return;
    }

    // Ignore data from a superseded upstream peer (see on_connect): relaying it would
    // mix a dead server session into the live client.
    if (player_->get_peer() != peer) {
        spdlog::debug("Dropping packet from a superseded upstream peer");
        enet_packet_destroy(packet);
        return;
    }

    ByteStream<std::uint16_t> byte_stream{ reinterpret_cast<std::byte*>(packet->data), packet->dataLength };
    
    
    // Shared ceiling (32 MiB, ENet's own maximum packet size). This used to be 8 MiB,
    // which dropped large world and item-database packets before they were forwarded.
    constexpr std::size_t kMaxIncomingPacketSize = packet::kMaxPacketSize;
    if (byte_stream.get_size() < packet::kMinPacketSize || byte_stream.get_size() > kMaxIncomingPacketSize) {
        // Oversized packets are dropped, and undersized ones are dropped too. A single
        // malformed or empty datagram used to tear the whole session down here, which is
        // far more destructive than the corrupt packet itself: one stray packet produced
        // an apparently random disconnect. Drop it and keep the session alive.
        spdlog::warn("Dropping out-of-bounds incoming packet: {} bytes (bounds {}..{})",
                     byte_stream.get_size(), packet::kMinPacketSize, kMaxIncomingPacketSize);
        enet_packet_destroy(packet);
        return;
    }

    enet_packet_destroy(packet);

    packet::NetMessageType type{};
    if (!byte_stream.read(type)) {
        // Same reasoning: a header we cannot parse is not a reason to disconnect.
        spdlog::warn("Dropping incoming packet with unreadable message type ({} bytes)",
                     byte_stream.get_size());
        return;
    }

    
    packet::filter::FilterContext context{
        .source_player = player_,
        .target_player = to_player,
        .incoming = false, 
        .timestamp = std::chrono::steady_clock::now()
    };

    auto action = packet::filter::PacketFilter::get_instance().process_packet(type, byte_stream, context);
    
    switch (action) {
        case packet::filter::Action::BLOCK:
            spdlog::debug("Packet blocked by filter");
            return;
            
        case packet::filter::Action::MODIFY:
            spdlog::debug("Packet modified by filter");
            break;
            
        case packet::filter::Action::REDIRECT:
            spdlog::debug("Packet redirected by filter");
            handle_redirected_packet(byte_stream, to_player);
            return;
            
        case packet::filter::Action::ALLOW:
        default:
            break;
    }

    
    switch (type) {
        case packet::NET_MESSAGE_SERVER_HELLO:
            handle_server_hello(byte_stream, to_player);
            break;
            
        case packet::NET_MESSAGE_GENERIC_TEXT:
        case packet::NET_MESSAGE_GAME_MESSAGE:
            handle_text_message(byte_stream, to_player, peer);
            break;
            
        case packet::NET_MESSAGE_GAME_PACKET:
            handle_game_packet(byte_stream, to_player, peer);
            break;

        case packet::NET_MESSAGE_TRACK:
            
            
            to_player->send_packet(byte_stream.get_data(), 0);
            break;
            
        default:
            handle_unknown_packet(static_cast<std::uint32_t>(type), byte_stream, to_player, peer);
            break;
    }
}

void Client::handle_server_hello(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player)
{
    packet::core::ServerHello server_hello{};
    packet::PacketHelper::send(server_hello, *to_player);
}

void Client::handle_text_message(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player, ENetPeer* peer)
{
    std::string message{};
    // Read every remaining byte. This previously subtracted 1 as well, silently dropping
    // the last character of the message; the message type was already consumed by the
    // caller, so get_remaining() is the exact payload length.
    byte_stream.read(message, byte_stream.get_remaining());

    TextParse text_parse{ message };

    auto send_console_local = [&](const std::string& msg) {
        packet::Variant var{};
        var.add("OnConsoleMessage");
        var.add(msg);
        std::vector<std::byte> ext_data = var.serialize();
        packet::GameUpdatePacket pkt{};
        pkt.type = packet::PACKET_CALL_FUNCTION;
        pkt.net_id = -1;
        pkt.flags.extended = 1;
        pkt.data_size = static_cast<uint32_t>(ext_data.size());
        ByteStream<std::uint16_t> bs{};
        bs.write(packet::NET_MESSAGE_GAME_PACKET);
        bs.write(pkt);
        bs.write_data(ext_data.data(), ext_data.size());
        to_player->send_packet(bs.get_data(), 0);
    };

    if (core_->get_config().get<bool>("log.printMessage")) {
        spdlog::info("Received server message:");
        for (const auto& key_value : text_parse.get_redacted_key_values()) {
            spdlog::info("  {}", key_value);
        }
    }

    
    if (!text_parse.get(DEC("tankIDName"), 0).empty() || !text_parse.get(DEC("ltoken"), 0).empty() || !text_parse.get(DEC("mac"), 0).empty()) {
        utils::ProxyLogger::log_login_packet(text_parse);
    }



    core::EventMessage event_message{ *player_, *to_player, text_parse };
    event_message.from = core::EventFrom::FromServer;
    core_->get_event_dispatcher().dispatch(event_message);

    if (!event_message.canceled) {
        to_player->send_packet(byte_stream.get_data(), 0);
    }
}

void Client::handle_game_packet(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player, ENetPeer* peer)
{
    try {
        packet::GameUpdatePacket game_update_packet{};
        if (!byte_stream.read(game_update_packet)) {
            spdlog::warn("Failed to read game update packet header");
            
            return;
        }

        std::vector<std::byte> ext_data{};
        const std::size_t remaining_bytes = byte_stream.get_size() - byte_stream.get_read_offset();

        
        
        
        if (game_update_packet.type == packet::PACKET_SEND_MAP_DATA) {
            if (remaining_bytes > 0 &&
                game_update_packet.data_size > 0 &&
                game_update_packet.data_size != remaining_bytes) {
                spdlog::info(
                    "SEND_MAP_DATA: data_size field={} but packet has {} bytes remaining; using remaining bytes",
                    game_update_packet.data_size, remaining_bytes
                );
            }

            if (remaining_bytes > 0) {
                ext_data.resize(remaining_bytes);
                if (!byte_stream.read_data(ext_data.data(), remaining_bytes)) {
                    spdlog::warn("SEND_MAP_DATA: failed to read remaining {} bytes", remaining_bytes);
                    ext_data.clear();
                }
            }

            game_update_packet.data_size = static_cast<uint32_t>(ext_data.size());
        } else {
            if (game_update_packet.data_size > 0) {
                
                
                if (game_update_packet.data_size < 256 * 1024 * 1024) { 
                    if (!byte_stream.read_vector(ext_data, static_cast<std::size_t>(game_update_packet.data_size))) {
                        spdlog::warn("Failed to read extended data for game packet, expected: {}, available: {}", 
                                    game_update_packet.data_size, 
                                    byte_stream.get_size() - byte_stream.get_read_offset());
                        
                    }
                } else {
                    spdlog::warn("Extended data too large (>{}): {}", 256 * 1024 * 1024, game_update_packet.data_size);
                    
                }
            }
        }

        
        
        
        
        
        
        if (game_update_packet.type == packet::PACKET_ITEM_CHANGE_OBJECT) {
            const auto& raw = byte_stream.get_data();
            if (raw.size() >= 4 + 32) {
                const uint8_t* b = reinterpret_cast<const uint8_t*>(raw.data()) + 4;
                uint8_t  obj_type      = b[1];
                uint8_t  jump_count    = b[2];
                uint32_t pkt_net_id    = 0;
                float    float_var     = 0.f;
                uint32_t pkt_value     = 0;
                float    vec_x         = 0.f;
                float    vec_y         = 0.f;
                memcpy(&pkt_net_id, b + 4,  4);
                memcpy(&float_var,  b + 16, 4);
                memcpy(&pkt_value,  b + 20, 4);
                memcpy(&vec_x,      b + 24, 4);
                memcpy(&vec_y,      b + 28, 4);

                auto& wm = utils::WorldManager::get_instance();

                if (pkt_net_id == 0xFFFFFFFF) {
                    
                    
                    uint32_t new_uid = 1;
                    {
                        const auto& live  = wm.get_live_objects();
                        const auto& items = wm.get_items();
                        for (const auto& it : live)  if (it.Uid >= new_uid) new_uid = it.Uid + 1;
                        for (const auto& it : items) if (it.Uid >= new_uid) new_uid = it.Uid + 1;
                    }
                    world::DroppedItemInfo di{};
                    di.ItemId = static_cast<uint16_t>(pkt_value);
                    di.X      = std::ceil(vec_x);
                    di.Y      = std::ceil(vec_y);
                    di.Amount = static_cast<uint32_t>(static_cast<uint8_t>(float_var));
                    di.Flag   = obj_type;
                    di.Uid    = new_uid;
                    wm.add_dropped_item(di);
                    spdlog::info("[ITEM_DROP] id={} uid={} x={:.0f} y={:.0f} count={}",
                                 di.ItemId, di.Uid, di.X, di.Y, di.Amount);

                } else if (pkt_net_id == 0xFFFFFFFC) {
                    

                } else if (pkt_net_id > 0) {
                    
                    wm.remove_dropped_item_by_uid(pkt_value);
                    wm.remove_live_object(pkt_value);
                    spdlog::info("[ITEM_COLLECT] uid={} by net_id={}", pkt_value, pkt_net_id);
                }
            }
        }

        
        if (game_update_packet.type == packet::PACKET_CALL_FUNCTION && !ext_data.empty()) {
            try {
                
                packet::Variant variant{};
                if (variant.deserialize(ext_data)) {
                    auto variants = variant.get_variants();
                    if (variants.size() >= 2) {
                        const std::string function_name = variant.get<std::string>(0);

                        
                        if (function_name == "OnSpawn") {
                            
                            std::string spawn_data = variant.get<std::string>(1);
                            TextParse text_parse{ spawn_data };
                            
                            
                            std::string player_name = text_parse.get("name");
                            uint32_t net_id = 0;
                            std::string spawn_platform_id = text_parse.get("platformID");
                            try {
                                std::string net_id_str = text_parse.get("netID");
                                if (!net_id_str.empty()) {
                                    net_id = std::stoul(net_id_str);
                                }
                            } catch (...) {
                                
                            }
                            
                            if (!player_name.empty() && net_id > 0) {
                                
                                
                                player_name.erase(std::remove(player_name.begin(), player_name.end(), '\''), player_name.end());
                                player_name.erase(std::remove(player_name.begin(), player_name.end(), '"'), player_name.end());
                                
                                size_t pos = 0;
                                while ((pos = player_name.find('`')) != std::string::npos) {
                                    if (pos + 1 < player_name.length()) {
                                        player_name.erase(pos, 2);
                                    } else {
                                        player_name.erase(pos, 1);
                                        break;
                                    }
                                }
                                
                                player_name.erase(0, player_name.find_first_not_of(" \t\r\n"));
                                player_name.erase(player_name.find_last_not_of(" \t\r\n") + 1);
                                
                                if (!player_name.empty()) {
                                    
                                    utils::PlayerTracker::get_instance().update_player_name(net_id, player_name);
                                    if (!spawn_platform_id.empty()) {
                                        utils::PlayerTracker::get_instance().update_platform_info(net_id, spawn_platform_id);
                                    }
                                    spdlog::info("[SPAWN-TRACKER] Tracked player spawn: netid={}, name='{}'", net_id, player_name);
                                }
                            }
                            
                            
                            std::string spawn_type = text_parse.get("type");
                            if (spawn_type != "local") {
                                
                                player_name = text_parse.get("name");
                                
                                
                                player_name.erase(std::remove(player_name.begin(), player_name.end(), '\''), player_name.end());
                                player_name.erase(std::remove(player_name.begin(), player_name.end(), '"'), player_name.end());
                                
                                spdlog::info("[CLIENT-SPAWN] Got player_name from spawn: '{}' ({} bytes)", 
                                            player_name, player_name.length());
                                if (!player_name.empty() && core_->get_server() && core_->get_server()->get_player()) {
                                    spdlog::info("[CLIENT-SPAWN] Calling JoinCommand::handle_spawn_packet with name: '{}'", 
                                               player_name);
                                    command::JoinCommand::handle_spawn_packet(
                                        core_->get_server()->get_player(),
                                        player_name
                                    );
                                    
                                    
                                    command::BanallCommand::add_spawned_player(player_name);
                                }

                                
                                command::ModDetectCommand::handle_spawn_packet(spawn_data);
                            }
                            
                            
                            if (text_parse.get("type") == "local") {
                                spdlog::info("Detected local player spawn - applying zoom mod, JP flag, and balance console message");
                                
                                std::string modified_data = spawn_data;
                                auto replace_or_add_field = [](std::string& data, const std::string& field, const std::string& value) {
                                    std::string search_pattern = field + "|";
                                    
                                    
                                    
                                    size_t pos = std::string::npos;
                                    size_t search_from = 0;
                                    while (true) {
                                        const size_t candidate = data.find(search_pattern, search_from);
                                        if (candidate == std::string::npos) {
                                            break;
                                        }
                                        
                                        const bool at_line_start = candidate == 0 || data[candidate - 1] == '\n';
                                        if (at_line_start) {
                                            pos = candidate;
                                            break;
                                        }
                                        search_from = candidate + 1;
                                    }
                                    
                                    if (pos != std::string::npos) {
                                        size_t value_start = pos + search_pattern.length();
                                        size_t value_end = data.find('\n', value_start);
                                        if (value_end == std::string::npos) {
                                            value_end = data.length();
                                        }
                                        data.replace(value_start, value_end - value_start, value);
                                    } else {
                                        size_t type_pos = data.find("type|local");
                                        if (type_pos != std::string::npos) {
                                            
                                            
                                            
                                            const size_t line_end = data.find('\n', type_pos);
                                            const size_t insert_at = line_end == std::string::npos
                                                ? data.length()
                                                : line_end + 1;
                                            data.insert(insert_at, field + "|" + value + "\n");
                                        }
                                    }
                                };
                                
                                replace_or_add_field(modified_data, "country", "jp");
                                bool invis_enabled = core_->get_config().get<bool>("player.invis_enabled");
                                replace_or_add_field(modified_data, "invis", invis_enabled ? "1" : "0");
                                replace_or_add_field(modified_data, "mstate", "1");
                                bool sm_enabled = core_->get_config().get<bool>("player.sm_enabled");
                                replace_or_add_field(modified_data, "smstate", sm_enabled ? "1" : "0");
                                int title_icon = core_->get_config().get<int>("player.title_icon");
                                if (title_icon > 0) {
                                    replace_or_add_field(modified_data, "titleIcon", std::to_string(title_icon));
                                }
                                bool vision_enabled = core_->get_config().get<bool>("player.vision_enabled");
                                replace_or_add_field(modified_data, "IsNightVision", vision_enabled ? "1" : "0");
                                
                                auto local_info = utils::PlayerTracker::get_instance().get_local_player();
                                if (!local_info.platform_id.empty()) {
                                    replace_or_add_field(modified_data, "platformID", local_info.platform_id);
                                }
                                
                                packet::Variant modified_variant{};
                                modified_variant.add("OnSpawn");
                                modified_variant.add(modified_data);
                                std::vector<std::byte> modified_ext_data = modified_variant.serialize();
                                game_update_packet.data_size = static_cast<uint32_t>(modified_ext_data.size());
                                ByteStream<std::uint16_t> modified_byte_stream{};
                                modified_byte_stream.write(packet::NET_MESSAGE_GAME_PACKET);
                                modified_byte_stream.write(game_update_packet);
                                modified_byte_stream.write_data(modified_ext_data.data(), modified_ext_data.size());
                                byte_stream = std::move(modified_byte_stream);
                                spdlog::info("Applied zoom mod and JP flag to local player spawn");
                                
                                
                                if (sm_enabled) {
                                    packet::Variant warning_var{};
                                    warning_var.add("OnAddNotification");
                                    warning_var.add("interface/atomic_button.rttex");
                                    warning_var.add("`4Long Punch Enabled `w(BANNABLE use /sm to turn off)");
                                    warning_var.add("audio/hub_open.wav");
                                    warning_var.add(0);
                                    std::vector<std::byte> warning_ext_data = warning_var.serialize();
                                    packet::GameUpdatePacket warning_pkt{};
                                    warning_pkt.type = packet::PACKET_CALL_FUNCTION;
                                    warning_pkt.net_id = -1;
                                    warning_pkt.flags.extended = 1;
                                    warning_pkt.data_size = static_cast<uint32_t>(warning_ext_data.size());
                                    ByteStream<std::uint16_t> warning_bs{};
                                    warning_bs.write(packet::NET_MESSAGE_GAME_PACKET);
                                    warning_bs.write(warning_pkt);
                                    warning_bs.write_data(warning_ext_data.data(), warning_ext_data.size());
                                    to_player->send_packet(warning_bs.get_data(), 0);
                                    spdlog::warn("Sent long punch warning notification - SM mod is enabled");
                                }
                            }
                        }
                        
                        else if (function_name == "OnSendToServer") {
                            bool handled = false;
                            for (size_t i = 1; i < variants.size(); ++i) {
                                std::string server_info = variant.get<std::string>(i);
                                if (!server_info.empty()) {
                                    command::DoorIDCommand::handle_send_to_server(server_info);
                                    handled = true;
                                }
                            }

                            
                            if (!handled) {
                                std::string server_info = variant.get<std::string>(4);
                                if (!server_info.empty()) {
                                    command::DoorIDCommand::handle_send_to_server(server_info);
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                spdlog::warn("Error processing variant packet: {}", e.what());
                
            }
        }

        
        try {
            core::EventPacket event_packet{ *player_, *to_player, game_update_packet, ext_data };
            event_packet.from = core::EventFrom::FromServer;
            core_->get_event_dispatcher().dispatch(event_packet);

            if (core_->get_config().get<bool>("log.printGameUpdatePacket")) {
                spdlog::info(
                    "Game packet: {} ({})",
                    magic_enum::enum_name(game_update_packet.type),
                    magic_enum::enum_integer(game_update_packet.type)
                );
            }

            if (!event_packet.canceled) {
                to_player->send_packet(byte_stream.get_data(), 0);
            }
        } catch (const std::exception& e) {
            spdlog::error("Error in game packet event dispatch: {}", e.what());
            
            to_player->send_packet(byte_stream.get_data(), 0);
        }
    } catch (const std::exception& e) {
        spdlog::error("Error processing game packet: {}", e.what());
        
        try {
            to_player->send_packet(byte_stream.get_data(), 0);
        } catch (const std::exception& send_error) {
            spdlog::error("Failed to forward game packet after error: {}", send_error.what());
        }
    }
}

void Client::handle_unknown_packet(std::uint32_t raw_type, ByteStream<std::uint16_t>& byte_stream, player::Player* to_player, ENetPeer* peer)
{
    const auto type = static_cast<packet::NetMessageType>(raw_type);

    spdlog::info(
        "Unknown packet from {}:{} - Type: {} ({})",
        network::format_ip_address(peer->address.host),
        peer->address.port,
        magic_enum::enum_name(type),
        raw_type
    );

    to_player->send_packet(byte_stream.get_data(), 0);
}

void Client::handle_redirected_packet(ByteStream<std::uint16_t>& byte_stream, player::Player* to_player) {
    spdlog::info("Handling redirected packet from server");
    
    
    to_player->send_packet(byte_stream.get_data(), 0);
}

void Client::retry_or_fail_pending_connect(const char* reason)
{
    auto* server = core_->get_server();
    player::Player* local = server ? server->get_player() : nullptr;

    // One silent retry absorbs a lost UDP handshake; after that, surface a real
    // error so the player sees "can't reach the server" instead of the game
    // hanging, timing out and hammering the login endpoint into its rate limiter.
    if (connect_attempts_ <= 1) {
        spdlog::warn("[CONNECTION] {} - retrying {}:{} once", reason,
                     connect_target_host_, connect_target_port_);
        if (!connect_target_host_.empty()) {
            std::ignore = connect(connect_target_host_, connect_target_port_);
        }
        return;
    }

    spdlog::error("[CONNECTION] {} - giving up on {}:{}", reason,
                  connect_target_host_, connect_target_port_);
    connect_attempts_ = 0;
    if (local) {
        send_connection_error_msg(local);
    }
}

void Client::on_local_disconnect()
{
    active_upstream_connect_id_ = 0;
    if (pending_connect_peer_) {
        const net::ENetLock guard{ net::enet_traffic_mutex() };
        if (pending_connect_peer_->state == ENET_PEER_STATE_CONNECTING) {
            enet_peer_reset(pending_connect_peer_);
        }
        pending_connect_peer_ = nullptr;
    }
    if (!player_) {
        return;
    }
    player_->disconnect_now();
    delete player_;
    player_ = nullptr;
    {
        const net::ENetLock guard{ net::enet_traffic_mutex() };
        enet_host_flush(host_);
    }
}

void Client::on_disconnect(ENetPeer* peer)
{
    // With multiple peer slots, a DISCONNECT event can be delivered for a stale peer:
    // a superseded session the server retired, or a connect attempt that timed out
    // while still occupying a slot. ENet also recycles peer objects once their slot
    // is freed, so a bare pointer match against a leftover player object means
    // nothing. Only the peer of the current session - same object AND same connectID
    // - may tear that session down. Killing the live session for a stale goodbye
    // looked like "random disconnects" (and, to the player, like the last feature
    // they used was at fault).
    if (!player_ || player_->get_peer() != peer ||
        peer->connectID != active_upstream_connect_id_) {
        spdlog::warn("[ENET] Ignoring disconnect event for a non-active upstream peer {}:{}",
                     network::format_ip_address(peer->address.host), peer->address.port);
        if (!player_ && socks5_tunnel::is_active()) {
            socks5_tunnel::disconnect();
            spdlog::info("[SOCKS5] Tunnel closed on upstream teardown");
        }
        // A refused/abandoned handshake reports as a disconnect for the pending
        // peer. ENet has already reset it; just decide whether to retry.
        if (peer == pending_connect_peer_) {
            pending_connect_peer_ = nullptr;
            retry_or_fail_pending_connect("upstream refused the handshake");
        }
        return;
    }

    spdlog::info(
        "Server connection terminated: {}:{}",
        network::format_ip_address(peer->address.host),
        peer->address.port
    );

    if (socks5_tunnel::is_active()) {
        socks5_tunnel::disconnect();
        spdlog::info("[SOCKS5] Tunnel closed on server disconnect");
    }

    core::EventDisconnection event_disconnection{ *player_ };
    event_disconnection.from = core::EventFrom::FromServer;
    core_->get_event_dispatcher().dispatch(event_disconnection);

    delete player_;
    player_ = nullptr;
    active_upstream_connect_id_ = 0;

    const player::Player* to_player{ core_->get_server()->get_player() };
    if (!to_player) {
        return;
    }

    
    send_connection_error_msg(const_cast<player::Player*>(to_player));

    {
        const net::ENetLock guard{ net::enet_traffic_mutex() };
        enet_host_flush(host_);
    }
    to_player->disconnect_now();
    core_->get_server()->on_disconnect(to_player->get_peer());
}
}
