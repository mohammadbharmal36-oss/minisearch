#pragma once

// MiniSearch - query language parser (spec module 7).
//
// Grammar (lowest precedence first):
//   or_expr  := and_expr ( OR and_expr )*
//   and_expr := not_expr ( [AND] not_expr )*     // implicit AND
//   not_expr := primary ( NOT primary )*
//   primary  := '(' or_expr ')' | '"' phrase '"' | TERM
//
// The parser is a hand-written recursive-descent parser over a lexed
// token stream. Terms are normalized with the same TextProcessor used
// for indexing, so queries and documents share one vocabulary.
// Operator words (AND/OR/NOT) are recognized case-insensitively; quoted
// phrases keep stop words because positions are indexed for them.

#include "processor/TextProcessor.h"

#include <memory>
#include <string>
#include <vector>

namespace minisearch::query {

struct QueryNode {
    enum class Kind { Term, Phrase, And, Or, Not };

    Kind kind = Kind::Term;
    std::vector<std::string> terms;                      // Term: single entry
    std::vector<std::unique_ptr<QueryNode>> children;    // And/Or/Not

    explicit QueryNode(Kind k) : kind(k) {}
};

struct ParseResult {
    bool ok = false;
    std::string error;
    std::unique_ptr<QueryNode> root;
    std::string rootString;  // human-readable tree, for tests and debugging
};

class QueryParser {
public:
    // Raw lexical tokens - public so tests can verify lexing directly.
    struct RawToken {
        enum class Type { Word, Quoted, LParen, RParen, And, Or, Not };
        Type type = Type::Word;
        std::string text;                     // original surface form
        std::vector<std::string> words;       // normalized terms (Word/Quoted)
    };

    explicit QueryParser(const TextProcessor& processor) : processor_(processor) {}

    // Returns a parse tree, or ok=false with a human-readable error.
    ParseResult parse(const std::string& query) const;

    std::vector<RawToken> lex(const std::string& query, bool* unbalancedQuote = nullptr) const;

private:
    const TextProcessor& processor_;
};

} // namespace minisearch::query
