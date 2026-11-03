// Unit tests for the core document model (common/Document).

#include "common/Document.h"
#include "test_framework.h"

namespace {

minisearch::Document makeSampleDocument() {
    minisearch::Document doc;
    doc.id = 7;
    doc.url = "https://example.org/article";
    doc.title = "Sample Article";
    doc.description = "A sample page about search engines";
    doc.content = "Body text of the sample article.";
    doc.headings = {"Intro", "Details"};
    doc.links = {"https://example.org/a", "https://example.org/b", "https://example.org/c"};
    doc.http_status = 200;
    doc.crawled_at = 1730000000;
    return doc;
}

} // namespace

MINI_TEST(default_document_is_invalid) {
    const minisearch::Document doc;
    MINI_EXPECT_TRUE(doc.id == -1);
    MINI_EXPECT_TRUE(doc.url.empty());
    MINI_EXPECT_TRUE(!doc.hasValidIdentity());
}

MINI_TEST(document_equality_compares_all_fields) {
    const minisearch::Document a = makeSampleDocument();
    minisearch::Document b = makeSampleDocument();
    MINI_EXPECT_TRUE(a == b);
    MINI_EXPECT_TRUE(!(a != b));

    b.title = "Changed Title";
    MINI_EXPECT_TRUE(a != b);

    b = makeSampleDocument();
    b.links.push_back("https://example.org/d");
    MINI_EXPECT_TRUE(a != b);
}

MINI_TEST(outgoing_link_count_matches_links_vector) {
    const minisearch::Document doc = makeSampleDocument();
    MINI_EXPECT_EQ(doc.outgoingLinkCount(), std::size_t{3});
}

MINI_TEST(valid_identity_requires_id_and_url) {
    minisearch::Document doc = makeSampleDocument();
    MINI_EXPECT_TRUE(doc.hasValidIdentity());

    doc.id = -1;
    MINI_EXPECT_TRUE(!doc.hasValidIdentity());

    doc = makeSampleDocument();
    doc.url.clear();
    MINI_EXPECT_TRUE(!doc.hasValidIdentity());
}

MINI_TEST(debug_string_contains_core_fields) {
    const minisearch::Document doc = makeSampleDocument();
    const std::string debug = doc.toDebugString();
    MINI_EXPECT_TRUE(debug.find("id=7") != std::string::npos);
    MINI_EXPECT_TRUE(debug.find("https://example.org/article") != std::string::npos);
    MINI_EXPECT_TRUE(debug.find("Sample Article") != std::string::npos);
    MINI_EXPECT_TRUE(debug.find("outgoing_links=3") != std::string::npos);
    MINI_EXPECT_TRUE(debug.find("http_status=200") != std::string::npos);
}

MINI_TEST(debug_string_truncates_long_text) {
    minisearch::Document doc;
    doc.id = 1;
    // 97 characters: over the 80-char truncation threshold.
    doc.url = "https://example.org/very/long/path/that/goes/on/and/on/and/on/and/on/and/on/and/on/forever";
    const std::string debug = doc.toDebugString();
    MINI_EXPECT_TRUE(debug.find("forever") == std::string::npos);   // truncated away
    MINI_EXPECT_TRUE(debug.find("...") != std::string::npos);       // truncation marker
}
