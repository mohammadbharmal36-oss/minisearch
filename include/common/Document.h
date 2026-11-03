#pragma once

// MiniSearch - core document model.
//
// A Document is the universal cargo of the pipeline: the crawler produces
// it, the HTML parser fills it, the text processor tokenizes it, the index
// stores references to it, and the API serializes it back out. Keeping it
// in one small POD-like structure is what lets every stage be tested in
// isolation with fixture documents (no network, no HTML required).

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace minisearch {

// Document identifiers are assigned by the storage layer and stay stable
// across restarts (SQLite row ids). -1 means "not yet assigned".
using DocumentId = std::int64_t;

struct Document {
    DocumentId id = -1;

    // --- identity ---------------------------------------------------------
    std::string url;            // final URL after redirects
    std::string canonical_url;  // <link rel="canonical"> when present

    // --- extracted content --------------------------------------------------
    std::string title;                   // <title>
    std::string description;             // <meta name="description">
    std::string content;                 // visible text (title/headings excluded),
                                         // pre-tokenization
    std::vector<std::string> headings;   // h1 and h2 texts, document order
    std::vector<std::string> links;      // hrefs discovered on the page

    // --- crawl bookkeeping ---------------------------------------------------
    int http_status = 0;         // final HTTP status code (200, 404, ...)
    std::int64_t crawled_at = 0; // unix epoch seconds

    std::size_t outgoingLinkCount() const noexcept { return links.size(); }
    bool hasValidIdentity() const noexcept { return id >= 0 && !url.empty(); }

    // Human-readable one-line summary used by the CLI and test output.
    std::string toDebugString() const;
};

bool operator==(const Document& lhs, const Document& rhs);
bool operator!=(const Document& lhs, const Document& rhs);
std::ostream& operator<<(std::ostream& stream, const Document& document);

} // namespace minisearch
