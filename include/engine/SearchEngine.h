#pragma once

// MiniSearch - the engine facade.
//
// Ties together storage, index, text processing, ranking, query
// processing and the crawler. This is the ONE entry point used by both
// front-ends (CLI and HTTP API), which is why search results can never
// diverge between them.
//
// Concurrency: searches take a shared lock over the in-memory index;
// crawl-driven indexing takes the exclusive lock briefly per page.
// SQLite access is serialized inside StorageManager. One crawl job may
// run at a time (background thread); its progress is exposed live.

#include "common/Config.h"
#include "common/Document.h"
#include "crawler/WebCrawler.h"
#include "index/InvertedIndex.h"
#include "processor/TextProcessor.h"
#include "ranking/Ranker.h"
#include "storage/StorageManager.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace minisearch {

struct SearchRequest {
    std::string query;
    enum class Algorithm { TFIDF, BM25 };
    Algorithm algorithm = Algorithm::BM25;
    int limit = 10;
    int offset = 0;
};

struct SearchItem {
    DocumentId id = -1;
    std::string title;
    std::string url;
    std::string snippet;
    double score = 0.0;
    std::vector<std::string> matchedTerms;
};

struct SearchResponse {
    bool ok = false;
    std::string error;          // set when ok == false
    std::string query;
    std::string algorithm;      // "tfidf" | "bm25"
    int total = 0;              // number of matching documents
    int offset = 0;
    int limit = 10;
    double tookMs = 0.0;
    std::vector<SearchItem> items;
};

struct EngineStats {
    std::size_t documents = 0;
    std::size_t uniqueTerms = 0;
    std::size_t totalPostings = 0;
    double averageDocumentTokens = 0.0;
    std::uintmax_t indexSizeBytes = 0;
    std::int64_t lastIndexedAtMs = 0;       // epoch ms; 0 = never indexed
    double averageSearchLatencyMs = 0.0;
    std::uint64_t searchCount = 0;
    std::int64_t pagesCrawled = 0;          // lifetime successful fetches
    std::int64_t pagesFailed = 0;           // lifetime failed fetches
    std::int64_t crawlsRun = 0;
    std::vector<std::pair<std::string, std::uint32_t>> topTerms;  // term, df
};

struct CrawlStatusSnapshot {
    bool hasJob = false;
    int jobId = 0;
    bool running = false;
    std::vector<std::string> seeds;
    int maxPages = 0;
    int maxDepth = 0;
    int delayMs = 0;
    int queued = 0;
    int fetched = 0;
    int succeeded = 0;
    int failed = 0;
    int skipped = 0;
    std::int64_t startedAtMs = 0;
    std::int64_t finishedAtMs = 0;
    std::vector<CrawlProgressSnapshot::Recent> recent;
};

class SearchEngine : private ICrawlSink {
public:
    explicit SearchEngine(EngineConfig config);
    ~SearchEngine();

    SearchEngine(const SearchEngine&) = delete;
    SearchEngine& operator=(const SearchEngine&) = delete;

    // Opens storage; loads the index file or rebuilds it from SQLite;
    // reconciles incremental differences. Returns false on storage errors.
    bool initialize();

    const TextProcessor& processor() const noexcept { return processor_; }
    const EngineConfig& config() const noexcept { return config_; }

    SearchResponse search(const SearchRequest& request);
    std::optional<Document> document(DocumentId id) const;
    EngineStats stats() const;
    std::vector<std::pair<std::string, std::uint32_t>> suggest(const std::string& prefix,
                                                               std::size_t limit) const;

    // Starts a background crawl job; returns its id, or -1 with *error set
    // (e.g. "a crawl is already running").
    int startCrawl(const CrawlConfig& crawlConfig, std::string* error);
    CrawlStatusSnapshot crawlStatus() const;
    bool isCrawlRunning() const;
    void waitForCrawl() const;  // CLI helper (no timeout)

    // Full rebuild of the in-memory index from SQLite documents.
    bool reindex();

    // Deletes all documents, counters and the index file.
    bool wipe();

    // Benchmark support: every stored document (the brute-force baseline
    // needs the raw corpus; the index alone cannot provide it).
    std::vector<Document> allDocumentsForBenchmark() const { return storage_.loadAllDocuments(); }

private:
    void onPage(const PageOutcome& outcome) override;

    void indexAnalyzedLocked(DocumentId id, const AnalyzedDocument& analyzed);
    void recomputeWeightedLengthSum();
    std::string makeSnippet(const Document& document,
                            const std::vector<std::string>& scoringTerms) const;
    std::unique_ptr<Ranker> makeRanker(SearchRequest::Algorithm algorithm) const;

    EngineConfig config_;
    StorageManager storage_;
    InvertedIndex index_;
    TextProcessor processor_;
    FieldWeights weights_;

    mutable std::shared_mutex indexMutex_;     // guards index_ + weightedLengthSum_
    double weightedLengthSum_ = 0.0;

    mutable std::mutex statsMutex_;
    std::uint64_t searchCount_ = 0;
    double totalSearchMs_ = 0.0;
    std::int64_t lastIndexedAtMs_ = 0;

    std::thread crawlThread_;
    int nextJobId_ = 1;
    mutable std::mutex crawlMutex_;
    std::shared_ptr<WebCrawler> liveCrawler_;
    CrawlStatusSnapshot lastCrawl_;
};

const char* toString(SearchRequest::Algorithm algorithm) noexcept;
bool parseAlgorithm(const std::string& name, SearchRequest::Algorithm& out) noexcept;

} // namespace minisearch
