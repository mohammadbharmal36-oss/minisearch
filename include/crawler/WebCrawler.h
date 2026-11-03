#pragma once

// MiniSearch - the web crawler (spec module 5).
//
// BFS frontier with depth tracking, a visited set of normalized URLs,
// robots.txt compliance (cached per host), per-domain politeness delay,
// a configurable page/depth budget, allowed-domain filtering and a
// login-ish URL blacklist. Cross-domain parallelism via a fixed worker
// pool; requests to the SAME host are serialized by the dispatcher's
// per-host next-allowed timestamps (never concurrent, never faster than
// the configured or robots.txt-requested delay).
//
// The dispatcher thread owns frontier/visited/host state; workers only
// download (each with its own libcurl handle), parse HTML and resolve
// links. Results flow back through a queue; the dispatcher applies
// politeness, budgets, dedup and calls the sink (the engine), which
// indexes documents single-threaded.

#include "common/Document.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace minisearch {

struct CrawlConfig {
    std::vector<std::string> seedUrls;
    int maxPages = 50;
    int maxDepth = 2;
    long timeoutSeconds = 10;
    int delayMs = 500;             // between requests to the same host
    std::vector<std::string> allowedDomains;  // empty = all hosts allowed
    std::string userAgent = "MiniSearchBot/1.0 (+MiniSearch educational search engine project)";
    int workerThreads = 4;
    std::size_t maxBytes = 2 * 1024 * 1024;   // per page
};

// One processed URL, handed to the engine (or CLI) via ICrawlSink.
struct PageOutcome {
    std::string url;               // final URL (after redirects)
    int httpStatus = 0;
    bool ok = false;               // fetched, parsed, ready to index
    bool skipped = false;          // robots/duplicate/non-html/login-ish
    std::string note;              // human-readable outcome detail
    Document document;             // filled when ok
    std::vector<std::string> discoveredLinks;  // normalized absolute URLs
};

class ICrawlSink {
public:
    virtual ~ICrawlSink() = default;
    virtual void onPage(const PageOutcome& outcome) = 0;
};

// Mutex-guarded live progress, copied out for reporting.
struct CrawlProgressSnapshot {
    bool running = false;
    bool finished = false;
    int queued = 0;
    int fetched = 0;
    int succeeded = 0;
    int failed = 0;
    int skipped = 0;
    std::int64_t startedAtMs = 0;
    std::int64_t finishedAtMs = 0;
    struct Recent {
        std::string url;
        std::string kind;   // "ok" | "failed" | "skipped"
        int httpStatus = 0;
        std::string note;
    };
    std::vector<Recent> recent;    // newest last, capped
};

class RobotsTxt;
class HttpDownloader;

class WebCrawler {
public:
    explicit WebCrawler(CrawlConfig config);
    ~WebCrawler();

    WebCrawler(const WebCrawler&) = delete;
    WebCrawler& operator=(const WebCrawler&) = delete;

    // Blocking; safe to run once per instance.
    void run(ICrawlSink& sink);

    CrawlProgressSnapshot progress() const;

private:
    struct Task {
        std::string url;
        int depth = 0;
        bool fetchRobots = false;
    };
    struct WorkerResult {
        std::string requestedUrl;
        std::string url;               // final URL when ok
        int depth = 0;
        bool ok = false;
        bool skipped = false;
        int httpStatus = 0;
        std::string note;
        Document document;
        std::vector<std::string> links;
    };

    void recordRecent(const std::string& url, const char* kind, int httpStatus,
                      const std::string& note);
    void handleResult(const WorkerResult& result, ICrawlSink& sink);
    bool tryDispatch();
    void drainResults(ICrawlSink& sink);
    void workerLoop();
    WorkerResult processTask(HttpDownloader& downloader, const Task& task);
    bool hostAllowed(const std::string& host) const;
    int effectiveDelayMs(const std::string& host);
    const RobotsTxt& robotsFor(const std::string& host);

    CrawlConfig config_;

    // Dispatcher-owned state (single thread).
    std::deque<std::pair<std::string, int>> frontier_;
    std::unordered_set<std::string> visited_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> hostNextAllowed_;

    // Shared with workers.
    std::mutex taskMutex_;
    std::condition_variable taskCv_;
    std::deque<Task> taskQueue_;
    bool workersStopping_ = false;

    std::mutex resultMutex_;
    std::condition_variable resultCv_;
    std::deque<WorkerResult> resultQueue_;
    int inFlight_ = 0;  // guarded by resultMutex_

    std::mutex robotsMutex_;
    std::unordered_map<std::string, RobotsTxt> robotsCache_;
    std::unordered_set<std::string> hostsPending_;  // robots fetch in flight

    mutable std::mutex stateMutex_;
    CrawlProgressSnapshot state_;

    std::vector<std::thread> workers_;
};

} // namespace minisearch
