// Unit tests for the hand-built inverted index (index/InvertedIndex).

#include "index/InvertedIndex.h"
#include "processor/TextProcessor.h"
#include "test_framework.h"

#include <string>
#include <vector>

namespace {

using minisearch::AnalyzedDocument;
using minisearch::Document;
using minisearch::DocumentId;
using minisearch::InvertedIndex;
using minisearch::TextProcessor;

const TextProcessor processor;

AnalyzedDocument analyze(const std::string& title, const std::string& content) {
    Document doc;
    doc.title = title;
    doc.content = content;
    return processor.analyze(doc);
}

std::vector<DocumentId> docIdsOf(const InvertedIndex& index, const std::string& term) {
    std::vector<DocumentId> ids;
    if (const auto* list = index.findTerm(term)) {
        for (const auto& posting : list->postings) ids.push_back(posting.doc);
    }
    return ids;
}

std::string joinIds(const std::vector<DocumentId>& ids) {
    std::string out;
    for (DocumentId id : ids) {
        if (!out.empty()) out += ',';
        out += std::to_string(id);
    }
    return out;
}

} // namespace

// The exact example from spec section 8:
//   Doc 1: "C++ is a programming language"
//   Doc 2: "Python is a programming language"
//   Doc 3: "C++ is used in competitive programming"
// (Terms shown stem-normalized, as our pipeline produces them.)
MINI_TEST(spec_example_index) {
    InvertedIndex index;
    index.indexDocument(1, analyze("", "C++ is a programming language"));
    index.indexDocument(2, analyze("", "Python is a programming language"));
    index.indexDocument(3, analyze("", "C++ is used in competitive programming"));

    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "cpp")), "1,3");
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "python")), "2");
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "program")), "1,2,3");
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "languag")), "1,2");
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "competitiv")), "3");
    MINI_EXPECT_TRUE(index.findTerm("nonexistent") == nullptr);
}

MINI_TEST(spec_example_positions_and_frequencies) {
    InvertedIndex index;
    index.indexDocument(1, analyze("", "C++ is a programming language"));
    index.indexDocument(2, analyze("", "Python is a programming language"));
    index.indexDocument(3, analyze("", "C++ is used in competitive programming"));

    const auto* program = index.findTerm("program");
    MINI_EXPECT_TRUE(program != nullptr);
    MINI_EXPECT_EQ(program->df(), 3u);

    const auto* posting1 = index.findPosting("program", 1);
    MINI_EXPECT_TRUE(posting1 != nullptr);
    MINI_EXPECT_EQ(posting1->tf_body, 1u);
    MINI_EXPECT_EQ(posting1->positions, std::vector<std::uint32_t>{3});  // "program" is token #3

    const auto* posting3 = index.findPosting("program", 3);
    MINI_EXPECT_TRUE(posting3 != nullptr);
    MINI_EXPECT_EQ(posting3->tf_body, 1u);
}

MINI_TEST(single_document_and_missing_term) {
    InvertedIndex index;
    index.indexDocument(1, analyze("Title", "hello world"));

    MINI_EXPECT_EQ(index.documentCount(), std::size_t{1});
    MINI_EXPECT_EQ(index.termCount(), std::size_t{3});  // titl, hello, world
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "hello")), "1");
    MINI_EXPECT_TRUE(index.findTerm("goodbye") == nullptr);
}

MINI_TEST(empty_document_is_indexed_without_postings) {
    InvertedIndex index;
    index.indexDocument(1, analyze("", ""));

    MINI_EXPECT_EQ(index.documentCount(), std::size_t{1});
    MINI_EXPECT_EQ(index.termCount(), std::size_t{0});
    MINI_EXPECT_EQ(index.totalPostings(), std::size_t{0});
    const auto* meta = index.findDocument(1);
    MINI_EXPECT_TRUE(meta != nullptr);
    MINI_EXPECT_EQ(meta->totalTokens(), 0u);
}

MINI_TEST(repeated_words_accumulate_frequency_and_positions) {
    InvertedIndex index;
    index.indexDocument(1, analyze("", "buffalo buffalo buffalo"));

    const auto* posting = index.findPosting("buffalo", 1);
    MINI_EXPECT_TRUE(posting != nullptr);
    MINI_EXPECT_EQ(posting->tf_body, 3u);
    MINI_EXPECT_EQ(posting->positions, (std::vector<std::uint32_t>{0, 1, 2}));
}

MINI_TEST(reindexing_same_id_updates_without_duplicates) {
    InvertedIndex index;
    index.indexDocument(1, analyze("", "alpha beta"));
    index.indexDocument(1, analyze("", "alpha gamma delta"));

    MINI_EXPECT_EQ(index.documentCount(), std::size_t{1});
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "alpha")), "1");
    MINI_EXPECT_EQ(index.findTerm("beta"), nullptr);          // old term gone
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "gamma")), "1");   // new term present
    const auto* alpha = index.findTerm("alpha");
    MINI_EXPECT_EQ(alpha->df(), 1u);                          // no duplicate posting
    MINI_EXPECT_EQ(alpha->postings.size(), std::size_t{1});
}

MINI_TEST(document_removal) {
    InvertedIndex index;
    index.indexDocument(1, analyze("", "alpha beta"));
    index.indexDocument(2, analyze("", "beta gamma"));
    index.indexDocument(3, analyze("", "gamma delta"));

    MINI_EXPECT_TRUE(index.removeDocument(2));
    MINI_EXPECT_EQ(index.documentCount(), std::size_t{2});
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "beta")), "1");
    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "gamma")), "3");
    MINI_EXPECT_TRUE(index.findTerm("beta") != nullptr);
    MINI_EXPECT_FALSE(index.removeDocument(42));  // never indexed
    MINI_EXPECT_FALSE(index.hasDocument(2));
}

MINI_TEST(posting_lists_stay_sorted_by_doc_id) {
    InvertedIndex index;
    // Insert out of ascending order on purpose.
    index.indexDocument(5, analyze("", "term"));
    index.indexDocument(2, analyze("", "term"));
    index.indexDocument(9, analyze("", "term"));
    index.indexDocument(7, analyze("", "term"));

    MINI_EXPECT_EQ(joinIds(docIdsOf(index, "term")), "2,5,7,9");
}

MINI_TEST(collection_statistics) {
    InvertedIndex index;
    index.indexDocument(1, analyze("cpp", "alpha beta"));        // 3 tokens
    index.indexDocument(2, analyze("", "gamma delta epsilon"));  // 3 tokens

    MINI_EXPECT_EQ(index.documentCount(), std::size_t{2});
    MINI_EXPECT_EQ(index.termCount(), std::size_t{6});
    MINI_EXPECT_EQ(index.totalPostings(), std::size_t{6});
    MINI_EXPECT_EQ(index.tokenSum(), std::uint64_t{6});
    MINI_EXPECT_NEAR(index.averageTokenLength(), 3.0, 1e-9);
}

MINI_TEST(field_frequencies_and_virtual_positions) {
    InvertedIndex index;
    // title: 2 tokens; heading: 2 headings of 1 token; body: 3 tokens.
    Document doc;
    doc.title = "cpp guide";
    doc.headings = {"overview", "usage"};
    doc.content = "cpp is fast";
    index.indexDocument(1, processor.analyze(doc));

    const auto* cpp = index.findPosting("cpp", 1);
    MINI_EXPECT_TRUE(cpp != nullptr);
    MINI_EXPECT_EQ(cpp->tf_title, 1u);
    MINI_EXPECT_EQ(cpp->tf_heading, 0u);
    MINI_EXPECT_EQ(cpp->tf_body, 1u);
    // title ordinal 0 -> virtual 0; body ordinal 0 -> virtual 2+2+2+1 = 7
    MINI_EXPECT_EQ(cpp->positions, (std::vector<std::uint32_t>{0, 7}));

    const auto* meta = index.findDocument(1);
    MINI_EXPECT_EQ(meta->title_len, 2u);
    MINI_EXPECT_EQ(meta->heading_len, 2u);
    MINI_EXPECT_EQ(meta->heading_count, 2u);
    MINI_EXPECT_EQ(meta->body_len, 3u);
    MINI_EXPECT_EQ(meta->headingStartVirtual(), 3u);  // gap after title
    MINI_EXPECT_EQ(meta->headingEndVirtual(), 7u);
    MINI_EXPECT_EQ(meta->bodyStartVirtual(), 7u);     // gap after last heading included
    MINI_EXPECT_TRUE(meta->classifyPosition(1) == minisearch::Field::Title);
    MINI_EXPECT_TRUE(meta->classifyPosition(3) == minisearch::Field::Heading);
    MINI_EXPECT_TRUE(meta->classifyPosition(9) == minisearch::Field::Body);
}

MINI_TEST(heading_positions_include_boundary_gaps) {
    InvertedIndex index;
    Document doc;
    doc.headings = {"alpha beta", "gamma delta"};
    index.indexDocument(1, processor.analyze(doc));

    // Virtual stream: [alpha@0 beta@1 | GAP | gamma@3 delta@4]
    MINI_EXPECT_EQ(index.findPosting("beta", 1)->positions,
                   (std::vector<std::uint32_t>{1}));
    MINI_EXPECT_EQ(index.findPosting("gamma", 1)->positions,
                   (std::vector<std::uint32_t>{3}));
    MINI_EXPECT_EQ(index.findPosting("delta", 1)->positions,
                   (std::vector<std::uint32_t>{4}));
    // Adjacency check that phrase search will rely on: "beta gamma" is NOT
    // consecutive (2 is a gap position), so a phrase cannot match here.
    MINI_EXPECT_NE(index.findPosting("beta", 1)->positions[0] + 1,
                   index.findPosting("gamma", 1)->positions[0]);
}

MINI_TEST(all_document_ids_and_terms_with_prefix) {
    InvertedIndex index;
    index.indexDocument(2, analyze("", "programming programs"));
    index.indexDocument(1, analyze("", "python"));

    MINI_EXPECT_EQ(index.allDocumentIds(), (std::vector<DocumentId>{1, 2}));

    const auto suggestions = index.termsWithPrefix("pro", 5);
    MINI_EXPECT_EQ(suggestions.size(), std::size_t{1});
    MINI_EXPECT_EQ(suggestions[0].first, "program");
    MINI_EXPECT_EQ(suggestions[0].second, 1u);

    MINI_EXPECT_TRUE(index.termsWithPrefix("zzz", 5).empty());
}

MINI_TEST(clear_resets_everything) {
    InvertedIndex index;
    index.indexDocument(1, analyze("", "alpha"));
    index.clear();
    MINI_EXPECT_EQ(index.documentCount(), std::size_t{0});
    MINI_EXPECT_EQ(index.termCount(), std::size_t{0});
    MINI_EXPECT_EQ(index.totalPostings(), std::size_t{0});
    MINI_EXPECT_EQ(index.tokenSum(), std::uint64_t{0});
    MINI_EXPECT_TRUE(index.findTerm("alpha") == nullptr);
}
