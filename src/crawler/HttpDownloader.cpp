#include "crawler/HttpDownloader.h"

#include <curl/curl.h>

#include <mutex>

namespace minisearch {

namespace {

std::once_flag globalCurlInit;

void ensureGlobalCurlInit() {
    std::call_once(globalCurlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct WriteContext {
    std::string* body = nullptr;
    std::size_t maxBytes = 0;
    bool tooLarge = false;
};

std::size_t writeCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    WriteContext* context = static_cast<WriteContext*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (context->body->size() + bytes > context->maxBytes) {
        context->tooLarge = true;
        return 0;  // returning less than requested aborts the transfer
    }
    context->body->append(ptr, bytes);
    return bytes;
}

} // namespace

HttpDownloader::HttpDownloader(const std::string& userAgent, long timeoutSeconds,
                               std::size_t maxBytes)
    : easy_(nullptr), timeoutSeconds_(timeoutSeconds), maxBytes_(maxBytes) {
    ensureGlobalCurlInit();
    easy_ = curl_easy_init();
    if (easy_ == nullptr) return;

    curl_easy_setopt(easy_, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(easy_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy_, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(easy_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy_, CURLOPT_WRITEFUNCTION, writeCallback);
    // Ask the server for compressed transfer and let libcurl decode it.
    curl_easy_setopt(easy_, CURLOPT_ACCEPT_ENCODING, "");
}

HttpDownloader::~HttpDownloader() {
    if (easy_ != nullptr) curl_easy_cleanup(easy_);
}

HttpDownloader::FetchResult HttpDownloader::fetch(const std::string& url) {
    FetchResult result;
    if (easy_ == nullptr) {
        result.error = "libcurl initialization failed";
        return result;
    }

    std::string body;
    WriteContext context{&body, maxBytes_, false};

    curl_easy_setopt(easy_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy_, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(easy_, CURLOPT_TIMEOUT, timeoutSeconds_);
    curl_easy_setopt(easy_, CURLOPT_CONNECTTIMEOUT,
                     timeoutSeconds_ < 10 ? timeoutSeconds_ : 10L);

    const CURLcode rc = curl_easy_perform(easy_);
    if (rc != CURLE_OK) {
        if (context.tooLarge) {
            result.error = "response exceeds the configured size limit";
        } else {
            result.error = curl_easy_strerror(rc);
        }
        return result;
    }

    long status = 0;
    curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &status);
    result.status = static_cast<int>(status);

    char* finalUrl = nullptr;
    curl_easy_getinfo(easy_, CURLINFO_EFFECTIVE_URL, &finalUrl);
    if (finalUrl != nullptr) result.finalUrl = finalUrl;

    char* contentType = nullptr;
    curl_easy_getinfo(easy_, CURLINFO_CONTENT_TYPE, &contentType);
    if (contentType != nullptr) result.contentType = contentType;

    result.body = std::move(body);
    result.ok = true;
    return result;
}

} // namespace minisearch
