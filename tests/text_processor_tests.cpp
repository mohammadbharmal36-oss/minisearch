// Unit tests for the text-processing pipeline (processor/TextProcessor).
//
// Every test runs the real library pipeline; expected values were derived
// by hand from the rule tables in docs/algorithms.md.

#include "processor/TextProcessor.h"
#include "test_framework.h"

#include <string>
#include <vector>

namespace {

using minisearch::Field;
using minisearch::TextProcessor;
using minisearch::TextProcessorConfig;

std::vector<std::string> termsOf(const TextProcessor& processor, const std::string& text) {
    std::vector<std::string> terms;
    for (const auto& token : processor.tokenizeField(text, Field::Body)) {
        terms.push_back(token.term);
    }
    return terms;
}

std::string join(const std::vector<std::string>& terms) {
    std::string out;
    for (const auto& term : terms) {
        if (!out.empty()) out += ' ';
        out += term;
    }
    return out;
}

std::string joinTerms(const std::vector<minisearch::Token>& tokens) {
    std::string out;
    for (const auto& token : tokens) {
        if (!out.empty()) out += ' ';
        out += token.term;
    }
    return out;
}

std::string joinPositions(const std::vector<minisearch::Token>& tokens) {
    std::string out;
    for (const auto& token : tokens) {
        if (!out.empty()) out += ',';
        out += std::to_string(token.position);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Basic tokenization
// ---------------------------------------------------------------------------

MINI_TEST(tokenizes_basic_sentence) {
    const TextProcessor processor;
    const auto tokens = processor.tokenizeField("The quick brown fox", Field::Body);
    MINI_EXPECT_EQ(joinTerms(tokens), "the quick brown fox");
    MINI_EXPECT_EQ(joinPositions(tokens), "0,1,2,3");
}

MINI_TEST(lowercases_and_strips_punctuation) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "Hello, WORLD! (multi-word) test.")),
                   "hello world multi word test");
}

MINI_TEST(normalizes_whitespace_and_empty_input) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "  spaced\t\tout\n\ntext  ")), "spac out text");
    MINI_EXPECT_TRUE(termsOf(processor, "").empty());
    MINI_EXPECT_TRUE(termsOf(processor, "   \t \n ").empty());
    MINI_EXPECT_TRUE(termsOf(processor, "!!! ??? *** ...").empty());
}

MINI_TEST(colon_separated_identifiers_split) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "std::vector and std::map")),
                   "std vector and std map");
}

MINI_TEST(snake_case_identifiers_survive) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "std_vector and my_func")),
                   "std_vector and my_func");
}

MINI_TEST(apostrophes_are_stripped) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "don't stop it's")),
                   "dont stop its");
}

// ---------------------------------------------------------------------------
// Technical-term preservation (spec: never destroy C++, C#, Node.js, ...)
// ---------------------------------------------------------------------------

MINI_TEST(preserves_technical_terms) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "C++ C# Node.js .NET")),
                   "cpp csharp nodejs dotnet");
}

MINI_TEST(technical_aliases_map_to_same_canonical_terms) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor,
                   "cpp csharp fsharp nodejs dotnet aspnet vbnet objectivec golang redis sqlite")),
                   "cpp csharp fsharp nodejs dotnet aspnet vbnet objectivec golang redis sqlite");
}

MINI_TEST(mixed_case_technical_terms) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "NODE.JS c++ F#")),
                   "nodejs cpp fsharp");
}

MINI_TEST(asp_and_vb_net_detected) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "ASP.NET and VB.NET rock")),
                   "aspnet and vbnet rock");
}

MINI_TEST(objective_c_detected) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "Objective-C rocks")),
                   "objectivec rock");
}

MINI_TEST(protected_match_respects_boundaries) {
    const TextProcessor processor;
    // "cppreference" must not be matched as "cpp"; "node.jsx" must not be
    // matched as "node.js"; "foo.net" is a domain, not ".NET".
    MINI_EXPECT_EQ(join(termsOf(processor, "cppreference node.jsx foo.net")),
                   "cppreferenc node jsx foo net");  // e-drop: cppreference -> cppreferenc
}

MINI_TEST(technical_terms_adjacent_to_punctuation) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "(C++), [Node.js];")),
                   "cpp nodejs");
}

// ---------------------------------------------------------------------------
// Numbers and identifiers
// ---------------------------------------------------------------------------

MINI_TEST(decimals_and_versions_preserved) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "get 3.14 now")),
                   "get 3.14 now");
    MINI_EXPECT_EQ(join(termsOf(processor, "upgrade 1.2.3 ok")),
                   "upgrad 1.2.3 ok");  // "upgrade" -> "upgrad" via final-e rule
}

MINI_TEST(digits_bypass_the_stemmer) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(processor.stem("python3"), "python3");
    MINI_EXPECT_EQ(processor.stem("2024"), "2024");
}

// ---------------------------------------------------------------------------
// Stemmer
// ---------------------------------------------------------------------------

MINI_TEST(stemming_unifies_common_inflections) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(processor.stem("programming"), "program");
    MINI_EXPECT_EQ(processor.stem("programs"), "program");
    MINI_EXPECT_EQ(processor.stem("program"), "program");
    MINI_EXPECT_EQ(processor.stem("running"), "run");
    MINI_EXPECT_EQ(processor.stem("runs"), "run");
    MINI_EXPECT_EQ(processor.stem("run"), "run");
    MINI_EXPECT_EQ(processor.stem("parsed"), "pars");
    MINI_EXPECT_EQ(processor.stem("parsing"), "pars");
    MINI_EXPECT_EQ(processor.stem("parse"), "pars");
    MINI_EXPECT_EQ(processor.stem("installing"), "install");
    MINI_EXPECT_EQ(processor.stem("installs"), "install");
}

MINI_TEST(stemming_plural_rules) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(processor.stem("classes"), "class");
    MINI_EXPECT_EQ(processor.stem("cities"), "city");
    MINI_EXPECT_EQ(processor.stem("matches"), "match");
    MINI_EXPECT_EQ(processor.stem("boxes"), "box");
    MINI_EXPECT_EQ(processor.stem("notes"), "note");
    MINI_EXPECT_EQ(processor.stem("languages"), "languag");
    MINI_EXPECT_EQ(processor.stem("language"), "languag");
    MINI_EXPECT_EQ(processor.stem("status"), "status");   // -us guard
    MINI_EXPECT_EQ(processor.stem("basis"), "basis");     // -is guard
    MINI_EXPECT_EQ(processor.stem("bliss"), "bliss");     // -ss guard
}

MINI_TEST(stemming_bypasses_protected_and_short_terms) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(processor.stem("cpp"), "cpp");
    MINI_EXPECT_EQ(processor.stem("nodejs"), "nodejs");
    MINI_EXPECT_EQ(processor.stem("redis"), "redis");
    MINI_EXPECT_EQ(processor.stem("sqlite"), "sqlite");
    MINI_EXPECT_EQ(processor.stem("sql"), "sql");     // below min length
    MINI_EXPECT_EQ(processor.stem("c"), "c");
    MINI_EXPECT_EQ(processor.stem("go"), "go");
}

MINI_TEST(stemming_can_be_disabled) {
    TextProcessorConfig config;
    config.applyStemming = false;
    const TextProcessor processor(config);
    MINI_EXPECT_EQ(processor.stem("programming"), "programming");
    MINI_EXPECT_EQ(join(termsOf(processor, "running programs")), "running programs");
}

MINI_TEST(technical_mapping_can_be_disabled) {
    TextProcessorConfig config;
    config.mapTechnicalTerms = false;
    const TextProcessor processor(config);
    MINI_EXPECT_EQ(join(termsOf(processor, "C++ rocks")), "c rock");
}

// ---------------------------------------------------------------------------
// Stop words
// ---------------------------------------------------------------------------

MINI_TEST(stopwords_kept_by_default) {
    const TextProcessor processor;
    const auto tokens = processor.tokenizeField("the cat and the hat", Field::Body);
    MINI_EXPECT_EQ(joinTerms(tokens), "the cat and the hat");
    MINI_EXPECT_EQ(joinPositions(tokens), "0,1,2,3,4");
}

MINI_TEST(stopword_removal_preserves_original_ordinals) {
    TextProcessorConfig config;
    config.removeStopwords = true;
    const TextProcessor processor(config);
    const auto tokens = processor.tokenizeField("the cat and the hat", Field::Body);
    MINI_EXPECT_EQ(joinTerms(tokens), "cat hat");
    MINI_EXPECT_EQ(joinPositions(tokens), "1,4");  // gaps are intentional
}

MINI_TEST(stopword_list_is_accessible) {
    MINI_EXPECT_TRUE(TextProcessor::stopwords().count("the") > 0);
    MINI_EXPECT_TRUE(TextProcessor::stopwords().count("and") > 0);
    MINI_EXPECT_TRUE(TextProcessor::stopwords().count("python") == 0);
}

// ---------------------------------------------------------------------------
// Fields and positions
// ---------------------------------------------------------------------------

MINI_TEST(analyze_tokenizes_fields_separately) {
    const TextProcessor processor;

    minisearch::Document doc;
    doc.title = "C++ Programming";
    doc.headings = {"Overview"};
    doc.content = "C++ is fun";

    const auto analyzed = processor.analyze(doc);

    MINI_EXPECT_EQ(joinTerms(analyzed.title), "cpp program");
    MINI_EXPECT_EQ(joinPositions(analyzed.title), "0,1");
    MINI_EXPECT_TRUE(!analyzed.title.empty() && analyzed.title[0].field == Field::Title);

    MINI_EXPECT_EQ(joinTerms(analyzed.headings[0]), "overview");
    MINI_EXPECT_TRUE(!analyzed.headings.empty()
                     && !analyzed.headings[0].empty()
                     && analyzed.headings[0][0].field == Field::Heading);

    MINI_EXPECT_EQ(joinTerms(analyzed.body), "cpp is fun");
    MINI_EXPECT_EQ(joinPositions(analyzed.body), "0,1,2");
    MINI_EXPECT_TRUE(!analyzed.body.empty() && analyzed.body[0].field == Field::Body);

    MINI_EXPECT_EQ(analyzed.totalTokenCount(), std::size_t{6});
}

MINI_TEST(analyze_tokenizes_each_heading_separately) {
    const TextProcessor processor;

    minisearch::Document doc;
    doc.headings = {"Getting Started", "Advanced Usage"};

    const auto analyzed = processor.analyze(doc);
    MINI_EXPECT_EQ(analyzed.headings.size(), std::size_t{2});
    MINI_EXPECT_EQ(joinTerms(analyzed.headings[0]), "get start");
    MINI_EXPECT_EQ(joinPositions(analyzed.headings[0]), "0,1");
    MINI_EXPECT_EQ(joinTerms(analyzed.headings[1]), "advanc usag");
    MINI_EXPECT_EQ(joinPositions(analyzed.headings[1]), "0,1");  // ordinals restart per heading
}

// ---------------------------------------------------------------------------
// Query tokenization
// ---------------------------------------------------------------------------

MINI_TEST(query_tokenization_normalizes_and_orders) {
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(processor.tokenizeQuery("  C++   programming ")),
                   "cpp program");
}

MINI_TEST(query_tokenization_keeps_stopwords) {
    // Boolean operator interpretation happens in the query parser
    // (later phase); here we only verify raw term extraction.
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(processor.tokenizeQuery("the best of times")),
                   "the best of time");
    MINI_EXPECT_EQ(join(processor.tokenizeQuery("C++ AND Python")),
                   "cpp and python");
}

// ---------------------------------------------------------------------------
// Documented limitations (pinned so regressions are visible)
// ---------------------------------------------------------------------------

MINI_TEST(unicode_degrades_to_ascii_fragments) {
    // Current limitation: non-ASCII bytes act as separators. "café" (with
    // a UTF-8 accented e) yields "caf". Tracked as a future improvement
    // in the README; this test pins the current, documented behaviour.
    const TextProcessor processor;
    MINI_EXPECT_EQ(join(termsOf(processor, "caf\xc3\xa9 cake")), "caf cake");
}

MINI_TEST(technical_terms_table_is_introspectable) {
    bool hasCpp = false;
    bool hasNet = false;
    for (const auto& [surface, canonical] : TextProcessor::technicalTerms()) {
        if (surface == "c++" && canonical == "cpp") hasCpp = true;
        if (surface == ".net" && canonical == "dotnet") hasNet = true;
    }
    MINI_EXPECT_TRUE(hasCpp);
    MINI_EXPECT_TRUE(hasNet);
}
