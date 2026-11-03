#include "crawler/RobotsTxt.h"

#include <cctype>

namespace minisearch {

namespace {

std::string toLower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// "MiniSearchBot/1.0 (+...)" -> "minisearchbot"
std::string productToken(const std::string& userAgent) {
    std::string token;
    for (const char c : userAgent) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '/') break;
        token.push_back(c);
    }
    return toLower(token);
}

bool startsWithCi(const std::string& line, const char* prefix) {
    const std::string lowered = toLower(line);
    return lowered.compare(0, std::string(prefix).size(), prefix) == 0;
}

} // namespace

RobotsTxt RobotsTxt::allowAll() {
    return RobotsTxt{};
}

RobotsTxt RobotsTxt::parse(const std::string& text, const std::string& userAgent) {
    const std::string ourAgent = productToken(userAgent);

    // Rules grouped per agent token; the last Crawl-delay seen in a group
    // applies to that group's agents.
    struct Group {
        std::vector<std::string> agents;
        std::vector<Rule> rules;
        std::optional<int> delayMs;
    };
    std::vector<Group> groups;

    std::vector<std::string> currentAgents;
    bool rulesStarted = false;

    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t newline = text.find('\n', lineStart);
        std::string line = text.substr(
            lineStart, newline == std::string::npos ? std::string::npos : newline - lineStart);
        lineStart = (newline == std::string::npos) ? text.size() + 1 : newline + 1;

        // Comments run to end of line.
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;

        if (startsWithCi(line, "user-agent:")) {
            if (rulesStarted) {
                currentAgents.clear();
                rulesStarted = false;
            }
            const std::string agent = toLower(trim(line.substr(std::string("user-agent:").size())));
            if (!agent.empty()) currentAgents.push_back(agent);
            continue;
        }

        if (currentAgents.empty()) continue;
        rulesStarted = true;

        if (startsWithCi(line, "disallow:")) {
            const std::string prefix = trim(line.substr(std::string("disallow:").size()));
            groups.push_back(Group{currentAgents, {{prefix, false}}, std::nullopt});
        } else if (startsWithCi(line, "allow:")) {
            const std::string prefix = trim(line.substr(std::string("allow:").size()));
            groups.push_back(Group{currentAgents, {{prefix, true}}, std::nullopt});
        } else if (startsWithCi(line, "crawl-delay:")) {
            const std::string value = trim(line.substr(std::string("crawl-delay:").size()));
            double seconds = 0.0;
            try {
                seconds = std::stod(value);
            } catch (const std::exception&) {
                continue;
            }
            const int ms = static_cast<int>(seconds * 1000.0);
            if (ms >= 0) groups.push_back(Group{currentAgents, {}, ms});
        }
        // Unknown directives (Sitemap:, etc.) are ignored per RFC 9309.
    }

    // Select the group whose agent list contains our token; prefer the
    // specific token over the wildcard.
    const auto findRules = [&groups](const std::string& agent) {
        std::vector<Rule> rules;
        std::optional<int> delay;
        for (const Group& group : groups) {
            for (const std::string& candidate : group.agents) {
                if (candidate != agent) continue;
                rules.insert(rules.end(), group.rules.begin(), group.rules.end());
                if (group.delayMs.has_value()) delay = group.delayMs;
            }
        }
        return std::make_pair(rules, delay);
    };

    auto [specificRules, specificDelay] = findRules(ourAgent);
    if (specificRules.empty() && !specificDelay.has_value()) {
        auto [wildcardRules, wildcardDelay] = findRules("*");
        specificRules = wildcardRules;
        specificDelay = wildcardDelay;
    }

    RobotsTxt robots;
    robots.rules_ = std::move(specificRules);
    robots.crawlDelayMs_ = specificDelay;
    return robots;
}

bool RobotsTxt::allows(const std::string& pathAndQuery) const {
    const Rule* best = nullptr;
    for (const Rule& rule : rules_) {
        if (rule.prefix.empty()) continue;  // "Disallow:" (empty) = allow all
        if (pathAndQuery.compare(0, rule.prefix.size(), rule.prefix) == 0) {
            if (best == nullptr || rule.prefix.size() > best->prefix.size()) {
                best = &rule;
            } else if (rule.prefix.size() == best->prefix.size() && rule.allow) {
                best = &rule;  // tie -> Allow wins
            }
        }
    }
    return best == nullptr || best->allow;
}

} // namespace minisearch
