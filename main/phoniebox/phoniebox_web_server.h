#ifndef PHONIEBOX_WEB_SERVER_H
#define PHONIEBOX_WEB_SERVER_H

#include <esp_http_server.h>
#include <cJSON.h>

#include <string>
#include <vector>

class PhonieboxService;

/**
 * Small HTTP server for the Phoniebox web interface and REST API.
 */
class PhonieboxWebServer {
public:
    explicit PhonieboxWebServer(PhonieboxService& service);
    ~PhonieboxWebServer();

    bool Start();

private:
    static esp_err_t HandleIndex(httpd_req_t* request);
    static esp_err_t HandleCss(httpd_req_t* request);
    static esp_err_t HandleJs(httpd_req_t* request);
    static esp_err_t HandleStatus(httpd_req_t* request);
    static esp_err_t HandleTagsGet(httpd_req_t* request);
    static esp_err_t HandleTagsPost(httpd_req_t* request);
    static esp_err_t HandleTagsDelete(httpd_req_t* request);
    static esp_err_t HandleFiles(httpd_req_t* request);
    static esp_err_t HandlePlay(httpd_req_t* request);
    static esp_err_t HandleControl(httpd_req_t* request);

    static PhonieboxService& Service(httpd_req_t* request);
    static bool ReadBody(httpd_req_t* request, std::string& body);
    static bool QueryValue(httpd_req_t* request, const char* key, std::string& value);
    static std::string UrlDecode(const std::string& value);
    static esp_err_t SendJson(httpd_req_t* request, const char* status, const char* body);
    static esp_err_t SendJsonRoot(httpd_req_t* request, const char* status, cJSON* root);
    static esp_err_t SendOk(httpd_req_t* request);
    static esp_err_t SendError(httpd_req_t* request, const char* status, const char* message);
    static esp_err_t SendEmbedded(httpd_req_t* request, const char* content_type,
                                  const uint8_t* start, const uint8_t* end);

    PhonieboxService& service_;
    httpd_handle_t server_ = nullptr;
    std::vector<httpd_uri_t> uris_;
};

#endif  // PHONIEBOX_WEB_SERVER_H
