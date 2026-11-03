#pragma once

// MiniSearch - hand-built inverted index (spec module 4).
//
// Maps TERM -> sorted posting list of the documents containing it.
// Every posting stores per-field term frequencies plus the term's
// positions in the document's *virtual position stream*.
//
// Virtual position stream layout per document:
//   [ title tokens | gap-per-heading heading tokens | gap | body tokens ]
// Gaps are inserted only where a boundary actually exists (a document
// with no title/headings has body positions starting at 0), and they
// guarantee that no two adjacent stream positions ever belong to
// different fields (or different headings). That is what makes phrase
// verification boundary-correct: a phrase can never falsely match across
// the title/body edge. DocMeta derives field membership of a position in
// O(1) from the stored lengths.

#include "common/Document.h"
#include "processor/TextProcessor.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minisearch {

// Per-document index metadata (kept small; URLs/titles live in SQLite).
struct DocMeta {
    DocumentId id = -1;
    std::uint32_t title_len = 0;      // token count in title
    std::uint32_t heading_len = 0;    // token count across all headings
    std::uint32_t body_len = 0;       // token count in body
    std::uint32_t heading_count = 0;  // number of separate headings
    std::vector<std::string> doc_terms;  // distinct terms (enables removal)

    std::uint32_t totalTokens() const noexcept { return title_len + heading_len + body_len; }

    // Virtual stream boundaries (see file header). A gap position is
    // inserted after the title (when present) and after every heading,
    // so adjacent positions never cross a field or heading boundary -
    // except in a pure-body document, where positions equal ordinals.
    std::uint32_t headingStartVirtual() const noexcept {
        return title_len == 0 ? 0 : title_len + 1;
    }
    std::uint32_t headingEndVirtual() const noexcept {
        // Includes the per-heading gaps (one after each heading).
        return headingStartVirtual() + heading_len + heading_count;
    }
    std::uint32_t bodyStartVirtual() const noexcept { return headingEndVirtual(); }

    Field classifyPosition(std::uint32_t pos) const noexcept {
        if (pos < headingStartVirtual()) return Field::Title;
        if (pos < headingEndVirtual()) return Field::Heading;
        return Field::Body;
    }
};

// One term occurrence record inside one document.
struct Posting {
    DocumentId doc = -1;
    std::uint32_t tf_title = 0;
    std::uint32_t tf_heading = 0;
    std::uint32_t tf_body = 0;
    std::vector<std::uint32_t> positions;  // ascending virtual positions
};

// Postings are always kept sorted by doc id (two-pointer friendly).
struct PostingList {
    std::vector<Posting> postings;
    std::uint32_t df() const noexcept { return static_cast<std::uint32_t>(postings.size()); }
};

class InvertedIndex {
public:
    // Indexes (or re-indexes) one analyzed document under a stable id.
    // Re-indexing the same id replaces its postings - no duplicates.
    void indexDocument(DocumentId id, const AnalyzedDocument& analyzed);

    // Removes a document and its postings; returns true if it existed.
    bool removeDocument(DocumentId id);

    bool hasDocument(DocumentId id) const;
    const DocMeta* findDocument(DocumentId id) const;

    // O(1) average term lookup - never scans documents.
    const PostingList* findTerm(const std::string& term) const;

    // Find a specific posting; null when absent.
    const Posting* findPosting(const std::string& term, DocumentId doc) const;

    // Vocabulary search for the suggestion endpoint.
    std::vector<std::pair<std::string, std::uint32_t>> termsWithPrefix(
        const std::string& prefix, std::size_t limit) const;

    // Collection statistics.
    std::size_t documentCount() const noexcept { return docs_.size(); }
    std::size_t termCount() const noexcept { return terms_.size(); }
    std::size_t totalPostings() const noexcept { return totalPostings_; }
    std::uint64_t tokenSum() const noexcept { return tokenSum_; }
    double averageTokenLength() const noexcept {
        return docs_.empty() ? 0.0
                             : static_cast<double>(tokenSum_)
                                   / static_cast<double>(docs_.size());
    }

    std::vector<DocumentId> allDocumentIds() const;

    // Replaces the whole index state; used by StorageManager when loading
    // a serialized index file (stats are provided by the loader).
    void restore(std::unordered_map<std::string, PostingList> terms,
                 std::unordered_map<DocumentId, DocMeta> docs,
                 std::size_t totalPostings, std::uint64_t tokenSum);

    // Read-only vocabulary access (serialization, benchmarks, tests).
    const std::unordered_map<std::string, PostingList>& termMap() const noexcept { return terms_; }
    const std::unordered_map<DocumentId, DocMeta>& documentMap() const noexcept { return docs_; }

    void clear() noexcept;

private:
    static const Posting* findPostingIn(const PostingList& list, DocumentId doc);

    std::unordered_map<std::string, PostingList> terms_;
    std::unordered_map<DocumentId, DocMeta> docs_;
    std::size_t totalPostings_ = 0;
    std::uint64_t tokenSum_ = 0;
};

} // namespace minisearch
