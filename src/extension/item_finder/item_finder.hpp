#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <cctype>
#include <sstream>

namespace extension::item_finder {

struct ItemInfo {
    int id;
    std::string name;
    int type;
    int rarity;
    std::string file_name;
    int clothing_type;
    int properties;
    std::string description;
    std::string info; 
};

class ItemDatabase {
public:
    ItemDatabase() { instance_ = this; }
    ~ItemDatabase() { if (instance_ == this) instance_ = nullptr; }
    static ItemDatabase* get_instance() { return instance_; }
    
    bool load_from_json(const std::string& json_path) {
        try {
            std::ifstream file(json_path);
            if (!file.is_open()) {
                return false;
            }
            
            nlohmann::json j;
            file >> j;
            
            
            if (!j.contains("items") || !j["items"].is_array()) {
                return false;
            }
            
            
            
            std::vector<ItemInfo> items{};
            std::unordered_map<int, std::size_t> id_index{};
            
            items.reserve(j["items"].size());
            
            std::size_t skipped{ 0 };
            std::size_t duplicate_ids{ 0 };
            
            for (const auto& item_json : j["items"]) {
                if (!item_json.is_object()) {
                    ++skipped;
                    continue;
                }
                
                const int id{ read_int(item_json, "id", -1) };
                if (id < 0) {
                    ++skipped;
                    continue;
                }
                
                ItemInfo item;
                item.id = id;
                item.name = read_string(item_json, "name", "Unknown");
                item.type = read_int(item_json, "type", 0);
                item.rarity = read_int(item_json, "rarity", 0);
                item.file_name = read_string(item_json, "file_name", "");
                item.clothing_type = read_int(item_json, "clothing_type", 0);
                item.properties = read_int(item_json, "properties", 0);
                item.info = read_string(item_json, "_unk10", "");
                
                item.description = build_description(item_json);
                
                
                
                
                if (!id_index.emplace(item.id, items.size()).second) {
                    ++duplicate_ids;
                    continue;
                }
                
                items.push_back(std::move(item));
            }
            
            
            
            items_ = std::move(items);
            id_index_ = std::move(id_index);
            
            
            schema_version_ = read_int(j, "version", 0);
            last_skipped_ = skipped;
            last_duplicate_ids_ = duplicate_ids;
            
            return !items_.empty();
        } catch (const std::exception& e) {
            
            
            load_error_ = e.what();
            return false;
        }
    }
    
    [[nodiscard]] int schema_version() const { return schema_version_; }
    [[nodiscard]] std::size_t last_skipped() const { return last_skipped_; }
    [[nodiscard]] std::size_t last_duplicate_ids() const { return last_duplicate_ids_; }
    [[nodiscard]] const std::string& load_error() const { return load_error_; }

    std::vector<ItemInfo> search_items(const std::string& query, int max_results = 20, const std::string& search_type = "all") const {
        std::vector<ItemInfo> results;
        std::string query_lower = to_lower(query);
        
        if (query_lower.empty()) {
            return results;
        }
        
        for (const auto& item : items_) {
            std::string name_lower = to_lower(item.name);
            
            
            bool type_match = true;
            if (search_type == "items") {
                
                
                type_match = (name_lower.find(" seed") == std::string::npos);
            } else if (search_type == "seeds") {
                
                
                type_match = (name_lower.find(" seed") != std::string::npos);
            }
            
            
            if (!type_match) continue;
            
            
            if (name_lower.find(query_lower) != std::string::npos ||
                std::to_string(item.id) == query) {
                results.push_back(item);
                
                if (results.size() >= max_results) {
                    break;
                }
            }
        }
        
        return results;
    }
    
    std::string build_dialog(const std::string& query, const std::vector<ItemInfo>& results, const std::string& search_type = "all") const {
        std::ostringstream dialog;
        
        dialog << "set_default_color|`o\n";
        dialog << "add_label_with_icon|big|`wItem Finder``|left|1796|\n";
        dialog << "add_spacer|small|\n";
        
        
        
        if (query.empty() && search_type.empty()) {
            dialog << "add_textbox|`oWhat would you like to search for?``|left|\n";
            dialog << "add_spacer|small|\n";
            
            
            dialog << "add_label_with_icon|big|`2Search Items Only``|left|242|\n";
            dialog << "add_smalltext|`oSearch for blocks, tools, and consumables``|\n";
            dialog << "add_button|search_items|`2Select Items Only``|\n";
            dialog << "add_spacer|small|\n";
            
            dialog << "add_label_with_icon|big|`5Search Items & Seeds``|left|5|\n";
            dialog << "add_smalltext|`oSearch for everything``|\n";
            dialog << "add_button|search_all|`5Select Items & Seeds``|\n";
            dialog << "add_spacer|small|\n";
            
            dialog << "add_label_with_icon|big|`9Search Seeds Only``|left|5|\n";
            dialog << "add_smalltext|`oSearch for seeds only``|\n";
            dialog << "add_button|search_seeds|`9Select Seeds Only``|\n";
            dialog << "add_spacer|small|\n";
            
            dialog << "add_quick_exit|\n";
            dialog << "end_dialog|itemfinder|Close||\n";
            return dialog.str();
        }
        
        
        std::string search_type_label;
        if (search_type == "items") search_type_label = " (Items Only)";
        else if (search_type == "seeds") search_type_label = " (Seeds Only)";
        else search_type_label = " (All)";
        
        dialog << "add_textbox|`5Search Type:" << search_type_label << "``|left|\n";
        dialog << "add_text_input|itemfind_search|Search:|" << query << "|30|\n";
        dialog << "add_spacer|small|\n";
        
        if (results.empty() && !query.empty()) {
            dialog << "add_textbox|`4No items found for: `w" << query << "``|\n";
            dialog << "add_spacer|small|\n";
        } else if (!query.empty()) {
            dialog << "add_textbox|`2Found " << results.size() << " items``|\n";
            dialog << "add_spacer|small|\n";
            
            
            for (const auto& item : results) {
                
                dialog << "add_label_with_icon|big|`w" << item.name << "``|left|" << item.id << "|\n";
                
                
                dialog << "add_smalltext|`o" << item.description << "``|\n";
                dialog << "add_smalltext|`5ID: " << item.id << " | Type: " << static_cast<int>(item.type) << " | Rarity: " << static_cast<int>(item.rarity) << "``|\n";
                
                
                dialog << "add_button|viewdetail_" << item.id << "|`2View Full Details``|\n";
                dialog << "add_button|wear_" << item.id << "|`9Wear Item``|\n";
                
                
                dialog << "add_spacer|small|\n";
            }
            
            if (results.size() >= 20) {
                dialog << "add_textbox|`4Showing first 20 results. Refine your search for more specific results.``|\n";
                dialog << "add_spacer|small|\n";
            }
        } else {
            dialog << "add_textbox|`oEnter an item name or ID to search``|\n";
            dialog << "add_textbox|`oExample: dirt, laser, 242``|\n";
        }
        
        dialog << "add_spacer|small|\n";
        dialog << "end_dialog|itemfinder|Close|Search|\n";
        
        return dialog.str();
    }
    
    const ItemInfo* get_item_by_id(int id) const {
        const auto it{ id_index_.find(id) };
        if (it == id_index_.end()) {
            return nullptr;
        }
        return &items_[it->second];
    }
    
    size_t get_item_count() const {
        return items_.size();
    }

private:
    inline static ItemDatabase* instance_ = nullptr;
    std::vector<ItemInfo> items_;
    std::unordered_map<int, std::size_t> id_index_;
    int schema_version_{ 0 };
    std::size_t last_skipped_{ 0 };
    std::size_t last_duplicate_ids_{ 0 };
    std::string load_error_{};

    
    
    
    
    
    
    
    
    static int read_int(const nlohmann::json& object,
                        const char* key,
                        int fallback) noexcept
    {
        if (!object.is_object() || !object.contains(key)) {
            return fallback;
        }

        const auto& value{ object[key] };

        try {
            if (value.is_number_integer()) {
                return value.get<int>();
            }
            if (value.is_number_unsigned()) {
                const auto raw{ value.get<unsigned long long>() };
                if (raw > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
                    return std::numeric_limits<int>::max();
                }
                return static_cast<int>(raw);
            }
            if (value.is_number_float()) {
                return static_cast<int>(value.get<double>());
            }
            
            if (value.is_string()) {
                const std::string text{ value.get<std::string>() };
                if (!text.empty()) {
                    return std::stoi(text);
                }
            }
        }
        catch (const std::exception&) {
            
        }

        return fallback;
    }

    static std::string read_string(const nlohmann::json& object,
                                   const char* key,
                                   const std::string& fallback) noexcept
    {
        if (!object.is_object() || !object.contains(key)) {
            return fallback;
        }

        const auto& value{ object[key] };

        try {
            if (value.is_string()) {
                return value.get<std::string>();
            }
            
            
            if (value.is_number_integer()) {
                return std::to_string(value.get<long long>());
            }
            if (value.is_number_unsigned()) {
                return std::to_string(value.get<unsigned long long>());
            }
            if (value.is_boolean()) {
                return value.get<bool>() ? "1" : "0";
            }
        }
        catch (const std::exception&) {
            
        }

        return fallback;
    }

    static std::string to_lower(const std::string& str) {
        std::string result = str;
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return result;
    }
    
    static std::string build_description(const nlohmann::json& item_json) {
        std::ostringstream desc;
        
        int type = read_int(item_json, "type", 0);
        desc << "Type: ";
        switch (type) {
            case 0: desc << "Block"; break;
            case 1: desc << "Door"; break;
            case 2: desc << "Lock"; break;
            case 3: desc << "Gems"; break;
            case 4: desc << "Sign"; break;
            case 5: desc << "SFX Foreground"; break;
            case 6: desc << "Toggleable Foreground"; break;
            case 7: desc << "Main Door"; break;
            case 8: desc << "Platform"; break;
            case 9: desc << "Bedrock"; break;
            case 10: desc << "Lava"; break;
            case 11: desc << "Foreground"; break;
            case 12: desc << "Background"; break;
            case 13: desc << "Seed"; break;
            case 14: desc << "Clothing"; break;
            case 15: desc << "Animated Block"; break;
            case 16: desc << "SFX Background"; break;
            case 17: desc << "Toggleable Background"; break;
            case 18: desc << "Bouncy"; break;
            case 19: desc << "Checkpoint"; break;
            case 20: desc << "Gateway"; break;
            case 21: desc << "Treasure"; break;
            case 22: desc << "Deadly Block"; break;
            case 23: desc << "Trampoline"; break;
            case 24: desc << "Consumable"; break;
            default: desc << "Type " << type; break;
        }
        
        desc << " | Rarity: " << read_int(item_json, "rarity", 0);
        
        
        
        return desc.str();
    }
};

} 
