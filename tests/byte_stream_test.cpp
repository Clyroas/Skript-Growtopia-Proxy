#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../src/utils/byte_stream.hpp"

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "ok  : " : "FAIL: ") << what << "\n";
    if (!ok) ++failures;
}

int main()
{
    using Stream = ByteStream<std::uint16_t>;

    std::cout << "=== get_data() returns a reference (no per-call copy) ===\n";
    {
        Stream s{};
        s.write<std::uint32_t>(0x11223344u);
        const std::vector<std::byte>& ref = s.get_data();
        check(&ref == &s.get_data(), "same object returned each call: no copy");
        check(ref.size() == sizeof(std::uint32_t), "buffer holds 4 bytes");
    }

    std::cout << "\n=== get_remaining() is the correct payload length ===\n";
    {
        Stream s{};
        s.write<std::uint32_t>(3u);              // simulated NetMessageType
        const std::string payload = "action|hello";
        s.write_data(payload.data(), payload.size());

        std::uint32_t discarded{};
        (void)s.read(discarded);                 // consume the message type
        check(s.get_remaining() == payload.size(), "remaining equals payload size");

        std::string read_back{};
        check(s.read(read_back, s.get_remaining()), "read of remaining bytes succeeds");
        check(read_back == payload, "payload round-trips intact");

        // This is the old client.cpp behaviour: it subtracted 1 and lost the last byte.
        Stream t{};
        t.write<std::uint32_t>(3u);
        t.write_data(payload.data(), payload.size());
        std::uint32_t discarded2{};
        (void)t.read(discarded2);
        std::string truncated{};
        const std::size_t old_len = t.get_size() - sizeof(std::uint32_t) - 1;
        (void)t.read(truncated, old_len);
        check(truncated != payload, "old size arithmetic dropped the last byte (the bug)");
        check(truncated.size() + 1 == payload.size(), "exactly one byte was lost");
    }

    std::cout << "\n=== skip() is relative and clamped ===\n";
    {
        Stream s{};
        s.write_data("abcdef", 6);
        s.skip(2);
        check(s.get_read_offset() == 2, "skip(2) advances by 2");
        s.skip(2);
        check(s.get_read_offset() == 4, "skip(2) again advances by 2 more");

        s.skip(1000);
        check(s.get_read_offset() == s.get_size(), "oversized skip clamps at the end");

        std::byte one{};
        check(!s.read_data(&one, 1), "read past the end fails instead of overrunning");
    }

    std::cout << "\n=== seek() is absolute ===\n";
    {
        Stream s{};
        s.write_data("abcdef", 6);
        s.seek(5);
        check(s.get_read_offset() == 5, "seek(5) jumps to 5");
        s.seek(0);
        check(s.get_read_offset() == 0, "seek(0) rewinds to 0");
        s.seek(999);
        check(s.get_read_offset() == s.get_size(), "seek past the end clamps");
    }

    std::cout << "\n=== the rewind-on-failure idiom now works ===\n";
    {
        // server.cpp records start_pos, then wants to resend from that exact offset.
        Stream s{};
        s.write<std::uint32_t>(7u);
        s.write_data("worlddata", 9);
        const std::size_t start_pos = s.get_read_offset();   // 0
        std::uint32_t type{};
        (void)s.read(type);
        check(s.get_read_offset() == sizeof(std::uint32_t), "advanced while parsing");

        Stream rewind{ const_cast<std::byte*>(s.get_data().data()), s.get_size() };
        rewind.seek(start_pos);
        check(rewind.get_read_offset() == start_pos, "seek restored the original offset");
        check(rewind.get_data() == s.get_data(), "buffer contents preserved for resend");
    }

    std::cout << "\n=== a >65535-byte payload survives (map-like packet) ===\n";
    {
        Stream s{};
        const std::size_t big = 200000;              // ~195 KB, exceeds uint16_t
        std::vector<std::byte> filler(big, std::byte{ 0x5A });
        s.write_data(filler.data(), filler.size());
        check(s.get_size() == big, "200000-byte payload written without truncation");
        check(s.get_remaining() == big, "all bytes still readable");

        std::vector<std::byte> out{};
        check(s.read_vector(out, big), "read_vector(size_t) handled the large payload");
        check(out == filler, "large payload round-trips byte-exactly");
    }

    std::cout << "\n=== summary ===\n";
    if (failures == 0) { std::cout << "ALL CHECKS PASSED\n"; return 0; }
    std::cout << failures << " CHECK(S) FAILED\n";
    return 1;
}
