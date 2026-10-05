#include <algorithm>
#include <iostream>
#include <string>

static int failures = 0;

static void check_eq(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected) {
        std::cout << "FAIL: " << what << "\n  expected: [" << expected << "]\n  actual  : [" << actual << "]\n";
        ++failures;
    }
    else {
        std::cout << "ok  : " << what << "\n";
    }
}

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

static std::string show(const std::string& s)
{
    std::string out;
    for (char c : s) out += (c == '\n' ? '.' : c);
    return out;
}

int main()
{
    
    
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

    std::cout << "=== case 1: existing field is replaced, not duplicated ===\n";
    {
        std::string data = "type|local\nnetID|42\ncountry|us\n";
        replace_or_add_field(data, "country", "jp");
        check_eq(show(data), "type|local.netID|42.country|jp.", "existing country replaced");
        check(data.find("country|us") == std::string::npos, "old value gone");
    }

    std::cout << "\n=== case 2: mstate must not match inside smstate (the real bug) ===\n";
    {
        std::string data = "type|local\nsmstate|1\n";
        replace_or_add_field(data, "mstate", "1");
        check(data.find("\nmstate|1\n") != std::string::npos,
              "mstate inserted at line start even though smstate exists");
        check(data.find("smstate|1") != std::string::npos, "smstate left intact");
        check_eq(show(data), "type|local.mstate|1.smstate|1.", "inserted after type|local, smstate intact");
    }

    std::cout << "\n=== case 3: mstate present is replaced in place ===\n";
    {
        std::string data = "type|local\nmstate|0\nsmstate|1\n";
        replace_or_add_field(data, "mstate", "1");
        check_eq(show(data), "type|local.mstate|1.smstate|1.", "mstate replaced in place");
    }

    std::cout << "\n=== case 4: field at very start of buffer ===\n";
    {
        std::string data = "mstate|0\nrest|1\n";
        replace_or_add_field(data, "mstate", "1");
        check_eq(show(data), "mstate|1.rest|1.", "leading field replaced");
    }

    std::cout << "\n=== case 5: last line has no trailing newline ===\n";
    {
        std::string data = "type|local\nsmstate|1";
        replace_or_add_field(data, "mstate", "1");
        check(data.find("mstate|1") != std::string::npos, "inserted when no trailing newline");
        check(data.find("smstate|1") != std::string::npos, "smstate intact");
    }

    std::cout << "\n=== case 6: insert lands after the type|local line, not before it ===\n";
    {
        std::string data = "type|local\nnetID|7\n";
        replace_or_add_field(data, "invis", "1");
        check_eq(show(data), "type|local.invis|1.netID|7.", "inserted right after type|local");
    }

    std::cout << "\n=== case 7: type|local absent and field absent => no-op ===\n";
    {
        std::string data = "netID|7\n";
        replace_or_add_field(data, "invis", "1");
        check_eq(show(data), "netID|7.", "untouched when no anchor");
    }

    std::cout << "\n=== case 8: titleIcon (mixed case) still works ===\n";
    {
        std::string data = "type|local\ntitleIcon|0\n";
        replace_or_add_field(data, "titleIcon", "5");
        check_eq(show(data), "type|local.titleIcon|5.", "titleIcon replaced");
    }

    std::cout << "\n=== summary ===\n";
    if (failures == 0) {
        std::cout << "ALL CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
