#include "processor/TextProcessor.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace minisearch {

namespace {

// ---------------------------------------------------------------------------
// Character classification (ASCII only - documented limitation; see
// docs/algorithms.md "Unicode". Non-ASCII bytes act as separators.)
// ---------------------------------------------------------------------------

char toLowerAscii(char c) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 'A' && u <= 'Z') return static_cast<char>(u + 32);
    return c;
}

bool isAsciiAlnum(char c) {
    const auto u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9');
}

// Word characters for the generic tokenizer: letters, digits and
// underscore (so identifiers like std_vector survive as single terms).
bool isWordChar(char c) {
    return isAsciiAlnum(c) || c == '_';
}

bool isConsonantLetter(char c) {
    switch (c) {
        case 'a': case 'e': case 'i': case 'o': case 'u':
            return false;
        default:
            return c >= 'a' && c <= 'z';
    }
}

bool equalsCaseInsensitive(const std::string& text, std::size_t pos, const std::string& key) {
    if (pos + key.size() > text.size()) return false;
    for (std::size_t k = 0; k < key.size(); ++k) {
        if (toLowerAscii(text[pos + k]) != key[k]) return false;  // keys are lowercase
    }
    return true;
}

bool endsWith(const std::string& text, const char* suffix, std::size_t suffixLength) {
    return text.size() >= suffixLength
        && text.compare(text.size() - suffixLength, suffixLength, suffix) == 0;
}

// ---------------------------------------------------------------------------
// Protected technical-term table.
//
// Surface forms (lowercase) map to canonical index terms. Matching is
// longest-first with word-boundary checks, so "asp.net" wins over ".net"
// and "cppreference" is NOT matched as "cpp". Canonical terms bypass the
// stemmer. Querying "c++" or "cpp" therefore hits the same postings.
// ---------------------------------------------------------------------------

const std::vector<std::pair<std::string, std::string>>& technicalTermTable() {
    static const std::vector<std::pair<std::string, std::string>> table = [] {
        std::vector<std::pair<std::string, std::string>> entries = {
            {"objective-c", "objectivec"},
            {"objectivec",  "objectivec"},
            {"cplusplus",   "cpp"},
            {"node.js",     "nodejs"},
            {"nodejs",      "nodejs"},
            {"asp.net",     "aspnet"},
            {"aspnet",      "aspnet"},
            {"c-sharp",     "csharp"},
            {"csharp",      "csharp"},
            {"vb.net",      "vbnet"},
            {"vbnet",       "vbnet"},
            {"dotnet",      "dotnet"},
            {".net",        "dotnet"},
            {"fsharp",      "fsharp"},
            {"golang",      "golang"},
            {"sqlite",      "sqlite"},
            {"redis",       "redis"},
            {"cpp",         "cpp"},
            {"c++",         "cpp"},
            {"c#",          "csharp"},
            {"f#",          "fsharp"},
        };
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) {
                      return a.first.size() > b.first.size();
                  });
        return entries;
    }();
    return table;
}

const std::unordered_map<std::string, std::string>& technicalAliasMap() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const auto& entry : technicalTermTable()) {
            m.emplace(entry.first, entry.second);
        }
        return m;
    }();
    return map;
}

const std::unordered_set<std::string>& canonicalTechnicalTerms() {
    static const std::unordered_set<std::string> set = [] {
        std::unordered_set<std::string> s;
        for (const auto& entry : technicalTermTable()) {
            s.insert(entry.second);
        }
        return s;
    }();
    return set;
}

} // namespace

// ---------------------------------------------------------------------------
// Public table accessors
// ---------------------------------------------------------------------------

const std::unordered_set<std::string>& TextProcessor::stopwords() {
    // Conservative English function-word list. Deliberately excludes
    // anything that doubles as a technical term. "not"/"and"/"or" appear
    // here for *document text*; the query parser recognizes Boolean
    // operators at the grammar level before tokenization, so this list
    // never interferes with Boolean queries.
    static const std::unordered_set<std::string> words = {
        "a", "an", "and", "are", "as", "at", "be", "been", "but", "by",
        "can", "could", "did", "do", "does", "for", "from", "had", "has",
        "have", "he", "her", "him", "his", "i", "if", "in", "into", "is",
        "it", "its", "just", "me", "my", "no", "nor", "not", "of", "on",
        "or", "our", "she", "so", "some", "such", "than", "that", "the",
        "their", "them", "then", "there", "these", "they", "this", "to",
        "too", "very", "was", "we", "were", "what", "when", "where",
        "which", "who", "why", "will", "with", "would", "you", "your",
    };
    return words;
}

const std::vector<std::pair<std::string, std::string>>& TextProcessor::technicalTerms() {
    return technicalTermTable();
}

// ---------------------------------------------------------------------------
// Tokenization
// ---------------------------------------------------------------------------

std::vector<std::string> TextProcessor::extractRawTerms(const std::string& text) const {
    std::vector<std::string> rawTerms;
    const std::size_t n = text.size();
    const auto& table = technicalTermTable();
    std::size_t i = 0;

    while (i < n) {
        const char c = text[i];

        // A token can start on a word character, or on '.' (for ".net").
        if (!(isWordChar(c) || c == '.')) {
            ++i;
            continue;
        }

        // 1) Protected technical terms: longest surface match with
        //    boundary checks on both sides.
        if (config_.mapTechnicalTerms) {
            bool matched = false;
            for (const auto& entry : table) {
                const std::string& surface = entry.first;
                const std::size_t len = surface.size();
                if (!equalsCaseInsensitive(text, i, surface)) continue;
                if (i > 0 && isWordChar(text[i - 1])) continue;          // "cppreference"
                if (i + len < n && isWordChar(text[i + len])) continue;  // "node.jsx"
                rawTerms.push_back(surface);
                i += len;
                matched = true;
                break;
            }
            if (matched) continue;
        }

        // '.' alone (not part of a protected term) is punctuation.
        if (c == '.') {
            ++i;
            continue;
        }

        // 2) Generic token: run of word characters, plus
        //    - internal apostrophes ("don't" -> "don't" -> "dont")
        //    - '.' between digits ("3.14", "1.2.3")
        std::size_t j = i + 1;
        while (j < n) {
            const char d = text[j];
            if (isWordChar(d)) {
                ++j;
                continue;
            }
            if (d == '\'' && j + 1 < n && isWordChar(text[j + 1])) {
                ++j;
                continue;
            }
            if (d == '.'
                && std::isdigit(static_cast<unsigned char>(text[j - 1])) != 0
                && j + 1 < n
                && std::isdigit(static_cast<unsigned char>(text[j + 1])) != 0) {
                ++j;
                continue;
            }
            break;
        }
        rawTerms.push_back(text.substr(i, j - i));
        i = j;
    }
    return rawTerms;
}

std::string TextProcessor::canonicalizeTerm(const std::string& rawTerm) const {
    std::string t;
    t.reserve(rawTerm.size());
    for (const char c : rawTerm) {
        if (c == '\'') continue;  // don't -> dont
        t.push_back(toLowerAscii(c));
    }
    if (t.empty()) return {};
    if (t.find_first_not_of("_.") == std::string::npos) return {};  // "___" etc.
    if (config_.mapTechnicalTerms) {
        const auto it = technicalAliasMap().find(t);
        if (it != technicalAliasMap().end()) return it->second;
    }
    return t;
}

std::vector<Token> TextProcessor::tokenizeField(const std::string& text, Field field) const {
    std::vector<Token> tokens;
    const std::vector<std::string> rawTerms = extractRawTerms(text);
    tokens.reserve(rawTerms.size());

    for (std::size_t index = 0; index < rawTerms.size(); ++index) {
        const std::string base = canonicalizeTerm(rawTerms[index]);
        if (base.empty()) continue;

        // Stop-word filtering uses the pre-stem dictionary form.
        if (config_.removeStopwords && isStopword(base)) continue;

        std::string term = stem(base);
        if (term.empty()) continue;

        Token token;
        token.term = std::move(term);
        token.position = static_cast<std::uint32_t>(index);  // original ordinal
        token.field = field;
        tokens.push_back(std::move(token));
    }
    return tokens;
}

AnalyzedDocument TextProcessor::analyze(const Document& document) const {
    AnalyzedDocument analyzed;
    analyzed.title = tokenizeField(document.title, Field::Title);
    analyzed.headings.reserve(document.headings.size());
    for (const std::string& heading : document.headings) {
        analyzed.headings.push_back(tokenizeField(heading, Field::Heading));
    }
    analyzed.body = tokenizeField(document.content, Field::Body);
    return analyzed;
}

std::vector<std::string> TextProcessor::tokenizeQuery(const std::string& query) const {
    std::vector<std::string> terms;
    for (const std::string& raw : extractRawTerms(query)) {
        const std::string base = canonicalizeTerm(raw);
        if (base.empty()) continue;
        // Stop words are intentionally KEPT in queries: the query parser
        // (later phase) keeps phrase terms verbatim and lets the ranking
        // layer decide how much weight each term carries.
        std::string term = stem(base);
        if (!term.empty()) terms.push_back(std::move(term));
    }
    return terms;
}

// ---------------------------------------------------------------------------
// Light stemmer
//
// Full rule table, rationale and known quirks: docs/algorithms.md.
// Design goals, in priority order:
//   1. never damage protected technical terms          (bypassed outright)
//   2. unify the common inflections of technical prose (programs/
//      programming/program, parsed/parsing/parse, runs/running/run)
//   3. stay simple and auditable - one pass, at most one suffix
//      transformation plus normalizations, minimum-length guards.
// ---------------------------------------------------------------------------

std::string TextProcessor::stem(const std::string& term) const {
    if (!config_.applyStemming) return term;

    const std::size_t length = term.size();
    if (length < 4) return term;                                       // run, sql, c
    if (term.find_first_of("0123456789") != std::string::npos) return term;  // 2024, python3
    if (canonicalTechnicalTerms().count(term) > 0) return term;        // cpp, nodejs

    std::string t = term;
    bool suffixStripped = false;

    // 1) Plurals (one application).
    if (endsWith(t, "sses", 4) && t.size() >= 6) {          // classes -> class
        t.resize(t.size() - 2);
    } else if (endsWith(t, "ches", 4) && t.size() >= 6) {   // matches -> match
        t.resize(t.size() - 2);
    } else if (endsWith(t, "shes", 4) && t.size() >= 6) {   // wishes -> wish
        t.resize(t.size() - 2);
    } else if (endsWith(t, "xes", 3) && t.size() >= 5) {    // boxes -> box
        t.resize(t.size() - 2);
    } else if (endsWith(t, "zes", 3) && t.size() >= 5) {    // analyzes -> analyz
        t.resize(t.size() - 2);
    } else if (endsWith(t, "ies", 3) && t.size() >= 5) {    // cities -> city
        t.resize(t.size() - 3);
        t.push_back('y');
    } else if (endsWith(t, "ss", 2)                        // bliss
               || endsWith(t, "us", 2)                     // status
               || endsWith(t, "is", 2)) {                  // basis
        // keep as-is
    } else if (endsWith(t, "s", 1) && t.size() >= 4) {     // notes -> note
        t.resize(t.size() - 1);
    }

    // 2) Progressive / past suffixes.
    if (endsWith(t, "ing", 3) && t.size() >= 6) {          // parsing -> pars
        t.resize(t.size() - 3);
        suffixStripped = true;
    } else if (endsWith(t, "ed", 2) && t.size() >= 5) {    // parsed -> pars
        t.resize(t.size() - 2);
        suffixStripped = true;
    }

    // 3) De-double the final consonant produced by stripping
    //    (running -> run, programming -> program). 'l', 's', 'z' are
    //    excluded so installing -> install and processing -> process.
    if (suffixStripped && t.size() >= 2) {
        const char a = t[t.size() - 1];
        const char b = t[t.size() - 2];
        if (a == b && isConsonantLetter(a) && a != 'l' && a != 's' && a != 'z') {
            t.resize(t.size() - 1);
        }
    }

    // 4) Final-e normalization (parse -> pars, language -> languag).
    if (t.size() >= 5 && t.back() == 'e') {
        t.resize(t.size() - 1);
    }

    if (t.size() < 2) return term;  // safety net: never emit a 0/1-char stem
    return t;
}

} // namespace minisearch
