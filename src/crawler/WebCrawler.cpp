#include "crawler/WebCrawler.h"

#include "crawler/HttpDownloader.h"
#include "crawler/RobotsTxt.h"
#include "crawler/Url.h"
#include "parser/HTMLParser.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace minisearch {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string toLower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// URLs that look like authentication or admin surfaces; the crawler
// deliberately stays away from login-protected content (spec section 5).
bool looksLikeLoginOrAdmin(const std::string& url) {
    const std::string lowered = toLower(url);
    static const char* kPatterns[] = {"login",  "logout",   "signin", "signup",
                                      "register", "admin",   "account", "password"};
    for (const char* pattern : kPatterns) {
        if (lowered.find(pattern) != std::string::npos) return true;
    }
    return false;
}

std::string schemeAndHost(const std::string& url) {
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return url;
    const std::size_t hostStart = schemeEnd + 3;
    std::size_t hostEnd = url.find_first_of("/?#", hostStart);
    if (hostEnd == std::string::npos) hostEnd = url.size();
    return url.substr(0, hostEnd);
}

std::string pathAndQueryOf(const std::string& url) {
    const std::size_t schemeEnd = url.find("://");
    const std::size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
    std::size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) {
        pathStart = url.find('?', hostStart);
        if (pathStart == std::string::npos) return "/";
    }
    return url.substr(pathStart);
}

bool isHtmlContent(const std::string& contentType) {
    if (contentType.empty()) return true;  // servers often omit it; give benefit of doubt
    return toLower(contentType).find("html") != std::string::npos;
}

} // namespace

WebCrawler::WebCrawler(CrawlConfig config) : config_(std::move(config)) {
    if (config_.workerThreads < 1) config_.workerThreads = 1;
    if (config_.workerThreads > 8) config_.workerThreads = 8;
    if (config_.maxPages < 1) config_.maxPages = 1;
    if (config_.maxDepth < 0) config_.maxDepth = 0;
    if (config_.delayMs < 0) config_.delayMs = 0;
}

WebCrawler::~WebCrawler() {
    // Defensive: run() normally joins; make destruction safe regardless.
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        workersStopping_ = true;
    }
    taskCv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

CrawlProgressSnapshot WebCrawler::progress() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return state_;
}

void WebCrawler::recordRecent(const std::string& url, const char* kind, int httpStatus,
                              const std::string& note) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    CrawlProgressSnapshot::Recent entry;
    entry.url = url;
    entry.kind = kind;
    entry.httpStatus = httpStatus;
    entry.note = note;
    state_.recent.push_back(std::move(entry));
    if (state_.recent.size() > 50) {
        state_.recent.erase(state_.recent.begin());
    }
}

bool WebCrawler::hostAllowed(const std::string& host) const {
    if (config_.allowedDomains.empty()) return true;
    for (const std::string& domain : config_.allowedDomains) {
        const std::string lowered = toLower(domain);
        if (host == lowered || (host.size() > lowered.size()
                                && host.compare(host.size() - lowered.size() - 1,
                                                lowered.size() + 1, "." + lowered) == 0)) {
            return true;
        }
    }
    return false;
}

int WebCrawler::effectiveDelayMs(const std::string& host) {
    int delay = config_.delayMs;
    std::lock_guard<std::mutex> lock(robotsMutex_);
    const auto it = robotsCache_.find(host);
    if (it != robotsCache_.end() && it->second.crawlDelayMs().has_value()) {
        delay = std::max(delay, *it->second.crawlDelayMs());
    }
    return delay;
}

const RobotsTxt& WebCrawler::robotsFor(const std::string& host) {
    // Called by workers after the cache entry exists (dispatcher guarantees
    // robots are fetched with the first task per host before any second
    // dispatch). Returns the cached rules.
    std::lock_guard<std::mutex> lock(robotsMutex_);
    const auto it = robotsCache_.find(host);
    static const RobotsTxt kAllowAll = RobotsTxt::allowAll();
    return it != robotsCache_.end() ? it->second : kAllowAll;
}

// ---------------------------------------------------------------------------
// Worker side
// ---------------------------------------------------------------------------

WebCrawler::WorkerResult WebCrawler::processTask(HttpDownloader& downloader, const Task& task) {
    WorkerResult result;
    result.requestedUrl = task.url;
    result.depth = task.depth;

    const std::string host = url::hostOf(task.url);

    // 1) robots.txt for a new host (fetched by whichever task first sees it).
    if (task.fetchRobots) {
        const std::string robotsUrl = schemeAndHost(task.url) + "/robots.txt";
        const auto robotsFetch = downloader.fetch(robotsUrl);
        RobotsTxt rules = RobotsTxt::allowAll();
        if (robotsFetch.ok && robotsFetch.status >= 200 && robotsFetch.status < 300) {
            rules = RobotsTxt::parse(robotsFetch.body, config_.userAgent);
        }
        // Unreachable (4xx/timeouts) means "no restrictions" per RFC 9309.
        {
            std::lock_guard<std::mutex> lock(robotsMutex_);
            robotsCache_[host] = std::move(rules);
            hostsPending_.erase(host);
        }
    }

    // 2) robots compliance check.
    if (!robotsFor(host).allows(pathAndQueryOf(task.url))) {
        result.skipped = true;
        result.note = "disallowed by robots.txt";
        return result;
    }

    // 3) Download the page.
    const auto fetch = downloader.fetch(task.url);
    if (!fetch.ok) {
        result.note = fetch.error;
        return result;
    }
    result.httpStatus = fetch.status;
    if (fetch.status < 200 || fetch.status >= 300) {
        result.note = "HTTP " + std::to_string(fetch.status);
        return result;
    }
    if (!isHtmlContent(fetch.contentType)) {
        result.skipped = true;
        result.note = "non-HTML content: " + fetch.contentType;
        return result;
    }

    // 4) Parse; the parser resolves links and records the final URL.
    const auto parsed = HTMLParser::parse(fetch.body, fetch.finalUrl.empty() ? task.url
                                                                              : fetch.finalUrl);
    if (!parsed.ok) {
        result.note = "parse error: " + parsed.error;
        return result;
    }
    result.ok = true;
    result.document = parsed.document;
    result.document.http_status = fetch.status;
    result.document.crawled_at = std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    result.links = parsed.document.links;
    result.url = result.document.url;
    return result;
}

void WebCrawler::workerLoop() {
    // One libcurl handle per worker for the crawler's whole lifetime.
    HttpDownloader downloader(config_.userAgent, config_.timeoutSeconds, config_.maxBytes);
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(taskMutex_);
            taskCv_.wait(lock, [this] { return workersStopping_ || !taskQueue_.empty(); });
            if (workersStopping_ && taskQueue_.empty()) return;
            task = taskQueue_.front();
            taskQueue_.pop_front();
        }

        WorkerResult result = processTask(downloader, task);

        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            resultQueue_.push_back(std::move(result));
        }
        resultCv_.notify_one();
    }
}

// ---------------------------------------------------------------------------
// Dispatcher side
// ---------------------------------------------------------------------------

void WebCrawler::handleResult(const WorkerResult& result, ICrawlSink& sink) {
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        --inFlight_;
    }

    PageOutcome outcome;
    outcome.url = result.url.empty() ? result.requestedUrl : result.url;
    outcome.httpStatus = result.httpStatus;
    outcome.note = result.note;

    if (result.skipped) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            ++state_.skipped;
        }
        recordRecent(outcome.url, "skipped", result.httpStatus, result.note);
        outcome.skipped = true;
        sink.onPage(outcome);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        ++state_.fetched;
    }

    if (!result.ok) {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            ++state_.failed;
        }
        recordRecent(outcome.url, "failed", result.httpStatus,
                     result.note.empty() ? "request failed" : result.note);
        sink.onPage(outcome);
        return;
    }

    // Redirect de-duplication: if the final URL differs from the requested
    // one and we already visited it, this is a duplicate.
    const std::string finalNormalized = outcome.url;
    if (finalNormalized != result.requestedUrl) {
        if (visited_.count(finalNormalized) > 0) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                ++state_.skipped;
            }
            recordRecent(finalNormalized, "skipped", result.httpStatus, "duplicate redirect target");
            outcome.skipped = true;
            outcome.note = "duplicate redirect target";
            sink.onPage(outcome);
            return;
        }
        visited_.insert(finalNormalized);
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        ++state_.succeeded;
    }
    recordRecent(finalNormalized, "ok", result.httpStatus, "");
    outcome.ok = true;
    outcome.document = result.document;
    outcome.discoveredLinks = result.links;
    sink.onPage(outcome);

    // Enqueue newly discovered links (BFS, depth-limited, deduplicated).
    const int childDepth = result.depth + 1;
    if (childDepth > config_.maxDepth) return;
    for (const std::string& link : result.links) {
        if (visited_.count(link) > 0) continue;
        const std::string host = url::hostOf(link);
        if (!hostAllowed(host)) continue;
        if (looksLikeLoginOrAdmin(link)) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                ++state_.skipped;
            }
            recordRecent(link, "skipped", 0, "login/admin-like URL avoided");
            PageOutcome skippedOutcome;
            skippedOutcome.url = link;
            skippedOutcome.skipped = true;
            skippedOutcome.note = "login/admin-like URL avoided";
            sink.onPage(skippedOutcome);
            continue;
        }
        visited_.insert(link);
        frontier_.emplace_back(link, childDepth);
        std::lock_guard<std::mutex> lock(stateMutex_);
        ++state_.queued;
    }
}

bool WebCrawler::tryDispatch() {
    if (frontier_.empty()) return false;

    int fetched = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        fetched = state_.fetched;
    }
    int inFlight = 0;
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        inFlight = inFlight_;
    }
    if (fetched + inFlight >= config_.maxPages) return false;

    const auto now = Clock::now();
    for (auto it = frontier_.begin(); it != frontier_.end(); ++it) {
        const std::string host = url::hostOf(it->first);

        // Robots must be fetched (or cached) before any second dispatch to
        // a host; the first-ever task carries the robots request.
        bool needRobots = false;
        {
            std::lock_guard<std::mutex> lock(robotsMutex_);
            if (robotsCache_.count(host) == 0) {
                if (hostsPending_.count(host) > 0) continue;  // robots fetch in flight
                hostsPending_.insert(host);
                needRobots = true;
            }
        }

        const auto next = hostNextAllowed_.find(host);
        if (next != hostNextAllowed_.end() && next->second > now) {
            // Not yet polite to hit this host again; later frontier entries
            // (other hosts) may still be dispatched now.
            if (needRobots) {
                std::lock_guard<std::mutex> lock(robotsMutex_);
                hostsPending_.erase(host);  // restore state; will retry later
            }
            continue;
        }

        const std::string dispatchUrl = it->first;
        const int depth = it->second;
        frontier_.erase(it);

        hostNextAllowed_[host] = now + std::chrono::milliseconds(effectiveDelayMs(host));
        {
            std::lock_guard<std::mutex> lock(taskMutex_);
            taskQueue_.push_back(Task{dispatchUrl, depth, needRobots});
        }
        taskCv_.notify_one();
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            ++inFlight_;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            --state_.queued;
        }
        return true;
    }
    return false;
}

void WebCrawler::drainResults(ICrawlSink& sink) {
    while (true) {
        WorkerResult result;
        {
            std::unique_lock<std::mutex> lock(resultMutex_);
            if (resultQueue_.empty()) return;
            result = std::move(resultQueue_.front());
            resultQueue_.pop_front();
        }
        handleResult(result, sink);
    }
}

void WebCrawler::run(ICrawlSink& sink) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_ = CrawlProgressSnapshot{};
        state_.running = true;
        state_.startedAtMs = epochMs();
    }

    // Seed validation and frontier setup. Every decision is reported to
    // the sink so the engine/API can present a complete crawl log.
    for (const std::string& rawSeed : config_.seedUrls) {
        const auto normalized = url::normalize(rawSeed);
        if (!normalized.has_value()) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                ++state_.skipped;
            }
            recordRecent(rawSeed, "skipped", 0, "invalid seed URL");
            PageOutcome outcome;
            outcome.url = rawSeed;
            outcome.skipped = true;
            outcome.note = "invalid seed URL";
            sink.onPage(outcome);
            continue;
        }
        const std::string host = url::hostOf(*normalized);
        if (!hostAllowed(host)) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                ++state_.skipped;
            }
            recordRecent(*normalized, "skipped", 0, "host not in allowed domains");
            PageOutcome outcome;
            outcome.url = *normalized;
            outcome.skipped = true;
            outcome.note = "host not in allowed domains";
            sink.onPage(outcome);
            continue;
        }
        if (looksLikeLoginOrAdmin(*normalized)) {
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                ++state_.skipped;
            }
            recordRecent(*normalized, "skipped", 0, "login/admin-like seed avoided");
            PageOutcome outcome;
            outcome.url = *normalized;
            outcome.skipped = true;
            outcome.note = "login/admin-like seed avoided";
            sink.onPage(outcome);
            continue;
        }
        if (visited_.insert(*normalized).second) {
            frontier_.emplace_back(*normalized, 0);
            std::lock_guard<std::mutex> lock(stateMutex_);
            ++state_.queued;
        }
    }

    const int workerCount = config_.workerThreads;
    workers_.reserve(static_cast<std::size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }

    // Dispatcher loop.
    while (true) {
        drainResults(sink);

        bool queueIdle = false;
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            queueIdle = resultQueue_.empty() && inFlight_ == 0;
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (state_.fetched >= config_.maxPages && queueIdle) break;
        }
        if (frontier_.empty() && queueIdle) break;

        if (!tryDispatch()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // Shut the worker pool down and drain whatever remains.
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        workersStopping_ = true;
    }
    taskCv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();
    drainResults(sink);

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        state_.running = false;
        state_.finished = true;
        state_.finishedAtMs = epochMs();
    }
}

} // namespace minisearch
