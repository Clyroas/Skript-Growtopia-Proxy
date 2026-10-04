#pragma once
#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

namespace utils {

struct ServerDataLine {
    std::string key{};
    std::string value{};
    bool is_field{ false };
    std::string raw{};
};

class ServerDataParser {
public:
    explicit ServerDataParser(const std::string& raw) { parse(raw); }

    [[nodiscard]] bool has_field(const std::string& key) const
    {
        return find(key) != nullptr;
    }

    [[nodiscard]] std::string get(const std::string& key) const
    {
        const ServerDataLine* line{ find(key) };
        return line ? line->value : std::string{};
    }

    [[nodiscard]] bool set(const std::string& key, const std::string& value)
    {
        ServerDataLine* line{ find(key) };
        if (!line) {
            return false;
        }

        line->value = value;
        return true;
    }

    [[nodiscard]] bool empty() const { return lines_.empty(); }

    [[nodiscard]] std::string serialize() const
    {
        std::string out{};
        out.reserve(estimate_size());

        for (const ServerDataLine& line : lines_) {
            if (line.is_field) {
                out += line.key;
                out += '|';
                out += line.value;
            }
            else {
                out += line.raw;
            }
            out += '\n';
        }

        return out;
    }

private:
    std::vector<ServerDataLine> lines_;

    void parse(const std::string& raw)
    {
        std::size_t begin{ 0 };
        while (begin <= raw.size()) {
            const std::size_t end{ raw.find('\n', begin) };
            const std::size_t length{ end == std::string::npos ? raw.size() - begin : end - begin };

            std::string line{ raw.substr(begin, length) };
            while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) {
                line.pop_back();
            }

            const std::size_t pipe{ line.find('|') };
            if (pipe != std::string::npos && pipe > 0) {
                ServerDataLine field{};
                field.is_field = true;
                field.key = line.substr(0, pipe);
                field.value = line.substr(pipe + 1);
                lines_.push_back(std::move(field));
            }
            else if (!line.empty()) {
                ServerDataLine marker{};
                marker.is_field = false;
                marker.raw = std::move(line);
                lines_.push_back(std::move(marker));
            }

            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    }

    [[nodiscard]] ServerDataLine* find(const std::string& key)
    {
        for (ServerDataLine& line : lines_) {
            if (line.is_field && line.key == key) {
                return &line;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const ServerDataLine* find(const std::string& key) const
    {
        for (const ServerDataLine& line : lines_) {
            if (line.is_field && line.key == key) {
                return &line;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t estimate_size() const
    {
        std::size_t total{ 0 };
        for (const ServerDataLine& line : lines_) {
            total += line.is_field ? line.key.size() + line.value.size() + 2 : line.raw.size() + 1;
        }
        return total + 16;
    }
};

[[nodiscard]] inline std::optional<unsigned int> parse_port(const std::string& value)
{
    if (value.empty()) {
        return std::nullopt;
    }

    unsigned int port{ 0 };
    const char* first{ value.data() };
    const char* last{ value.data() + value.size() };
    const std::from_chars_result result{ std::from_chars(first, last, port) };
    if (result.ec != std::errc{} || result.ptr == first || port == 0 || port > 65535) {
        return std::nullopt;
    }

    return port;
}

}
