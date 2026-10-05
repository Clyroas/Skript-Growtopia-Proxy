#include "player.hpp"
#include "../utils/packet_limits.hpp"
#include "../utils/enet_lock.hpp"
#include <spdlog/spdlog.h>
#include <cstring>
#include <string>

namespace player {
namespace {
// One shared ceiling for both directions. This used to be 786432 (768 KiB), which was
// LOWER than the client's 8 MiB receive allowance, so any packet between those two
// sizes was received successfully and then silently dropped when relayed. A real
// item-database packet is ~5.3 MiB, so this was reachable in normal play.
constexpr std::size_t kMaxPacketSize = packet::kMaxPacketSize;
constexpr std::size_t kMinPacketSize = packet::kMinPacketSize;
}

bool Player::send_packet(const std::vector<std::byte>& data, const int channel) const
{
    if (data.size() < kMinPacketSize || data.size() > kMaxPacketSize) {
        spdlog::warn("Refusing to send out-of-bounds packet: {} bytes (bounds {}..{})",
                     data.size(), kMinPacketSize, kMaxPacketSize);
        return false;
    }

    // Serialised against enet_host_service(): see utils/enet_lock.hpp. Without this,
    // sends from automation/UI threads corrupt ENet's lists and cause random disconnects.
    const net::ENetLock guard{ net::enet_traffic_mutex() };

    ENetPacket* packet{ enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE) };
    if (const int ret{ enet_peer_send(peer_, channel, packet) }; ret != 0) {
        enet_packet_destroy(packet);
        return false;
    }

    return true;
}

bool Player::send_packet_unreliable(const std::vector<std::byte>& data, const int channel) const
{
    if (data.size() < kMinPacketSize || data.size() > kMaxPacketSize) {
        spdlog::warn("Refusing to send out-of-bounds packet: {} bytes (bounds {}..{})",
                     data.size(), kMinPacketSize, kMaxPacketSize);
        return false;
    }

    const net::ENetLock guard{ net::enet_traffic_mutex() };

    ENetPacket* packet{ enet_packet_create(data.data(), data.size(), 0) };
    if (const int ret{ enet_peer_send(peer_, channel, packet) }; ret != 0) {
        enet_packet_destroy(packet);
        return false;
    }

    return true;
}

void Player::send_log(const std::string& message) const
{
    if (!is_connected()) {
        return;
    }

    std::string full_message = "action|log\nmsg|" + message;
    
    std::vector<std::byte> packet_data;
    packet_data.resize(sizeof(uint32_t) + sizeof(uint16_t) + full_message.length());
    
    
    uint32_t message_type = 3;
    std::memcpy(packet_data.data(), &message_type, sizeof(uint32_t));
    
    
    uint16_t str_len = static_cast<uint16_t>(full_message.length());
    std::memcpy(packet_data.data() + sizeof(uint32_t), &str_len, sizeof(uint16_t));
    
    
    std::memcpy(packet_data.data() + sizeof(uint32_t) + sizeof(uint16_t), 
                full_message.c_str(), full_message.length());
    
    send_packet(packet_data);
}
} 
