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

int main()
{
    using extension::item_finder::ItemDatabase;

    std::cout << "=== load the real bundled database ===\n";

    ItemDatabase db{};
    const bool loaded = db.load_from_json("resources/decoded_items.json");

    check(loaded, "decoded_items.json loaded");
    if (!loaded) {
        std::cout << "cannot continue without the database\n";
        return 1;
    }

    std::cout << "     schema version : " << db.schema_version() << "\n";
    std::cout << "     item count     : " << db.get_item_count() << "\n";

    check(db.schema_version() == 24, "schema version 24 reported");
    check(db.get_item_count() > 15000, "more than 15000 items loaded");

    std::cout << "\n=== id -> item lookup is correct, not index-based ===\n";
    {
        const auto* blank = db.get_item_by_id(0);
        check(blank != nullptr, "item 0 found");
        if (blank) {
            check(blank->name == "Blank", "item 0 is 'Blank' (got '" + blank->name + "')");
        }
    }
    {
        const auto* wl = db.get_item_by_id(242);
        check(wl != nullptr, "item 242 found");
        if (wl) {
            std::cout << "     item 242 name: " << wl->name << "\n";
            check(wl->name.find("Lock") != std::string::npos,
                  "item 242 is a Lock (got '" + wl->name + "')");
        }
    }
    {
        const auto* dl = db.get_item_by_id(1796);
        check(dl != nullptr, "item 1796 found");
        if (dl) {
            std::cout << "     item 1796 name: " << dl->name << "\n";
        }
    }

    std::cout << "\n=== lookups that the old index-based code got wrong ===\n";
    
    
    {
        const auto* late = db.get_item_by_id(15000);
        check(late != nullptr, "item 15000 found (old code indexed the vector)");
        if (late) {
            check(late->id == 15000, "returned item really has id 15000");
            std::cout << "     item 15000 name: " << late->name << "\n";
        }
    }

    std::cout << "\n=== absent ids are clean misses ===\n";
    check(db.get_item_by_id(-1) == nullptr, "negative id rejected");
    check(db.get_item_by_id(999999) == nullptr, "out-of-range id rejected");

    std::cout << "\n=== every returned item matches the requested id (spot scan) ===\n";
    {
        bool all_match = true;
        int checked = 0;
        for (int id : { 0, 1, 5, 50, 242, 500, 1000, 1796, 3000, 5000, 8000, 12000, 15000 }) {
            const auto* item = db.get_item_by_id(id);
            if (item) {
                ++checked;
                if (item->id != id) {
                    std::cout << "     MISMATCH: asked " << id << " got " << item->id
                              << " (" << item->name << ")\n";
                    all_match = false;
                }
            }
        }
        check(all_match, "all " + std::to_string(checked) + " spot-checked ids map to themselves");
    }

    std::cout << "\n=== malformed input is rejected, not crashed on ===\n";
    {
        ItemDatabase bad{};
        check(!bad.load_from_json("resources/does_not_exist.json"), "missing file rejected");
        check(!bad.load_from_json("CMakeLists.txt"), "non-JSON file rejected");
        check(bad.get_item_count() == 0, "failed load leaves database empty");
        check(bad.get_item_by_id(0) == nullptr, "lookup on empty database is safe");
    }

    std::cout << "\n=== summary ===\n";
    if (failures == 0) {
        std::cout << "ALL CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
