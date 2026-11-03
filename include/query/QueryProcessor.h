#pragma once

// MiniSearch - query evaluation (spec module 7).
//
// Evaluates a parsed query tree against the inverted index:
//   Term    -> posting-list documents
//   Phrase  -> documents with consecutive, in-order positions
//              (virtual stream guarantees field boundaries are respected)
//   And     -> two-pointer intersection of children (NOT children subtract)
//   Or      -> two-pointer union
//
// The result carries the candidate documents plus the distinct positive
// terms (used for ranking); terms under NOT never contribute to scores.

#include "index/InvertedIndex.h"
#include "query/QueryParser.h"

#include <string>
#include <vector>

namespace minisearch {

struct QueryEvaluation {
    bool ok = false;
    std::string error;
    std::vector<DocumentId> candidates;      // ascending document ids
    std::vector<std::string> scoringTerms;   // distinct, positive, in order
};

class QueryProcessor {
public:
    explicit QueryProcessor(const TextProcessor& processor) : processor_(processor) {}

    QueryEvaluation evaluate(const query::QueryNode& root, const InvertedIndex& index) const;

    // Set operations over ascending id vectors (two-pointer merges).
    static std::vector<DocumentId> intersect(const std::vector<DocumentId>& a,
                                              const std::vector<DocumentId>& b);
    static std::vector<DocumentId> unionOf(const std::vector<DocumentId>& a,
                                            const std::vector<DocumentId>& b);
    static std::vector<DocumentId> subtract(const std::vector<DocumentId>& a,
                                             const std::vector<DocumentId>& b);

    // Documents where the terms occur consecutively, in order, within one
    // field. Exposed for unit tests.
    static std::vector<DocumentId> phraseMatches(const std::vector<std::string>& terms,
                                                 const InvertedIndex& index);

private:
    const TextProcessor& processor_;
};

} // namespace minisearch
