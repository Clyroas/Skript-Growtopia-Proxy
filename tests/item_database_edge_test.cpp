#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "../src/extension/item_finder/item_finder.hpp"

static int failures = 0;

static void check(bool condition, const std::string& what)
{
    if (!condition) {
        std::cout << "FAIL: " << what << "\n";
        ++failures;
    }
    else {
        std::cout << "ok  : " << what << "\n";
    }
}

static void write_file(const std::string& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
}

int main()
{
    using extension::item_finder::ItemDatabase;

    const std::string tmp = "tests/_tmp_items.json";

    std::cout << "=== regression: version emitted as a STRING (not integer) ===\n";
    {
        write_file(tmp, R"({"version":"24","items":[{"id":1,"name":"Stone","type":0,"rarity":1}]})");
        ItemDatabase db{};
        const bool loaded = db.load_from_json(tmp);
        std::cout << "     load=" << (loaded ? "true" : "false")
                  << " count=" << db.get_item_count()
                  << " version=" << db.schema_version() << "\n";
        check(loaded, "string version still loads (was a crash-the-whole-load regression)");
        check(db.get_item_count() == 1, "the single item was kept");
        check(db.schema_version() == 24, "string version parsed to 24");
    }

    std::cout << "\n=== version absent entirely ===\n";
    {
        write_file(tmp, R"({"items":[{"id":1,"name":"Stone"}]})");
        ItemDatabase db{};
        check(db.load_from_json(tmp), "no version key still loads");
        check(db.schema_version() == 0, "version defaults to 0");
    }

    std::cout << "\n=== regression: a single type-mismatched field ===\n";
    {
        write_file(tmp, R"({"version":24,"items":[
            {"id":1,"name":"Stone","type":0},
            {"id":2,"name":12345,"type":"7","rarity":null,"_unk10":99}
        ]})");
        ItemDatabase db{};
        const bool loaded = db.load_from_json(tmp);
        std::cout << "     load=" << (loaded ? "true" : "false")
                  << " count=" << db.get_item_count() << "\n";
        check(loaded, "type-mismatched field does not reject the whole database (was: whole DB lost)");
        check(db.get_item_count() == 2, "both items retained");

        const auto* second = db.get_item_by_id(2);
        check(second != nullptr, "coerced item present");
        if (second) {
            check(second->name == "12345", "numeric name coerced to string");
            check(second->type == 7, "string type coerced to int");
            check(second->rarity == 0, "null rarity fell back to 0");
            check(second->info == "99", "numeric _unk10 coerced to string");
        }
    }

    std::cout << "\n=== duplicate ids: first occurrence wins, count reported ===\n";
    {
        write_file(tmp, R"({"version":24,"items":[
            {"id":10,"name":"First"},
            {"id":10,"name":"Second"},
            {"id":11,"name":"Other"}
        ]})");
        ItemDatabase db{};
        check(db.load_from_json(tmp), "duplicate-id database loads");
        check(db.get_item_count() == 2, "duplicate dropped from the item list");
        check(db.last_duplicate_ids() == 1, "one duplicate reported");
        const auto* item = db.get_item_by_id(10);
        check(item != nullptr && item->name == "First", "first occurrence deterministically wins");
    }

    std::cout << "\n=== malformed items are skipped, not fatal ===\n";
    {
        write_file(tmp, R"({"version":24,"items":[
            "not-an-object",
            42,
            {"name":"no id here"},
            {"id":-5,"name":"negative id"},
            {"id":7,"name":"Valid"}
        ]})");
        ItemDatabase db{};
        const bool loaded = db.load_from_json(tmp);
        check(loaded, "loads despite junk entries");
        check(db.get_item_count() == 1, "only the valid item kept");
        check(db.last_skipped() == 4, "four junk entries counted as skipped");
        check(db.get_item_by_id(7) != nullptr, "valid item reachable");
    }

    std::cout << "\n=== 'items' wrapped as an object / missing ===\n";
    {
        write_file(tmp, R"({"version":24,"items":{"0":{"id":0,"name":"X"}}})");
        ItemDatabase db{};
        check(!db.load_from_json(tmp), "object-shaped 'items' rejected");

        write_file(tmp, R"({"version":24,"data":{"items":[{"id":1}]}})");
        ItemDatabase db2{};
        check(!db2.load_from_json(tmp), "nested 'items' rejected");
        check(db2.get_item_count() == 0, "database left empty after rejection");
    }

    std::cout << "\n=== a failed load never leaves a half-populated database ===\n";
    {
        write_file(tmp, R"({"version":24,"items":[{"id":1,"name":"Good"}]})");
        ItemDatabase db{};
        check(db.load_from_json(tmp), "first load succeeds");
        check(db.get_item_count() == 1, "one item present");

        write_file(tmp, R"({ this is not json at all )");
        check(!db.load_from_json(tmp), "second load fails on garbage");
        check(db.get_item_count() == 1, "previous good data preserved (atomic swap, no partial state)");
        check(db.get_item_by_id(1) != nullptr, "previous item still reachable");
    }

    std::remove(tmp.c_str());

    std::cout << "\n=== summary ===\n";
    if (failures == 0) {
        std::cout << "ALL CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
