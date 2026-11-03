#pragma once

// MiniSearch - hand-written TF-IDF ranking (spec module 5).
//
//   TF(t,d)   = weighted_tf(t,d) / weighted_length(d)
//   IDF(t)    = ln((N + 1) / (df + 1)) + 1     (smoothed; always > 0)
//   TFIDF     = TF * IDF, summed over query terms
//
// The +1 smoothing inside IDF and the natural log keep every term's
// contribution strictly positive and avoid log(0)/division-by-zero edges
// (df == N, df == 0, empty documents).

#include "ranking/Ranker.h"

namespace minisearch {

class TFIDFRanker : public Ranker {
public:
    explicit TFIDFRanker(FieldWeights weights = FieldWeights{});

    double scoreTerm(const std::string& term, const PostingList& list, const Posting& posting,
                     const DocMeta& meta, const CollectionStats& stats) const override;

    const char* name() const override { return "tfidf"; }

    // Smoothed inverse document frequency (also used by tests).
    static double idf(std::size_t totalDocs, std::uint32_t df);

private:
    FieldWeights weights_;
};

} // namespace minisearch
