#pragma once

// MiniSearch - HTTP API + static file server (spec module 9).
//
// Endpoints (JSON):
//   GET  /api/search?q=&algorithm=tfidf|bm25&limit=&offset=
//   GET  /api/suggest?q=
//   GET  /api/stats
//   GET  /api/document/<id>
//   POST /api/crawl            {seeds, max_depth, max_pages, delay_ms?, allowed_domains?}
//   GET  /api/crawl/status
// Static: /, /search, /analytics, /crawler (HTML), /css/*, /js/*, /assets/*,
//         /corpus/* (the seed corpus, so the crawler can crawl this very
//         server over real HTTP).
//
// Validation errors -> 400; unknown document -> 404; crawl while a crawl
// runs -> 409; handler exceptions -> 500 with a generic message (no
// internal paths leaked).

#include "engine/SearchEngine.h"

#include <crow.h>

#include <future>
#include <string>

namespace minisearch {

class SearchAPI {
public:
    SearchAPI(SearchEngine& engine, EngineConfig config);

    // Stops both HTTP servers. run_async()'s futures block in their
    // destructor until the servers exit, so they MUST be stopped before
    // the members are destroyed - otherwise the process hangs at shutdown.
    ~SearchAPI();

    // Blocking; returns when the server stops (Ctrl+C).
    void run();

    // For integration tests: runs in a background thread.
    void runAsync();
    // Blocks until the listening socket accepts (or timeoutMs elapses).
    bool waitUntilReady(int timeoutMs) const;
    void stop();

    int port() const noexcept { return config_.port; }
    // The seed corpus is served as its own site (robots.txt at its root,
    // so crawler compliance is genuinely exercised).
    int corpusPort() const noexcept { return config_.port + 1; }

private:
    void setupRoutes();
    void setupCorpusRoutes();
    bool serveFileFromRoot(const std::string& root, const std::string& relativePath,
                           crow::response& response) const;
    crow::response handleStatic(const std::string& path) const;

    SearchEngine& engine_;
    EngineConfig config_;
    crow::SimpleApp app_;
    crow::SimpleApp corpusApp_;
    // Crow's run_async() returns futures whose destructors block until the
    // servers stop - they must be kept alive for the process lifetime.
    std::future<void> appFuture_;
    std::future<void> corpusFuture_;
};

} // namespace minisearch
