# Changelog

## [2.3.2] — Floating items on official servers, GrowScan accuracy, warning noise

### Fixed — floating items still undetected on official worlds

The item-section scan only tried GTPS-style record layouts: record sizes 16-24 with
a 32-bit uid, and it always assumed 4 filler bytes between the item count and the
first record. Official servers use the classic layout — 14-byte records with an
8-bit count and 16-bit uid, with no filler — which the scan therefore never found,
so official worlds showed blocks but zero floating items. The scan now:

- tries every record size from 13 to 28 bytes, with per-size count/uid field
  widths and positions (16-bit uids included);
- tries both item starts (immediately after the count, and after 4 filler bytes);
- bounds coordinates by the actual world dimensions instead of a fixed ceiling;
- searches up to 4096 bytes past the tile region, so a mildly misaligned tile
  parse no longer hides the item section.

A new `world_parser` test feeds synthetic worlds in both layouts to the parser and
fails if the items are not decoded, so this cannot silently regress again.

### Fixed — GrowScan misinformation

- GrowScan read the world manager's unsynchronized reference getters, so a world
  change mid-scan could produce torn, inconsistent results. It now uses locked
  snapshots.
- Dropped items were double-counted: the interceptor copies world items into the
  live-object list when they are touched, and GrowScan summed both lists. Live
  copies are now deduplicated by uid, and synthetic uids assigned to live drops
  start at 1'000'000 so they can never collide with real world uids (which also
  protects `/pickup` and AutoCollect matching).

### Changed — world-parse warning overlay

The "World parse warning" overlay fired on any parser warning, including benign
byte-resyncs and count clamps. It now appears only when tiles were actually lost
(filled as empty / fewer tiles than the header claims), with accurate wording.

## [2.3.1] — Session-kill fixes: reconnects no longer destroy the live session

The proxy's own reconnect machinery was killing healthy sessions, which looked to the
player like "the last feature I used caused a random disconnect". Three defects, all
visible in the run logs (`peer slots in use`, superseded peers saying goodbye after
their session had already been replaced, and login packets dropped during every
reconnect):

### Fixed — a stale peer's goodbye tore down the live session

Both hosts allow overlapping peers (8 slots), but neither `Client::on_disconnect` nor
`Server::on_disconnect` checked *which* peer had disconnected. A DISCONNECT event for
a stale peer — a superseded session, a rejected connection, or a timed-out connect
attempt still occupying a slot — destroyed the **current** `player_`, sent the client
an error, and disconnected both sides. Every fresh reconnect inherited whatever stale
peers were still saying goodbye. Both handlers now ignore disconnect events for peers
that are not the active session's peer, matching the identity checks the receive
paths already had.

### Fixed — login packets dropped during the upstream handshake race

The client starts sending its login the moment the proxy accepts it, but the upstream
ENet handshake usually completes a few round-trips later. `Server::on_receive` waited
only 50 ms for the upstream and then **dropped** the packet — often the login hello
itself. The server saw an empty session, the client hung and gave up, and the retry
loop restarted the cycle (visible in the log as `Real server still not connected,
dropping packet` on every reconnect). Early packets are now queued (up to 32) and
replayed in order by `Client::on_connect` once the upstream peer exists. Queued
packets from a *previous* client session are purged on every new client connect, on
disconnect, and in the destructor, so they can never leak into a fresh handshake.

## [2.3.0] — Teleporter and dropped-item detection

### Added — teleporter

- `/tp <tile_x> <tile_y>` teleports the local player to a tile position (validated
  against the loaded world's dimensions before sending).
- `/player <name>` is now functional: it looks the player up in the position tracker
  (case- and colour-code-insensitive) and teleports to their last known position.
- `/sp tp` traverses the scanpath marker list by teleporting through it instead of
  pathfinding; `/sp`-again stop and world-change abort behave as in walk mode.

### Fixed — floating items undetected in many worlds

The dropped-items section of incoming world data was located by a loose heuristic
that accepted the first plausible-looking offset. In many worlds it locked onto
garbage (decoding zero items) or found nothing, so a world would load with tiles
but report no dropped items at all. The scan now:

- validates candidates structurally instead of accepting the first guess: the u32
  list length must fit the remaining bytes, sampled uids should run consecutively,
  and the u32 trailing the block should equal the last object's uid;
- falls back to the next-best candidate when one decodes zero items, instead of
  giving up;
- widens the search window (128 -> 256 bytes) and the item-count ceiling
  (15000 -> 100000);
- widens coordinate plausibility to large custom worlds (8192 px) and rejects
  non-finite floats;
- warns with decoded/claimed counts when a header claims more items than were
  decoded, so format variants are diagnosable in the log.

## [2.2.0] — Convenience commands, door-ID overlay, and scripting polish

Builds on the 2.1.0 compatibility work with quality-of-life commands driven from the
proxy's chat interface, a visual door-ID overlay, and Lua API conveniences.

### Added — chat commands

| Command | Aliases | What it does |
| --- | --- | --- |
| `/pickup <item_id>` | — | Collects the nearest cached dropped stack of an item (reuses the AutoCollect collect packet; searches both world items and live objects) |
| `/scanpath [scan]` | `/sp` | `/scanpath scan` lists Path Marker (item 1684) and Objective Marker (item 4482) tiles; `/sp` traverses the Path Markers in map order asynchronously, `/sp` again stops it, and a world change aborts the traversal |
| `/res` | `/respawn` | Requests an immediate respawn |
| `/rndm` | `/randomworld` | Warps to a recently visited world |
| `/spinall` | — | Toggles QQ/REME annotations for reported wheel spins |
| `/fastwheel` | — | Toggles instant display for reported roulette spins |

### Added — door-ID overlay

The ImGui GUI can now render door IDs directly over the Growtopia window
("Show Door IDs over Growtopia doors" checkbox), backed by `UpdateDoorIdOverlay()`
/ `HideDoorOverlay()` and feeding off the same data as `/doorid`.

### Added — Lua API

- `GetPlayerByNetID(net_id)` — direct player lookup by network ID.
- Compatibility aliases so older scripts keep working: `SleepMS`, `SendVariant`,
  `GetPlayerList`, `CheckTile`, `GetItemInfo`, `GetItemByID`.
- New command `/scanpath` replaces the earlier `scripts/scanpath.lua` prototype: same
  `/scan` + `/sp` surface, but implemented natively on a cancellable worker thread and
  no longer dependent on user-side Lua scripts being installed.

### Fixed

- `WorldManager::get_tiles_snapshot()` added: a locked copy of the tile grid. The
  existing reference getter returns unsynchronized storage, so worker threads could
  iterate a vector that a concurrent world swap invalidates. The scanpath traversal
  and `FindPathCommand::run_path` now use the snapshot.

### Changed

- `WorldManager` exposes a live-objects snapshot consumed by `/pickup`.
- `/doorid` and `/vendloc` output refinements; `/find` and `/vendfind` updates.
- `TextParse` gained `get_redacted_key_values()`: login-traffic logging now redacts
  credentials (`tankidPass`, tokens, MAC, `rid`, `wk`, ...) instead of writing them
  to disk.
- `scripts/pathfinding.lua` marker handling tweaks.

### Verification

All 7 standalone test suites pass (`tests/run_tests.ps1`): byte_stream,
server_data_parser, spawn_field, packet_variant, declaration_handling,
item_database, item_database_edge.

## [2.1.0] — Latest-Growtopia compatibility patch

This release makes the proxy survive Growtopia client updates instead of breaking on
them. The previous revision hard-coded a `5.21` / protocol `312` pair that was **never
read by any code**, and parsed the login response in a way that silently discarded
fields the modern client depends on.

### Why a version bump alone never needed a patch

`client.game_version` and `client.protocol` were **dead config**. Every occurrence in
the whole source tree was their own declaration:

```
$ rg -n 'game_version|client\.protocol' src/
src/core/config.cpp:12:    { "client.game_version", "5.21" },
src/core/config.cpp:13:    { "client.protocol", 312 },
```

No read site existed. The proxy forwards the client's own `version=` / `protocol=` form
body to Growtopia verbatim, so a client update was always transparently relayed. What
actually breaks on a new client is the **wire-format** handling, which this patch fixes.

### Fixed — login response round-trip (the critical one)

`TextParse` was used to parse `server_data.php` and re-serialise it. It destroyed data:

| Defect | Consequence |
| --- | --- |
| Lines without a `\|` were dropped entirely | The trailing ProtonSDK `RTENDMARKERBS1001` marker **disappeared** from the response |
| `std::unordered_map` storage | Field order was **randomised** (some clients are order-sensitive) |
| `set()` was a silent no-op on a missing key | A renamed `server` key meant the client got the **real** server address and the proxy was **silently bypassed** |
| `std::stoi(get("port"))` threw on missing/non-numeric | Renamed or absent `port` produced an **HTTP 500** |
| `tokenize` skipped empty tokens | A `key\|` line's key **vanished** from the response |
| No `\r` / `\0` trimming | A trailing `\r` was kept in the hostname and broke `enet_address_set_host` |
| JSON bodies unsupported | Any JSON response produced `502 Invalid server response` |

Replaced with a purpose-built order-preserving parser ([`server_data_parser.hpp`](src/utils/server_data_parser.hpp))
that keeps every field, empty values, unknown/new fields, the end marker and the exact
original line order, while rewriting only `server`, `port` and (when present)
`beta_server`, `beta_port`, `type2`. Port parsing is exception-free (`std::from_chars`)
and rejects `0` / `> 65535`. A response with no usable `server` value now returns an
actionable 502 diagnostic naming the client version, protocol and upstream reply instead
of failing obscurely.

### Added — version/protocol observability and pinning

- The proxy now reads the `version` / `protocol` / `platform` the client sends, logs them
  once per session, and **persists** them into `client.game_version` / `client.protocol`
  whenever the client changes. These keys are finally live rather than dead config.
- New optional `client.version_override` / `client.protocol_override` keys force a
  specific declaration upstream (blank / `0` = pass the client's own values through).
  Defaults are transparent, so behaviour is unchanged unless you opt in.
- New keys default to `auto` and `0` so a fresh install self-populates on first connect
  and never needs a code change for a future version bump.

### Fixed — silent proxy bypass on world change

`OnSendToServer` parsing could throw `std::out_of_range` when the address field carried
only one token. The exception was swallowed by the variant dispatcher's catch handler and
`evt.canceled` was never set, so the **original packet was forwarded and the client
connected straight to the real sub-server, bypassing the proxy for every later world
join**. Now bounds-checked, validated, and non-throwing; on anything unexpected it leaves
the packet untouched and logs a warning rather than half-rewriting it.

### Fixed — variant numeric type strictness

`Variant::get<int32_t>` returned `0` whenever a field arrived as `UNSIGNED` or `FLOAT`
(`std::get` throws `bad_variant_access`, which was swallowed). Added
`Variant::get_any_int()` and used it for the `OnSendToServer` port/token/user/login_mode
fields, so a type-tag change no longer silently zeroes the handoff.

### Fixed — spawn field matching

`replace_or_add_field` searched for `"<field>|"` anywhere in the buffer, so `mstate|`
**matched inside `smstate|`**. When `mstate` was absent but `smstate` present, `mstate`
was never inserted. Matching is now anchored to a line start.

### Fixed — item database robustness

- `get_item_by_id` indexed the vector with the item ID, which is only correct while the
  JSON is perfectly contiguous and ordered. It now uses an explicit id→index map.
- **A single bad field in a single item used to reject the entire database.** Every read
  went through `json::value()`, which does a strict `get<T>()` and throws
  `type_error::302` on a type mismatch; the shared catch discarded the reason, so the
  proxy silently fell back to "Item finder will not be available". All reads now go
  through type-tolerant helpers that coerce numerics/strings where sensible and fall back
  to a default instead of aborting.
- The loader parses into local containers and swaps on success, so a failed reload can no
  longer leave a half-populated database.
- The shipped database has 13 IDs shared by *different* items. Duplicate IDs are now
  resolved deterministically (first occurrence wins) and counted, instead of the previous
  index-based behaviour that could return the wrong item for `viewdetail_` / `wear_`.
- Malformed entries (non-objects, missing/negative IDs) are skipped and counted rather
  than being fatal; `items` being absent or not an array is rejected cleanly.
- The schema `version` is read defensively (a string `"24"` no longer kills the load) and
  reported at startup alongside the item count, so a stale database is visible.
- Unknown item `type` values now render as `Type <n>` instead of `Unknown` (1,082 shipped
  items already fell into that bucket).
- Removed a hard-coded developer path (`C:\Users\11User\...`) that was searched before the
  bundled database. A `SKRIPT_ITEMS_JSON` environment variable can now override it, and
  the extension registers its listener even if the initial load fails, retrying lazily on
  the first `/find` instead of staying disabled for the whole session.

### Changed

- `server.port` default corrected to `17091` (was `16999`, which disagreed with the
  shipped `config.json`) and registered as `unsigned int` to match its read sites; as an
  `int` default, `get<unsigned int>` silently returned `0`.
- `client.game_version` default `"5.21"` → `"auto"`; `client.protocol` default `312` → `0`.

### Added — one-command build

`build.ps1` installs Conan if missing, configures CMake and builds Release, and reports
where `Skript.exe` landed. It also handles the traps that this project actually hits:

- Visual Studio's bundled CMake and Ninja are used when no separate CMake is on `PATH`.
- The bundled CMake is exposed through a small `cmake.bat` shim, because **Conan shells out
  to `cmake` when it builds dependencies from source** and fails with
  `'cmake' is not recognized` otherwise.
- CMake 4.x is detected and given `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`, since the vendored
  libraries declare `cmake_minimum_required` below 3.5.
- Native tools writing to stderr no longer abort the script, and `config.json` is copied
  next to the binary because it is read from the working directory.

### Fixed — random disconnects during play

- **ENet was called from several threads with no synchronisation.** `Player::send_packet`
  reaches `enet_peer_send` from detached automation workers (dropat, autocollect, autocomp,
  packet_utils, …) and from the ImGui command thread, while the relay thread walked the same
  intrusive command/reliable lists inside `enet_host_service`. Concurrent list mutation
  corrupts ENet's reliable-sequence bookkeeping, which does not fail at once — the peer
  stops advancing and times out 5–20 s later, presenting exactly as a random disconnect.
  All `enet_host_service`, `enet_peer_send`, `enet_peer_disconnect*` and `enet_host_flush`
  calls now hold one recursive mutex ([`enet_lock.hpp`](src/utils/enet_lock.hpp)).
- **A malformed or empty datagram tore the whole session down.** The client's receive path
  called `player_->disconnect()` on an out-of-bounds packet and again on an unreadable
  message type. Both now drop the packet and log a warning, leaving the session alive.
- **Both ENet hosts had `peerCount = 1`.** A client that reconnects before the previous peer
  has timed out — which happens on every sub-server switch and relog — was refused for lack
  of a free slot, and `enet_host_connect`'s `NULL` return was ignored at all three call
  sites. Both hosts now allow 8 peer slots and a failed `enet_host_connect` is logged.
- The client's transmit path never flushed: `enet_host_flush` is now called after servicing
  the upstream host, so outbound reliable packets are not batched behind the 16 ms service
  timeout.
- The upstream peer kept ENet's default timeout (32 / 5000 / 30000 ms, an effective budget
  of only ~5–15 s of un-ACKed reliable data) while the client-facing peer used a longer one.
  Both now use the same settings.

### Fixed — packet size limits were inconsistent and silently dropped data

The three caps disagreed, and a packet accepted by one side could be refused by the other
and discarded without any diagnostic:

| Path | Before | After |
| --- | --- | --- |
| `Player::send_packet` (both directions) | **768 KiB, silent** | 32 MiB, logged |
| client→proxy receive | 64 KiB, dropped | 32 MiB |
| server→proxy receive | 8 MiB, dropped | 32 MiB |

The 768 KiB send cap was the worst of the three: it was *lower* than the receive
allowances, so a large world-map packet or the ~5.3 MiB item-database packet was received
correctly and then silently discarded on the way out — the client sat on "Loading world",
which looks like a random disconnect. All three now share
[`packet_limits.hpp`](src/utils/packet_limits.hpp) so they cannot drift apart again.

### Fixed — packet handling

- `get_data()` returned the whole buffer **by value**, so every call copied it. The relay
  path calls it up to four times per packet, including multi-hundred-KB map packets. It now
  returns a reference; a `ByteStream(const std::byte*, size)` constructor was added so
  callers can construct from it without `const_cast`.
- `read_data`/`read_vector`/`read(string)` compared `data_.size() - read_offset_`, which
  **underflows** if the offset ever passes the end, letting the bounds check pass and
  `memcpy` read out of bounds. They now use a `get_remaining()` helper.
- `skip()` was unchecked and its semantics were ambiguous: 79 call sites use it as a
  *relative* advance (the world parser) while `server.cpp` used it as an *absolute* seek to
  rewind before re-forwarding a packet. `skip()` is now relative and clamped, and the
  absolute intent has an explicit `seek()`.
- The client's text-message reader subtracted one byte too many
  (`get_size() - sizeof(NetMessageType) - 1`), silently dropping the **last character** of
  every server→client text message before parsing. It now reads `get_remaining()`, matching
  the server side. The same defect in `filter.cpp` is fixed too.

### Notes and limitations

- A version bump on its own still needs **no** patch: the client's declaration is relayed
  verbatim. This patch removes the parsing traps that a wire-format change would hit.
- The bundled `resources/decoded_items.json` is a fixed snapshot (schema version 24,
  15947 items). New items are invisible to `/find` until the database is regenerated from
  a current `items.dat`; the loader tolerates both older and newer schemas but cannot
  invent data. Override the path with `SKRIPT_ITEMS_JSON`.
- Only three request headers (`User-Agent`, `Host`, `Content-Type`) are forwarded to
  Growtopia. If the upstream ever requires a client build hash or session header, it
  would be dropped — that is a known limitation, not fixed here.
- The ENet framing layer (`usingNewPacket`, `enet_crc32`) carries no version number, so a
  framing change surfaces as a 10-second connect timeout rather than a version error.

### Verification

Six standalone test suites under `tests/` cover the fixes. They depend on nothing but the
standard library plus the vendored `nlohmann/json`, so they run without Conan:

```powershell
powershell -ExecutionPolicy Bypass -File tests/run_tests.ps1
```

| Suite | Covers |
| --- | --- |
| `server_data_parser_test.cpp` | Field/order/marker preservation, CRLF, port parsing, form helpers |
| `spawn_field_test.cpp` | Line-anchored field matching, the `mstate`/`smstate` collision |
| `packet_variant_test.cpp` | `get_any_int` across signed/unsigned/float, the silent-zero regression |
| `declaration_handling_test.cpp` | Version/protocol recording, partial and non-numeric declarations |
| `item_database_test.cpp` | Real 30 MB database: id→item mapping, schema version, clean misses |
| `item_database_edge_test.cpp` | String version, type mismatches, duplicate IDs, junk entries, atomic swap |

Beyond those, every header this patch touches was compiled against the real
Conan-generated include paths and preprocessor definitions (`cl /Zs`, `SPDLOG_FMT_EXTERNAL`,
`CPPHTTPLIB_OPENSSL_SUPPORT`, and the rest) to confirm it builds in the project's actual
configuration, not just in isolation.
