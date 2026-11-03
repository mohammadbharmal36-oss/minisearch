#include "index/InvertedIndex.h"

#include <algorithm>

namespace minisearch {

namespace {

const Posting* findPostingByDoc(const std::vector<Posting>& postings, DocumentId doc) {
    const auto it = std::lower_bound(postings.begin(), postings.end(), doc,
                                     [](const Posting& p, DocumentId d) { return p.doc < d; });
    if (it == postings.end() || it->doc != doc) return nullptr;
    return &*it;
}

} // namespace

const Posting* InvertedIndex::findPostingIn(const PostingList& list, DocumentId doc) {
    return findPostingByDoc(list.postings, doc);
}

void InvertedIndex::indexDocument(DocumentId id, const AnalyzedDocument& analyzed) {
    if (id < 0) return;

    // Update semantics: drop any previous version of this document first.
    removeDocument(id);

    DocMeta meta;
    meta.id = id;
    meta.title_len = static_cast<std::uint32_t>(analyzed.title.size());
    meta.heading_count = static_cast<std::uint32_t>(analyzed.headings.size());
    for (const auto& heading : analyzed.headings) {
        meta.heading_len += static_cast<std::uint32_t>(heading.size());
    }
    meta.body_len = static_cast<std::uint32_t>(analyzed.body.size());

    // Build this document's postings locally, mapping per-field ordinals
    // onto the virtual stream (with boundary gaps between headings/fields).
    std::unordered_map<std::string, Posting> local;
    const auto record = [&local, id](const std::string& term, std::uint32_t virtualPos,
                                     Field field) {
        Posting& posting = local[term];
        if (posting.doc != id) {
            posting = Posting{};
            posting.doc = id;
        }
        switch (field) {
            case Field::Title:   ++posting.tf_title;   break;
            case Field::Heading: ++posting.tf_heading; break;
            case Field::Body:    ++posting.tf_body;    break;
        }
        posting.positions.push_back(virtualPos);
    };

    for (const Token& token : analyzed.title) {
        record(token.term, token.position, Field::Title);
    }

    std::uint32_t headingPrefix = 0;  // tokens consumed by earlier headings
    const std::uint32_t headingStart = meta.headingStartVirtual();
    for (std::size_t h = 0; h < analyzed.headings.size(); ++h) {
        const std::uint32_t headingBase = headingStart + headingPrefix + static_cast<std::uint32_t>(h);
        for (const Token& token : analyzed.headings[h]) {
            record(token.term, headingBase + token.position, Field::Heading);
        }
        headingPrefix += static_cast<std::uint32_t>(analyzed.headings[h].size());
    }

    const std::uint32_t bodyBase = meta.bodyStartVirtual();
    for (const Token& token : analyzed.body) {
        record(token.term, bodyBase + token.position, Field::Body);
    }

    // Merge into the global term map, keeping every posting list sorted
    // by document id (ids are usually increasing, so appends dominate).
    meta.doc_terms.reserve(local.size());
    for (auto& entry : local) {
        meta.doc_terms.push_back(entry.first);
        PostingList& list = terms_[entry.first];
        auto it = std::lower_bound(list.postings.begin(), list.postings.end(), entry.second.doc,
                                   [](const Posting& p, DocumentId d) { return p.doc < d; });
        list.postings.insert(it, std::move(entry.second));
        ++totalPostings_;
    }
    // Deterministic term order inside DocMeta (stable serialization).
    std::sort(meta.doc_terms.begin(), meta.doc_terms.end());

    tokenSum_ += meta.totalTokens();
    docs_[id] = std::move(meta);
}

bool InvertedIndex::removeDocument(DocumentId id) {
    const auto docIt = docs_.find(id);
    if (docIt == docs_.end()) return false;

    const DocMeta& meta = docIt->second;
    for (const std::string& term : meta.doc_terms) {
        const auto termIt = terms_.find(term);
        if (termIt == terms_.end()) continue;
        auto& postings = termIt->second.postings;
        const auto it = std::lower_bound(postings.begin(), postings.end(), id,
                                         [](const Posting& p, DocumentId d) { return p.doc < d; });
        if (it != postings.end() && it->doc == id) {
            postings.erase(it);
            --totalPostings_;
        }
        if (postings.empty()) {
            terms_.erase(termIt);
        }
    }

    tokenSum_ -= meta.totalTokens();
    docs_.erase(docIt);
    return true;
}

bool InvertedIndex::hasDocument(DocumentId id) const { return docs_.count(id) > 0; }

const DocMeta* InvertedIndex::findDocument(DocumentId id) const {
    const auto it = docs_.find(id);
    return it == docs_.end() ? nullptr : &it->second;
}

const PostingList* InvertedIndex::findTerm(const std::string& term) const {
    const auto it = terms_.find(term);
    return it == terms_.end() ? nullptr : &it->second;
}

const Posting* InvertedIndex::findPosting(const std::string& term, DocumentId doc) const {
    const PostingList* list = findTerm(term);
    if (list == nullptr) return nullptr;
    return findPostingIn(*list, doc);
}

std::vector<std::pair<std::string, std::uint32_t>> InvertedIndex::termsWithPrefix(
    const std::string& prefix, std::size_t limit) const {
    std::vector<std::pair<std::string, std::uint32_t>> matches;
    for (const auto& entry : terms_) {
        if (entry.first.compare(0, prefix.size(), prefix) == 0) {
            matches.emplace_back(entry.first, entry.second.df());
        }
    }
    std::sort(matches.begin(), matches.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;  // rarer is boring,
                  return a.first < b.first;                              // common is useful
              });
    if (matches.size() > limit) matches.resize(limit);
    return matches;
}

std::vector<DocumentId> InvertedIndex::allDocumentIds() const {
    std::vector<DocumentId> ids;
    ids.reserve(docs_.size());
    for (const auto& entry : docs_) ids.push_back(entry.first);
    std::sort(ids.begin(), ids.end());
    return ids;
}

void InvertedIndex::restore(std::unordered_map<std::string, PostingList> terms,
                            std::unordered_map<DocumentId, DocMeta> docs,
                            std::size_t totalPostings, std::uint64_t tokenSum) {
    terms_ = std::move(terms);
    docs_ = std::move(docs);
    totalPostings_ = totalPostings;
    tokenSum_ = tokenSum;
}

void InvertedIndex::clear() noexcept {
    terms_.clear();
    docs_.clear();
    totalPostings_ = 0;
    tokenSum_ = 0;
}

} // namespace minisearch
