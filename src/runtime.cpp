#include "nlohmann/json.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

struct TTStringArray {
    std::vector<std::string> items;
};

struct TTJson {
    json value;
};

static char* copyCString(const char* s) {
    if (!s) return nullptr;
    size_t len = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(len + 1));
    if (!out) return nullptr;
    std::memcpy(out, s, len + 1);
    return out;
}

static char* copyString(const std::string& s) {
    return copyCString(s.c_str());
}

static const char* asCString(void* ptr) {
    return ptr ? static_cast<const char*>(ptr) : nullptr;
}

extern "C" {

void* tt_input(void* prompt) {
    const char* text = asCString(prompt);
    if (text) {
        std::cout << text;
        std::cout.flush();
    }
    std::string line;
    if (!std::getline(std::cin, line)) {
        return copyString("");
    }
    return copyString(line);
}

void* tt_read_file(void* path) {
    const char* filePath = asCString(path);
    if (!filePath) return copyString("");

    std::ifstream input(filePath, std::ios::binary);
    if (!input.is_open()) {
        return copyString("");
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    return copyString(contents.str());
}

long long tt_write_file(void* path, void* content) {
    const char* filePath = asCString(path);
    const char* text = asCString(content);
    if (!filePath || !text) return -1;

    std::ofstream output(filePath, std::ios::binary);
    if (!output.is_open()) return -1;
    output << text;
    if (!output.good()) return -1;
    return static_cast<long long>(std::strlen(text));
}

void* tt_split(void* str, void* delim) {
    const char* text = asCString(str);
    const char* sep = asCString(delim);
    if (!text) return nullptr;
    if (!sep || std::strlen(sep) == 0) {
        auto* array = new TTStringArray();
        array->items.emplace_back(text);
        return array;
    }

    auto* array = new TTStringArray();
    std::string current;
    const char* p = text;
    size_t sepLen = std::strlen(sep);
    while (*p) {
        const char* match = std::strstr(p, sep);
        if (!match) {
            current.append(p);
            break;
        }
        current.append(p, static_cast<size_t>(match - p));
        array->items.push_back(current);
        current.clear();
        p = match + sepLen;
    }
    array->items.push_back(current);
    return array;
}

long long tt_array_len(void* arr) {
    if (!arr) return 0;
    TTStringArray* array = static_cast<TTStringArray*>(arr);
    return static_cast<long long>(array->items.size());
}

void* tt_array_get(void* arr, long long index) {
    if (!arr || index < 0) return nullptr;
    TTStringArray* array = static_cast<TTStringArray*>(arr);
    if (static_cast<size_t>(index) >= array->items.size()) return nullptr;
    return copyString(array->items[static_cast<size_t>(index)].c_str());
}

void* tt_json_new_object() {
    auto* object = new TTJson();
    object->value = json::object();
    return object;
}

void* tt_json_parse(void* str) {
    const char* text = asCString(str);
    if (!text) return nullptr;
    try {
        auto* object = new TTJson();
        object->value = json::parse(text);
        return object;
    } catch (const std::exception&) {
        return nullptr;
    }
}

void* tt_json_read_file(void* path) {
    const char* filePath = asCString(path);
    if (!filePath) return nullptr;
    std::ifstream input(filePath);
    if (!input.is_open()) return nullptr;
    try {
        auto* object = new TTJson();
        input >> object->value;
        return object;
    } catch (const std::exception&) {
        return nullptr;
    }
}

long long tt_json_write_file(void* root, void* path) {
    if (!root) return -1;
    TTJson* object = static_cast<TTJson*>(root);
    const char* filePath = asCString(path);
    if (!filePath) return -1;
    std::ofstream output(filePath);
    if (!output.is_open()) return -1;
    output << object->value.dump();
    if (!output.good()) return -1;
    return static_cast<long long>(output.tellp());
}

void* tt_json_stringify(void* root) {
    if (!root) return copyString("null");
    TTJson* object = static_cast<TTJson*>(root);
    return copyString(object->value.dump());
}

void tt_json_set_string(void* root, void* key, void* value) {
    if (!root) return;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    const char* text = asCString(value);
    if (!name) return;
    object->value[name] = text ? std::string(text) : std::string();
}

void tt_json_set_int(void* root, void* key, long long value) {
    if (!root) return;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return;
    object->value[name] = value;
}

void tt_json_set_double(void* root, void* key, double value) {
    if (!root) return;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return;
    object->value[name] = value;
}

void tt_json_set_bool(void* root, void* key, unsigned char value) {
    if (!root) return;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return;
    object->value[name] = (value != 0);
}

void* tt_json_get_string(void* root, void* key) {
    if (!root) return nullptr;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return nullptr;
    if (!object->value.contains(name) || !object->value[name].is_string()) return nullptr;
    return copyString(object->value[name].get<std::string>());
}

long long tt_json_get_int(void* root, void* key) {
    if (!root) return 0;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return 0;
    if (!object->value.contains(name) || !object->value[name].is_number_integer()) return 0;
    return object->value[name].get<long long>();
}

double tt_json_get_double(void* root, void* key) {
    if (!root) return 0.0;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return 0.0;
    if (!object->value.contains(name) || !object->value[name].is_number()) return 0.0;
    return object->value[name].get<double>();
}

unsigned char tt_json_get_bool(void* root, void* key) {
    if (!root) return 0;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return 0;
    if (!object->value.contains(name) || !object->value[name].is_boolean()) return 0;
    return object->value[name].get<bool>() ? 1 : 0;
}

void* tt_json_get_object(void* root, void* key) {
    if (!root) return nullptr;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return nullptr;
    if (!object->value.contains(name) || !object->value[name].is_object()) return nullptr;
    auto* nested = new TTJson();
    nested->value = object->value[name];
    return nested;
}

unsigned char tt_json_has(void* root, void* key) {
    if (!root) return 0;
    TTJson* object = static_cast<TTJson*>(root);
    const char* name = asCString(key);
    if (!name) return 0;
    return object->value.contains(name) ? 1 : 0;
}

void* tt_str_concat(void* a, void* b) {
    const char* left = asCString(a);
    const char* right = asCString(b);
    std::string result;
    if (left) result += left;
    if (right) result += right;
    return copyString(result);
}

unsigned char tt_str_eq(void* a, void* b) {
    const char* left = asCString(a);
    const char* right = asCString(b);
    if (!left || !right) return (left == right) ? 1 : 0;
    return std::strcmp(left, right) == 0 ? 1 : 0;
}

long long tt_str_len(void* a) {
    const char* text = asCString(a);
    if (!text) return 0;
    return static_cast<long long>(std::strlen(text));
}

void tt_print_i32(int32_t value) {
    std::cout << value;
}

void tt_print_i64(long long value) {
    std::cout << value;
}

void tt_print_double(double value) {
    std::cout << value;
}

void tt_print_bool(unsigned char value) {
    std::cout << (value ? "true" : "false");
}

void tt_print_char(unsigned char value) {
    std::cout << static_cast<char>(value);
}

void tt_print_str(void* str) {
    const char* text = asCString(str);
    std::cout << (text ? text : "(null)");
}

} // extern "C"
