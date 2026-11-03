#include "query/QueryProcessor.h"

#include <algorithm>
#include <unordered_set>

namespace minisearch {

namespace {

void addScoringTerm(std::vector<std::string>& scoringTerms, const std::string& term) {
    if (std::find(scoringTerms.begin(), scoringTerms.end(), term) == scoringTerms.end()) {
        scoringTerms.push_back(term);
    }
}

// Document ids of a term's posting list (already ascending by construction).
std::vector<DocumentId> docsOfTerm(const InvertedIndex& index, const std::string& term) {
    std::vector<DocumentId> docs;
    if (const PostingList* list = index.findTerm(term)) {
        docs.reserve(list->postings.size());
        for (const Posting& posting : list->postings) docs.push_back(posting.doc);
    }
    return docs;
}

} // namespace

std::vector<DocumentId> QueryProcessor::intersect(const std::vector<DocumentId>& a,
                                                   const std::vector<DocumentId>& b) {
    std::vector<DocumentId> out;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) {
            ++i;
        } else if (b[j] < a[i]) {
            ++j;
        } else {
            out.push_back(a[i]);
            ++i;
            ++j;
        }
    }
    return out;
}

std::vector<DocumentId> QueryProcessor::unionOf(const std::vector<DocumentId>& a,
                                                 const std::vector<DocumentId>& b) {
    std::vector<DocumentId> out;
    out.reserve(a.size() + b.size());
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) {
            out.push_back(a[i++]);
        } else if (b[j] < a[i]) {
            out.push_back(b[j++]);
        } else {
            out.push_back(a[i++]);
            ++j;
        }
    }
    while (i < a.size()) out.push_back(a[i++]);
    while (j < b.size()) out.push_back(b[j++]);
    return out;
}

std::vector<DocumentId> QueryProcessor::subtract(const std::vector<DocumentId>& a,
                                                  const std::vector<DocumentId>& b) {
    std::vector<DocumentId> out;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size()) {
        if (j >= b.size() || a[i] < b[j]) {
            out.push_back(a[i++]);
        } else if (b[j] < a[i]) {
            ++j;
        } else {
            ++i;
            ++j;
        }
    }
    return out;
}

std::vector<DocumentId> QueryProcessor::phraseMatches(const std::vector<std::string>& terms,
                                                      const InvertedIndex& index) {
    if (terms.empty()) return {};

    // Documents containing every phrase term.
    std::vector<DocumentId> candidates = docsOfTerm(index, terms.front());
    for (std::size_t t = 1; t < terms.size() && !candidates.empty(); ++t) {
        candidates = intersect(candidates, docsOfTerm(index, terms[t]));
    }
    if (candidates.empty()) return {};

    std::vector<DocumentId> matches;
    matches.reserve(candidates.size());
    for (DocumentId doc : candidates) {
        // Position sets per term for this document.
        std::vector<std::unordered_set<std::uint32_t>> positions(terms.size());
        bool allPresent = true;
        for (std::size_t t = 0; t < terms.size(); ++t) {
            const Posting* posting = index.findPosting(terms[t], doc);
            if (posting == nullptr) {
                allPresent = false;
                break;
            }
            positions[t].insert(posting->positions.begin(), posting->positions.end());
        }
        if (!allPresent) continue;

        // A phrase matches if some start position p has terms[t] at p + t
        // for every t. Field gaps in the virtual stream automatically
        // reject phrases that would cross field or heading boundaries.
        for (const std::uint32_t start : positions.front()) {
            bool consecutive = true;
            for (std::size_t t = 1; t < terms.size(); ++t) {
                if (positions[t].count(start + static_cast<std::uint32_t>(t)) == 0) {
                    consecutive = false;
                    break;
                }
            }
            if (consecutive) {
                matches.push_back(doc);
                break;
            }
        }
    }
    return matches;
}

QueryEvaluation QueryProcessor::evaluate(const query::QueryNode& node,
                                          const InvertedIndex& index) const {
    QueryEvaluation result;
    switch (node.kind) {
        case query::QueryNode::Kind::Term: {
            if (node.terms.empty()) {
                result.error = "empty term";
                return result;
            }
            // A term token usually normalizes to exactly one index term;
            // if the surface form maps to several, treat them as OR.
            std::vector<DocumentId> docs = docsOfTerm(index, node.terms.front());
            for (std::size_t t = 1; t < node.terms.size(); ++t) {
                docs = unionOf(docs, docsOfTerm(index, node.terms[t]));
            }
            result.candidates = std::move(docs);
            for (const std::string& term : node.terms) addScoringTerm(result.scoringTerms, term);
            result.ok = true;
            return result;
        }

        case query::QueryNode::Kind::Phrase: {
            result.candidates = phraseMatches(node.terms, index);
            for (const std::string& term : node.terms) addScoringTerm(result.scoringTerms, term);
            result.ok = true;
            return result;
        }

        case query::QueryNode::Kind::Or: {
            std::vector<DocumentId> docs;
            for (const auto& child : node.children) {
                QueryEvaluation part = evaluate(*child, index);
                if (!part.ok) return part;
                docs = unionOf(docs, part.candidates);
                for (const std::string& term : part.scoringTerms) {
                    addScoringTerm(result.scoringTerms, term);
                }
            }
            result.candidates = std::move(docs);
            result.ok = true;
            return result;
        }

        case query::QueryNode::Kind::And: {
            // Positive children intersect; NOT children subtract.
            std::vector<DocumentId> docs;
            std::vector<DocumentId> excluded;
            bool havePositive = false;
            for (const auto& child : node.children) {
                if (child->kind == query::QueryNode::Kind::Not) {
                    QueryEvaluation negated = evaluate(*child->children.front(), index);
                    if (!negated.ok) return negated;
                    excluded = unionOf(excluded, negated.candidates);
                    continue;
                }
                QueryEvaluation part = evaluate(*child, index);
                if (!part.ok) return part;
                docs = havePositive ? intersect(docs, part.candidates) : part.candidates;
                havePositive = true;
                for (const std::string& term : part.scoringTerms) {
                    addScoringTerm(result.scoringTerms, term);
                }
            }
            if (!havePositive) {
                result.error = "query requires at least one positive term";
                return result;
            }
            result.candidates = subtract(docs, excluded);
            result.ok = true;
            return result;
        }

        case query::QueryNode::Kind::Not: {
            // A bare NOT term has no positive side; the grammar never
            // produces this at top level, but nested evaluation through
            // Or children could reach it via malformed trees.
            result.error = "NOT requires a left operand";
            return result;
        }
    }
    result.error = "unknown query node";
    return result;
}

} // namespace minisearch
