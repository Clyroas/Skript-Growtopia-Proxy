// Regression tests for the dropped-items ("floating items") section of the world
// parser. These exist because the Smart-Scan silently found zero items in many
// real worlds: it never tried the classic official record layout (14 bytes with a
// 16-bit uid) nor the no-filler item start used by official servers.
//
// Layouts covered:
//   official : [count u32][records: id u16, x f32, y f32, count u8, flags u8, uid u16][last_uid u32]
//   gtps     : [count u32][4 filler bytes][records: id u16, x f32, y f32, count u32, flags u8, uid u32][last_uid u32]
//   empty    : [count u32 = 0][last_uid u32]

#include "../src/utils/world_parser_v2.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Writer {
    std::vector<uint8_t> bytes;

    template <typename T>
    void put(T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }

    void put_string(const std::string& s) {
        put<uint16_t>(static_cast<uint16_t>(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }

    void put_record_official(uint16_t id, float x, float y, uint8_t count, uint16_t uid) {
        put<uint16_t>(id);
        put<float>(x);
        put<float>(y);
        put<uint8_t>(count);
        put<uint8_t>(0);
        put<uint16_t>(uid);
    }

    void put_record_gtps(uint16_t id, float x, float y, uint32_t count, uint32_t uid) {
        put<uint16_t>(id);
        put<float>(x);
        put<float>(y);
        put<uint32_t>(count);
        put<uint8_t>(0);
        put<uint32_t>(uid);
    }
};

// Builds the fixed world header + tile grid (all tiles plain, no extras).
Writer make_world(const std::string& name, uint32_t width, uint32_t height) {
    Writer w;
    w.put<uint16_t>(0x1A);            // version
    w.put<uint32_t>(0);               // flags
    w.put_string(name);
    w.put<uint32_t>(width);
    w.put<uint32_t>(height);
    const uint32_t tile_count = width * height;
    w.put<uint32_t>(tile_count);
    for (int i = 0; i < 5; ++i) w.put<uint8_t>(0);   // skipped header bytes
    for (uint32_t t = 0; t < tile_count; ++t) {
        w.put<uint16_t>(t % 7 == 0 ? 2 : 0);         // fg: some dirt
        w.put<uint16_t>(0);                          // bg
        w.put<uint16_t>(0);                          // parent
        w.put<uint16_t>(0);                          // flags: no extra data
    }
    return w;
}

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("  ok: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

bool close(float a, float b) {
    return std::fabs(a - b) < 0.01f;
}

void test_official_layout() {
    std::printf("official 14-byte records, no filler:\n");
    Writer w = make_world("OFFICIAL", 100, 60);
    w.put<uint32_t>(3);  // object count
    w.put_record_official(2, 32.0f, 32.0f, 1, 1);
    w.put_record_official(32, 640.0f, 96.0f, 5, 2);
    w.put_record_official(100, 128.0f, 1600.0f, 200, 3);
    w.put<uint32_t>(3);  // last uid

    world_v2::World world;
    const bool ok = world.parse(w.bytes.data(), w.bytes.size());
    check(ok, "parse succeeds");
    check(world.dropped_items.size() == 3, "all 3 floating items detected");
    if (world.dropped_items.size() == 3) {
        check(world.dropped_items[0].id == 2 && world.dropped_items[1].id == 32 &&
                  world.dropped_items[2].id == 100,
              "item ids decoded");
        check(world.dropped_items[1].count == 5 && world.dropped_items[2].count == 200,
              "item counts decoded");
        check(world.dropped_items[2].uid == 3, "16-bit uids decoded");
        check(close(world.dropped_items[1].x, 640.0f) && close(world.dropped_items[1].y, 96.0f),
              "positions decoded");
    }
    check(world.last_dropped_item_uid == 3, "last uid high-water mark decoded");
}

void test_gtps_layout() {
    std::printf("gtps 19-byte records with 4 filler bytes:\n");
    Writer w = make_world("GTPS", 100, 60);
    w.put<uint32_t>(2);       // object count
    w.put<uint32_t>(0);       // filler
    w.put_record_gtps(32, 64.0f, 32.0f, 10, 100);
    w.put_record_gtps(2, 96.0f, 64.0f, 1, 101);
    w.put<uint32_t>(101);     // last uid

    world_v2::World world;
    const bool ok = world.parse(w.bytes.data(), w.bytes.size());
    check(ok, "parse succeeds");
    check(world.dropped_items.size() == 2, "both floating items detected");
    if (world.dropped_items.size() == 2) {
        check(world.dropped_items[0].id == 32 && world.dropped_items[0].count == 10,
              "32-bit counts decoded");
        check(world.dropped_items[1].uid == 101, "32-bit uids decoded");
    }
}

void test_empty_and_broken_sections() {
    std::printf("empty object section and garbage records:\n");
    {
        Writer w = make_world("EMPTY", 100, 60);
        w.put<uint32_t>(0);   // object count
        w.put<uint32_t>(0);   // last uid

        world_v2::World world;
        const bool ok = world.parse(w.bytes.data(), w.bytes.size());
        check(ok, "parse succeeds");
        check(world.dropped_items.empty(), "no items invented for empty world");
        check(!world.had_warnings, "empty object section is not a warning");
    }
    {
        Writer w = make_world("GARBAGE", 100, 60);
        for (int i = 0; i < 64; ++i) {
            w.put<uint32_t>(0xDEADBEEF);
        }

        world_v2::World world;
        const bool ok = world.parse(w.bytes.data(), w.bytes.size());
        check(ok, "parse succeeds");
        check(world.dropped_items.empty(), "garbage trailing bytes yield no items");
    }
}

}  // namespace

int main() {
    test_official_layout();
    test_gtps_layout();
    test_empty_and_broken_sections();
    if (failures != 0) {
        std::printf("WORLD PARSER TEST FAILED (%d check(s))\n", failures);
        return 1;
    }
    std::printf("ALL CHECKS PASSED\n");
    return 0;
}
