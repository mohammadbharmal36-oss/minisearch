#pragma once

// MiniSearch - ranking strategy interface (spec modules 5 and 6).
//
// TF-IDF and BM25 are interchangeable strategies behind this interface:
// the query processor produces candidate documents, the engine asks a
// Ranker how much each query term contributes to each candidate, and the
// scores are summed per document. Both implementations are hand-written
// (no third-party ranking library), per the project ground rules.

#include "index/InvertedIndex.h"

#include <string>

namespace minisearch {

// Configurable per-field importance (spec's initial values; not universal
// optima - exposed via config.json).
struct FieldWeights {
    double title = 3.0;
    double heading = 2.0;
    double body = 1.0;
};

// Collection-level statistics the rankers need. Maintained by the engine.
struct CollectionStats {
    std::size_t totalDocs = 0;
    double avgWeightedDocLength = 0.0;
};

// Weighted term frequency: w_t*tf_title + w_h*tf_heading + w_b*tf_body.
double weightedTermFrequency(const Posting& posting, const FieldWeights& weights);

// Weighted document length: w_t*len_title + w_h*len_heading + w_b*len_body.
// Using the same weights for tf and length keeps f/|D| scale-consistent.
double weightedDocumentLength(const DocMeta& meta, const FieldWeights& weights);

class Ranker {
public:
    virtual ~Ranker() = default;

    // Contribution of one query term to one candidate document.
    // The posting list is passed so the ranker can read the term's
    // document frequency (IDF input).
    virtual double scoreTerm(const std::string& term, const PostingList& list,
                              const Posting& posting, const DocMeta& meta,
                              const CollectionStats& stats) const = 0;

    virtual const char* name() const = 0;
};

} // namespace minisearch
