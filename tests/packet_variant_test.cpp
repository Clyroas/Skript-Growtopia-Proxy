#include <iostream>
#include <string>

#include "../src/packet/packet_variant.hpp"

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
    using packet::Variant;

    std::cout << "=== get_any_int: each numeric alternative ===\n";
    {
        Variant v{};
        v.add<int32_t>(-12345);
        const auto value = v.get_any_int(0);
        check(value.has_value() && *value == -12345, "signed read back");
    }
    {
        Variant v{};
        v.add<uint32_t>(4294967295u);
        const auto value = v.get_any_int(0);
        check(value.has_value(), "unsigned yields a value");
    }
    {
        Variant v{};
        v.add<uint32_t>(17091u);
        const auto value = v.get_any_int(0);
        check(value.has_value() && *value == 17091, "unsigned port 17091 decoded");
    }
    {
        Variant v{};
        v.add<float>(3.9f);
        const auto value = v.get_any_int(0);
        check(value.has_value() && *value == 3, "float truncated");
    }

    std::cout << "\n=== get_any_int: non-numeric and out-of-range ===\n";
    {
        Variant v{};
        v.add("OnSendToServer");
        check(!v.get_any_int(0).has_value(), "string has no int value");
    }
    {
        Variant v{};
        v.add("x");
        check(!v.get_any_int(9).has_value(), "out-of-range index yields nullopt");
    }

    std::cout << "\n=== this is the regression: get<int32_t> on an unsigned field ===\n";
    {
        Variant v{};
        v.add<uint32_t>(17091u);

        const int32_t old_way = v.get<int32_t>(0);
        const auto new_way = v.get_any_int(0);

        std::cout << "     old get<int32_t> -> " << old_way << " (silently 0 = the bug)\n";
        std::cout << "     new get_any_int  -> " << (new_way ? std::to_string(*new_way) : "nullopt") << "\n";

        check(old_way == 0, "confirms old accessor silently returned 0");
        check(new_way.has_value() && *new_way == 17091, "new accessor recovers the real value");
    }

    std::cout << "\n=== realistic OnSendToServer payload ===\n";
    {
        Variant v{};
        v.add("OnSendToServer");
        v.add<uint32_t>(17091u);
        v.add<int32_t>(1234);
        v.add<int32_t>(5678);
        v.add("1.2.3.4|door|uuid");
        v.add<int32_t>(2);

        check(v.get<std::string>(0) == "OnSendToServer", "function name");
        check(v.get_any_int(1).value_or(0) == 17091, "port");
        check(v.get_any_int(2).value_or(0) == 1234, "token");
        check(v.get_any_int(3).value_or(0) == 5678, "user");
        check(v.get<std::string>(4) == "1.2.3.4|door|uuid", "address blob");
        check(v.get_any_int(5).value_or(-1) == 2, "login_mode");
    }

    std::cout << "\n=== summary ===\n";
    if (failures == 0) {
        std::cout << "ALL CHECKS PASSED\n";
        return 0;
    }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
