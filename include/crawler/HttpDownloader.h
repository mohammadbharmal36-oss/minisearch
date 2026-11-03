#pragma once

// MiniSearch - HTTP downloading via libcurl (spec module 5).
//
// One HttpDownloader per worker thread (libcurl easy handles must not be
// shared across threads). Handles: timeouts, redirect following with a
// cap, automatic gzip/deflate decoding, response-size limiting and
// honest error reporting via curl error strings.

#include <cstddef>
#include <string>

typedef void CURL;

namespace minisearch {

class HttpDownloader {
public:
    struct FetchResult {
        bool ok = false;          // transport-level success (2xx/3xx/4xx all ok)
        int status = 0;           // final HTTP status code
        std::string body;         // decoded body (empty unless ok)
        std::string finalUrl;     // effective URL after redirects
        std::string contentType;  // Content-Type of the final response
        std::string error;        // human-readable failure reason
    };

    HttpDownloader(const std::string& userAgent, long timeoutSeconds,
                   std::size_t maxBytes);
    ~HttpDownloader();

    HttpDownloader(const HttpDownloader&) = delete;
    HttpDownloader& operator=(const HttpDownloader&) = delete;

    FetchResult fetch(const std::string& url);

private:
    CURL* easy_ = nullptr;
    long timeoutSeconds_ = 0;
    std::size_t maxBytes_ = 0;
};

} // namespace minisearch
