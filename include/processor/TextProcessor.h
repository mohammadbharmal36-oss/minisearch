#pragma once

// MiniSearch - text-processing pipeline (spec module 3).
//
// Turns raw field text (title / headings / body) and raw queries into
// normalized, ordered tokens ready for the inverted index.
//
// Pipeline stages for every token:
//   1. extraction   - protected technical terms are matched first
//                     (longest match, word-boundary aware), everything
//                     else goes through the generic tokenizer
//   2. canonicalize - lowercase, strip apostrophes, map technical
//                     surface forms to canonical terms (C++ -> cpp)
//   3. stem         - light rule-based suffix stemmer (optional),
//                     bypassed for protected terms and digit-containing
//                     tokens
//   4. stop words   - optional removal; ordinals are NOT renumbered so
//                     phrase adjacency semantics stay well defined
//
// Positions are per-field ordinals (each field restarts at 0). The index
// will compose them into a virtual stream "title || headings || body"
// with per-document field offsets - that powers both field-weighted
// ranking (title=3, heading=2, body=1) and boundary-correct phrase
// search in later phases.

#include "common/Document.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace minisearch {

enum class Field : std::uint8_t {
    Title = 0,
    Heading = 1,
    Body = 2,
};

inline const char* toString(Field field) noexcept {
    switch (field) {
        case Field::Title:   return "title";
        case Field::Heading: return "heading";
        case Field::Body:    return "body";
    }
    return "unknown";
}

// One analyzed term occurrence. `position` is the ordinal of the token
// slot inside its own field (gaps are possible when stop-word removal
// is enabled - ordinals refer to the pre-filter sequence on purpose).
struct Token {
    std::string term;
    std::uint32_t position = 0;
    Field field = Field::Body;
};

// Full analysis result for one document. Each vector is ordered by
// ascending position; fields are tokenized independently. Headings are
// kept as per-heading token groups (ordinals restart at 0 within each
// heading) so the index can insert boundary gaps between them.
struct AnalyzedDocument {
    std::vector<Token> title;
    std::vector<std::vector<Token>> headings;
    std::vector<Token> body;

    std::size_t totalTokenCount() const noexcept {
        std::size_t count = title.size() + body.size();
        for (const auto& heading : headings) count += heading.size();
        return count;
    }
};

struct TextProcessorConfig {
    // Index-time stop-word removal is OFF by default: keeping stop words
    // preserves phrase positions, and IDF naturally de-values them at
    // ranking time. The query layer filters stop words from *ranking*
    // terms while keeping them for phrase matching.
    bool removeStopwords = false;

    bool applyStemming = true;
    bool mapTechnicalTerms = true;   // C++ -> cpp, Node.js -> nodejs, ...
};

class TextProcessor {
public:
    TextProcessor() = default;
    explicit TextProcessor(const TextProcessorConfig& config) : config_(config) {}

    // Tokenize one field's raw text; positions restart at 0 per call.
    std::vector<Token> tokenizeField(const std::string& text, Field field) const;

    // Tokenize all fields of a document in one call.
    AnalyzedDocument analyze(const Document& document) const;

    // Tokenize a query string into normalized terms, in order.
    // Deliberately keeps stop words and does not interpret Boolean
    // operators - that is the query parser's job (later phase).
    std::vector<std::string> tokenizeQuery(const std::string& query) const;

    // Canonicalization without stemming: lowercase, strip apostrophes,
    // map protected technical terms. Returns "" for junk tokens.
    std::string canonicalizeTerm(const std::string& rawTerm) const;

    // Light rule-based stemmer (see docs/algorithms.md for the exact
    // rule table). Honours config_.applyStemming; protected canonical
    // terms and tokens containing digits are returned unchanged.
    std::string stem(const std::string& term) const;

    bool isStopword(const std::string& term) const {
        return stopwords().count(term) > 0;
    }

    const TextProcessorConfig& config() const noexcept { return config_; }
    void setConfig(const TextProcessorConfig& config) { config_ = config; }

    // Shared immutable tables (exposed for introspection, the CLI demo
    // and the test suite).
    static const std::unordered_set<std::string>& stopwords();
    // Surface form -> canonical term, sorted longest-surface-first.
    static const std::vector<std::pair<std::string, std::string>>& technicalTerms();

private:
    // Raw term extraction from text (protected matching + generic scan).
    std::vector<std::string> extractRawTerms(const std::string& text) const;

    TextProcessorConfig config_;
};

} // namespace minisearch
