#pragma once
#include <spdlog/spdlog.h>

#include "sub_server_switch.hpp"
#include "../parser/parser.hpp"
#include "../../core/core.hpp"
#include "../../client/client.hpp"
#include "../../packet/packet_helper.hpp"
#include "../../packet/game/core.hpp"

namespace extension::sub_server_switch {
class SubServerSwitchExtension final : public ISubServerSwitchExtension {
    core::Core* core_;

    std::string address_;
    int port_;

public:
    explicit SubServerSwitchExtension(core::Core* core)
        : core_{ core }
        , port_{ -1 }
    {

    }

    ~SubServerSwitchExtension() override = default;

    void init() override
    {
        auto ext { core_->query_extension<IParserExtension>() };
        if (!ext) {
            spdlog::error("SubServerSwitchExtension: IParserExtension not found");
            return;
        }

        core_->get_event_dispatcher().prependListener(
            core::EventType::Connection,
            [&](const core::EventConnection& evt)
            {
                if (evt.from != core::EventFrom::FromClient) {
                    return;
                }

                if (address_.empty() || port_ == -1) {
                    return;
                }

                std::ignore = core_->get_client()->connect(address_, port_);

                address_.clear();
                port_ = -1;

                evt.canceled = true;
            }
        );

        ext->get_event_dispatcher().appendListener(
            IParserExtension::EventType::CallFunction,
            [this](const IParserExtension::EventCallFunction& evt)
            {
                if (evt.from != core::EventFrom::FromServer) {
                    return;
                }

                if (evt.get_function_name() != "OnSendToServer") {
                    return;
                }

                const packet::Variant evt_variant{ evt.get_args() };

                if (evt_variant.size() < 5) {
                    spdlog::warn("OnSendToServer: unexpected variant arity {} (expected >= 5); "
                                 "leaving packet untouched", evt_variant.size());
                    return;
                }

                std::vector<std::string> tokenize{ TextParse::tokenize(evt_variant.get(4)) };
                if (tokenize.empty()) {
                    spdlog::warn("OnSendToServer: variant[4] carried no tokens; leaving packet untouched");
                    return;
                }

                address_ = tokenize.at(0);
                port_ = evt_variant.get_any_int(1).value_or(0);

                if (address_.empty() || port_ <= 0) {
                    spdlog::warn("OnSendToServer: could not derive a usable relay target "
                                 "(address='{}', port={}); leaving packet untouched", address_, port_);
                    address_.clear();
                    port_ = -1;
                    return;
                }

                packet::game::OnSendToServer packet{};
                packet.port = core_->get_config().get<unsigned int>("server.port", 17091);

                packet.token = evt_variant.get_any_int(2).value_or(0);
                packet.user = evt_variant.get_any_int(3).value_or(0);
                packet.address = "127.0.0.1";

                
                
                
                
                
                if (tokenize.size() == 1) {
                    packet.door_id = "";
                    packet.uuid_token = "";
                }
                else if (tokenize.size() == 2) {
                    packet.door_id = "";
                    packet.uuid_token = tokenize.at(1);
                }
                else {
                    packet.door_id = tokenize.at(1);
                    packet.uuid_token = tokenize.at(2);
                }

                packet.login_mode = static_cast<uint8_t>(evt_variant.get_any_int(5).value_or(0));

                packet::PacketHelper::send(packet, evt.get_target());
                evt.canceled = true;
            }
        );
    }

    void free() override
    {
        delete this;
    }
};
}
