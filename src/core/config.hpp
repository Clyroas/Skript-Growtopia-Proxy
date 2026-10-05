#pragma once
#include <string>
#include <unordered_map>
#include <variant>
#include <mutex>

namespace core {
using ConfigStorage = std::variant<int, unsigned int, std::string, bool, std::vector<std::string>>;

class Config {
public:
    Config();
    ~Config() = default;

    // X9: key-existence check so callers can seed defaults without wiping
    // values the user already set in config.json.
    [[nodiscard]] bool contains(const std::string& key) const
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return config_.find(key) != config_.end();
    }

    template <typename T = std::string>
    [[nodiscard]] T get(const std::string& key) const
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        
        try {
            return std::get<T>(config_.at(key));
        }
        catch (const std::exception&) {
            return T{};
        }
    }

    
    template <typename T = std::string>
    [[nodiscard]] T get(const std::string& key, const T& def) const
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        
        try {
            return std::get<T>(config_.at(key));
        }
        catch (const std::exception&) {
            return def;
        }
    }

    template <typename T = std::string>
    void set_runtime(const std::string& key, const T& value)
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        config_[key] = value;
    }

    template <typename T = std::string>
    void set(const std::string& key, const T& value)
    {
        set_runtime(key, value);
        save();
    }
    
    void save(); 

private:
    std::unordered_map<std::string, ConfigStorage> config_;
    mutable std::mutex config_mutex_;
};
}
