#include "internal.hpp"

#include <curl/curl.h>

namespace weflow {

namespace {

std::mutex g_cloud_mutex;
std::string g_cloud_base_url = "https://api.weflow.top";
std::string g_cloud_token;
int64_t g_cloud_token_updated_at = 0;

size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<const char*>(contents), totalSize);
    return totalSize;
}

int PerformCloudHttpRequest(
    const std::string& method,
    const std::string& url,
    const std::string& bodyJson,
    const std::string& bearerToken,
    std::string& outBody,
    long& outHttpCode) {
    CURL* curl = ::curl_easy_init();
    if (!curl) return -1;

    struct curl_slist* headers = nullptr;
    headers = ::curl_slist_append(headers, "Content-Type: application/json");
    if (!bearerToken.empty()) {
        std::string authHeader = "Authorization: Bearer " + bearerToken;
        headers = ::curl_slist_append(headers, authHeader.c_str());
    }

    ::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    ::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    ::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    ::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBody);
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    ::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);

    if (method == "POST") {
        ::curl_easy_setopt(curl, CURLOPT_POST, 1L);
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyJson.c_str());
        ::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyJson.size()));
    }

    CURLcode res = ::curl_easy_perform(curl);
    ::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &outHttpCode);
    ::curl_slist_free_all(headers);
    ::curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : -2;
}

} // namespace

} // namespace weflow

extern "C" {

WCDB_API int32_t wcdb_submit_cloud_report(const char* payloadJson, char** outResponseJson) {
    if (!payloadJson || !outResponseJson) return -1;
    *outResponseJson = nullptr;

    std::string baseUrl, token;
    {
        std::lock_guard<std::mutex> lk(weflow::g_cloud_mutex);
        baseUrl = weflow::g_cloud_base_url;
        token = weflow::g_cloud_token;
    }
    std::string url = baseUrl + "/api/report";
    std::string respBody;
    long httpCode = 0;
    int rc = weflow::PerformCloudHttpRequest("POST", url, payloadJson, token, respBody, httpCode);
    if (rc != 0) {
        *outResponseJson = weflow::AllocCString("{\"success\":false,\"error\":\"network_error\"}");
        return rc;
    }
    *outResponseJson = weflow::AllocCString(respBody.empty() ? "{\"success\":true}" : respBody);
    return 0;
}

WCDB_API int32_t wcdb_submit_cloud_reports_batch(const char* payloadsJson, char** outResponseJson) {
    if (!payloadsJson || !outResponseJson) return -1;
    *outResponseJson = nullptr;

    std::string baseUrl, token;
    {
        std::lock_guard<std::mutex> lk(weflow::g_cloud_mutex);
        baseUrl = weflow::g_cloud_base_url;
        token = weflow::g_cloud_token;
    }
    std::string url = baseUrl + "/api/reports/batch";
    std::string respBody;
    long httpCode = 0;
    int rc = weflow::PerformCloudHttpRequest("POST", url, payloadsJson, token, respBody, httpCode);
    if (rc != 0) {
        *outResponseJson = weflow::AllocCString("{\"success\":false,\"error\":\"network_error\"}");
        return rc;
    }
    *outResponseJson = weflow::AllocCString(respBody.empty() ? "{\"success\":true}" : respBody);
    return 0;
}

WCDB_API int32_t wcdb_set_cloud_token(const char* token) {
    if (!token) return -1;
    std::lock_guard<std::mutex> lk(weflow::g_cloud_mutex);
    weflow::g_cloud_token = token;
    weflow::g_cloud_token_updated_at = static_cast<int64_t>(std::time(nullptr));
    return 0;
}

WCDB_API int32_t wcdb_clear_cloud_token(void) {
    std::lock_guard<std::mutex> lk(weflow::g_cloud_mutex);
    weflow::g_cloud_token.clear();
    weflow::g_cloud_token_updated_at = 0;
    return 0;
}

WCDB_API int32_t wcdb_fetch_cloud_token(char** outResponseJson) {
    if (!outResponseJson) return -1;
    *outResponseJson = nullptr;

    std::string baseUrl;
    {
        std::lock_guard<std::mutex> lk(weflow::g_cloud_mutex);
        baseUrl = weflow::g_cloud_base_url;
    }
    std::string url = baseUrl + "/api/token";
    std::string respBody;
    long httpCode = 0;
    int rc = weflow::PerformCloudHttpRequest("POST", url, "{}", "", respBody, httpCode);
    if (rc != 0) {
        *outResponseJson = weflow::AllocCString("{\"success\":false,\"error\":\"network_error\"}");
        return rc;
    }
    *outResponseJson = weflow::AllocCString(respBody.empty() ? "{}" : respBody);
    return 0;
}

WCDB_API int32_t wcdb_fetch_and_set_cloud_token(char** outResponseJson) {
    char* resp = nullptr;
    int32_t rc = wcdb_fetch_cloud_token(&resp);
    if (rc == 0 && resp) {
        std::string tok = weflow::ParseJsonStringField(resp, "token");
        if (!tok.empty()) {
            wcdb_set_cloud_token(tok.c_str());
        }
    }
    if (outResponseJson) {
        *outResponseJson = resp;
    } else {
        wcdb_free_string(resp);
    }
    return rc;
}

WCDB_API int32_t wcdb_get_cloud_token_status(char** outStatusJson) {
    if (!outStatusJson) return -1;
    std::lock_guard<std::mutex> lk(weflow::g_cloud_mutex);
    std::ostringstream oss;
    oss << "{\"has_token\":" << (weflow::g_cloud_token.empty() ? "false" : "true") << ","
        << "\"updated_at\":" << weflow::g_cloud_token_updated_at << ","
        << "\"base_url\":\"" << weflow::EscapeJson(weflow::g_cloud_base_url) << "\"}";
    *outStatusJson = weflow::AllocCString(oss.str());
    return 0;
}

WCDB_API int32_t wcdb_set_cloud_base_url(const char* baseUrl) {
    if (!baseUrl || baseUrl[0] == '\0') return -1;
    std::lock_guard<std::mutex> lk(weflow::g_cloud_mutex);
    weflow::g_cloud_base_url = baseUrl;
    while (!weflow::g_cloud_base_url.empty() && weflow::g_cloud_base_url.back() == '/') {
        weflow::g_cloud_base_url.pop_back();
    }
    return 0;
}

WCDB_API int32_t wcdb_get_cloud_base_url(char** outBaseUrl) {
    if (!outBaseUrl) return -1;
    std::lock_guard<std::mutex> lk(weflow::g_cloud_mutex);
    *outBaseUrl = weflow::AllocCString(weflow::g_cloud_base_url);
    return 0;
}

} // extern "C"
