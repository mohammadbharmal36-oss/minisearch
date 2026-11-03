// MiniSearch - integration tests.
//
// Boots the REAL stack in-process: SearchEngine + SearchAPI (Crow) with
// the repository's seed corpus served as its own site on a dedicated
// port, then exercises it through genuine HTTP requests (libcurl):
// crawling the corpus, searching it (keyword / phrase / Boolean / NOT /
// both algorithms), suggestions, stats, documents, error handling, and
// persistence across engine restarts.

#include "api/SearchAPI.h"
#include "common/Config.h"
#include "engine/SearchEngine.h"

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "test_framework.h"

namespace {

using Json = nlohmann::json;
using minisearch::EngineConfig;
using minisearch::SearchAPI;
using minisearch::SearchEngine;

constexpr int kTestPort = 18099;

struct HttpResult {
    int status = 0;
    std::string body;
    bool ok = false;
};

std::size_t writeToString(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

HttpResult httpRequest(const std::string& method, const std::string& url,
                       const std::string& body = "") {
    HttpResult result;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) return result;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    }
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        result.status = static_cast<int>(status);
        result.ok = true;
    }
    curl_easy_cleanup(curl);
    return result;
}

std::string apiUrl(const std::string& path) {
    return "http://127.0.0.1:" + std::to_string(kTestPort) + path;
}

EngineConfig makeTestConfig() {
    EngineConfig config;
    config.port = kTestPort;
    config.host = "127.0.0.1";
    const std::string dataDir = std::string(MINISEARCH_TEST_TMP) + "/integration";
    std::filesystem::create_directories(dataDir + "/database");
    std::filesystem::create_directories(dataDir + "/index");
    config.dbPath = dataDir + "/database/integration.db";
    config.indexPath = dataDir + "/index/inverted.index";
    config.staticRoot = std::string(MINISEARCH_SOURCE_DIR) + "/frontend";
    config.corpusRoot = std::string(MINISEARCH_SOURCE_DIR) + "/tools/corpus";
    config.maxDepth = 3;
    config.maxPages = 40;
    config.delayMs = 10;
    config.workerThreads = 2;
    return config;
}

// One shared server for the whole suite (booting Crow per test would be
// needlessly slow); documents accumulate across tests exactly like a real
// long-running deployment, which the later tests rely on.
SearchEngine& sharedEngine() {
    static SearchEngine engine(makeTestConfig());
    return engine;
}

SearchAPI& sharedApi() {
    static SearchAPI api(sharedEngine(), makeTestConfig());
    return api;
}

bool bootServer() {
    static bool started = false;
    static bool ok = false;
    if (started) return ok;
    started = true;
    if (!sharedEngine().initialize()) return false;
    sharedApi().runAsync();
    ok = sharedApi().waitUntilReady(15000);
    return ok;
}

Json parseJson(const std::string& body) {
    return Json::parse(body);
}

// Waits until the crawl job is finished (or the timeout fires).
bool waitForCrawl(int timeoutSeconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const HttpResult result = httpRequest("GET", apiUrl("/api/crawl/status"));
        if (result.ok) {
            const Json status = parseJson(result.body);
            if (!status.value("running", true)) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

std::string joinIdsFromSearch(const std::string& query, const std::string& algorithm) {
    const HttpResult result = httpRequest(
        "GET", apiUrl("/api/search?q=" + query + "&algorithm=" + algorithm + "&limit=50"));
    if (!result.ok) return "HTTP-FAIL";
    const Json body = parseJson(result.body);
    std::string ids;
    for (const auto& item : body["items"]) {
        if (!ids.empty()) ids += ',';
        ids += std::to_string(item["id"].get<int>());
    }
    return ids;
}

} // namespace

MINI_TEST(server_boots_and_serves_static_pages) {
    MINI_EXPECT_TRUE(bootServer());
    const HttpResult home = httpRequest("GET", apiUrl("/"));
    MINI_EXPECT_EQ(home.status, 200);
    MINI_EXPECT_TRUE(home.body.find("MiniSearch") != std::string::npos);
    MINI_EXPECT_TRUE(home.body.find("Explore information") != std::string::npos);

    const HttpResult css = httpRequest("GET", apiUrl("/css/style.css"));
    MINI_EXPECT_EQ(css.status, 200);

    const HttpResult unknown = httpRequest("GET", apiUrl("/definitely-not-here"));
    MINI_EXPECT_EQ(unknown.status, 404);
}

MINI_TEST(corpus_site_serves_robots_and_pages) {
    MINI_EXPECT_TRUE(bootServer());
    const std::string corpusUrl =
        "http://127.0.0.1:" + std::to_string(kTestPort + 1) + "/robots.txt";
    const HttpResult robots = httpRequest("GET", corpusUrl);
    MINI_EXPECT_EQ(robots.status, 200);
    MINI_EXPECT_TRUE(robots.body.find("Disallow: /private/") != std::string::npos);
}

MINI_TEST(crawl_rejects_invalid_requests) {
    MINI_EXPECT_TRUE(bootServer());

    const HttpResult notJson = httpRequest("POST", apiUrl("/api/crawl"), "this is not json");
    MINI_EXPECT_EQ(notJson.status, 400);

    const HttpResult noSeeds = httpRequest("POST", apiUrl("/api/crawl"), "{}");
    MINI_EXPECT_EQ(noSeeds.status, 400);

    const HttpResult badSeed =
        httpRequest("POST", apiUrl("/api/crawl"), R"({"seeds":["ftp://example.com/x"]})");
    MINI_EXPECT_EQ(badSeed.status, 400);

    const HttpResult badDepth =
        httpRequest("POST", apiUrl("/api/crawl"), R"({"seeds":["http://127.0.0.1/"],"max_depth":9})");
    MINI_EXPECT_EQ(badDepth.status, 400);
}

MINI_TEST(crawl_indexes_the_corpus_and_respects_robots) {
    MINI_EXPECT_TRUE(bootServer());

    const HttpResult started = httpRequest(
        "POST", apiUrl("/api/crawl"),
        R"({"seeds":[")" + std::string("http://127.0.0.1:") + std::to_string(kTestPort + 1)
            + R"(/"],"max_depth":3,"max_pages":40,"delay_ms":10})");
    MINI_EXPECT_EQ(started.status, 200);
    MINI_EXPECT_TRUE(parseJson(started.body).contains("job_id"));

    MINI_EXPECT_TRUE(waitForCrawl(60));

    const HttpResult statusResult = httpRequest("GET", apiUrl("/api/crawl/status"));
    const Json status = parseJson(statusResult.body);
    MINI_EXPECT_GT(status["succeeded"].get<int>(), 15);
    MINI_EXPECT_EQ(status["failed"].get<int>(), 0);
    MINI_EXPECT_GE(status["skipped"].get<int>(), 1);  // the robots-blocked private page

    bool sawPrivateSkip = false;
    for (const auto& entry : status["recent"]) {
        if (entry["url"].get<std::string>().find("/private/") != std::string::npos) {
            sawPrivateSkip = true;
            MINI_EXPECT_EQ(entry["status"].get<std::string>(), "skipped");
        }
    }
    MINI_EXPECT_TRUE(sawPrivateSkip);
}

MINI_TEST(search_keyword_ranking_with_both_algorithms) {
    MINI_EXPECT_TRUE(bootServer());

    const HttpResult result =
        httpRequest("GET", apiUrl("/api/search?q=c%2B%2B%20programming&limit=5"));
    MINI_EXPECT_EQ(result.status, 200);
    const Json body = parseJson(result.body);
    MINI_EXPECT_GT(body["total"].get<int>(), 3);
    MINI_EXPECT_EQ(body["algorithm"].get<std::string>(), "bm25");
    MINI_EXPECT_GT(body["took_ms"].get<double>(), 0.0);

    double previous = 1e9;
    for (const auto& item : body["items"]) {
        const double score = item["score"].get<double>();
        MINI_EXPECT_LE(score, previous);  // descending order
        previous = score;
        MINI_EXPECT_FALSE(item["title"].get<std::string>().empty());
        MINI_EXPECT_FALSE(item["url"].get<std::string>().empty());
        MINI_EXPECT_TRUE(item.contains("snippet"));
        MINI_EXPECT_TRUE(item["matched_terms"].is_array());
    }

    const HttpResult tfidf = httpRequest(
        "GET", apiUrl("/api/search?q=c%2B%2B%20programming&algorithm=tfidf&limit=5"));
    const Json tfidfBody = parseJson(tfidf.body);
    MINI_EXPECT_EQ(tfidfBody["algorithm"].get<std::string>(), "tfidf");
    MINI_EXPECT_EQ(tfidfBody["total"].get<int>(), body["total"].get<int>());

    // The technical term mapping must make 'C++' searchable via 'cpp'.
    const HttpResult cppOnly = httpRequest("GET", apiUrl("/api/search?q=cpp"));
    MINI_EXPECT_GT(parseJson(cppOnly.body)["total"].get<int>(), 0);
}

MINI_TEST(search_phrase_boolean_and_not) {
    MINI_EXPECT_TRUE(bootServer());

    // Exact phrase: order and adjacency required.
    const HttpResult phrase =
        httpRequest("GET", apiUrl("/api/search?q=%22competitive%20programming%22"));
    const Json phraseBody = parseJson(phrase.body);
    MINI_EXPECT_GT(phraseBody["total"].get<int>(), 0);
    bool sawCompetitiveProgrammingPage = false;
    for (const auto& item : phraseBody["items"]) {
        if (item["title"].get<std::string>().find("Competitive Programming") != std::string::npos) {
            sawCompetitiveProgrammingPage = true;
        }
    }
    MINI_EXPECT_TRUE(sawCompetitiveProgrammingPage);

    // Reversed word order must not match the phrase.
    const HttpResult reversed =
        httpRequest("GET", apiUrl("/api/search?q=%22programming%20competitive%22"));
    MINI_EXPECT_EQ(parseJson(reversed.body)["total"].get<int>(), 0);

    // AND narrows, OR widens.
    const std::string justIndex =
        joinIdsFromSearch("index", "bm25");
    const std::string andResult =
        joinIdsFromSearch("inverted%20AND%20index", "bm25");
    const std::string orResult =
        joinIdsFromSearch("inverted%20OR%20index", "bm25");
    MINI_EXPECT_GT(orResult.size(), andResult.size());
    MINI_EXPECT_GT(andResult.size(), 0);

    // NOT excludes documents containing the operand.
    const HttpResult notResult =
        httpRequest("GET", apiUrl("/api/search?q=index%20NOT%20sql&limit=50"));
    const Json notBody = parseJson(notResult.body);
    MINI_EXPECT_GT(notBody["total"].get<int>(), 0);
    for (const auto& item : notBody["items"]) {
        // The excluded term must not appear in the matched-terms list.
        for (const auto& term : item["matched_terms"]) {
            MINI_EXPECT_NE(term.get<std::string>(), "sql");
        }
    }
}

MINI_TEST(robots_disallowed_page_is_not_searchable) {
    MINI_EXPECT_TRUE(bootServer());
    // 'zanzibar' exists on the private page AND in the hub's note about it,
    // so results may appear - but never the private page itself.
    const HttpResult result = httpRequest("GET", apiUrl("/api/search?q=zanzibar&limit=50"));
    const Json body = parseJson(result.body);
    for (const auto& item : body["items"]) {
        MINI_EXPECT_TRUE(item["url"].get<std::string>().find("/private/") == std::string::npos);
    }
}

MINI_TEST(suggestions_and_stats_reflect_the_index) {
    MINI_EXPECT_TRUE(bootServer());

    const HttpResult suggest = httpRequest("GET", apiUrl("/api/suggest?q=prog"));
    MINI_EXPECT_EQ(suggest.status, 200);
    const Json suggestions = parseJson(suggest.body)["suggestions"];
    MINI_EXPECT_GT(suggestions.size(), 0u);
    MINI_EXPECT_EQ(suggestions[0]["term"].get<std::string>(), "program");

    const HttpResult stats = httpRequest("GET", apiUrl("/api/stats"));
    const Json statsBody = parseJson(stats.body);
    MINI_EXPECT_GT(statsBody["documents"].get<int>(), 15);
    MINI_EXPECT_GT(statsBody["unique_terms"].get<int>(), 500);
    MINI_EXPECT_GT(statsBody["total_postings"].get<int>(), 1000);
    MINI_EXPECT_GT(statsBody["index_size_bytes"].get<int>(), 0);
    MINI_EXPECT_GT(statsBody["pages_crawled"].get<int>(), 15);
    MINI_EXPECT_GT(statsBody["crawls_run"].get<int>(), 0);
    MINI_EXPECT_GT(statsBody["search_count"].get<int>(), 0);
    MINI_EXPECT_FALSE(statsBody["last_indexed_at"].get<std::string>().empty());
}

MINI_TEST(document_endpoint_round_trip) {
    MINI_EXPECT_TRUE(bootServer());

    const HttpResult search = httpRequest("GET", apiUrl("/api/search?q=inverted%20index&limit=1"));
    const Json items = parseJson(search.body)["items"];
    MINI_EXPECT_GT(items.size(), 0u);
    const int id = items[0]["id"].get<int>();

    const HttpResult doc = httpRequest("GET", apiUrl("/api/document/" + std::to_string(id)));
    MINI_EXPECT_EQ(doc.status, 200);
    const Json docBody = parseJson(doc.body);
    MINI_EXPECT_EQ(docBody["id"].get<int>(), id);
    MINI_EXPECT_FALSE(docBody["url"].get<std::string>().empty());
    MINI_EXPECT_FALSE(docBody["title"].get<std::string>().empty());
    MINI_EXPECT_TRUE(docBody["headings"].is_array());
    MINI_EXPECT_GT(docBody["outgoing_links"].get<int>(), 0);

    const HttpResult missing = httpRequest("GET", apiUrl("/api/document/999999"));
    MINI_EXPECT_EQ(missing.status, 404);
}

MINI_TEST(search_validation_errors) {
    MINI_EXPECT_TRUE(bootServer());

    const HttpResult noQuery = httpRequest("GET", apiUrl("/api/search"));
    MINI_EXPECT_EQ(noQuery.status, 400);

    const HttpResult badAlgorithm =
        httpRequest("GET", apiUrl("/api/search?q=x&algorithm=pagerank"));
    MINI_EXPECT_EQ(badAlgorithm.status, 400);

    const HttpResult syntaxError =
        httpRequest("GET", apiUrl("/api/search?q=%22unclosed"));
    MINI_EXPECT_EQ(syntaxError.status, 400);
}

MINI_TEST(persistence_survives_engine_restart) {
    MINI_EXPECT_TRUE(bootServer());
    const HttpResult before = httpRequest("GET", apiUrl("/api/stats"));
    const int documentsBefore = parseJson(before.body)["documents"].get<int>();
    MINI_EXPECT_GT(documentsBefore, 15);

    // A SECOND engine instance over the same files: what a restart is.
    const EngineConfig config = makeTestConfig();
    SearchEngine restarted(config);
    MINI_EXPECT_TRUE(restarted.initialize());
    const minisearch::EngineStats stats = restarted.stats();
    MINI_EXPECT_EQ(stats.documents, static_cast<std::size_t>(documentsBefore));
    MINI_EXPECT_GT(stats.uniqueTerms, 500u);

    // And it answers queries without any re-crawl.
    const minisearch::SearchRequest request{"inverted index",
                                             minisearch::SearchRequest::Algorithm::BM25, 10, 0};
    const minisearch::SearchResponse response = restarted.search(request);
    MINI_EXPECT_TRUE(response.ok);
    MINI_EXPECT_GT(response.total, 0);
    MINI_EXPECT_GT(response.items.size(), 0u);
}

MINI_TEST(crawl_status_reports_configuration) {
    MINI_EXPECT_TRUE(bootServer());
    const HttpResult result = httpRequest("GET", apiUrl("/api/crawl/status"));
    const Json status = parseJson(result.body);
    MINI_EXPECT_TRUE(status["has_job"].get<bool>());
    MINI_EXPECT_FALSE(status["running"].get<bool>());
    MINI_EXPECT_TRUE(status.contains("seeds"));
    MINI_EXPECT_TRUE(status.contains("recent"));
    MINI_EXPECT_GT(status["elapsed_ms"].get<int>(), 0);
}
