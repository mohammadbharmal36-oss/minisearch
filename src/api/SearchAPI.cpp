#include "api/SearchAPI.h"

#include "crawler/Url.h"

#include <crow.h>
#include <nlohmann/json.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

namespace minisearch {

namespace {

using Json = nlohmann::ordered_json;

crow::response jsonResponse(int status, const Json& body) {
    crow::response response(status, body.dump());
    response.set_header("Content-Type", "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
    return response;
}

crow::response jsonError(int status, const std::string& message) {
    Json body;
    body["error"] = message;
    return jsonResponse(status, body);
}

std::string toLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string trimCopy(std::string text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// Human-readable byte size for the stats panel.
std::string humanSize(std::uintmax_t bytes) {
    char buffer[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

std::string isoTimestamp(std::int64_t epochMillis) {
    if (epochMillis <= 0) return "";
    const std::time_t seconds = static_cast<std::time_t>(epochMillis / 1000);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&seconds));
    return buffer;
}

const char* mimeFor(const std::string& path) {
    const std::string lowered = toLower(path);
    const auto endsWith = [&](const char* suffix) {
        const std::size_t len = std::string(suffix).size();
        return lowered.size() >= len
               && lowered.compare(lowered.size() - len, len, suffix) == 0;
    };
    if (endsWith(".html") || endsWith(".htm")) return "text/html; charset=utf-8";
    if (endsWith(".css")) return "text/css; charset=utf-8";
    if (endsWith(".js")) return "text/javascript; charset=utf-8";
    if (endsWith(".svg")) return "image/svg+xml";
    if (endsWith(".png")) return "image/png";
    if (endsWith(".ico")) return "image/x-icon";
    if (endsWith(".json")) return "application/json";
    if (endsWith(".txt")) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool readFileBytes(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

// Path traversal guard: no ".." or absolute escape.
bool safeRelativePath(const std::string& path) {
    if (path.empty()) return false;
    if (path.find("..") != std::string::npos) return false;
    if (path.front() == '/' || path.front() == '\\') return false;
    return true;
}

// Wraps a handler so unexpected exceptions become a clean 500 without
// leaking internals.
template <typename Handler>
crow::response guarded(Handler&& handler) {
    try {
        return handler();
    } catch (const std::exception&) {
        return jsonError(500, "internal server error");
    } catch (...) {
        return jsonError(500, "internal server error");
    }
}

} // namespace

SearchAPI::SearchAPI(SearchEngine& engine, EngineConfig config)
    : engine_(engine), config_(std::move(config)) {}

// ---------------------------------------------------------------------------
// Static content
// ---------------------------------------------------------------------------

bool SearchAPI::serveFileFromRoot(const std::string& root, const std::string& relativePath,
                                  crow::response& response) const {
    if (!safeRelativePath(relativePath)) return false;
    std::string bytes;
    if (!readFileBytes(root + "/" + relativePath, bytes)) return false;
    response = crow::response(bytes);
    response.set_header("Content-Type", mimeFor(relativePath));
    return true;
}

crow::response SearchAPI::handleStatic(const std::string& path) const {
    crow::response response;
    std::string clean = path;
    if (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());

    // Pretty routes for the four pages.
    if (clean.empty() || clean == "index" || clean == "index.html") {
        if (serveFileFromRoot(config_.staticRoot, "index.html", response)) return response;
    } else if (clean == "search" || clean == "search.html") {
        if (serveFileFromRoot(config_.staticRoot, "search.html", response)) return response;
    } else if (clean == "analytics" || clean == "analytics.html") {
        if (serveFileFromRoot(config_.staticRoot, "analytics.html", response)) return response;
    } else if (clean == "crawler" || clean == "crawler.html") {
        if (serveFileFromRoot(config_.staticRoot, "crawler.html", response)) return response;
    } else if (clean == "favicon.ico" || clean == "favicon.svg") {
        if (serveFileFromRoot(config_.staticRoot, "assets/favicon.svg", response)) return response;
    } else if (clean.rfind("corpus/", 0) == 0) {
        const std::string relative = clean.substr(std::string("corpus/").size());
        if (!relative.empty() && serveFileFromRoot(config_.corpusRoot, relative, response)) {
            return response;
        }
    } else if (clean.rfind("css/", 0) == 0 || clean.rfind("js/", 0) == 0
               || clean.rfind("assets/", 0) == 0) {
        if (serveFileFromRoot(config_.staticRoot, clean, response)) return response;
    }
    return jsonError(404, "not found");
}

// ---------------------------------------------------------------------------
// Server lifecycle
// ---------------------------------------------------------------------------

void SearchAPI::setupRoutes() {
    CROW_ROUTE(app_, "/api/search")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& req) {
            return guarded([&]() {
                const char* query = req.url_params.get("q");
                const char* algorithm = req.url_params.get("algorithm");
                const char* limitParam = req.url_params.get("limit");
                const char* offsetParam = req.url_params.get("offset");

                SearchRequest request;
                request.query = query != nullptr ? query : "";
                if (algorithm != nullptr && !parseAlgorithm(algorithm, request.algorithm)) {
                    return jsonError(400, "invalid algorithm: use 'tfidf' or 'bm25'");
                }
                request.limit = limitParam != nullptr ? std::max(1, std::atoi(limitParam)) : 10;
                request.offset =
                    offsetParam != nullptr ? std::max(0, std::atoi(offsetParam)) : 0;
                if (request.limit > 50) request.limit = 50;

                const SearchResponse result = engine_.search(request);
                if (!result.ok) return jsonError(400, result.error);

                Json items = Json::array();
                for (const SearchItem& item : result.items) {
                    Json entry;
                    entry["id"] = item.id;
                    entry["title"] = item.title;
                    entry["url"] = item.url;
                    entry["snippet"] = item.snippet;
                    entry["score"] = item.score;
                    entry["matched_terms"] = item.matchedTerms;
                    items.push_back(std::move(entry));
                }
                Json body;
                body["query"] = result.query;
                body["algorithm"] = result.algorithm;
                body["total"] = result.total;
                body["offset"] = result.offset;
                body["limit"] = result.limit;
                body["took_ms"] = result.tookMs;
                body["items"] = std::move(items);
                return jsonResponse(200, body);
            });
        });

    CROW_ROUTE(app_, "/api/suggest")
        .methods(crow::HTTPMethod::GET)([this](const crow::request& req) {
            return guarded([&]() {
                const char* prefix = req.url_params.get("q");
                if (prefix == nullptr) return jsonError(400, "missing q parameter");
                const std::string normalized = toLower(trimCopy(prefix));
                if (normalized.empty() || normalized.size() > 64) {
                    Json body;
                    body["suggestions"] = Json::array();
                    return jsonResponse(200, body);
                }
                const auto suggestions = engine_.suggest(normalized, 8);
                Json array = Json::array();
                for (const auto& [term, df] : suggestions) {
                    Json entry;
                    entry["term"] = term;
                    entry["documents"] = df;
                    array.push_back(std::move(entry));
                }
                Json body;
                body["suggestions"] = std::move(array);
                return jsonResponse(200, body);
            });
        });

    CROW_ROUTE(app_, "/api/stats")
        .methods(crow::HTTPMethod::GET)([this]() {
            return guarded([&]() {
                const EngineStats stats = engine_.stats();
                Json body;
                body["documents"] = stats.documents;
                body["unique_terms"] = stats.uniqueTerms;
                body["total_postings"] = stats.totalPostings;
                body["index_size_bytes"] = stats.indexSizeBytes;
                body["index_size_human"] = humanSize(stats.indexSizeBytes);
                body["average_document_tokens"] = stats.averageDocumentTokens;
                body["average_search_latency_ms"] = stats.averageSearchLatencyMs;
                body["search_count"] = stats.searchCount;
                body["pages_crawled"] = stats.pagesCrawled;
                body["pages_failed"] = stats.pagesFailed;
                body["crawls_run"] = stats.crawlsRun;
                body["last_indexed_at"] = isoTimestamp(stats.lastIndexedAtMs);
                Json topTerms = Json::array();
                for (const auto& [term, df] : stats.topTerms) {
                    Json entry;
                    entry["term"] = term;
                    entry["documents"] = df;
                    topTerms.push_back(std::move(entry));
                }
                body["top_terms"] = std::move(topTerms);
                return jsonResponse(200, body);
            });
        });

    CROW_ROUTE(app_, "/api/document/<int>")
        .methods(crow::HTTPMethod::GET)([this](int id) {
            return guarded([&]() {
                const auto document = engine_.document(static_cast<DocumentId>(id));
                if (!document.has_value()) {
                    return jsonError(404, "no document with id " + std::to_string(id));
                }
                std::string content = document->content;
                if (content.size() > 20000) content = content.substr(0, 20000) + "...";
                Json body;
                body["id"] = document->id;
                body["url"] = document->url;
                body["canonical_url"] = document->canonical_url;
                body["title"] = document->title;
                body["description"] = document->description;
                body["content"] = content;
                body["headings"] = document->headings;
                body["outgoing_links"] = document->outgoingLinkCount();
                body["http_status"] = document->http_status;
                body["crawled_at"] = isoTimestamp(document->crawled_at * 1000);
                return jsonResponse(200, body);
            });
        });

    CROW_ROUTE(app_, "/api/crawl")
        .methods(crow::HTTPMethod::POST)([this](const crow::request& req) {
            return guarded([&]() {
                Json body;
                try {
                    body = Json::parse(req.body);
                } catch (const Json::exception&) {
                    return jsonError(400, "request body must be valid JSON");
                }
                if (!body.is_object() || !body.contains("seeds")
                    || !body["seeds"].is_array() || body["seeds"].empty()) {
                    return jsonError(400, "body must contain a non-empty 'seeds' array");
                }
                if (body["seeds"].size() > 20) {
                    return jsonError(400, "at most 20 seed URLs per crawl");
                }

                CrawlConfig crawl;
                crawl.userAgent = config_.userAgent;
                crawl.timeoutSeconds = config_.timeoutSeconds;
                for (const auto& seed : body["seeds"]) {
                    if (!seed.is_string()) {
                        return jsonError(400, "seeds must be strings");
                    }
                    const std::string url = seed.get<std::string>();
                    if (!url::normalize(url).has_value()) {
                        return jsonError(400, "invalid or non-http seed URL: " + url);
                    }
                    crawl.seedUrls.push_back(url);
                }

                const auto readInt = [&](const char* key, int minValue, int maxValue,
                                         int fallback) -> std::optional<int> {
                    if (!body.contains(key)) return std::optional<int>(fallback);
                    if (!body[key].is_number_integer()) {
                        return std::nullopt;  // signal type error
                    }
                    const int value = body[key].get<int>();
                    if (value < minValue || value > maxValue) return std::nullopt;
                    return value;
                };

                const auto maxDepth = readInt("max_depth", 0, 5, config_.maxDepth);
                if (!maxDepth.has_value()) {
                    return jsonError(400, "max_depth must be an integer between 0 and 5");
                }
                crawl.maxDepth = *maxDepth;

                const auto maxPages = readInt("max_pages", 1, 500, config_.maxPages);
                if (!maxPages.has_value()) {
                    return jsonError(400, "max_pages must be an integer between 1 and 500");
                }
                crawl.maxPages = *maxPages;

                const auto delayMs = readInt("delay_ms", 0, 10000, config_.delayMs);
                if (!delayMs.has_value()) {
                    return jsonError(400, "delay_ms must be an integer between 0 and 10000");
                }
                crawl.delayMs = *delayMs;

                crawl.workerThreads = config_.workerThreads;

                if (body.contains("allowed_domains")) {
                    if (!body["allowed_domains"].is_array()
                        || body["allowed_domains"].size() > 20) {
                        return jsonError(400, "allowed_domains must be an array of at most 20 strings");
                    }
                    for (const auto& domain : body["allowed_domains"]) {
                        if (!domain.is_string()) {
                            return jsonError(400, "allowed_domains must be strings");
                        }
                        crawl.allowedDomains.push_back(domain.get<std::string>());
                    }
                }

                std::string error;
                const int jobId = engine_.startCrawl(crawl, &error);
                if (jobId < 0) return jsonError(409, error);

                Json response;
                response["job_id"] = jobId;
                return jsonResponse(200, response);
            });
        });

    CROW_ROUTE(app_, "/api/crawl/status")
        .methods(crow::HTTPMethod::GET)([this]() {
            return guarded([&]() {
                const CrawlStatusSnapshot status = engine_.crawlStatus();
                Json body;
                body["has_job"] = status.hasJob;
                body["job_id"] = status.jobId;
                body["running"] = status.running;
                body["seeds"] = status.seeds;
                Json configuration;
                configuration["max_pages"] = status.maxPages;
                configuration["max_depth"] = status.maxDepth;
                configuration["delay_ms"] = status.delayMs;
                body["config"] = configuration;
                body["queued"] = status.queued;
                body["fetched"] = status.fetched;
                body["succeeded"] = status.succeeded;
                body["failed"] = status.failed;
                body["skipped"] = status.skipped;
                body["started_at"] = isoTimestamp(status.startedAtMs);
                body["finished_at"] = isoTimestamp(status.finishedAtMs);
                if (status.running && status.startedAtMs > 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count()
                                         - status.startedAtMs;
                    body["elapsed_ms"] = elapsed > 0 ? elapsed : 0;
                } else {
                    body["elapsed_ms"] = status.finishedAtMs > 0
                                             ? status.finishedAtMs - status.startedAtMs
                                             : 0;
                }
                Json recent = Json::array();
                const std::size_t shown = std::min<std::size_t>(status.recent.size(), 50);
                for (std::size_t i = status.recent.size() - shown; i < status.recent.size();
                     ++i) {
                    Json entry;
                    entry["url"] = status.recent[i].url;
                    entry["status"] = status.recent[i].kind;
                    entry["http_status"] = status.recent[i].httpStatus;
                    entry["note"] = status.recent[i].note;
                    recent.push_back(std::move(entry));
                }
                body["recent"] = std::move(recent);
                return jsonResponse(200, body);
            });
        });

    // Static content LAST so the literal /api routes win.
    CROW_CATCHALL_ROUTE(app_)([this](const crow::request& req) {
        return guarded([&]() { return handleStatic(req.url); });
    });
}

void SearchAPI::setupCorpusRoutes() {
    // A dedicated virtual host for the seed corpus so that /robots.txt
    // sits at the site root, exactly where a crawler looks for it.
    CROW_ROUTE(corpusApp_, "/")
        .methods(crow::HTTPMethod::GET)([this](const crow::request&) {
            crow::response response;
            if (serveFileFromRoot(config_.corpusRoot, "index.html", response)) {
                return response;
            }
            return jsonError(404, "corpus not found");
        });

    CROW_CATCHALL_ROUTE(corpusApp_)([this](const crow::request& req) {
        crow::response response;
        std::string clean = req.url;
        if (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());
        if (serveFileFromRoot(config_.corpusRoot, clean, response)) {
            return response;
        }
        return jsonError(404, "not found");
    });
}

void SearchAPI::run() {
    setupRoutes();
    setupCorpusRoutes();
    // The futures must be stored: destroying a std::async future blocks
    // until its server exits, which would wedge this thread.
    corpusFuture_ = corpusApp_  // the corpus site lives on main port + 1
        .bindaddr(config_.host)
        .port(static_cast<std::uint16_t>(config_.port + 1))
        .loglevel(crow::LogLevel::Warning)
        .run_async();
    app_.multithreaded()
        .concurrency(4)
        .bindaddr(config_.host)
        .port(static_cast<std::uint16_t>(config_.port))
        .loglevel(crow::LogLevel::Warning)
        .run();
    // The main app only returns from run() when it is stopped (Ctrl+C);
    // the corpus site must be stopped too, or its future wedges shutdown.
    corpusApp_.stop();
}

SearchAPI::~SearchAPI() {
    // Must run before appFuture_/corpusFuture_ are destroyed: those futures
    // (std::async) block in ~future() until the server loops exit.
    stop();
}

void SearchAPI::runAsync() {
    setupRoutes();
    setupCorpusRoutes();
    corpusFuture_ = corpusApp_  // the corpus site lives on main port + 1
        .bindaddr("127.0.0.1")
        .port(static_cast<std::uint16_t>(config_.port + 1))
        .loglevel(crow::LogLevel::Warning)
        .run_async();
    appFuture_ = app_.multithreaded()
        .concurrency(2)
        .bindaddr("127.0.0.1")
        .port(static_cast<std::uint16_t>(config_.port))
        .loglevel(crow::LogLevel::Warning)
        .run_async();
}

bool SearchAPI::waitUntilReady(int timeoutMs) const {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<std::uint16_t>(config_.port));
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const int result =
                ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            ::close(fd);
            if (result == 0) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void SearchAPI::stop() {
    app_.stop();
    corpusApp_.stop();
}

} // namespace minisearch
