#pragma once
#include <ranges>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

class TextParse {
public:
    TextParse() = default;

    explicit TextParse(const std::string& str, const std::string& delimiter = "|")
    {
        for (const auto& line : tokenize(str, "\n")) {
            std::vector tokens{ tokenize(line, delimiter) };

            if (tokens.empty()) {
                continue;
            }

            if (tokens.size() == 1) {
                continue;
            }

            std::string key{ tokens.front() };

            tokens.erase(tokens.begin());
            tokens.shrink_to_fit();

            data_.emplace(key, tokens);
        }
    }

    static std::vector<std::string> tokenize(const std::string& string, const std::string& delimiter = "|")
    {
        std::vector<std::string> tokens{};
        for (auto&& token : string | std::views::split(delimiter)) {
            if (token.empty()) {
                continue;
            }

            tokens.emplace_back(token.begin(), token.end());
        }

        return tokens;
    }

    [[nodiscard]] std::string get(const std::string& key, const int index = 0) const
    {
        const auto it{ data_.find(key) };
        if (it == data_.end()) {
            return {};
        }

        if (index < 0 || index >= it->second.size()) {
            return {};
        }

        return it->second[index];
    }

    template <typename T>
    [[nodiscard]] T get(const std::string& key, const int index = 0) const
    {
        if constexpr (std::is_integral_v<T>) {
            if constexpr (std::is_unsigned_v<T>) {
                return std::stoul(get(key, index));
            }
            else {
                return std::stoi(get(key, index));
            }
        }
        else if constexpr (std::is_same_v<T, double>) {
            return std::stod(get(key, index));
        }
        else if constexpr (std::is_same_v<T, long double>) {
            return std::stold(get(key, index));
        }
        else if constexpr (std::is_same_v<T, float>) {
            return std::stof(get(key, index));
        }

        return {};
    }

    void add(const std::string& key, const std::vector<std::string>& value)
    {
        data_.emplace(key, value);
    }

    void set(const std::string& key, const std::vector<std::string>& value)
    {
        if (!data_.contains(key)) {
            return;
        }

        data_[key] = value;
    }

    void remove(const std::string& key)
    {
        const auto it{ data_.find(key) };
        if (it == data_.end()) {
            return;
        }

        data_.erase(it);
    }

    [[nodiscard]] std::string get_raw(const std::string& delimiter = "|", const std::string& prepend_text = "") const
    {
        std::string raw_data{};
        for (auto it = data_.cbegin(); it != data_.cend(); ++it) {
            raw_data += prepend_text + it->first;
            for (const auto& token : it->second) {
                raw_data += delimiter + token;
            }

            if (std::next(it) != data_.cend() && !std::next(it)->first.empty()) {
                raw_data += '\n';
            }
        }

        return raw_data;
    }

    [[nodiscard]] std::vector<std::string> get_key_values(const std::string& delimiter = "|") const
    {
        std::vector<std::string> key_values{};
        for (auto it = data_.cbegin(); it != data_.cend(); ++it) {
            std::string key_value{ it->first };
            for (const auto& token : it->second) {
                key_value += delimiter + token;
            }

            key_values.emplace_back(key_value);
        }

        return key_values;
    }

    [[nodiscard]] std::vector<std::string> get_redacted_key_values(const std::string& delimiter = "|") const
    {
        static constexpr std::array<std::string_view, 17> sensitive_keys{
            "tankidname", "tankidpass", "password", "passwd", "pass",
            "token", "ltoken", "access_token", "refresh_token", "mac",
            "clienttoken", "uuidtoken", "uuid_token", "rid", "hash", "hash2", "wk"
        };

        std::vector<std::string> key_values{};
        key_values.reserve(data_.size());
        for (const auto& [key, values] : data_) {
            std::string normalized_key{ key };
            std::ranges::transform(normalized_key, normalized_key.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });

            std::string key_value{ key };
            if (std::ranges::find(sensitive_keys, std::string_view{ normalized_key }) != sensitive_keys.end()) {
                key_value += delimiter + "[REDACTED]";
            }
            else {
                for (const auto& value : values) {
                    key_value += delimiter + value;
                }
            }
            key_values.emplace_back(std::move(key_value));
        }
        return key_values;
    }

    [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>> get_data() const { return data_; }
    [[nodiscard]] bool empty() const { return data_.empty(); }

private:
    std::unordered_map<std::string, std::vector<std::string>> data_;
};
