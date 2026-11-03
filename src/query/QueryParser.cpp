#include "query/QueryParser.h"

#include <cctype>

namespace minisearch::query {

namespace {

bool equalsIgnoreCase(const std::string& text, const char* lower) {
    const std::size_t len = std::string(lower).size();
    if (text.size() != len) return false;
    for (std::size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != lower[i]) return false;
    }
    return true;
}

std::vector<std::string> normalizeWords(const TextProcessor& processor,
                                        const std::string& raw) {
    return processor.tokenizeQuery(raw);
}

} // namespace

std::vector<QueryParser::RawToken> QueryParser::lex(const std::string& query,
                                                    bool* unbalancedQuote) const {
    std::vector<RawToken> tokens;
    if (unbalancedQuote != nullptr) *unbalancedQuote = false;

    const std::size_t n = query.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = query[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '(') {
            RawToken token;
            token.type = RawToken::Type::LParen;
            token.text = "(";
            tokens.push_back(std::move(token));
            ++i;
            continue;
        }
        if (c == ')') {
            RawToken token;
            token.type = RawToken::Type::RParen;
            token.text = ")";
            tokens.push_back(std::move(token));
            ++i;
            continue;
        }
        if (c == '"') {
            const std::size_t close = query.find('"', i + 1);
            if (close == std::string::npos) {
                if (unbalancedQuote != nullptr) *unbalancedQuote = true;
                i = n;  // parser will surface the error
                break;
            }
            RawToken token;
            token.type = RawToken::Type::Quoted;
            token.text = query.substr(i + 1, close - i - 1);
            token.words = normalizeWords(processor_, token.text);
            tokens.push_back(std::move(token));
            i = close + 1;
            continue;
        }
        // Plain word: run of non-space, non-quote, non-paren characters.
        std::size_t j = i + 1;
        while (j < n) {
            const char d = query[j];
            if (std::isspace(static_cast<unsigned char>(d)) || d == '"' || d == '(' || d == ')') {
                break;
            }
            ++j;
        }
        RawToken token;
        token.text = query.substr(i, j - i);
        if (equalsIgnoreCase(token.text, "and")) {
            token.type = RawToken::Type::And;
        } else if (equalsIgnoreCase(token.text, "or")) {
            token.type = RawToken::Type::Or;
        } else if (equalsIgnoreCase(token.text, "not")) {
            token.type = RawToken::Type::Not;
        } else {
            token.type = RawToken::Type::Word;
            token.words = normalizeWords(processor_, token.text);
        }
        tokens.push_back(std::move(token));
        i = j;
    }
    return tokens;
}

namespace {

// Recursive-descent parser over the token vector.
class Parser {
public:
    Parser(const std::vector<QueryParser::RawToken>& tokens) : tokens_(tokens) {}

    ParseResult run() {
        ParseResult result;
        if (tokens_.empty()) {
            result.error = "empty query";
            return result;
        }
        std::unique_ptr<QueryNode> root = parseOr();
        if (root == nullptr || pos_ != tokens_.size()) {
            result.error = error_.empty() ? "unexpected token" : error_;
            return result;
        }
        result.ok = true;
        result.root = std::move(root);
        return result;
    }

private:
    bool atEnd() const { return pos_ >= tokens_.size(); }
    const QueryParser::RawToken& peek() const { return tokens_[pos_]; }

    bool consumeIf(QueryParser::RawToken::Type type) {
        if (!atEnd() && peek().type == type) {
            ++pos_;
            return true;
        }
        return false;
    }

    // A primary can START here (used for implicit-AND detection).
    bool startsPrimary() const {
        return !atEnd()
               && (peek().type == QueryParser::RawToken::Type::Word
                   || peek().type == QueryParser::RawToken::Type::Quoted
                   || peek().type == QueryParser::RawToken::Type::LParen);
    }

    void fail(const std::string& message) {
        if (error_.empty()) error_ = message;
    }

    std::unique_ptr<QueryNode> parseOr() {
        std::unique_ptr<QueryNode> first = parseAnd();
        if (first == nullptr) return nullptr;
        std::vector<std::unique_ptr<QueryNode>> parts;
        parts.push_back(std::move(first));
        while (consumeIf(QueryParser::RawToken::Type::Or)) {
            std::unique_ptr<QueryNode> next = parseAnd();
            if (next == nullptr) return nullptr;
            parts.push_back(std::move(next));
        }
        if (parts.size() == 1) return std::move(parts.front());
        auto node = std::make_unique<QueryNode>(QueryNode::Kind::Or);
        node->children = std::move(parts);
        return node;
    }

    std::unique_ptr<QueryNode> parseAnd() {
        std::unique_ptr<QueryNode> first = parseNot();
        if (first == nullptr) return nullptr;
        std::vector<std::unique_ptr<QueryNode>> parts;
        parts.push_back(std::move(first));
        while (true) {
            const bool explicitAnd =
                consumeIf(QueryParser::RawToken::Type::And);
            if (!explicitAnd && !startsPrimary()) break;
            if (explicitAnd && !startsPrimary()) {
                fail("AND requires a following term");
                return nullptr;
            }
            std::unique_ptr<QueryNode> next = parseNot();
            if (next == nullptr) return nullptr;
            parts.push_back(std::move(next));
        }
        if (parts.size() == 1) return std::move(parts.front());
        auto node = std::make_unique<QueryNode>(QueryNode::Kind::And);
        node->children = std::move(parts);
        return node;
    }

    // not_expr := primary ( NOT primary )*  ->  And(primary, Not(operand), ...)
    std::unique_ptr<QueryNode> parseNot() {
        std::unique_ptr<QueryNode> first = parsePrimary();
        if (first == nullptr) return nullptr;
        std::vector<std::unique_ptr<QueryNode>> parts;
        parts.push_back(std::move(first));
        while (consumeIf(QueryParser::RawToken::Type::Not)) {
            if (!startsPrimary()) {
                fail("NOT requires a following term");
                return nullptr;
            }
            std::unique_ptr<QueryNode> operand = parsePrimary();
            if (operand == nullptr) return nullptr;
            auto negated = std::make_unique<QueryNode>(QueryNode::Kind::Not);
            negated->children.push_back(std::move(operand));
            parts.push_back(std::move(negated));
        }
        if (parts.size() == 1) return std::move(parts.front());
        auto node = std::make_unique<QueryNode>(QueryNode::Kind::And);
        node->children = std::move(parts);
        return node;
    }

    std::unique_ptr<QueryNode> parsePrimary() {
        if (atEnd()) {
            fail("unexpected end of query");
            return nullptr;
        }
        const auto& token = peek();
        switch (token.type) {
            case QueryParser::RawToken::Type::LParen: {
                ++pos_;
                std::unique_ptr<QueryNode> inner = parseOr();
                if (inner == nullptr) return nullptr;
                if (!consumeIf(QueryParser::RawToken::Type::RParen)) {
                    fail("missing closing parenthesis");
                    return nullptr;
                }
                return inner;
            }
            case QueryParser::RawToken::Type::Quoted: {
                ++pos_;
                if (token.words.empty()) {
                    fail("empty phrase");
                    return nullptr;
                }
                auto node = std::make_unique<QueryNode>(QueryNode::Kind::Phrase);
                node->terms = token.words;
                return node;
            }
            case QueryParser::RawToken::Type::Word: {
                ++pos_;
                if (token.words.empty()) {
                    fail("unrecognized term: '" + token.text + "'");
                    return nullptr;
                }
                auto node = std::make_unique<QueryNode>(QueryNode::Kind::Term);
                node->terms = token.words;  // technical terms may yield one
                return node;
            }
            default:
                fail("unexpected token '" + token.text + "'");
                return nullptr;
        }
    }

    const std::vector<QueryParser::RawToken>& tokens_;
    std::size_t pos_ = 0;
    std::string error_;
};

std::string describe(const QueryNode& node) {
    switch (node.kind) {
        case QueryNode::Kind::Term:
        case QueryNode::Kind::Phrase: {
            std::string out = node.kind == QueryNode::Kind::Phrase ? "\"" : "";
            for (std::size_t i = 0; i < node.terms.size(); ++i) {
                if (i > 0) out += node.kind == QueryNode::Kind::Phrase ? " " : "|";
                out += node.terms[i];
            }
            if (node.kind == QueryNode::Kind::Phrase) out += "\"";
            return out;
        }
        case QueryNode::Kind::And: {
            std::string out = "AND(";
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                if (i > 0) out += ", ";
                out += describe(*node.children[i]);
            }
            return out + ")";
        }
        case QueryNode::Kind::Or: {
            std::string out = "OR(";
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                if (i > 0) out += ", ";
                out += describe(*node.children[i]);
            }
            return out + ")";
        }
        case QueryNode::Kind::Not: return "NOT(" + describe(*node.children.front()) + ")";
    }
    return "?";
}

} // namespace

ParseResult QueryParser::parse(const std::string& query) const {
    bool unbalancedQuote = false;
    const std::vector<RawToken> tokens = lex(query, &unbalancedQuote);
    if (unbalancedQuote) {
        ParseResult result;
        result.error = "unbalanced quote";
        return result;
    }
    Parser parser(tokens);
    ParseResult result = parser.run();
    result.rootString = result.ok ? describe(*result.root) : std::string();
    return result;
}

} // namespace minisearch::query
