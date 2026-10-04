#pragma once
#include "../extension/extension.hpp"
#include "../utils/lua_manager.hpp"
#include "../utils/world_manager.hpp"
#include "../utils/player_tracker.hpp"
#include "../utils/inventory_manager.hpp"
#include "../core/core.hpp"
#include "../client/client.hpp"
#include "../server/server.hpp"
#include "../utils/byte_stream.hpp"
#include "../packet/packet_types.hpp"
#include "../packet/packet_variant.hpp"
#include "../utils/text_parse.hpp"
#include "../extension/command_handler/autocollect_command.hpp"
#include "../extension/command_handler/utility_commands.hpp"
#include "../extension/item_finder/item_finder.hpp"
#include "../proxy_imgui_gui.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
#include <functional>
#include <algorithm>
#include <atomic>
#include <spdlog/spdlog.h>

namespace extension::lua_scripting {

struct LuaCallbackEntry { std::string name, event; int ref = LUA_NOREF; };
inline std::mutex g_cb_mutex;
inline std::vector<LuaCallbackEntry> g_callbacks;
inline std::atomic<bool> g_lua_stopping{false};
inline std::mutex g_lua_threads_mutex;
inline std::vector<std::thread> g_lua_threads;

static void lua_invoke_callbacks(lua_State* L, const std::string& event,
    std::function<int()> push_args, bool* canceled = nullptr)
{
    auto* bridge = lua::LuaManager::get();
    if (!bridge || bridge->get_state() != L || g_lua_stopping.load()) return;
    std::lock_guard<std::recursive_mutex> state_lock(bridge->state_mutex());
    std::vector<LuaCallbackEntry> cbs;
    { std::lock_guard<std::mutex> lk(g_cb_mutex);
      for (auto& e : g_callbacks) if (e.event == event) cbs.push_back(e); }
    for (auto& cb : cbs) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
        if (!lua_isfunction(L, -1)) { lua_pop(L, 1); continue; }
        int pushed = push_args ? push_args() : 0;
        if (lua_pcall(L, pushed, 1, 0) == LUA_OK) {
            if (canceled && lua_isboolean(L, -1) && lua_toboolean(L, -1)) *canceled = true;
            lua_pop(L, 1);
        } else {
            AppendLog("[LUA] CB error: " + std::string(lua_tostring(L, -1)));
            lua_pop(L, 1);
        }
    }
}

class LuaScriptingExtension final : public IExtension {
    core::Core* core_;
    bool hooked_ = false;
    std::shared_ptr<std::atomic<bool>> active_ = std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::recursive_mutex> listener_gate_ = std::make_shared<std::recursive_mutex>();
    std::vector<std::pair<core::EventType, core::EventDispatcher::Handle>> listener_handles_;
public:
    PROVIDE_EXT_UID(0x4C554153);
    explicit LuaScriptingExtension(core::Core* c) : core_{c} {}
    ~LuaScriptingExtension() override = default;

    void init() override {
        g_lua_stopping.store(false);
        lua::LuaManager::initialize();
        register_globals();
        hook_events();
        load_scripts();
    }
    void free() override {
        active_->store(false);
        { std::lock_guard<std::recursive_mutex> callback_barrier(*listener_gate_); }
        for (const auto& [event, handle] : listener_handles_)
            core_->get_event_dispatcher().removeListener(event, handle);
        listener_handles_.clear();

        g_lua_stopping.store(true);
        std::vector<std::thread> workers;
        { std::lock_guard<std::mutex> lock(g_lua_threads_mutex); workers.swap(g_lua_threads); }
        for (auto& worker : workers) if (worker.joinable()) worker.join();

        if (auto* bridge = lua::LuaManager::get()) {
            {
                std::lock_guard<std::recursive_mutex> state_lock(bridge->state_mutex());
                auto* L = bridge->get_state();
                std::lock_guard<std::mutex> lock(g_cb_mutex);
                for (const auto& callback : g_callbacks)
                    if (callback.ref != LUA_NOREF && callback.ref != LUA_REFNIL)
                        luaL_unref(L, LUA_REGISTRYINDEX, callback.ref);
                g_callbacks.clear();
            }
            lua::LuaManager::shutdown();
        }
        delete this;
    }

private:
    static void push_gup(lua_State* L, const packet::GameUpdatePacket& p) {
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, p.type);                      lua_setfield(L, -2, "type");
        lua_pushinteger(L, p.net_id);                    lua_setfield(L, -2, "netid");
        lua_pushinteger(L, (lua_Integer)p.flags.value);  lua_setfield(L, -2, "flags");
        lua_pushinteger(L, p.data_size);                 lua_setfield(L, -2, "int_data");
        lua_pushinteger(L, p.decompressed_data_size);    lua_setfield(L, -2, "decompressed_size");
    }

    
    static void read_gup(lua_State* ls, int idx, packet::GameUpdatePacket& p) {
        memset(&p, 0, sizeof(p));
        auto gi = [&](const char* k, int def) -> int {
            lua_getfield(ls, idx, k);
            int v = lua_isnumber(ls, -1) ? (int)lua_tointeger(ls, -1) : def;
            lua_pop(ls, 1); return v;
        };
        p.type        = (packet::PacketType)gi("type", 0);
        p.net_id      = gi("netid", -1);
        p.flags.value = (packet::PacketFlag)gi("flags", 0);
        p.data_size   = gi("int_data", 0);
        p.decompressed_data_size = gi("decompressed_size", 0);
    }

    void hook_events() {
        if (hooked_) return; hooked_ = true;
        
        auto active = active_;
        auto gate = listener_gate_;
        listener_handles_.push_back({core::EventType::Packet, core_->get_event_dispatcher().appendListener(core::EventType::Packet,
            [active, gate](const core::EventPacket& ev) {
                std::lock_guard<std::recursive_mutex> callback_lock(*gate);
                if (!active->load() || g_lua_stopping.load()) return;
                if (ev.from != core::EventFrom::FromServer) return;
                if (ev.get_packet().type != packet::PACKET_CALL_FUNCTION) return;
                auto* L = lua::LuaManager::get()->get_state();
                packet::Variant var{};
                if (!var.deserialize(ev.get_ext_data())) return;
                bool canceled = false;
                lua_invoke_callbacks(L, "OnVarlist", [&]() -> int {
                    lua_createtable(L, 0, 8);
                    size_t n = var.size();
                    for (size_t i = 0; i < n && i < 6; i++) {
                        lua_pushinteger(L, (int)i);
                        try { auto s = var.get<std::string>(i); lua_pushstring(L, s.c_str()); }
                        catch (...) { try { lua_pushnumber(L, var.get<float>(i)); } catch (...) { lua_pushnil(L); } }
                        lua_settable(L, -3);
                    }
                    lua_pushinteger(L, ev.get_packet().net_id); lua_setfield(L, -2, "netid");
                    lua_pushstring(L, "");
                    return 2;
                }, &canceled);
                if (canceled) const_cast<core::EventPacket&>(ev).canceled = true;
            })});
        
        listener_handles_.push_back({core::EventType::Message, core_->get_event_dispatcher().appendListener(core::EventType::Message,
            [active, gate](const core::EventMessage& ev) {
                std::lock_guard<std::recursive_mutex> callback_lock(*gate);
                if (!active->load() || g_lua_stopping.load()) return;
                if (ev.from != core::EventFrom::FromClient) return;
                auto* L = lua::LuaManager::get()->get_state();
                std::string raw = ev.get_message().get_raw();
                bool canceled = false;
                lua_invoke_callbacks(L, "OnPacket", [&]() -> int {
                    lua_pushinteger(L, 2); lua_pushstring(L, raw.c_str()); return 2;
                }, &canceled);
                if (canceled) const_cast<core::EventMessage&>(ev).canceled = true;
            })});
        
        listener_handles_.push_back({core::EventType::Packet, core_->get_event_dispatcher().appendListener(core::EventType::Packet,
            [active, gate](const core::EventPacket& ev) {
                std::lock_guard<std::recursive_mutex> callback_lock(*gate);
                if (!active->load() || g_lua_stopping.load()) return;
                if (ev.from != core::EventFrom::FromClient) return;
                auto* L = lua::LuaManager::get()->get_state();
                bool canceled = false;
                lua_invoke_callbacks(L, "OnRawPacket", [&]() -> int {
                    push_gup(L, ev.get_packet()); return 1;
                }, &canceled);
                if (canceled) const_cast<core::EventPacket&>(ev).canceled = true;
            })});
        
        listener_handles_.push_back({core::EventType::Packet, core_->get_event_dispatcher().appendListener(core::EventType::Packet,
            [active, gate](const core::EventPacket& ev) {
                std::lock_guard<std::recursive_mutex> callback_lock(*gate);
                if (!active->load() || g_lua_stopping.load()) return;
                if (ev.from != core::EventFrom::FromServer) return;
                if (ev.get_packet().type == packet::PACKET_CALL_FUNCTION) return;
                auto* L = lua::LuaManager::get()->get_state();
                bool canceled = false;
                lua_invoke_callbacks(L, "OnIncomingRawPacket", [&]() -> int {
                    push_gup(L, ev.get_packet()); return 1;
                }, &canceled);
                if (canceled) const_cast<core::EventPacket&>(ev).canceled = true;
            })});
    }

    
    static int tbl_int(lua_State* ls, const char* k, int def) {
        lua_getfield(ls, 1, k);
        int v = lua_isnumber(ls, -1) ? (int)lua_tointeger(ls, -1) : def;
        lua_pop(ls, 1); return v;
    }

    void register_globals() {
        lua_State* L = lua::LuaManager::get()->get_state();

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            AppendLog(std::string("[LUA] ") + luaL_checkstring(ls, 1));
            return 0;
        }, 0);
        lua_setglobal(L, "log");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            const int duration_ms = (int)luaL_checkinteger(ls, 1);
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(std::max(0, duration_ms));
            while (!g_lua_stopping.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            return 0;
        }, 0);
        lua_setglobal(L, "Sleep");
        lua_getglobal(L, "Sleep"); lua_setglobal(L, "SleepMS");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            luaL_checktype(ls, 1, LUA_TFUNCTION);
            lua_pushvalue(ls, 1);
            int ref = luaL_ref(ls, LUA_REGISTRYINDEX);
            std::lock_guard<std::mutex> lock(g_lua_threads_mutex);
            if (g_lua_stopping.load()) {
                luaL_unref(ls, LUA_REGISTRYINDEX, ref);
                return 0;
            }
            g_lua_threads.emplace_back([ls, ref]() {
                auto* bridge = lua::LuaManager::get();
                if (!bridge || bridge->get_state() != ls) return;
                std::lock_guard<std::recursive_mutex> state_lock(bridge->state_mutex());
                if (!g_lua_stopping.load()) {
                    lua_rawgeti(ls, LUA_REGISTRYINDEX, ref);
                    if (lua_pcall(ls, 0, 0, 0) != LUA_OK) {
                        AppendLog("[LUA] Thread error: " + std::string(lua_tostring(ls, -1)));
                        lua_pop(ls, 1);
                    }
                }
                luaL_unref(ls, LUA_REGISTRYINDEX, ref);
            });
            return 0;
        }, 0);
        lua_setglobal(L, "RunThread");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            int type = (int)luaL_checkinteger(ls, 1);
            const char* data = luaL_checkstring(ls, 2);
            auto* client = c->get_client();
            if (!client || !client->get_player()) {
                AppendLog("[LUA] SendPacket: not connected"); return 0;
            }
            uint32_t mt = (type == 3) ? (uint32_t)packet::NET_MESSAGE_GAME_MESSAGE
                                      : (uint32_t)packet::NET_MESSAGE_GENERIC_TEXT;
            
            std::string pkt_str(data);
            if (!pkt_str.empty() && pkt_str.back() != '\n') pkt_str += '\n';
            ByteStream<std::uint16_t> byte_stream{};
            byte_stream.write(mt);
            byte_stream.write(pkt_str, false);
            bool ok = client->get_player()->send_packet(byte_stream.get_data(), 0);
            AppendLog("[LUA] SendPacket t=" + std::to_string(mt)
                + " b=" + std::to_string(strlen(data)) + (ok ? " OK" : " FAIL"));
            return 0;
        }, 1);
        lua_setglobal(L, "SendPacket");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            luaL_checktype(ls, 1, LUA_TTABLE);
            auto* cl = c->get_client();
            if (!cl || !cl->get_player()) {
                AppendLog("[LUA] SendPacketRaw: not connected"); return 0;
            }
            packet::GameUpdatePacket pkt{};
            read_gup(ls, 1, pkt);
            ByteStream<std::uint16_t> bs;
            bs.write((uint32_t)packet::NET_MESSAGE_GAME_PACKET);
            bs.write(pkt);
            cl->get_player()->send_packet(bs.get_data(), 0);
            return 0;
        }, 1);
        lua_setglobal(L, "SendPacketRaw");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            luaL_checktype(ls, 1, LUA_TTABLE);
            auto* sv = c->get_server();
            if (!sv || !sv->get_player() || !sv->get_player()->is_connected()) {
                AppendLog("[LUA] SendPacketRawClient: not connected"); return 0;
            }
            packet::GameUpdatePacket pkt{};
            read_gup(ls, 1, pkt);
            ByteStream<std::uint16_t> bs;
            bs.write((uint32_t)packet::NET_MESSAGE_GAME_PACKET);
            bs.write(pkt);
            sv->get_player()->send_packet(bs.get_data(), 0);
            return 0;
        }, 1);
        lua_setglobal(L, "SendPacketRawClient");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            luaL_checktype(ls, 1, LUA_TTABLE);
            auto* sv = c->get_server();
            if (!sv || !sv->get_player() || !sv->get_player()->is_connected()) {
                AppendLog("[LUA] SendVarlist: not connected"); return 0;
            }
            packet::Variant var{};
            for (int i = 0; i <= 5; i++) {
                lua_pushinteger(ls, i); lua_gettable(ls, 1);
                if (lua_isstring(ls, -1))      var.add(std::string(lua_tostring(ls, -1)));
                else if (lua_isnumber(ls, -1)) var.add((float)lua_tonumber(ls, -1));
                lua_pop(ls, 1);
            }
            lua_getfield(ls, 1, "netid");
            int netid = lua_isnumber(ls, -1) ? (int)lua_tointeger(ls, -1) : -1;
            lua_pop(ls, 1);
            auto ext = var.serialize();
            packet::GameUpdatePacket pkt{};
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = packet::PACKET_CALL_FUNCTION;
            pkt.net_id = netid;
            pkt.flags.extended = 1;
            pkt.data_size = (uint32_t)ext.size();
            ByteStream<std::uint16_t> bs;
            bs.write((uint32_t)packet::NET_MESSAGE_GAME_PACKET);
            bs.write(pkt);
            bs.write_data(ext.data(), ext.size());
            sv->get_player()->send_packet(bs.get_data(), 0);
            return 0;
        }, 1);
        lua_setglobal(L, "SendVarlist");
        lua_getglobal(L, "SendVarlist"); lua_setglobal(L, "SendVariant");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            int type = (int)luaL_checkinteger(ls, 1);
            const char* data = luaL_checkstring(ls, 2);
            auto* sv = c->get_server();
            if (!sv || !sv->get_player() || !sv->get_player()->is_connected()) {
                AppendLog("[LUA] SendPacketToClient: not connected"); return 0;
            }
            uint32_t mt = ((type >= 2 && type <= 8) ? (uint32_t)type
                                                     : (uint32_t)packet::NET_MESSAGE_GAME_MESSAGE);
            ByteStream<std::uint16_t> bs;
            bs.write(mt);
            bs.write(std::string(data), false);
            sv->get_player()->send_packet(bs.get_data(), 0);
            return 0;
        }, 1);
        lua_setglobal(L, "SendPacketToClient");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            const char* text = luaL_checkstring(ls, 1);
            auto* sv = c->get_server() ? c->get_server()->get_player() : nullptr;
            auto* cl = c->get_client() ? c->get_client()->get_player() : nullptr;
            if (!sv || !cl) { AppendLog("[LUA] SendChat: not connected"); return 0; }
            std::string raw = "action|input\ntext|"; raw += text; raw += "\n";
            TextParse tp{raw};
            core::EventMessage em{*sv, *cl, tp};
            em.from = core::EventFrom::FromClient;
            c->get_event_dispatcher().dispatch(em);
            if (!em.canceled) {
                ByteStream<std::uint16_t> bs;
                bs.write((uint32_t)packet::NET_MESSAGE_GENERIC_TEXT);
                bs.write(raw, false);
                cl->send_packet(bs.get_data(), 0);
            }
            return 0;
        }, 1);
        lua_setglobal(L, "SendChat");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            const char* world = luaL_checkstring(ls, 1);
            auto* cl = c->get_client();
            if (!cl || !cl->get_player() || !cl->get_player()->is_connected()) {
                AppendLog("[LUA] Warp: not connected"); return 0;
            }
            std::string raw = "action|join_request\nname|"; raw += world; raw += "\ninvitedWorld|0\n";
            ByteStream<std::uint16_t> bs;
            bs.write((uint32_t)packet::NET_MESSAGE_GAME_MESSAGE);
            bs.write(raw, false);
            cl->get_player()->send_packet(bs.get_data(), 0);
            AppendLog(std::string("[LUA] Warp -> ") + world);
            return 0;
        }, 1);
        lua_setglobal(L, "Warp");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto lp = utils::PlayerTracker::get_instance().get_local_player();
            auto& wm = utils::WorldManager::get_instance();
            lua_createtable(ls, 0, 12);
            lua_pushstring(ls, lp.name.c_str());                lua_setfield(ls, -2, "name");
            lua_pushstring(ls, wm.get_world_name().c_str());    lua_setfield(ls, -2, "world");
            lua_pushstring(ls, lp.country.c_str());             lua_setfield(ls, -2, "country");
            lua_pushnumber(ls, lp.position.x);                  lua_setfield(ls, -2, "pos_x");
            lua_pushnumber(ls, lp.position.y);                  lua_setfield(ls, -2, "pos_y");
            lua_pushinteger(ls, (int)(lp.position.x / 32));     lua_setfield(ls, -2, "tile_x");
            lua_pushinteger(ls, (int)(lp.position.y / 32));     lua_setfield(ls, -2, "tile_y");
            lua_pushinteger(ls, lp.netID);                      lua_setfield(ls, -2, "netid");
            lua_pushinteger(ls, lp.userID);                     lua_setfield(ls, -2, "userid");
            lua_pushinteger(ls, 0);                             lua_setfield(ls, -2, "gems");
            lua_pushboolean(ls, 0);                             lua_setfield(ls, -2, "facing_left");
            lua_pushinteger(ls, 0);                             lua_setfield(ls, -2, "flags");
            return 1;
        }, 0);
        lua_setglobal(L, "GetLocal");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto all = utils::PlayerTracker::get_instance().get_all_players();
            lua_createtable(ls, (int)all.size(), 0);
            int i = 1;
            for (auto& kv : all) {
                auto& p = kv.second;
                lua_createtable(ls, 0, 8);
                lua_pushstring(ls, p.name.c_str());              lua_setfield(ls, -2, "name");
                lua_pushstring(ls, p.country.c_str());           lua_setfield(ls, -2, "country");
                lua_pushnumber(ls, p.position.x);                lua_setfield(ls, -2, "pos_x");
                lua_pushnumber(ls, p.position.y);                lua_setfield(ls, -2, "pos_y");
                lua_pushinteger(ls, (int)(p.position.x / 32));   lua_setfield(ls, -2, "tile_x");
                lua_pushinteger(ls, (int)(p.position.y / 32));   lua_setfield(ls, -2, "tile_y");
                lua_pushinteger(ls, p.netID);                    lua_setfield(ls, -2, "netid");
                lua_pushinteger(ls, p.userID);                   lua_setfield(ls, -2, "userid");
                lua_rawseti(ls, -2, i++);
            }
            return 1;
        }, 0);
        lua_setglobal(L, "GetPlayers");
        lua_getglobal(L, "GetPlayers"); lua_setglobal(L, "GetPlayerList");

        lua_pushcclosure(L, [](lua_State* ls) -> int {
            const auto net_id = static_cast<uint32_t>(luaL_checkinteger(ls, 1));
            auto player = utils::PlayerTracker::get_instance().get_player_by_netid(net_id);
            if (player.name.empty()) { lua_pushnil(ls); return 1; }
            lua_createtable(ls, 0, 9);
            lua_pushstring(ls, player.name.c_str()); lua_setfield(ls, -2, "name");
            lua_pushstring(ls, player.country.c_str()); lua_setfield(ls, -2, "country");
            lua_pushinteger(ls, player.netID); lua_setfield(ls, -2, "netid");
            lua_pushinteger(ls, player.userID); lua_setfield(ls, -2, "userid");
            lua_pushnumber(ls, player.position.x); lua_setfield(ls, -2, "pos_x");
            lua_pushnumber(ls, player.position.y); lua_setfield(ls, -2, "pos_y");
            lua_pushinteger(ls, static_cast<int>(player.position.x / 32.0f)); lua_setfield(ls, -2, "tile_x");
            lua_pushinteger(ls, static_cast<int>(player.position.y / 32.0f)); lua_setfield(ls, -2, "tile_y");
            lua_pushboolean(ls, player.is_local); lua_setfield(ls, -2, "is_local");
            return 1;
        }, 0);
        lua_setglobal(L, "GetPlayerByNetID");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto items = utils::InventoryManager::get_instance().get_items_snapshot();
            lua_createtable(ls, (int)items.size(), 0);
            int i = 1;
            for (auto& it : items) {
                lua_createtable(ls, 0, 2);
                lua_pushinteger(ls, it.id);     lua_setfield(ls, -2, "id");
                lua_pushinteger(ls, it.amount); lua_setfield(ls, -2, "count");
                lua_rawseti(ls, -2, i++);
            }
            return 1;
        }, 0);
        lua_setglobal(L, "GetInventory");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            lua_pushinteger(ls, utils::InventoryManager::get_instance()
                .get_item_count((uint16_t)luaL_checkinteger(ls, 1)));
            return 1;
        }, 0);
        lua_setglobal(L, "GetItemCount");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            const auto& items = utils::WorldManager::get_instance().get_items();
            lua_createtable(ls, (int)items.size(), 0);
            int i = 1;
            for (auto& it : items) {
                lua_createtable(ls, 0, 6);
                lua_pushinteger(ls, it.ItemId); lua_setfield(ls, -2, "id");
                lua_pushinteger(ls, it.Uid);    lua_setfield(ls, -2, "oid");
                lua_pushnumber(ls,  it.X);      lua_setfield(ls, -2, "pos_x");
                lua_pushnumber(ls,  it.Y);      lua_setfield(ls, -2, "pos_y");
                lua_pushinteger(ls, it.Amount); lua_setfield(ls, -2, "count");
                lua_pushinteger(ls, it.Flag);   lua_setfield(ls, -2, "flags");
                lua_rawseti(ls, -2, i++);
            }
            return 1;
        }, 0);
        lua_setglobal(L, "GetObjects");
        lua_getglobal(L, "GetObjects"); lua_setglobal(L, "get_dropped_items");
        lua_getglobal(L, "GetObjects"); lua_setglobal(L, "GetWorldObject");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            int x = (int)luaL_checkinteger(ls, 1), y = (int)luaL_checkinteger(ls, 2);
            auto& wm = utils::WorldManager::get_instance();
            const auto& tiles = wm.get_tiles();
            int idx = y * (int)wm.get_world_width() + x;
            lua_createtable(ls, 0, 8);
            if (idx >= 0 && idx < (int)tiles.size()) {
                lua_pushinteger(ls, tiles[idx].Fg); lua_setfield(ls, -2, "fg");
                lua_pushinteger(ls, tiles[idx].Bg); lua_setfield(ls, -2, "bg");
            } else {
                lua_pushinteger(ls, 0); lua_setfield(ls, -2, "fg");
                lua_pushinteger(ls, 0); lua_setfield(ls, -2, "bg");
            }
            lua_pushinteger(ls, x); lua_setfield(ls, -2, "pos_x");
            lua_pushinteger(ls, y); lua_setfield(ls, -2, "pos_y");
            lua_pushinteger(ls, 0); lua_setfield(ls, -2, "flags");
            lua_pushboolean(ls, 0); lua_setfield(ls, -2, "water");
            lua_pushboolean(ls, 0); lua_setfield(ls, -2, "fire");
            lua_pushboolean(ls, 0); lua_setfield(ls, -2, "ready");
            return 1;
        }, 0);
        lua_setglobal(L, "GetTile");
        lua_getglobal(L, "GetTile"); lua_setglobal(L, "CheckTile");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto& wm = utils::WorldManager::get_instance();
            const auto& tiles = wm.get_tiles();
            uint32_t w = wm.get_world_width();
            lua_createtable(ls, (int)tiles.size(), 0);
            for (int i = 0; i < (int)tiles.size(); i++) {
                lua_createtable(ls, 0, 6);
                lua_pushinteger(ls, tiles[i].Fg);       lua_setfield(ls, -2, "fg");
                lua_pushinteger(ls, tiles[i].Bg);       lua_setfield(ls, -2, "bg");
                lua_pushinteger(ls, i % (int)w);        lua_setfield(ls, -2, "pos_x");
                lua_pushinteger(ls, i / (int)w);        lua_setfield(ls, -2, "pos_y");
                lua_pushinteger(ls, 0);                 lua_setfield(ls, -2, "flags");
                lua_pushboolean(ls, 0);                 lua_setfield(ls, -2, "water");
                lua_rawseti(ls, -2, i + 1);
            }
            return 1;
        }, 0);
        lua_setglobal(L, "GetTiles");
        lua_getglobal(L, "GetTiles"); lua_setglobal(L, "get_world_tiles");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto lp = utils::PlayerTracker::get_instance().get_local_player();
            lua_pushnumber(ls, lp.position.x);
            lua_pushnumber(ls, lp.position.y);
            return 2;
        }, 0);
        lua_setglobal(L, "GetPlayerPos");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            lua_pushstring(ls, utils::PlayerTracker::get_instance().get_local_player().name.c_str());
            return 1;
        }, 0);
        lua_setglobal(L, "GetPlayerName");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto& wm = utils::WorldManager::get_instance();
            lua_pushstring(ls, wm.has_world() ? wm.get_world_name().c_str() : "");
            return 1;
        }, 0);
        lua_setglobal(L, "GetWorldName");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            auto* cl = c->get_client();
            if (!cl || !cl->get_player() || !cl->get_player()->is_connected()) {
                lua_pushinteger(ls, 0); return 1;
            }
            lua_pushinteger(ls, (int)cl->get_player()->get_peer()->roundTripTime);
            return 1;
        }, 1);
        lua_setglobal(L, "GetPing");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            const char* title   = luaL_checkstring(ls, 1);
            const char* content = luaL_checkstring(ls, 2);
            auto* sv = c->get_server();
            if (!sv || !sv->get_player()) return 0;
            std::string dlg = "add_label_with_icon|big|";
            dlg += title; dlg += "|left|0|\nadd_textbox|";
            dlg += content; dlg += "|\nend_dialog|msgbox||OK|\n";
            packet::Variant var{};
            var.add(std::string("OnDialogRequest")); var.add(dlg);
            auto ext = var.serialize();
            packet::GameUpdatePacket pkt{};
            memset(&pkt, 0, sizeof(pkt));
            pkt.type = packet::PACKET_CALL_FUNCTION;
            pkt.net_id = -1; pkt.flags.extended = 1;
            pkt.data_size = (uint32_t)ext.size();
            ByteStream<std::uint16_t> bs;
            bs.write((uint32_t)packet::NET_MESSAGE_GAME_PACKET);
            bs.write(pkt); bs.write_data(ext.data(), ext.size());
            sv->get_player()->send_packet(bs.get_data(), 0);
            return 0;
        }, 1);
        lua_setglobal(L, "MessageBox");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            const char* name  = luaL_checkstring(ls, 1);
            const char* event = luaL_checkstring(ls, 2);
            luaL_checktype(ls, 3, LUA_TFUNCTION);
            lua_pushvalue(ls, 3);
            int ref = luaL_ref(ls, LUA_REGISTRYINDEX);
            std::lock_guard<std::mutex> lk(g_cb_mutex);
            g_callbacks.push_back({name, event, ref});
            return 0;
        }, 0);
        lua_setglobal(L, "AddCallback");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            const char* name = luaL_checkstring(ls, 1);
            auto* bridge = lua::LuaManager::get();
            if (!bridge) return 0;
            std::lock_guard<std::recursive_mutex> state_lock(bridge->state_mutex());
            std::lock_guard<std::mutex> lk(g_cb_mutex);
            auto it = g_callbacks.begin();
            while (it != g_callbacks.end()) {
                if (it->name == name) {
                    if (it->ref != LUA_NOREF && it->ref != LUA_REFNIL)
                        luaL_unref(ls, LUA_REGISTRYINDEX, it->ref);
                    it = g_callbacks.erase(it);
                } else ++it;
            }
            return 0;
        }, 0);
        lua_setglobal(L, "RemoveCallback");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* bridge = lua::LuaManager::get();
            if (!bridge) return 0;
            std::lock_guard<std::recursive_mutex> state_lock(bridge->state_mutex());
            std::lock_guard<std::mutex> lk(g_cb_mutex);
            for (const auto& callback : g_callbacks)
                if (callback.ref != LUA_NOREF && callback.ref != LUA_REFNIL)
                    luaL_unref(ls, LUA_REGISTRYINDEX, callback.ref);
            g_callbacks.clear(); return 0;
        }, 0);
        lua_setglobal(L, "RemoveCallbacks");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            const int id = static_cast<int>(luaL_checkinteger(ls, 1));
            auto* database = extension::item_finder::ItemDatabase::get_instance();
            const auto* item = database ? database->get_item_by_id(id) : nullptr;
            if (!item) { lua_pushnil(ls); return 1; }
            lua_createtable(ls, 0, 9);
            lua_pushinteger(ls, item->id); lua_setfield(ls, -2, "id");
            lua_pushstring(ls, item->name.c_str()); lua_setfield(ls, -2, "name");
            lua_pushinteger(ls, item->type); lua_setfield(ls, -2, "type");
            lua_pushinteger(ls, item->type); lua_setfield(ls, -2, "item_type");
            lua_pushinteger(ls, item->rarity); lua_setfield(ls, -2, "rarity");
            lua_pushstring(ls, item->file_name.c_str()); lua_setfield(ls, -2, "file_name");
            lua_pushinteger(ls, item->clothing_type); lua_setfield(ls, -2, "clothing_type");
            lua_pushinteger(ls, item->properties); lua_setfield(ls, -2, "properties");
            lua_pushstring(ls, item->description.c_str()); lua_setfield(ls, -2, "description");
            lua_pushstring(ls, item->info.c_str()); lua_setfield(ls, -2, "info");
            return 1;
        }, 0);
        lua_setglobal(L, "GetIteminfo");
        lua_getglobal(L, "GetIteminfo"); lua_setglobal(L, "GetItemInfo");
        lua_getglobal(L, "GetIteminfo"); lua_setglobal(L, "GetItemByID");

        
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            lua_pushboolean(ls, 0); return 1;
        }, 0);
        lua_setglobal(L, "IsSolid");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            uint32_t id = (uint32_t)luaL_checkinteger(ls, 1);
            auto* cl = c->get_client();
            if (!cl || !cl->get_player()) return 0;
            auto* srv = cl->get_player();

            auto& wm = utils::WorldManager::get_instance();
            const auto& items = wm.get_items();
            const auto& live = wm.get_live_objects();

            int sent = 0;
            auto process = [&](const world::DroppedItemInfo& item) {
                if (item.ItemId == id) {
                    
                    for (int brute = -10; brute <= 10; ++brute) {
                        uint32_t b_uid = static_cast<uint32_t>(static_cast<int32_t>(item.Uid) + brute);
                        if (b_uid == 0) continue;
                        command::AutoCollectCommand::send_collect_packet(srv, b_uid, item.X, item.Y);
                    }
                    sent++;
                }
            };
            for (const auto& it : items) process(it);
            for (const auto& it : live)  process(it);
            lua_pushinteger(ls, sent); return 1;
        }, 1);
        lua_setglobal(L, "Collect");

        
        lua_pushlightuserdata(L, core_);
        lua_pushcclosure(L, [](lua_State* ls) -> int {
            auto* c = static_cast<core::Core*>(lua_touserdata(ls, lua_upvalueindex(1)));
            uint32_t tx = (uint32_t)luaL_checkinteger(ls, 1), ty = (uint32_t)luaL_checkinteger(ls, 2);
            auto* cl = c->get_client();
            if (!cl || !cl->get_player()) { lua_pushinteger(ls, -1); return 1; }
            int steps = command::FindPathCommand::run_path(cl, tx, ty, false);
            lua_pushinteger(ls, steps); return 1;
        }, 1);
        lua_setglobal(L, "FindPath");
    }

    void load_scripts() {
        auto* bridge = lua::LuaManager::get();
        if (!bridge) return;
        std::lock_guard<std::recursive_mutex> state_lock(bridge->state_mutex());
        std::filesystem::path dir = "scripts";
        if (!std::filesystem::exists(dir))
            std::filesystem::create_directory(dir);
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".lua") {
                auto err = lua::LuaManager::get()->execute_with_error([&]() -> std::string {
                    std::ifstream f(entry.path());
                    return std::string(std::istreambuf_iterator<char>(f),
                                       std::istreambuf_iterator<char>());
                }());
                if (!err.empty())
                    AppendLog("[LUA] Error in " + entry.path().filename().string() + ": " + err);
            }
        }
    }
};

} 
