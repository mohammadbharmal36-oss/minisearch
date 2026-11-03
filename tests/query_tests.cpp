// Unit tests for the query parser and evaluator (query/QueryParser,
// query/QueryProcessor).

#include "index/InvertedIndex.h"
#include "processor/TextProcessor.h"
#include "query/QueryParser.h"
#include "query/QueryProcessor.h"
#include "test_framework.h"

#include <string>
#include <vector>

namespace {

using minisearch::Document;
using minisearch::DocumentId;
using minisearch::InvertedIndex;
using minisearch::QueryProcessor;
using minisearch::TextProcessor;
using minisearch::query::QueryParser;
using minisearch::query::QueryNode;
using RawType = QueryParser::RawToken::Type;

const TextProcessor processor;

std::string joinIds(const std::vector<DocumentId>& ids) {
    std::string out;
    for (DocumentId id : ids) {
        if (!out.empty()) out += ',';
        out += std::to_string(id);
    }
    return out;
}

// Fixture corpus (default text processing):
//   D1: title "Cpp Guide",            body "cpp is used in competitive programming"
//   D2: title "Python Guide",          body "python is a programming language"
//   D3: title "Sql Guide",             body "sql queries store data in tables"
//   D4: title "",                      body "the state of the art in search"
struct Fixture {
    InvertedIndex index;
    QueryParser parser{processor};
    QueryProcessor evaluator{processor};

    Fixture() {
        Document d1;
        d1.title = "Cpp Guide";
        d1.content = "cpp is used in competitive programming";
        Document d2;
        d2.title = "Python Guide";
        d2.content = "python is a programming language";
        Document d3;
        d3.title = "Sql Guide";
        d3.content = "sql queries store data in tables";
        Document d4;
        d4.content = "the state of the art in search";
        index.indexDocument(1, processor.analyze(d1));
        index.indexDocument(2, processor.analyze(d2));
        index.indexDocument(3, processor.analyze(d3));
        index.indexDocument(4, processor.analyze(d4));
    }
};

std::vector<DocumentId> evalDocs(const Fixture& fx, const std::string& query) {
    const auto parsed = fx.parser.parse(query);
    if (!parsed.ok) return {};
    return fx.evaluator.evaluate(*parsed.root, fx.index).candidates;
}

} // namespace

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

MINI_TEST(lexer_splits_words_quotes_and_parens) {
    const QueryParser parser(processor);
    const auto tokens = parser.lex("cpp AND (python OR \"sql guide\")");

    MINI_EXPECT_EQ(tokens.size(), std::size_t{7});
    MINI_EXPECT_TRUE(tokens[0].type == RawType::Word && tokens[0].text == "cpp");
    MINI_EXPECT_TRUE(tokens[1].type == RawType::And);
    MINI_EXPECT_TRUE(tokens[2].type == RawType::LParen);
    MINI_EXPECT_TRUE(tokens[3].type == RawType::Word && tokens[3].text == "python");
    MINI_EXPECT_TRUE(tokens[4].type == RawType::Or);
    MINI_EXPECT_TRUE(tokens[5].type == RawType::Quoted && tokens[5].text == "sql guide");
    MINI_EXPECT_TRUE(tokens[6].type == RawType::RParen);
}

MINI_TEST(lexer_normalizes_terms) {
    const QueryParser parser(processor);
    const auto tokens = parser.lex("C++ Node.js");
    MINI_EXPECT_EQ(tokens.size(), std::size_t{2});
    MINI_EXPECT_EQ(tokens[0].words, (std::vector<std::string>{"cpp"}));
    MINI_EXPECT_EQ(tokens[1].words, (std::vector<std::string>{"nodejs"}));
}

MINI_TEST(lexer_operators_case_insensitive) {
    const QueryParser parser(processor);
    const auto tokens = parser.lex("a and b or not c");
    MINI_EXPECT_TRUE(tokens[1].type == RawType::And);
    MINI_EXPECT_TRUE(tokens[3].type == RawType::Or);
    MINI_EXPECT_TRUE(tokens[4].type == RawType::Not);
}

MINI_TEST(lexer_detects_unbalanced_quote) {
    const QueryParser parser(processor);
    bool unbalanced = false;
    parser.lex("\"unclosed phrase", &unbalanced);
    MINI_EXPECT_TRUE(unbalanced);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

MINI_TEST(parse_simple_keyword_query) {
    const QueryParser parser(processor);
    const auto result = parser.parse("cpp programming");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.rootString, "AND(cpp, program)");
}

MINI_TEST(parse_boolean_precedence) {
    const QueryParser parser(processor);
    // OR binds looser than AND: a OR b AND c == a OR (b AND c)
    const auto result = parser.parse("cpp OR python AND sql");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.rootString, "OR(cpp, AND(python, sql))");
}

MINI_TEST(parse_not_operator) {
    const QueryParser parser(processor);
    const auto result = parser.parse("programming NOT sql");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.rootString, "AND(program, NOT(sql))");
}

MINI_TEST(parse_parentheses_group) {
    const QueryParser parser(processor);
    const auto result = parser.parse("(cpp OR python) NOT sql");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.rootString, "AND(OR(cpp, python), NOT(sql))");
}

MINI_TEST(parse_phrase) {
    const QueryParser parser(processor);
    const auto result = parser.parse("\"competitive programming\"");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.rootString, "\"competitiv program\"");
}

MINI_TEST(parse_phrase_keeps_stop_words) {
    const QueryParser parser(processor);
    const auto result = parser.parse("\"state of the art\"");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.rootString, "\"stat of the art\"");
}

MINI_TEST(parse_case_insensitive_operators_and_terms) {
    const QueryParser parser(processor);
    const auto result = parser.parse("C++ and PYTHON");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.rootString, "AND(cpp, python)");
}

MINI_TEST(parse_errors) {
    const QueryParser parser(processor);
    MINI_EXPECT_FALSE(parser.parse("").ok);
    MINI_EXPECT_FALSE(parser.parse("   ").ok);
    MINI_EXPECT_FALSE(parser.parse("NOT cpp").ok);               // no left operand
    MINI_EXPECT_FALSE(parser.parse("cpp AND").ok);               // dangling AND
    MINI_EXPECT_FALSE(parser.parse("cpp OR").ok);                // dangling OR
    MINI_EXPECT_FALSE(parser.parse("(cpp OR python").ok);        // unbalanced paren
    MINI_EXPECT_FALSE(parser.parse("cpp OR python)").ok);        // unbalanced paren
    MINI_EXPECT_FALSE(parser.parse("\"unclosed").ok);            // unbalanced quote
    MINI_EXPECT_FALSE(parser.parse("\"\"").ok);                  // empty phrase
    MINI_EXPECT_FALSE(parser.parse("!!!").ok);                   // junk term
}

// ---------------------------------------------------------------------------
// Set operations (two-pointer)
// ---------------------------------------------------------------------------

MINI_TEST(set_operations) {
    using Ids = std::vector<DocumentId>;
    const Ids a{1, 3, 5, 7};
    const Ids b{3, 4, 5, 8};

    MINI_EXPECT_EQ(QueryProcessor::intersect(a, b), (Ids{3, 5}));
    MINI_EXPECT_EQ(QueryProcessor::unionOf(a, b), (Ids{1, 3, 4, 5, 7, 8}));
    MINI_EXPECT_EQ(QueryProcessor::subtract(a, b), (Ids{1, 7}));
    MINI_EXPECT_EQ(QueryProcessor::subtract(a, Ids{}), a);
    MINI_EXPECT_EQ(QueryProcessor::intersect(a, Ids{}), Ids{});
    MINI_EXPECT_EQ(QueryProcessor::unionOf(a, Ids{}), a);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

MINI_TEST(evaluate_single_term) {
    const Fixture fx;
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "cpp")), "1");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "python")), "2");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "nonexistent")), "");
}

MINI_TEST(evaluate_implicit_and_explicit_and) {
    const Fixture fx;
    // "program" appears in D1 and D2; "cpp" only in D1 -> intersection = D1.
    // Implicit AND and explicit AND must behave identically.
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "cpp program")), "1");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "cpp AND program")), "1");
}

MINI_TEST(evaluate_or) {
    const Fixture fx;
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "cpp OR python")), "1,2");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "cpp OR sql")), "1,3");
}

MINI_TEST(evaluate_not) {
    const Fixture fx;
    // program -> D1, D2; minus sql docs (D3) -> still D1, D2.
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "program NOT sql")), "1,2");
    // program -> D1, D2; minus python docs (D2) -> D1.
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "program NOT python")), "1");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "cpp NOT programming")), "");
}

MINI_TEST(evaluate_missing_terms_yield_empty_not_error) {
    const Fixture fx;
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "nonexistent AND cpp")), "");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "nonexistent OR cpp")), "1");
}

MINI_TEST(evaluate_phrase_adjacency) {
    const Fixture fx;
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "\"competitive programming\"")), "1");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "\"programming competitive\"")), "");  // order matters
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "\"programming language\"")), "2");
}

MINI_TEST(evaluate_phrase_with_stop_words) {
    const Fixture fx;
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "\"state of the art\"")), "4");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "\"state the art\"")), "");  // wrong adjacency
}

MINI_TEST(evaluate_phrase_does_not_cross_field_boundaries) {
    // D2 title "Python Guide" ends with "guide"; body begins "python".
    // A title/body phrase must NOT match even though the words are
    // "logically adjacent" in reading order.
    const Fixture fx;
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "\"guide python\"")), "");
    // Both words present as individual terms, so this is not an index gap:
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "guide AND python")), "2");
}

MINI_TEST(evaluate_phrase_matches_inside_title) {
    const Fixture fx;
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "\"cpp guide\"")), "1");
}

MINI_TEST(evaluate_case_insensitive_query) {
    const Fixture fx;
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "CPP")), "1");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "Python OR SQL")), "2,3");
}

MINI_TEST(scoring_terms_track_positive_terms_only) {
    const Fixture fx;
    const auto parsed = fx.parser.parse("program NOT sql");
    MINI_EXPECT_TRUE(parsed.ok);
    const auto evaluation = fx.evaluator.evaluate(*parsed.root, fx.index);
    MINI_EXPECT_TRUE(evaluation.ok);
    MINI_EXPECT_EQ(evaluation.scoringTerms, (std::vector<std::string>{"program"}));

    const auto parsedOr = fx.parser.parse("cpp OR (python AND program)");
    MINI_EXPECT_TRUE(parsedOr.ok);
    const auto evaluationOr = fx.evaluator.evaluate(*parsedOr.root, fx.index);
    MINI_EXPECT_EQ(evaluationOr.scoringTerms,
                   (std::vector<std::string>{"cpp", "python", "program"}));
}

MINI_TEST(evaluate_combined_boolean_and_phrase) {
    const Fixture fx;
    // Docs matching phrase "competitive programming" OR sql docs.
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "\"competitive programming\" OR sql")), "1,3");
    // cpp docs that do NOT match the phrase.
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "cpp NOT \"programming language\"")), "1");
    MINI_EXPECT_EQ(joinIds(evalDocs(fx, "program NOT \"programming language\"")), "1");
}
