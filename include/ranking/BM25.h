#pragma once

// MiniSearch - hand-written BM25 ranking (spec module 6).
//
//   IDF_bm25(t) = ln( 1 + (N - df + 0.5) / (df + 0.5) )
//   score(t,D)  = IDF * f(t,D) * (k1 + 1) /
//                 ( f(t,D) + k1 * (1 - b + b * |D| / avgdl) )
//
// k1 controls term-frequency saturation, b controls document-length
// normalization (0 = none, 1 = full). Both configurable; the +1 inside
// the log (Lucene-style) keeps IDF positive when df > N/2.

#include "ranking/Ranker.h"

namespace minisearch {

class BM25Ranker : public Ranker {
public:
    explicit BM25Ranker(double k1 = 1.2, double b = 0.75, FieldWeights weights = FieldWeights{});

    double scoreTerm(const std::string& term, const PostingList& list, const Posting& posting,
                     const DocMeta& meta, const CollectionStats& stats) const override;

    const char* name() const override { return "bm25"; }

    static double idf(std::size_t totalDocs, std::uint32_t df);

    double k1() const noexcept { return k1_; }
    double b() const noexcept { return b_; }

private:
    double k1_;
    double b_;
    FieldWeights weights_;
};

} // namespace minisearch
