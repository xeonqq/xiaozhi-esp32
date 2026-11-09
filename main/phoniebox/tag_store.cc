#include "tag_store.h"

#include <esp_log.h>
#include <nvs.h>

#include <cstdio>
#include <cstring>

#define TAG "PhonieboxTags"

namespace {
constexpr const char* kNamespace = "phoniebox";
// NVS allows keys of at most 15 characters; add one for the terminator.
constexpr size_t kMaxKeyLength = 16;
constexpr size_t kMaxNameLength = 96;
}  // namespace

std::string TagStore::NormalizeUid(const std::string& uid) {
    std::string normalized;
    normalized.reserve(uid.size());
    for (char c : uid) {
        if (c == ' ' || c == ':' || c == '-') {
            continue;
        }
        if (c >= '0' && c <= '9') {
            normalized.push_back(c);
        } else if (c >= 'a' && c <= 'f') {
            normalized.push_back(static_cast<char>(c - 'a' + 'A'));
        } else if (c >= 'A' && c <= 'F') {
            normalized.push_back(c);
        } else {
            return {};
        }
    }
    if (normalized.size() < 4 || normalized.size() > 14 || (normalized.size() % 2) != 0) {
        return {};
    }
    return normalized;
}

bool TagStore::MakeKey(const std::string& uid, char* key, size_t key_size) {
    std::string normalized = NormalizeUid(uid);
    if (normalized.empty() || key_size < normalized.size() + 2 ||
        normalized.size() + 1 > kMaxKeyLength) {
        return false;
    }
    std::snprintf(key, key_size, "t%s", normalized.c_str());
    return true;
}

bool TagStore::Get(const std::string& uid, PhonieboxTag& tag) {
    char key[kMaxKeyLength + 1] = {};
    if (!MakeKey(uid, key, sizeof(key))) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t length = 0;
    err = nvs_get_str(handle, key, nullptr, &length);
    if (err != ESP_OK || length == 0 || length > 512) {
        nvs_close(handle);
        return false;
    }

    std::string value(length, '\0');
    err = nvs_get_str(handle, key, value.data(), &length);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }

    size_t separator = value.find('\n');
    tag.uid = NormalizeUid(uid);
    if (separator == std::string::npos) {
        tag.path = value;
        tag.name.clear();
    } else {
        tag.path = value.substr(0, separator);
        tag.name = value.substr(separator + 1);
    }
    return !tag.path.empty();
}

bool TagStore::Set(const std::string& uid, const std::string& path, const std::string& name) {
    char key[kMaxKeyLength + 1] = {};
    if (!MakeKey(uid, key, sizeof(key)) || path.empty()) {
        return false;
    }

    std::string safe_name = name.substr(0, kMaxNameLength);
    std::string value = path + "\n" + safe_name;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, key, value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save tag %s: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool TagStore::Remove(const std::string& uid) {
    char key[kMaxKeyLength + 1] = {};
    if (!MakeKey(uid, key, sizeof(key))) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

std::vector<PhonieboxTag> TagStore::List() {
    std::vector<PhonieboxTag> tags;
    nvs_iterator_t iterator = nullptr;
    esp_err_t err = nvs_entry_find("nvs", kNamespace, NVS_TYPE_STR, &iterator);
    while (err == ESP_OK && iterator != nullptr) {
        nvs_entry_info_t info = {};
        nvs_entry_info(iterator, &info);
        if (info.key[0] == 't') {
            PhonieboxTag tag;
            if (Get(&info.key[1], tag)) {
                tags.push_back(std::move(tag));
            }
        }
        err = nvs_entry_next(&iterator);
    }
    if (iterator != nullptr) {
        nvs_release_iterator(iterator);
    }
    return tags;
}
