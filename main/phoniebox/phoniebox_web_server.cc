#include "phoniebox_web_server.h"

#include <esp_log.h>
#include <cJSON.h>

#include <cstdlib>
#include <cstring>

#include "phoniebox_service.h"

#define TAG "PhonieboxWeb"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
extern const uint8_t app_css_end[] asm("_binary_app_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");

namespace {

const char* JsonString(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}

const char* PlayerStateName(const PhonieboxStatus& status) {
    if (!status.player_active) {
        return "stopped";
    }
    return status.player_paused ? "paused" : "playing";
}

}  // namespace

PhonieboxWebServer::PhonieboxWebServer(PhonieboxService& service) : service_(service) {}

PhonieboxWebServer::~PhonieboxWebServer() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

bool PhonieboxWebServer::Start() {
    if (server_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Port 80 belongs to the Wi-Fi configuration captive portal. Using a
    // separate port lets both servers coexist when the device enters config
    // mode (the portal starts its own HTTP server on port 80).
    config.server_port = 8080;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        server_ = nullptr;
        return false;
    }

    auto add_uri = [this](const char* uri, httpd_method_t method,
                          esp_err_t (*handler)(httpd_req_t*)) {
        httpd_uri_t entry = {};
        entry.uri = uri;
        entry.method = method;
        entry.handler = handler;
        entry.user_ctx = &service_;
        esp_err_t err = httpd_register_uri_handler(server_, &entry);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", uri, esp_err_to_name(err));
            return false;
        }
        uris_.push_back(entry);
        return true;
    };

    bool ok =
        add_uri("/", HTTP_GET, HandleIndex) && add_uri("/app.css", HTTP_GET, HandleCss) &&
        add_uri("/app.js", HTTP_GET, HandleJs) && add_uri("/api/status", HTTP_GET, HandleStatus) &&
        add_uri("/api/tags", HTTP_GET, HandleTagsGet) &&
        add_uri("/api/tags", HTTP_POST, HandleTagsPost) &&
        add_uri("/api/tags", HTTP_DELETE, HandleTagsDelete) &&
        add_uri("/api/fs", HTTP_GET, HandleFiles) && add_uri("/api/play", HTTP_POST, HandlePlay) &&
        add_uri("/api/control", HTTP_POST, HandleControl);

    if (!ok) {
        httpd_stop(server_);
        server_ = nullptr;
        uris_.clear();
        return false;
    }

    ESP_LOGI(TAG, "Web interface available on port %d", config.server_port);
    return true;
}

PhonieboxService& PhonieboxWebServer::Service(httpd_req_t* request) {
    return *static_cast<PhonieboxService*>(request->user_ctx);
}

esp_err_t PhonieboxWebServer::HandleIndex(httpd_req_t* request) {
    return SendEmbedded(request, "text/html; charset=utf-8", index_html_start, index_html_end);
}

esp_err_t PhonieboxWebServer::HandleCss(httpd_req_t* request) {
    return SendEmbedded(request, "text/css; charset=utf-8", app_css_start, app_css_end);
}

esp_err_t PhonieboxWebServer::HandleJs(httpd_req_t* request) {
    return SendEmbedded(request, "application/javascript; charset=utf-8", app_js_start, app_js_end);
}

esp_err_t PhonieboxWebServer::HandleStatus(httpd_req_t* request) {
    PhonieboxStatus status = Service(request).GetStatus();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "sd_mounted", status.sd_mounted);
    cJSON_AddBoolToObject(root, "rfid_ready", status.rfid_ready);
    cJSON_AddStringToObject(root, "mount_point", status.mount_point.c_str());
    cJSON_AddNumberToObject(root, "volume", status.volume);

    cJSON* player = cJSON_AddObjectToObject(root, "player");
    cJSON_AddStringToObject(player, "state", PlayerStateName(status));
    cJSON_AddStringToObject(player, "path", status.current_path.c_str());
    cJSON_AddStringToObject(player, "title", status.current_title.c_str());
    cJSON_AddNumberToObject(player, "position_ms", status.position_ms);
    cJSON_AddNumberToObject(player, "index", status.playlist_index);
    cJSON_AddNumberToObject(player, "count", status.playlist_count);

    cJSON* last_tag = cJSON_AddObjectToObject(root, "last_tag");
    cJSON_AddStringToObject(last_tag, "uid", status.last_uid.c_str());
    cJSON_AddStringToObject(last_tag, "path", status.last_tag_path.c_str());
    cJSON_AddStringToObject(last_tag, "name", status.last_tag_name.c_str());
    cJSON_AddBoolToObject(last_tag, "assigned", status.last_tag_assigned);

    return SendJsonRoot(request, "200 OK", root);
}

esp_err_t PhonieboxWebServer::HandleTagsGet(httpd_req_t* request) {
    auto tags = Service(request).GetTags();
    cJSON* root = cJSON_CreateArray();
    for (const auto& tag : tags) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "uid", tag.uid.c_str());
        cJSON_AddStringToObject(item, "path", tag.path.c_str());
        cJSON_AddStringToObject(item, "name", tag.name.c_str());
        cJSON_AddItemToArray(root, item);
    }
    return SendJsonRoot(request, "200 OK", root);
}

esp_err_t PhonieboxWebServer::HandleTagsPost(httpd_req_t* request) {
    std::string body;
    if (!ReadBody(request, body)) {
        return SendError(request, "400 Bad Request", "Invalid request body");
    }

    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return SendError(request, "400 Bad Request", "Invalid JSON");
    }

    const char* uid = JsonString(json, "uid");
    const char* path = JsonString(json, "path");
    const char* name = JsonString(json, "name");
    bool ok = uid != nullptr && path != nullptr &&
              Service(request).AssignTag(uid, path, name != nullptr ? name : "");
    cJSON_Delete(json);

    if (!ok) {
        return SendError(request, "400 Bad Request", "Invalid tag or path");
    }
    return SendOk(request);
}

esp_err_t PhonieboxWebServer::HandleTagsDelete(httpd_req_t* request) {
    std::string uid;
    if (!QueryValue(request, "uid", uid)) {
        return SendError(request, "400 Bad Request", "Missing uid");
    }
    if (!Service(request).RemoveTag(uid)) {
        return SendError(request, "404 Not Found", "Tag not found");
    }
    return SendOk(request);
}

esp_err_t PhonieboxWebServer::HandleFiles(httpd_req_t* request) {
    std::string path;
    QueryValue(request, "path", path);

    std::vector<PhonieboxFsEntry> entries;
    if (!Service(request).Browse(path, entries)) {
        return SendError(request, "404 Not Found", "Folder not available");
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path.empty() ? "/sdcard" : path.c_str());

    cJSON* array = cJSON_AddArrayToObject(root, "entries");
    for (const auto& entry : entries) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entry.name.c_str());
        cJSON_AddStringToObject(item, "path", entry.path.c_str());
        cJSON_AddBoolToObject(item, "dir", entry.is_dir);
        cJSON_AddBoolToObject(item, "playable", entry.playable);
        cJSON_AddItemToArray(array, item);
    }
    return SendJsonRoot(request, "200 OK", root);
}

esp_err_t PhonieboxWebServer::HandlePlay(httpd_req_t* request) {
    std::string body;
    if (!ReadBody(request, body)) {
        return SendError(request, "400 Bad Request", "Invalid request body");
    }

    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return SendError(request, "400 Bad Request", "Invalid JSON");
    }
    const char* path = JsonString(json, "path");
    bool ok = path != nullptr && Service(request).PlayPath(path);
    cJSON_Delete(json);

    if (!ok) {
        return SendError(request, "400 Bad Request", "Cannot play this path");
    }
    return SendOk(request);
}

esp_err_t PhonieboxWebServer::HandleControl(httpd_req_t* request) {
    std::string body;
    if (!ReadBody(request, body)) {
        return SendError(request, "400 Bad Request", "Invalid request body");
    }

    cJSON* json = cJSON_Parse(body.c_str());
    if (json == nullptr) {
        return SendError(request, "400 Bad Request", "Invalid JSON");
    }

    const char* action = JsonString(json, "action");
    if (action == nullptr) {
        cJSON_Delete(json);
        return SendError(request, "400 Bad Request", "Missing action");
    }

    auto& service = Service(request);
    bool ok = true;
    if (std::strcmp(action, "pause") == 0) {
        service.Pause();
    } else if (std::strcmp(action, "resume") == 0) {
        service.Resume();
    } else if (std::strcmp(action, "toggle") == 0) {
        service.TogglePause();
    } else if (std::strcmp(action, "next") == 0) {
        service.Next();
    } else if (std::strcmp(action, "previous") == 0) {
        service.Previous();
    } else if (std::strcmp(action, "stop") == 0) {
        service.Stop();
    } else if (std::strcmp(action, "volume") == 0) {
        cJSON* volume = cJSON_GetObjectItemCaseSensitive(json, "volume");
        ok = cJSON_IsNumber(volume) && service.SetVolume(static_cast<int>(volume->valuedouble));
    } else {
        ok = false;
    }
    cJSON_Delete(json);

    if (!ok) {
        return SendError(request, "400 Bad Request", "Unknown action");
    }
    return SendOk(request);
}

bool PhonieboxWebServer::ReadBody(httpd_req_t* request, std::string& body) {
    if (request->content_len == 0 || request->content_len > 2048) {
        return false;
    }
    body.resize(request->content_len);
    size_t received = 0;
    while (received < body.size()) {
        int ret = httpd_req_recv(request, body.data() + received, body.size() - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return false;
        }
        received += static_cast<size_t>(ret);
    }
    return true;
}

bool PhonieboxWebServer::QueryValue(httpd_req_t* request, const char* key, std::string& value) {
    size_t length = httpd_req_get_url_query_len(request);
    if (length == 0 || length > 1024) {
        return false;
    }
    std::vector<char> query(length + 1, '\0');
    if (httpd_req_get_url_query_str(request, query.data(), query.size()) != ESP_OK) {
        return false;
    }
    std::vector<char> raw(length + 1, '\0');
    if (httpd_query_key_value(query.data(), key, raw.data(), raw.size()) != ESP_OK) {
        return false;
    }
    value = UrlDecode(raw.data());
    return true;
}

std::string PhonieboxWebServer::UrlDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            result.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            int high = hex(value[i + 1]);
            int low = hex(value[i + 2]);
            if (high >= 0 && low >= 0) {
                result.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
            result.push_back(value[i]);
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

esp_err_t PhonieboxWebServer::SendJsonRoot(httpd_req_t* request, const char* status, cJSON* root) {
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t err = SendJson(request, status, body != nullptr ? body : "{}");
    cJSON_free(body);
    return err;
}

esp_err_t PhonieboxWebServer::SendOk(httpd_req_t* request) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return SendJsonRoot(request, "200 OK", root);
}

esp_err_t PhonieboxWebServer::SendJson(httpd_req_t* request, const char* status, const char* body) {
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t PhonieboxWebServer::SendError(httpd_req_t* request, const char* status,
                                        const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message);
    return SendJsonRoot(request, status, root);
}

esp_err_t PhonieboxWebServer::SendEmbedded(httpd_req_t* request, const char* content_type,
                                           const uint8_t* start, const uint8_t* end) {
    httpd_resp_set_type(request, content_type);
    return httpd_resp_send(request, reinterpret_cast<const char*>(start), end - start);
}
