# C++ High-Performance Network Interceptor & Lua Scripting Proxy

[![Discord](https://img.shields.io/badge/Discord-Join%20Community-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.gg/XcMrhfwuMf)

A modular, multi-threaded C++ network interception proxy and scripting sandbox. The application intercepts local game client connection requests via transparent SSL/DNS redirection, intercepts raw network packets, and exposes system hooks to an embedded **LuaJIT** scripting engine.

---

> [!IMPORTANT]
> ### 🛑 Educational Purposes Only
> This software is developed solely as an educational research prototype in network security, protocol reverse-engineering, and embedded systems design. It is designed to demonstrate local packet filtering, Win32 desktop overlays, and scripting integrations. 
> 
> **It is not intended to be used for unauthorized game automation, cheating, or exploiting on public servers.** The author does not condone, support, or assume any responsibility for misuse of this tool in online games.

---

## Key Features

- **Transparent SSL/DNS Redirection**: Intercepts HTTPS server list lookups (`server_data.php`) and dynamically routes traffic through a local custom proxy.
- **Asynchronous Protocol Engine**: Operates on a multi-threaded event loop, processing client and server game packet queues via **ENet**.
- **Embedded LuaJIT Core**: Embeds a high-performance Lua scripting compiler to inject packets, query client state, and execute real-time automation.
- **ImGui Desktop UI**: Implements a graphical control interface using **Dear ImGui** and DirectX 9/Win32.
- **Offline & Privacy Preserving**: Bypasses cloud licensing, remote database logging, and telemetry server connections. All logs, vending scans, and Locke trackers run 100% locally.

---

## Tech Stack & Architecture

- **Core**: C++23 (MSVC)
- **Dependency Management**: Conan Package Manager
- **Build System**: CMake (minimum version 3.24)
- **Network Stack**: ENet (UDP connection loop), cpp-httplib (HTTP/HTTPS tunnels)
- **Scripting Engine**: LuaJIT 2.1
- **UI Engine**: Dear ImGui (DirectX 9 / Win32 integration)
- **Logging & Events**: spdlog, eventpp dispatcher

---

## Getting Started

### Prerequisites
- Windows OS
- CMake (version 3.24 or later)
- Visual Studio 2022 (MSVC compiler with C++23 support)
- Conan Package Manager installed and configured in PATH

### Build Instructions

> **Quick start:** run `build.ps1` from an ordinary PowerShell window. It installs Conan if
> missing, configures CMake, builds Release, and reports where `Skript.exe` landed:
> ```powershell
> powershell -ExecutionPolicy Bypass -File build.ps1
> ```
> It also detects the common traps: CMake 4.x rejecting the vendored libraries, a missing
> C++ workload, and Conan not being on PATH. The manual steps are below if you prefer them.

1. Configure the project using CMake:
   ```powershell
   cmake -B build
   ```
   *Note: CMake will automatically call Conan to retrieve all external library dependencies (fmt, glm, spdlog, magic_enum, luajit).*

2. Build the executable in Release mode:
   ```powershell
   cmake --build build --config Release
   ```

3. Run the compiled binary:
   ```powershell
   .\build\src\Release\Skript.exe
   ```

#### Build notes and gotchas

- **Prefer CMake 3.31 over 4.x.** `lib/enet`, `lib/glm`, `lib/eventpp`, `lib/libressl` and
  `lib/nlohmann_json` declare `cmake_minimum_required` below 3.5, which CMake 4.x treats as
  a hard error. Install a 3.x CMake with `pip install --user cmake==3.31.6`, or pass
  `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.
- **Build from an ordinary terminal.** Restricted shells, sandboxes and some security
  products block the temporary folders pip and Conan need; the symptom is `Access is denied`
  on a `pip-*` path.
- **Visual Studio's bundled CMake works** (`...\Common7\IDE\CommonExtensions\Microsoft\CMake`),
  so a separate CMake install is optional.
- `config.json` is read from the **current working directory**, not the source tree, so run
  the binary from the folder containing it (the build copies it next to `Skript.exe`).
- Running the proxy needs **administrator rights** (it binds port 443 and edits `hosts`).

---

## Supporting a New Growtopia Version

The proxy relays the client's own `version=` / `protocol=` declaration to Growtopia
verbatim, so **a plain client version bump needs no code change**. It also has no version
gating: `client.game_version` and `client.protocol` are a *record of what the client
declared*, not a switch.

On first connect the proxy logs and stores what your client reports:

```
Growtopia client declared version=5.36 protocol=245 platform=0
Proxied server_data.php -> <host>:<port> (client version 5.36, protocol 245)
```

Those values are written back to `config.json` automatically whenever they change, so the
file always reflects the client you are actually running.

### Configuration keys

| Key | Default | Meaning |
| --- | --- | --- |
| `client.game_version` | `"auto"` | Recorded from the client on connect. Informational. |
| `client.protocol` | `0` | Recorded from the client on connect. Informational. |
| `client.version_override` | `""` | If set, forces this version upstream instead of the client's. |
| `client.protocol_override` | `0` | If non-zero, forces this protocol upstream. |

Overrides default to *transparent pass-through*. Only set them if you need to present a
different declaration than the client sends; the proxy logs a warning when one is active.

### If a new version does not connect

Work through the failure modes in order:

1. **`502 Growtopia did not return a usable server_data.php response`** — the proxy now
   prints the client version, protocol and the upstream reply, so you can see exactly what
   Growtopia rejected. This is usually a genuinely out-of-date client, not a proxy bug.
2. **`[CONNECTION] Server did not respond within 10s`** — the ENet framing layer
   (`usingNewPacket`, `enet_crc32`) changed. This is in `lib/enet/` and
   `src/client/client.cpp`; the framing carries no version number, so it cannot be
   detected earlier.
3. **`OnSendToServer: unexpected variant arity`** in the log — the world-switch packet
   layout changed. The handler is now bounds-checked and safe, but the field mapping in
   [`sub_server_switch_impl.hpp`](src/extension/sub_server_switch/sub_server_switch_impl.hpp)
   may need the new indices.
4. **New items missing from `/find`** — regenerate `resources/decoded_items.json` from a
   current `items.dat`, or point `SKRIPT_ITEMS_JSON` at your own copy. The loader tolerates
   both older and newer schemas, but it cannot invent item data.

`server_data.php` responses are preserved byte-for-byte except for the `server` / `port`
(and `beta_server` / `beta_port` / `type2`) fields, so **new or unknown fields added by a
future Growtopia update pass through untouched** — including the trailing
`RTENDMARKERBS1001` marker and field ordering.

### Tests

Five standalone suites under `tests/` cover the compatibility logic. They need nothing but
a compiler and the vendored `nlohmann/json`, so they run without Conan:

```powershell
powershell -ExecutionPolicy Bypass -File tests/run_tests.ps1
```

| Suite | Covers |
| --- | --- |
| `server_data_parser_test.cpp` | Field/order/marker preservation, CRLF, port parsing |
| `spawn_field_test.cpp` | Line-anchored spawn field matching |
| `packet_variant_test.cpp` | Type-tolerant numeric reads |
| `declaration_handling_test.cpp` | Version/protocol declaration recording |
| `item_database_test.cpp` | Real 30 MB database: id→item mapping, schema version |
| `item_database_edge_test.cpp` | String version, type mismatches, duplicate IDs, atomic swap |

See [CHANGELOG.md](CHANGELOG.md) for the full list of compatibility fixes.

---

## Lua Scripting API Documentation

The embedded scripting system runs `.lua` files placed in the `scripts/` folder. It exposes the following native API to automate player actions, analyze maps, and inject network packets:

### Native Globals

#### 1. System & Threads
- `log(text)`: Appends a text string to the local debug log file.
- `Sleep(ms)`: Yields thread execution for the specified milliseconds.
- `RunThread(function)`: Runs a Lua function on a managed worker. Workers are joined during Lua runtime shutdown.
- `SleepMS(ms)`: Compatibility alias for `Sleep(ms)`.

#### 2. Network Transmission (Outbound to Server)
- `SendPacket(type, raw_data)`: Sends a generic text/game message packet to the server (type `2` or `3`).
- `SendPacketRaw(gup_table)`: Injects a raw `GameUpdatePacket` structure to the server.
- `SendChat(text)`: Simulates sending chat input. Checks event dispatcher lists and submits the message.
- `Warp(world_name)`: Submits a join/warp packet to connect to the target world.
- `Collect(item_id)`: Sends item sweep-collect packets for the target item ID, brute-forcing dropped UIDs locally.

#### 3. Network Transmission (Inbound to Client)
- `SendPacketToClient(type, raw_data)`: Routes a generic text/game message packet to the local client.
- `SendPacketRawClient(gup_table)`: Routes a raw `GameUpdatePacket` structure to the local client.
- `SendVarlist(varlist_table)`: Sends a serialized variant list (such as `OnDialogRequest`) directly to the client.
- `SendVariant(varlist_table)`: Compatibility alias for `SendVarlist`.
- `MessageBox(title, content)`: Displays a custom visual popup dialog box inside the client window.

#### 4. Client State & Inventory
- `GetLocal()`: Returns a table representing the local player state.
  ```lua
  local player = GetLocal()
  print("Name: " .. player.name)
  print("Position: (" .. player.tile_x .. ", " .. player.tile_y .. ")")
  ```
- `GetPlayers()`: Returns an indexed list of all active players in the current world.
- `GetPlayerList()`: Compatibility alias for `GetPlayers()`.
- `GetPlayerByNetID(net_id)`: Returns the matching player table, or `nil` if that player is not tracked.
- `GetInventory()`: Returns an array snapshot of inventory slots (`{id, count}`).
- `GetItemCount(item_id)`: Queries the quantity of a specific item in the local inventory.
- `GetItemInfo(item_id)` / `GetItemByID(item_id)`: Returns decoded item metadata, or `nil` when the item database is unavailable or the ID is unknown. `GetIteminfo` remains as a compatibility alias.

#### 5. Map & Tile Analysis
- `GetWorldName()`: Returns the name of the current world.
- `GetObjects()`: Returns all dropped items currently located on the map.
- `GetTile(x, y)`: Returns metadata representing the tile block at map coordinate `(x, y)` (`{fg, bg, pos_x, pos_y, ready, water, fire}`).
- `CheckTile(x, y)`: Compatibility alias for `GetTile(x, y)`.
- `GetWorldObject()`: Compatibility alias for `GetObjects()`.
- `GetTiles()`: Returns an array of all map tile blocks.
- `FindPath(x, y)`: Calculates pathfinding routes and walks the player to coordinate `(x, y)`.

#### 6. Networking Callback Event Hooks
- `AddCallback(name, event, function)`: Registers an event listener function.
- `RemoveCallback(name)`: Unregisters a callback by name.
- `RemoveCallbacks()`: Unregisters all callback handlers.

##### Supported Events:
- `"OnVarlist"`: Triggers when the server submits a variant list (e.g. `OnDialogRequest`, `OnConsoleMessage`).
- `"OnPacket"`: Triggers when the client submits an outgoing generic text packet.
- `"OnRawPacket"`: Triggers on outgoing raw `GameUpdatePacket` items from the client.
- `"OnIncomingRawPacket"`: Triggers on incoming raw `GameUpdatePacket` items from the server.

---

### Lua Script Examples

#### 1. Basic Outbound Packet Injection
```lua
-- Warp to a world and log connection
RunThread(function()
    log("Warping to world START...")
    Warp("START")
    Sleep(3000)
    SendChat("Hello from Lua!")
end)
```

#### 2. Event Interception Callback
```lua
-- Block specified incoming chat packets
AddCallback("my_filter", "OnVarlist", function(varlist, netid)
    if varlist[0] == "OnTalkBubble" then
        local sender_id = varlist[1]
        local message = varlist[2]
        log("TalkBubble from netid " .. sender_id .. ": " .. message)
        
        if string.find(message, "CSN") then
            log("Blocked chat event message containing keyword")
            return true -- Return true to cancel packet propagation
        end
    end
    return false
end)
```
