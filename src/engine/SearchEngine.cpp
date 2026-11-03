#include "engine/SearchEngine.h"

#include "query/QueryParser.h"
#include "query/QueryProcessor.h"
#include "ranking/BM25.h"
#include "ranking/TFIDF.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <unordered_set>

namespace minisearch {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string trimCopy(std::string text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

void log(const std::string& message) {
    std::fprintf(stderr, "[minisearch] %s\n", message.c_str());
}

} // namespace

const char* toString(SearchRequest::Algorithm algorithm) noexcept {
    return algorithm == SearchRequest::Algorithm::TFIDF ? "tfidf" : "bm25";
}

bool parseAlgorithm(const std::string& name, SearchRequest::Algorithm& out) noexcept {
    if (name == "tfidf") {
        out = SearchRequest::Algorithm::TFIDF;
        return true;
    }
    if (name == "bm25") {
        out = SearchRequest::Algorithm::BM25;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

SearchEngine::SearchEngine(EngineConfig config)
    : config_(std::move(config)),
      storage_(config_.dbPath, config_.indexPath),
      processor_(config_.text),
      weights_(config_.weights) {}

SearchEngine::~SearchEngine() {
    if (crawlThread_.joinable()) crawlThread_.join();
}

bool SearchEngine::initialize() {
    if (!storage_.open()) {
        log("storage error: " + storage_.lastError());
        return false;
    }

    const auto status = storage_.loadIndex(index_);
    if (status == StorageManager::LoadStatus::Loaded) {
        // Incremental reconciliation: pick up documents added by other
        // runs, drop postings for deleted ones.
        const std::vector<Document> documents = storage_.loadAllDocuments();
        std::unordered_set<DocumentId> storedIds;
        storedIds.reserve(documents.size());
        for (const Document& document : documents) storedIds.insert(document.id);

        std::size_t added = 0;
        std::size_t removed = 0;
        {
            std::unique_lock<std::shared_mutex> lock(indexMutex_);
            for (const Document& document : documents) {
                if (index_.hasDocument(document.id)) continue;
                indexAnalyzedLocked(document.id, processor_.analyze(document));
                ++added;
            }
            for (DocumentId id : index_.allDocumentIds()) {
                if (storedIds.count(id) > 0) continue;
                index_.removeDocument(id);
                ++removed;
            }
        }
        log("loaded index file: " + std::to_string(index_.documentCount()) + " documents, "
                + std::to_string(index_.termCount()) + " terms"
                + (added ? " (+" + std::to_string(added) + " new)" : "")
                + (removed ? " (-" + std::to_string(removed) + " stale)" : ""));
        if (added > 0) storage_.saveIndex(index_);
    } else {
        const char* reason = status == StorageManager::LoadStatus::Missing
                                 ? "no index file yet"
                                 : "index file corrupted (rebuilding)";
        std::size_t count = 0;
        {
            std::unique_lock<std::shared_mutex> lock(indexMutex_);
            index_.clear();
            for (const Document& document : storage_.loadAllDocuments()) {
                indexAnalyzedLocked(document.id, processor_.analyze(document));
                ++count;
            }
        }
        log(std::string(reason) + ": rebuilt index for " + std::to_string(count)
                + " documents");
        if (count > 0) storage_.saveIndex(index_);
    }

    {
        std::shared_lock<std::shared_mutex> lock(indexMutex_);
        recomputeWeightedLengthSum();
    }
    return true;
}

void SearchEngine::indexAnalyzedLocked(DocumentId id, const AnalyzedDocument& analyzed) {
    // indexMutex_ must be held exclusively by the caller.
    if (const DocMeta* previous = index_.findDocument(id)) {
        weightedLengthSum_ -= weightedDocumentLength(*previous, weights_);
    }
    index_.indexDocument(id, analyzed);
    if (const DocMeta* meta = index_.findDocument(id)) {
        weightedLengthSum_ += weightedDocumentLength(*meta, weights_);
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        lastIndexedAtMs_ = epochMs();
    }
}

void SearchEngine::recomputeWeightedLengthSum() {
    weightedLengthSum_ = 0.0;
    for (const auto& entry : index_.documentMap()) {
        weightedLengthSum_ += weightedDocumentLength(entry.second, weights_);
    }
}

bool SearchEngine::reindex() {
    const auto documents = storage_.loadAllDocuments();
    {
        std::unique_lock<std::shared_mutex> lock(indexMutex_);
        index_.clear();
        weightedLengthSum_ = 0.0;
        for (const Document& document : documents) {
            indexAnalyzedLocked(document.id, processor_.analyze(document));
        }
    }
    const bool saved = storage_.saveIndex(index_);
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        lastIndexedAtMs_ = epochMs();
    }
    log("reindexed " + std::to_string(documents.size()) + " documents"
            + (saved ? "" : " (warning: index save failed: " + storage_.lastError() + ")"));
    return saved;
}

bool SearchEngine::wipe() {
    if (!storage_.wipeAll()) {
        log("wipe failed: " + storage_.lastError());
        return false;
    }
    {
        std::unique_lock<std::shared_mutex> lock(indexMutex_);
        index_.clear();
        weightedLengthSum_ = 0.0;
    }
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        lastIndexedAtMs_ = 0;
    }
    log("wiped all documents and the index");
    return true;
}

// ---------------------------------------------------------------------------
// Crawl sink
// ---------------------------------------------------------------------------

void SearchEngine::onPage(const PageOutcome& outcome) {
    if (outcome.skipped) return;  // never fetched; crawl log already shows it

    if (!outcome.ok) {
        storage_.addMeta("pages_failed", 1);
        return;
    }

    const Document& document = outcome.document;
    const auto upserted = storage_.upsertDocument(document);
    if (upserted.id < 0) {
        log("storage error while indexing " + document.url + ": " + storage_.lastError());
        storage_.addMeta("pages_failed", 1);
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(indexMutex_);
        indexAnalyzedLocked(upserted.id, processor_.analyze(document));
        storage_.saveIndex(index_);  // incremental persistence per page
    }
    storage_.addMeta("pages_crawled", 1);
}

// ---------------------------------------------------------------------------
// Crawl jobs
// ---------------------------------------------------------------------------

int SearchEngine::startCrawl(const CrawlConfig& crawlConfig, std::string* error) {
    if (isCrawlRunning()) {
        if (error != nullptr) *error = "a crawl is already running";
        return -1;
    }
    if (crawlThread_.joinable()) crawlThread_.join();  // reaped finished thread

    const int jobId = nextJobId_++;
    storage_.addMeta("crawls_run", 1);

    auto crawler = std::make_shared<WebCrawler>(crawlConfig);
    {
        std::lock_guard<std::mutex> lock(crawlMutex_);
        liveCrawler_ = crawler;
        lastCrawl_ = CrawlStatusSnapshot{};
        lastCrawl_.hasJob = true;
        lastCrawl_.jobId = jobId;
        lastCrawl_.running = true;
        lastCrawl_.seeds = crawlConfig.seedUrls;
        lastCrawl_.maxPages = crawlConfig.maxPages;
        lastCrawl_.maxDepth = crawlConfig.maxDepth;
        lastCrawl_.delayMs = crawlConfig.delayMs;
        lastCrawl_.startedAtMs = epochMs();
    }

    crawlThread_ = std::thread([this, crawler, jobId] {
        crawler->run(*this);
        const CrawlProgressSnapshot finalProgress = crawler->progress();
        {
            std::lock_guard<std::mutex> lock(crawlMutex_);
            liveCrawler_ = nullptr;
            lastCrawl_.running = false;
            lastCrawl_.queued = finalProgress.queued;
            lastCrawl_.fetched = finalProgress.fetched;
            lastCrawl_.succeeded = finalProgress.succeeded;
            lastCrawl_.failed = finalProgress.failed;
            lastCrawl_.skipped = finalProgress.skipped;
            lastCrawl_.finishedAtMs = finalProgress.finishedAtMs;
            lastCrawl_.recent = std::move(finalProgress.recent);
        }
    });
    return jobId;
}

CrawlStatusSnapshot SearchEngine::crawlStatus() const {
    std::lock_guard<std::mutex> lock(crawlMutex_);
    CrawlStatusSnapshot snapshot = lastCrawl_;
    if (liveCrawler_ != nullptr) {
        // The crawler object is alive => the job is running, regardless of
        // the crawler thread's own start-of-run initialization race.
        const CrawlProgressSnapshot live = liveCrawler_->progress();
        snapshot.running = true;
        snapshot.queued = live.queued;
        snapshot.fetched = live.fetched;
        snapshot.succeeded = live.succeeded;
        snapshot.failed = live.failed;
        snapshot.skipped = live.skipped;
        snapshot.finishedAtMs = live.finishedAtMs;
        snapshot.recent = live.recent;
    }
    return snapshot;
}

bool SearchEngine::isCrawlRunning() const {
    std::lock_guard<std::mutex> lock(crawlMutex_);
    return liveCrawler_ != nullptr;
}

void SearchEngine::waitForCrawl() const {
    while (isCrawlRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

std::unique_ptr<Ranker> SearchEngine::makeRanker(SearchRequest::Algorithm algorithm) const {
    if (algorithm == SearchRequest::Algorithm::TFIDF) {
        return std::make_unique<TFIDFRanker>(weights_);
    }
    return std::make_unique<BM25Ranker>(config_.bm25K1, config_.bm25B, weights_);
}

SearchResponse SearchEngine::search(const SearchRequest& request) {
    const auto started = Clock::now();
    SearchResponse response;
    response.query = request.query;
    response.algorithm = toString(request.algorithm);
    response.limit = request.limit;
    response.offset = request.offset;

    const std::string query = trimCopy(request.query);
    if (query.empty()) {
        response.error = "empty query";
        return response;
    }
    if (query.size() > 512) {
        response.error = "query too long (max 512 characters)";
        return response;
    }

    const query::QueryParser parser(processor_);
    const auto parsed = parser.parse(query);
    if (!parsed.ok) {
        response.error = "query syntax error: " + parsed.error;
        return response;
    }

    QueryEvaluation evaluation;
    {
        std::shared_lock<std::shared_mutex> lock(indexMutex_);
        const QueryProcessor evaluator(processor_);
        evaluation = evaluator.evaluate(*parsed.root, index_);
    }
    if (!evaluation.ok) {
        response.error = "query error: " + evaluation.error;
        return response;
    }

    // Stop words are dropped from scoring (IDF handles them anyway) unless
    // they are the entire query.
    std::vector<std::string> scoringTerms;
    scoringTerms.reserve(evaluation.scoringTerms.size());
    for (const std::string& term : evaluation.scoringTerms) {
        if (!processor_.isStopword(term)) scoringTerms.push_back(term);
    }
    if (scoringTerms.empty()) scoringTerms = evaluation.scoringTerms;

    struct ScoredDocument {
        double score = 0.0;
        DocumentId id = -1;
        std::vector<std::string> matched;
    };
    std::vector<ScoredDocument> scored;
    {
        std::shared_lock<std::shared_mutex> lock(indexMutex_);
        const auto ranker = makeRanker(request.algorithm);
        CollectionStats collection;
        collection.totalDocs = index_.documentCount();
        collection.avgWeightedDocLength =
            collection.totalDocs > 0
                ? weightedLengthSum_ / static_cast<double>(collection.totalDocs)
                : 0.0;

        scored.reserve(evaluation.candidates.size());
        for (DocumentId id : evaluation.candidates) {
            ScoredDocument entry;
            entry.id = id;
            const DocMeta* meta = index_.findDocument(id);
            if (meta == nullptr) continue;
            for (const std::string& term : scoringTerms) {
                const PostingList* list = index_.findTerm(term);
                if (list == nullptr) continue;
                const Posting* posting = index_.findPosting(term, id);
                if (posting == nullptr) continue;
                entry.score += ranker->scoreTerm(term, *list, *posting, *meta, collection);
                entry.matched.push_back(term);
            }
            scored.push_back(std::move(entry));
        }
    }

    std::stable_sort(scored.begin(), scored.end(), [](const ScoredDocument& a,
                                                      const ScoredDocument& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.id < b.id;
    });

    response.total = static_cast<int>(scored.size());
    const std::size_t begin = static_cast<std::size_t>(std::max(0, request.offset));
    const std::size_t end =
        std::min(scored.size(), begin + static_cast<std::size_t>(std::max(0, request.limit)));
    for (std::size_t i = begin; i < end; ++i) {
        SearchItem item;
        item.id = scored[i].id;
        item.score = scored[i].score;
        item.matchedTerms = scored[i].matched;

        const auto stored = storage_.loadDocument(scored[i].id);
        if (stored.has_value()) {
            item.title = stored->title.empty() ? stored->url : stored->title;
            item.url = stored->url;
            item.snippet = makeSnippet(*stored, scoringTerms);
        } else {
            item.title = "(document metadata unavailable)";
            item.url = "";
        }
        response.items.push_back(std::move(item));
    }

    const auto finished = Clock::now();
    response.tookMs = std::chrono::duration<double, std::milli>(finished - started).count();
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        ++searchCount_;
        totalSearchMs_ += response.tookMs;
    }
    response.ok = true;
    return response;
}

std::optional<Document> SearchEngine::document(DocumentId id) const {
    return storage_.loadDocument(id);
}

EngineStats SearchEngine::stats() const {
    EngineStats result;
    {
        std::shared_lock<std::shared_mutex> lock(indexMutex_);
        result.documents = index_.documentCount();
        result.uniqueTerms = index_.termCount();
        result.totalPostings = index_.totalPostings();
        result.averageDocumentTokens = index_.averageTokenLength();
        result.topTerms = index_.termsWithPrefix("", 12);
    }
    result.indexSizeBytes = storage_.indexFileSize();
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        result.searchCount = searchCount_;
        result.averageSearchLatencyMs =
            searchCount_ > 0 ? totalSearchMs_ / static_cast<double>(searchCount_) : 0.0;
        result.lastIndexedAtMs = lastIndexedAtMs_;
    }
    result.pagesCrawled = storage_.getMeta("pages_crawled");
    result.pagesFailed = storage_.getMeta("pages_failed");
    result.crawlsRun = storage_.getMeta("crawls_run");
    return result;
}

std::vector<std::pair<std::string, std::uint32_t>> SearchEngine::suggest(
    const std::string& prefix, std::size_t limit) const {
    std::shared_lock<std::shared_mutex> lock(indexMutex_);
    return index_.termsWithPrefix(prefix, limit);
}

// ---------------------------------------------------------------------------
// Snippets
// ---------------------------------------------------------------------------

std::string SearchEngine::makeSnippet(const Document& document,
                                      const std::vector<std::string>& scoringTerms) const {
    if (document.content.empty()) {
        return document.description.empty() ? document.url : document.description;
    }

    // Split into raw whitespace-separated words.
    std::vector<std::string> words;
    {
        std::string current;
        std::istringstream stream(document.content);
        while (stream >> current) words.push_back(current);
    }
    if (words.empty()) return document.description;

    // Stemmed query-term set (first canonical token of each raw word).
    std::unordered_set<std::string> stems;
    for (const std::string& term : scoringTerms) stems.insert(term);

    std::size_t hitIndex = words.size();
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::vector<std::string> tokens = processor_.tokenizeQuery(words[i]);
        if (!tokens.empty() && stems.count(tokens.front()) > 0) {
            hitIndex = i;
            break;
        }
    }

    const std::size_t windowBegin = hitIndex > 12 ? hitIndex - 12 : 0;
    const std::size_t windowEnd = std::min(words.size(), hitIndex + 18);

    std::string snippet;
    if (windowBegin > 0) snippet += "... ";
    for (std::size_t i = windowBegin; i < windowEnd; ++i) {
        if (!snippet.empty() && snippet.back() != ' ') snippet += ' ';
        snippet += words[i];
    }
    if (windowEnd < words.size()) snippet += " ...";

    if (snippet.size() > 280) snippet = snippet.substr(0, 277) + "...";
    return snippet;
}

} // namespace minisearch
