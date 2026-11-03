#include "ranking/TFIDF.h"

#include <cmath>

namespace minisearch {

TFIDFRanker::TFIDFRanker(FieldWeights weights) : weights_(weights) {}

double TFIDFRanker::idf(std::size_t totalDocs, std::uint32_t df) {
    const double n = static_cast<double>(totalDocs);
    const double d = static_cast<double>(df);
    return std::log((n + 1.0) / (d + 1.0)) + 1.0;
}

double TFIDFRanker::scoreTerm(const std::string& /*term*/, const PostingList& list,
                               const Posting& posting, const DocMeta& meta,
                               const CollectionStats& stats) const {
    const double length = weightedDocumentLength(meta, weights_);
    if (length <= 0.0) return 0.0;  // empty document guard

    const double tf = weightedTermFrequency(posting, weights_) / length;
    return tf * idf(stats.totalDocs, list.df());
}

} // namespace minisearch
