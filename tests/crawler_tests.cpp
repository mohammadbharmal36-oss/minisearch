// Unit tests for robots.txt parsing (crawler/RobotsTxt) and offline
// crawler behavior (crawler/WebCrawler). Full HTTP crawling is covered by
// the integration suite against a locally served corpus.

#include "crawler/RobotsTxt.h"
#include "crawler/Url.h"
#include "crawler/WebCrawler.h"
#include "test_framework.h"

#include <string>

namespace {

using minisearch::CrawlConfig;
using minisearch::ICrawlSink;
using minisearch::PageOutcome;
using minisearch::RobotsTxt;
using minisearch::WebCrawler;

const std::string kUserAgent = "MiniSearchBot/1.0 (+educational project)";

} // namespace

// ---------------------------------------------------------------------------
// Robots.txt
// ---------------------------------------------------------------------------

MINI_TEST(robots_empty_text_allows_everything) {
    const RobotsTxt robots = RobotsTxt::parse("", kUserAgent);
    MINI_EXPECT_TRUE(robots.allows("/"));
    MINI_EXPECT_TRUE(robots.allows("/anything/at/all"));
    MINI_EXPECT_TRUE(robots.isEmpty());
    MINI_EXPECT_FALSE(robots.crawlDelayMs().has_value());
}

MINI_TEST(robots_wildcard_disallow) {
    const RobotsTxt robots = RobotsTxt::parse(
        "User-agent: *\nDisallow: /private/\nDisallow: /admin\n", kUserAgent);
    MINI_EXPECT_FALSE(robots.allows("/private/secret.html"));
    MINI_EXPECT_FALSE(robots.allows("/admin/login"));
    MINI_EXPECT_TRUE(robots.allows("/public/index.html"));
    MINI_EXPECT_FALSE(robots.allows("/administrator"));  // prefix match per RFC 9309
    MINI_EXPECT_TRUE(robots.allows("/xadmin"));          // substring, not prefix
    MINI_EXPECT_TRUE(robots.allows("/"));
}

MINI_TEST(robots_specific_agent_beats_wildcard) {
    const std::string text =
        "User-agent: *\nDisallow: /\n\n"
        "User-agent: MiniSearchBot\nDisallow: /private/\n";
    const RobotsTxt robots = RobotsTxt::parse(text, kUserAgent);
    MINI_EXPECT_TRUE(robots.allows("/public/page.html"));   // specific group wins
    MINI_EXPECT_FALSE(robots.allows("/private/page.html"));
}

MINI_TEST(robots_other_agents_use_wildcard) {
    const std::string text =
        "User-agent: MiniSearchBot\nDisallow: /only-for-us/\n\n"
        "User-agent: *\nDisallow: /everyone/\n";
    const RobotsTxt robots = RobotsTxt::parse(text, "OtherBot/2.0");
    MINI_EXPECT_TRUE(robots.allows("/only-for-us/x"));   // group does not apply
    MINI_EXPECT_FALSE(robots.allows("/everyone/x"));
}

MINI_TEST(robots_allow_beats_disallow_by_length) {
    const RobotsTxt robots = RobotsTxt::parse(
        "User-agent: *\nDisallow: /forum\nAllow: /forum/public\n", kUserAgent);
    MINI_EXPECT_FALSE(robots.allows("/forum/private"));
    MINI_EXPECT_TRUE(robots.allows("/forum/public/thread-1"));
}

MINI_TEST(robots_crawl_delay_is_parsed) {
    const RobotsTxt robots = RobotsTxt::parse(
        "User-agent: *\nCrawl-delay: 1.5\nDisallow: /x\n", kUserAgent);
    MINI_EXPECT_TRUE(robots.crawlDelayMs().has_value());
    MINI_EXPECT_EQ(*robots.crawlDelayMs(), 1500);
}

MINI_TEST(robots_comments_and_case_insensitivity) {
    const RobotsTxt robots = RobotsTxt::parse(
        "# top comment\nUser-Agent: *\n# inline comment\nDISALLOW: /shown\n", kUserAgent);
    MINI_EXPECT_FALSE(robots.allows("/shown/page"));
}

MINI_TEST(robots_empty_disallow_means_allow_all) {
    const RobotsTxt robots = RobotsTxt::parse(
        "User-agent: *\nDisallow:\n", kUserAgent);
    MINI_EXPECT_TRUE(robots.allows("/anything"));
}

MINI_TEST(robots_multiple_user_agent_lines_in_one_group) {
    const std::string text =
        "User-agent: FooBot\nUser-agent: MiniSearchBot\nDisallow: /shared/\n";
    const RobotsTxt robots = RobotsTxt::parse(text, kUserAgent);
    MINI_EXPECT_FALSE(robots.allows("/shared/file"));
}

// ---------------------------------------------------------------------------
// URL helpers used by the crawler
// ---------------------------------------------------------------------------

MINI_TEST(url_path_and_query_variants) {
    using minisearch::url::normalize;
    MINI_EXPECT_EQ(*normalize("http://h/a/b?x=1"), "http://h/a/b?x=1");
    MINI_EXPECT_EQ(*normalize("http://h"), "http://h/");
    MINI_EXPECT_EQ(*normalize("HTTP://H:80/x#f"), "http://h/x");
}

// ---------------------------------------------------------------------------
// Offline crawler behavior (no network dependencies in unit tests)
// ---------------------------------------------------------------------------

namespace {

struct CollectingSink : ICrawlSink {
    std::vector<PageOutcome> outcomes;

    void onPage(const PageOutcome& outcome) override { outcomes.push_back(outcome); }
};

} // namespace

MINI_TEST(crawler_invalid_seeds_are_skipped) {
    CrawlConfig config;
    // Loopback addresses only: no DNS in unit tests (the sandbox resolver
    // can block longer than libcurl's timeout with NOSIGNAL).
    config.seedUrls = {"not a url", "ftp://example.com/x", "http://127.0.0.1:9/"};
    config.maxPages = 1;
    config.maxDepth = 0;
    config.timeoutSeconds = 2;
    config.workerThreads = 1;
    config.delayMs = 0;

    CollectingSink sink;
    WebCrawler crawler(config);
    crawler.run(sink);

    // Two invalid seeds skipped; the valid one is attempted and fails to
    // connect (nothing listens on loopback port 9) - honest failure
    // handling, not a crash.
    auto progress = crawler.progress();
    MINI_EXPECT_GE(progress.skipped, 2);
    MINI_EXPECT_TRUE(progress.finished);
    MINI_EXPECT_FALSE(progress.running);
    MINI_EXPECT_EQ(progress.succeeded, 0);
    MINI_EXPECT_GE(progress.failed, 1);
}

MINI_TEST(crawler_connection_failures_are_counted_not_crashed) {
    CrawlConfig config;
    // Port 1 on loopback: connection refused essentially immediately.
    config.seedUrls = {"http://127.0.0.1:1/unreachable"};
    config.maxPages = 1;
    config.maxDepth = 0;
    config.timeoutSeconds = 3;
    config.delayMs = 0;
    config.workerThreads = 2;

    CollectingSink sink;
    WebCrawler crawler(config);
    crawler.run(sink);

    const auto progress = crawler.progress();
    MINI_EXPECT_EQ(progress.succeeded, 0);
    MINI_EXPECT_GE(progress.failed, 1);
    MINI_EXPECT_TRUE(progress.finished);
    MINI_EXPECT_FALSE(sink.outcomes.empty());
    MINI_EXPECT_FALSE(sink.outcomes.front().ok);
}

MINI_TEST(crawler_respects_allowed_domains) {
    CrawlConfig config;
    // Two loopback hosts; only the first is allowed.
    config.seedUrls = {"http://127.0.0.1:9/a", "http://127.0.0.2:9/b"};
    config.allowedDomains = {"127.0.0.1"};
    config.maxPages = 1;
    config.maxDepth = 0;
    config.timeoutSeconds = 2;
    config.workerThreads = 1;
    config.delayMs = 0;

    CollectingSink sink;
    WebCrawler crawler(config);
    crawler.run(sink);

    const auto progress = crawler.progress();
    // The denied-domain seed was skipped before any request was made.
    MINI_EXPECT_GE(progress.skipped, 1);
    bool sawDenied = false;
    for (const PageOutcome& outcome : sink.outcomes) {
        if (outcome.url.find("127.0.0.2") != std::string::npos) {
            sawDenied = true;
            MINI_EXPECT_TRUE(outcome.skipped);
        }
    }
    MINI_EXPECT_TRUE(sawDenied);
}

MINI_TEST(crawler_avoids_login_like_urls) {
    CrawlConfig config;
    config.seedUrls = {"http://127.0.0.1:9/wp-login.php?action=login"};
    config.maxPages = 1;
    config.maxDepth = 0;
    config.timeoutSeconds = 2;
    config.workerThreads = 1;
    config.delayMs = 0;

    CollectingSink sink;
    WebCrawler crawler(config);
    crawler.run(sink);

    const auto progress = crawler.progress();
    MINI_EXPECT_GE(progress.skipped, 1);
    MINI_EXPECT_EQ(progress.fetched, 0);  // never actually downloaded
}
