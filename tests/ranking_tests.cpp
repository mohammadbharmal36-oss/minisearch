// Unit tests for TF-IDF and BM25 (ranking/TFIDF, ranking/BM25).
//
// Expected values were computed by hand from the formulas (natural log)
// and cross-checked numerically; see docs/algorithms.md section 4.

#include "ranking/BM25.h"
#include "ranking/TFIDF.h"
#include "index/InvertedIndex.h"
#include "processor/TextProcessor.h"
#include "test_framework.h"

#include <string>

namespace {

using minisearch::BM25Ranker;
using minisearch::CollectionStats;
using minisearch::Document;
using minisearch::FieldWeights;
using minisearch::InvertedIndex;
using minisearch::Posting;
using minisearch::PostingList;
using minisearch::TextProcessor;
using minisearch::TFIDFRanker;

const TextProcessor processor;

// Fixture corpus (weights: title 3, heading 2, body 1):
//   D1: title  "cpp program"                    -> weighted length 6
//   D2: body   "cpp program program"            -> weighted length 3
//   D3: body   "python"                         -> weighted length 1
// df(cpp)=2, df(program)=2, df(python)=1, N=3.
struct Fixture {
    InvertedIndex index;
    CollectionStats stats;

    Fixture() {
        Document d1;
        d1.title = "cpp program";
        Document d2;
        d2.content = "cpp program program";
        Document d3;
        d3.content = "python";
        index.indexDocument(1, processor.analyze(d1));
        index.indexDocument(2, processor.analyze(d2));
        index.indexDocument(3, processor.analyze(d3));
        stats.totalDocs = index.documentCount();
        stats.avgWeightedDocLength = (6.0 + 3.0 + 1.0) / 3.0;
    }
};

double scoreOf(const minisearch::Ranker& ranker, const InvertedIndex& index,
               const CollectionStats& stats, const std::string& term,
               minisearch::DocumentId doc) {
    const auto* list = index.findTerm(term);
    const auto* posting = index.findPosting(term, doc);
    const auto* meta = index.findDocument(doc);
    if (list == nullptr || posting == nullptr || meta == nullptr) return -1.0;
    return ranker.scoreTerm(term, *list, *posting, *meta, stats);
}

} // namespace

MINI_TEST(tfidf_hand_computed_score) {
    const Fixture fx;
    const TFIDFRanker ranker;
    // TF = (3*1)/6 = 0.5 ; IDF = ln(4/3) + 1 ; score = 0.6438410362...
    MINI_EXPECT_NEAR(scoreOf(ranker, fx.index, fx.stats, "cpp", 1), 0.6438410362258904, 1e-9);
}

MINI_TEST(ranker_scores_zero_tf_posting_as_zero) {
    // A term that does not occur in a document never reaches the ranker
    // in the engine (no posting). Directly verify the ranker is safe when
    // handed a zero-frequency posting anyway.
    const TFIDFRanker tfidf;
    const BM25Ranker bm25;
    const PostingList list;
    const Posting posting;  // all tf = 0
    const minisearch::DocMeta meta;
    const CollectionStats stats{2, 6.0};
    MINI_EXPECT_NEAR(tfidf.scoreTerm("x", list, posting, meta, stats), 0.0, 1e-12);
    MINI_EXPECT_NEAR(bm25.scoreTerm("x", list, posting, meta, stats), 0.0, 1e-12);
}

MINI_TEST(tfidf_idf_properties) {
    // Smoothed IDF: always > 0, decreasing in df.
    MINI_EXPECT_GT(TFIDFRanker::idf(3, 1), 0.0);
    MINI_EXPECT_GT(TFIDFRanker::idf(3, 3), 0.0);
    MINI_EXPECT_GT(TFIDFRanker::idf(3, 1), TFIDFRanker::idf(3, 2));
    MINI_EXPECT_GT(TFIDFRanker::idf(3, 2), TFIDFRanker::idf(3, 3));
}

MINI_TEST(tfidf_rarer_terms_rank_higher) {
    const Fixture fx;
    const TFIDFRanker ranker;
    // "python" (df=1) contributes more than "cpp" (df=2) to equal docs.
    MINI_EXPECT_GT(scoreOf(ranker, fx.index, fx.stats, "python", 3),
                   scoreOf(ranker, fx.index, fx.stats, "cpp", 2) * 3.0);
}

MINI_TEST(bm25_hand_computed_score) {
    const Fixture fx;
    const BM25Ranker ranker;
    // IDF = ln(1.6); f = 3; |D| = 6; avgdl = 10/3; k1 = 1.2; b = 0.75
    // score = 0.6304926734...
    MINI_EXPECT_NEAR(scoreOf(ranker, fx.index, fx.stats, "cpp", 1), 0.630492673378426, 1e-9);
}

MINI_TEST(bm25_idf_always_positive) {
    // Lucene-style +1 inside the log keeps IDF > 0 even when df = N.
    MINI_EXPECT_GT(BM25Ranker::idf(3, 3), 0.0);
    MINI_EXPECT_GT(BM25Ranker::idf(100, 100), 0.0);
    MINI_EXPECT_GT(BM25Ranker::idf(3, 1), BM25Ranker::idf(3, 3));
}

MINI_TEST(bm25_parameters_are_configurable) {
    const Fixture fx;
    const BM25Ranker defaultRanker(1.2, 0.75);
    const BM25Ranker tunedRanker(2.0, 0.0);
    const double a = scoreOf(defaultRanker, fx.index, fx.stats, "cpp", 1);
    const double b = scoreOf(tunedRanker, fx.index, fx.stats, "cpp", 1);
    MINI_EXPECT_GT(a, 0.0);
    MINI_EXPECT_GT(b, 0.0);
    MINI_EXPECT_NEAR(a, 0.630492673378426, 1e-9);
    // b = 0 disables length normalization -> different score for |D|=6 vs avgdl=10/3.
    MINI_EXPECT_NE(b, a);
}

MINI_TEST(field_boost_makes_title_matches_outrank_body_matches) {
    // Doc A: term in title of a 2-token title. Doc B: term in body of a
    // 6-token body. Equal weighted lengths (6) under default weights -
    // only the field differs. df(x) = 2, N = 2.
    InvertedIndex index;
    Document a;
    a.title = "x y";
    Document b;
    b.content = "x y z z z z";
    index.indexDocument(1, processor.analyze(a));
    index.indexDocument(2, processor.analyze(b));
    CollectionStats stats;
    stats.totalDocs = 2;
    stats.avgWeightedDocLength = 6.0;

    const TFIDFRanker tfidf;
    MINI_EXPECT_NEAR(scoreOf(tfidf, index, stats, "x", 1), 0.5, 1e-9);
    MINI_EXPECT_NEAR(scoreOf(tfidf, index, stats, "x", 2), 0.16666666666666666, 1e-9);
    MINI_EXPECT_GT(scoreOf(tfidf, index, stats, "x", 1), scoreOf(tfidf, index, stats, "x", 2));

    const BM25Ranker bm25;
    MINI_EXPECT_NEAR(scoreOf(bm25, index, stats, "x", 1), 0.28650530353335724, 1e-9);
    MINI_EXPECT_NEAR(scoreOf(bm25, index, stats, "x", 2), 0.1823215567939546, 1e-9);
    MINI_EXPECT_GT(scoreOf(bm25, index, stats, "x", 1), scoreOf(bm25, index, stats, "x", 2));
}

MINI_TEST(custom_field_weights_change_boost) {
    InvertedIndex index;
    Document a;
    a.title = "x y";
    Document b;
    b.content = "x y";  // same token count as the title
    index.indexDocument(1, processor.analyze(a));
    index.indexDocument(2, processor.analyze(b));
    CollectionStats stats;
    stats.totalDocs = 2;
    stats.avgWeightedDocLength = 4.0;

    FieldWeights flat;
    flat.title = 1.0;
    flat.heading = 1.0;
    flat.body = 1.0;
    const TFIDFRanker ranker(flat);
    // With flat weights both docs have tf = 1/2 -> identical scores.
    MINI_EXPECT_NEAR(scoreOf(ranker, index, stats, "x", 1),
                     scoreOf(ranker, index, stats, "x", 2), 1e-12);
}

MINI_TEST(bm25_term_frequency_saturation) {
    // Doubling tf less than doubles the BM25 term contribution, while raw
    // TF-IDF here scales linearly - the defining property of k1.
    InvertedIndex index;
    Document one;
    one.content = "x";
    Document two;
    two.content = "x x";
    index.indexDocument(1, processor.analyze(one));
    index.indexDocument(2, processor.analyze(two));
    CollectionStats stats;
    stats.totalDocs = 2;
    stats.avgWeightedDocLength = 1.5;

    const BM25Ranker bm25;
    const double s1 = scoreOf(bm25, index, stats, "x", 1);
    const double s2 = scoreOf(bm25, index, stats, "x", 2);
    MINI_EXPECT_GT(s2, s1);
    MINI_EXPECT_LT(s2, 2.0 * s1);
}

MINI_TEST(bm25_longer_documents_are_normalized_down) {
    // Same tf, longer document -> lower score (b > 0).
    InvertedIndex index;
    Document shortDoc;
    shortDoc.content = "x y";
    Document longDoc;
    longDoc.content = "x y z z z z z z z z z z";
    index.indexDocument(1, processor.analyze(shortDoc));
    index.indexDocument(2, processor.analyze(longDoc));
    CollectionStats stats;
    stats.totalDocs = 2;
    stats.avgWeightedDocLength = 6.5;

    const BM25Ranker bm25;
    MINI_EXPECT_GT(scoreOf(bm25, index, stats, "x", 1),
                   scoreOf(bm25, index, stats, "x", 2));

    // With b = 0 the length penalty disappears (equal tf, equal score).
    const BM25Ranker bm25NoLength(1.2, 0.0);
    MINI_EXPECT_NEAR(scoreOf(bm25NoLength, index, stats, "x", 1),
                     scoreOf(bm25NoLength, index, stats, "x", 2), 1e-12);
}

MINI_TEST(empty_document_scores_zero_without_crashing) {
    const TFIDFRanker tfidf;
    const BM25Ranker bm25;
    const PostingList list;
    const Posting posting;
    const minisearch::DocMeta meta;  // all lengths zero
    const CollectionStats stats{1, 0.0};
    MINI_EXPECT_NEAR(tfidf.scoreTerm("x", list, posting, meta, stats), 0.0, 1e-12);
    MINI_EXPECT_NEAR(bm25.scoreTerm("x", list, posting, meta, stats), 0.0, 1e-12);
}

MINI_TEST(both_rankers_report_their_names) {
    MINI_EXPECT_EQ(std::string(TFIDFRanker{}.name()), "tfidf");
    MINI_EXPECT_EQ(std::string(BM25Ranker{}.name()), "bm25");
}
