#pragma once
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace utils {

[[nodiscard]] inline std::string url_decode(std::string_view value)
{
    std::string out{};
    out.reserve(value.size());

    for (std::size_t i{ 0 }; i < value.size(); ++i) {
        const char ch{ value[i] };
        if (ch == '+') {
            out += ' ';
            continue;
        }

        if (ch == '%' && i + 2 < value.size()) {
            const auto hex_to_int = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };

            const int high{ hex_to_int(value[i + 1]) };
            const int low{ hex_to_int(value[i + 2]) };
            if (high >= 0 && low >= 0) {
                out += static_cast<char>((high << 4) | low);
                i += 2;
                continue;
            }
        }

        out += ch;
    }

    return out;
}

[[nodiscard]] inline std::string url_encode(std::string_view value)
{
    static constexpr char hex_digits[] = "0123456789ABCDEF";

    std::string out{};
    out.reserve(value.size());

    for (const char ch : value) {
        const unsigned char uc{ static_cast<unsigned char>(ch) };
        const bool unreserved{ std::isalnum(uc) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~' };
        if (unreserved) {
            out += ch;
            continue;
        }

        out += '%';
        out += hex_digits[(uc >> 4) & 0x0F];
        out += hex_digits[uc & 0x0F];
    }

    return out;
}

[[nodiscard]] inline std::optional<std::string> find_form_field(const std::string& body, const std::string& field)
{
    std::size_t begin{ 0 };
    while (begin <= body.size()) {
        const std::size_t end{ body.find('&', begin) };
        const std::string_view pair{ body.data() + begin,
                                     (end == std::string::npos ? body.size() : end) - begin };
        if (!pair.empty()) {
            const std::size_t eq{ pair.find('=') };
            const std::string_view key{ eq == std::string_view::npos ? pair : pair.substr(0, eq) };
            if (key == field) {
                return url_decode(eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1));
            }
        }

        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }

    return std::nullopt;
}

[[nodiscard]] inline std::string set_form_field(const std::string& body,
                                                const std::string& field,
                                                const std::string& value)
{
    std::string out{};
    out.reserve(body.size() + field.size() + value.size() + 2);

    bool replaced{ false };
    std::size_t begin{ 0 };
    while (begin <= body.size()) {
        const std::size_t end{ body.find('&', begin) };
        const std::string_view pair{ body.data() + begin,
                                     (end == std::string::npos ? body.size() : end) - begin };

        if (!pair.empty()) {
            const std::size_t eq{ pair.find('=') };
            const std::string_view key{ eq == std::string_view::npos ? pair : pair.substr(0, eq) };
            if (key == field) {
                out += field;
                out += '=';
                out += url_encode(value);
                replaced = true;
            }
            else {
                out.append(pair.data(), pair.size());
            }
        }

        if (end == std::string::npos) {
            break;
        }

        out += '&';
        begin = end + 1;
    }

    if (!replaced) {
        if (!out.empty()) {
            out += '&';
        }
        out += field;
        out += '=';
        out += url_encode(value);
    }

    return out;
}

struct ClientDeclaration {
    std::string version{};
    std::string protocol{};
    std::string platform{};
};

[[nodiscard]] inline ClientDeclaration parse_client_declaration(const std::string& body)
{
    ClientDeclaration declaration{};

    if (const auto version{ find_form_field(body, "version") }; version) {
        declaration.version = *version;
    }
    if (const auto protocol{ find_form_field(body, "protocol") }; protocol) {
        declaration.protocol = *protocol;
    }
    if (const auto platform{ find_form_field(body, "platform") }; platform) {
        declaration.platform = *platform;
    }

    return declaration;
}

}
