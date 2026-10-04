#pragma once

#include "command_base.hpp"
#include "autocollect_command.hpp"
#include "../../core/core.hpp"
#include "../../server/server.hpp"
#include "../../utils/world_manager.hpp"
#include "../../utils/player_tracker.hpp"
#include "../../utils/packet_utils.hpp"

#include <limits>

namespace command {

class PickupCommand final : public CommandBase {
public:
    PickupCommand()
        : CommandBase({"pickup"}, {"<item_id>"}, "Pick up the nearest dropped stack of an item", 1) {}

    void execute(client::Client* client, const std::vector<std::string>& args) override {
        if (!s_core || !client || !client->get_player()) return;
        auto* server = s_core->get_server();
        if (!server || !server->get_player()) return;
        if (args.size() < 2) {
            utils::PacketUtils::send_chat_message(server->get_player(), "`4Usage: /pickup <item_id>");
            return;
        }

        int item_id = -1;
        try { item_id = std::stoi(args[1]); } catch (...) { }
        if (item_id < 0 || item_id > std::numeric_limits<uint16_t>::max()) {
            utils::PacketUtils::send_chat_message(server->get_player(), "`4Invalid item ID.");
            return;
        }

        const auto local = utils::PlayerTracker::get_instance().get_local_player();
        auto& worlds = utils::WorldManager::get_instance();
        auto items = worlds.get_items_snapshot();
        auto live_items = worlds.get_live_objects_snapshot();
        items.insert(items.end(), live_items.begin(), live_items.end());

        const world::DroppedItemInfo* nearest = nullptr;
        float nearest_distance = std::numeric_limits<float>::max();
        for (const auto& item : items) {
            if (item.ItemId != static_cast<uint16_t>(item_id) || item.Uid == 0) continue;
            const float dx = item.X - local.position.x;
            const float dy = item.Y - local.position.y;
            const float distance = dx * dx + dy * dy;
            if (distance < nearest_distance) {
                nearest = &item;
                nearest_distance = distance;
            }
        }

        if (!nearest) {
            utils::PacketUtils::send_chat_message(server->get_player(),
                "`4No dropped stack with that item ID is cached in this world.");
            return;
        }

        auto* upstream = s_core->get_client() ? s_core->get_client()->get_player() : nullptr;
        if (!upstream) {
            utils::PacketUtils::send_chat_message(server->get_player(), "`4Not connected to the game server.");
            return;
        }

        AutoCollectCommand::send_collect_packet(upstream, nearest->Uid, nearest->X, nearest->Y);
        utils::PacketUtils::send_chat_message(server->get_player(),
            "`2Pickup requested for item " + std::to_string(item_id) + ".");
    }

    std::unique_ptr<CommandBase> clone() const override {
        return std::make_unique<PickupCommand>();
    }

    static void set_core(core::Core* core) { s_core = core; }

private:
    inline static core::Core* s_core = nullptr;
};

}
