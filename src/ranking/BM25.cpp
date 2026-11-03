#include "ranking/BM25.h"

#include <cmath>

namespace minisearch {

BM25Ranker::BM25Ranker(double k1, double b, FieldWeights weights)
    : k1_(k1), b_(b), weights_(weights) {}

double BM25Ranker::idf(std::size_t totalDocs, std::uint32_t df) {
    const double n = static_cast<double>(totalDocs);
    const double d = static_cast<double>(df);
    return std::log(1.0 + (n - d + 0.5) / (d + 0.5));
}

double BM25Ranker::scoreTerm(const std::string& /*term*/, const PostingList& list,
                              const Posting& posting, const DocMeta& meta,
                              const CollectionStats& stats) const {
    const double f = weightedTermFrequency(posting, weights_);
    if (f <= 0.0) return 0.0;

    const double docLength = weightedDocumentLength(meta, weights_);
    double avgdl = stats.avgWeightedDocLength;
    if (avgdl <= 0.0) avgdl = docLength > 0.0 ? docLength : 1.0;  // degenerate guard

    const double normalization = 1.0 - b_ + b_ * (docLength / avgdl);
    const double denominator = f + k1_ * normalization;
    if (denominator <= 0.0) return 0.0;

    return idf(stats.totalDocs, list.df()) * (f * (k1_ + 1.0)) / denominator;
}

} // namespace minisearch
