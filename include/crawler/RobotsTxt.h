#pragma once

// MiniSearch - robots.txt compliance (spec module 5, crawling restrictions).
//
// A pragmatic robots.txt subset per RFC 9309: User-agent groups with
// Allow/Disallow path prefixes and Crawl-delay. Group selection prefers
// an exact product-token match over "*". For a path, the LONGEST matching
// prefix wins; ties go to Allow. Unreachable/absent robots.txt means
// "allow everything" (the RFC default), recorded honestly in crawl logs.

#include <optional>
#include <string>
#include <vector>

namespace minisearch {

class RobotsTxt {
public:
    // userAgent: full UA string; its first token (e.g. "MiniSearchBot" of
    // "MiniSearchBot/1.0 (+...)") selects the rule group.
    static RobotsTxt parse(const std::string& text, const std::string& userAgent);

    static RobotsTxt allowAll();

    // pathAndQuery: the URL path plus query (e.g. "/docs/x.html?q=1").
    bool allows(const std::string& pathAndQuery) const;

    // Robots-requested crawl delay in milliseconds, if any.
    std::optional<int> crawlDelayMs() const { return crawlDelayMs_; }

    // True when no rules were parsed (treat as allow-all).
    bool isEmpty() const { return rules_.empty(); }

private:
    struct Rule {
        std::string prefix;
        bool allow = false;
    };

    std::vector<Rule> rules_;
    std::optional<int> crawlDelayMs_;
};

} // namespace minisearch
