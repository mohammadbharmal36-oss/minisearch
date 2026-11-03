#include "crawler/Url.h"

#include <cctype>

namespace minisearch::url {

namespace {

struct ParsedUrl {
    std::string scheme;
    std::string host;    // lowercase, no userinfo, no port
    std::string port;    // ":8080" or empty
    std::string path;    // starts with '/' (or empty before defaulting)
    std::string query;   // "?..." or empty
    std::string fragment;// "#..." or empty
};

std::string toLower(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool hasSchemePrefix(const std::string& text) {
    // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
    if (text.empty() || !std::isalpha(static_cast<unsigned char>(text[0]))) return false;
    std::size_t i = 0;
    while (i < text.size() && text[i] != ':'
           && text[i] != '/' && text[i] != '?' && text[i] != '#') {
        const char c = text[i];
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '+'
                        || c == '-' || c == '.';
        if (!ok) return false;
        ++i;
    }
    return i > 0 && i < text.size() && text[i] == ':';
}

// Splits an absolute URL. Returns nullopt when scheme is missing.
std::optional<ParsedUrl> parseAbsolute(const std::string& url) {
    if (!hasSchemePrefix(url)) return std::nullopt;
    const std::size_t schemeEnd = url.find(':');
    if (schemeEnd == std::string::npos) return std::nullopt;
    if (schemeEnd + 3 > url.size()
        || url.compare(schemeEnd + 1, 2, "//") != 0) {
        return std::nullopt;  // we only handle scheme://authority forms
    }

    ParsedUrl parsed;
    parsed.scheme = toLower(url.substr(0, schemeEnd));

    const std::size_t authorityStart = schemeEnd + 3;
    std::size_t authorityEnd = url.size();
    std::size_t pathStart = url.size();
    std::size_t queryStart = url.size();
    std::size_t fragmentStart = url.size();
    for (std::size_t i = authorityStart; i < url.size(); ++i) {
        if (url[i] == '/' && pathStart == url.size()) {
            pathStart = i;
            break;
        }
        if (url[i] == '?' || url[i] == '#') {
            pathStart = i;  // empty path
            break;
        }
    }
    // Locate query/fragment anywhere after the authority. A '?' inside a
    // fragment is not a query separator.
    for (std::size_t i = authorityStart; i < url.size(); ++i) {
        if (url[i] == '?' && queryStart == url.size()) {
            queryStart = i;
            break;
        }
    }
    for (std::size_t i = authorityStart; i < url.size(); ++i) {
        if (url[i] == '#' && fragmentStart == url.size()) {
            fragmentStart = i;
            break;
        }
    }
    if (queryStart != url.size() && fragmentStart != url.size()
        && queryStart > fragmentStart) {
        queryStart = url.size();
    }
    authorityEnd = std::min(std::min(pathStart, queryStart), fragmentStart);

    std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
    // Strip userinfo.
    const std::size_t atSign = authority.rfind('@');
    if (atSign != std::string::npos) authority = authority.substr(atSign + 1);
    // Split host / port.
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos
        && authority.find(']', colon) == std::string::npos) {  // not inside IPv6 literal
        parsed.host = toLower(authority.substr(0, colon));
        parsed.port = authority.substr(colon);
    } else {
        parsed.host = toLower(authority);
    }
    if (parsed.host.empty()) return std::nullopt;

    if (pathStart < url.size() && url[pathStart] == '/') {
        const std::size_t pathEnd = std::min(queryStart, fragmentStart);
        parsed.path = url.substr(pathStart, pathEnd - pathStart);
    }
    if (queryStart < url.size() && url[queryStart] == '?') {
        const std::size_t queryEnd = fragmentStart;
        parsed.query = url.substr(queryStart, queryEnd - queryStart);
    }
    if (fragmentStart < url.size()) {
        parsed.fragment = url.substr(fragmentStart);
    }
    return parsed;
}

std::string rebuild(const ParsedUrl& parsed) {
    std::string port = parsed.port;
    if ((parsed.scheme == "http" && port == ":80")
        || (parsed.scheme == "https" && port == ":443")) {
        port.clear();  // default ports are noise in a visited-set key
    }
    std::string out = parsed.scheme + "://" + parsed.host + port;
    out += parsed.path.empty() ? "/" : parsed.path;
    out += parsed.query;
    return out;
}

} // namespace

bool isHttpScheme(const std::string& url) {
    if (!hasSchemePrefix(url)) return false;
    const std::size_t schemeEnd = url.find(':');
    if (schemeEnd == std::string::npos) return false;
    const std::string scheme = toLower(url.substr(0, schemeEnd));
    return scheme == "http" || scheme == "https";
}

std::string removeDotSegments(const std::string& path) {
    std::string input = path;
    std::string output;

    const auto startsWith = [&input](const char* prefix) {
        const std::size_t len = std::string(prefix).size();
        return input.size() >= len && input.compare(0, len, prefix) == 0;
    };
    const auto removeLastSegment = [&output]() {
        const std::size_t slash = output.find_last_of('/');
        if (slash != std::string::npos) output.erase(slash);
    };

    while (!input.empty()) {
        if (startsWith("../")) {
            input.erase(0, 3);
        } else if (startsWith("./")) {
            input.erase(0, 2);
        } else if (startsWith("/./")) {
            input.replace(0, 3, "/");
        } else if (input == "/.") {
            input = "/";
        } else if (startsWith("/../")) {
            input.replace(0, 4, "/");
            removeLastSegment();
        } else if (input == "/..") {
            input = "/";
            removeLastSegment();
        } else if (input == "." || input == "..") {
            input.clear();
        } else {
            // Move the first path segment (including its leading '/') to
            // the output, up to (not including) the next '/'.
            const std::size_t nextSlash = input.find('/', 1);
            if (nextSlash == std::string::npos) {
                output += input;
                input.clear();
            } else {
                output += input.substr(0, nextSlash);
                input.erase(0, nextSlash);
            }
        }
    }
    return output;
}

std::optional<std::string> normalize(const std::string& url) {
    const auto parsed = parseAbsolute(url);
    if (!parsed.has_value()) return std::nullopt;
    if (parsed->scheme != "http" && parsed->scheme != "https") return std::nullopt;
    return rebuild(*parsed);
}

std::optional<std::string> resolve(const std::string& base, const std::string& ref) {
    if (ref.empty() || ref[0] == '#') return normalize(base);

    // Drop the reference's fragment before any other processing.
    std::string reference = ref;
    const std::size_t hash = reference.find('#');
    if (hash != std::string::npos) reference.erase(hash);
    if (reference.empty()) return normalize(base);

    if (hasSchemePrefix(reference)) {
        return isHttpScheme(reference) ? normalize(reference) : std::optional<std::string>{};
    }

    const auto parsedBase = parseAbsolute(base);
    if (!parsedBase.has_value()) return std::nullopt;
    const ParsedUrl& b = *parsedBase;

    if (reference.compare(0, 2, "//") == 0) {
        return normalize(b.scheme + ":" + reference);
    }

    // Split the reference into path and query parts.
    std::string refPath = reference;
    std::string refQuery;
    const std::size_t questionMark = reference.find('?');
    if (questionMark != std::string::npos) {
        refPath = reference.substr(0, questionMark);
        refQuery = reference.substr(questionMark);
    }

    ParsedUrl merged = b;
    merged.query = refQuery;
    merged.fragment.clear();
    if (!refPath.empty() && refPath[0] == '/') {
        merged.path = refPath;
    } else if (refPath.empty()) {
        // Keep the base path (query-only reference).
    } else {
        const std::string directory = b.path.substr(0, b.path.find_last_of('/') + 1);
        merged.path = directory + refPath;
    }
    merged.path = removeDotSegments(merged.path);
    if (merged.path.empty()) merged.path = "/";
    return rebuild(merged);
}

std::string hostOf(const std::string& url) {
    const auto parsed = parseAbsolute(url);
    return parsed.has_value() ? parsed->host : std::string{};
}

} // namespace minisearch::url
