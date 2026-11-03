#include "ranking/Ranker.h"

namespace minisearch {

double weightedTermFrequency(const Posting& posting, const FieldWeights& weights) {
    return weights.title * static_cast<double>(posting.tf_title)
         + weights.heading * static_cast<double>(posting.tf_heading)
         + weights.body * static_cast<double>(posting.tf_body);
}

double weightedDocumentLength(const DocMeta& meta, const FieldWeights& weights) {
    return weights.title * static_cast<double>(meta.title_len)
         + weights.heading * static_cast<double>(meta.heading_len)
         + weights.body * static_cast<double>(meta.body_len);
}

} // namespace minisearch
