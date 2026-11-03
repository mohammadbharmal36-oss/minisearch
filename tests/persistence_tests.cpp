// Unit tests for persistent storage (storage/StorageManager):
// SQLite document metadata + serialized inverted index.

#include "index/InvertedIndex.h"
#include "processor/TextProcessor.h"
#include "storage/StorageManager.h"
#include "test_framework.h"

#include <filesystem>
#include <memory>
#include <fstream>
#include <string>
#include <vector>

namespace {

using minisearch::Document;
using minisearch::DocumentId;
using minisearch::InvertedIndex;
using minisearch::StorageManager;
using minisearch::TextProcessor;

const TextProcessor processor;

std::string testDir() {
    std::string dir = MINISEARCH_TEST_TMP;
    dir += "/persistence";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string dbPath(const std::string& name) { return testDir() + "/" + name + ".db"; }
std::string indexPath(const std::string& name) { return testDir() + "/" + name + ".idx"; }

// Every test starts from clean files, so runs are repeatable.
std::unique_ptr<StorageManager> freshStorage(const std::string& name) {
    std::error_code ec;
    std::filesystem::remove(dbPath(name), ec);
    std::filesystem::remove(indexPath(name), ec);
    auto storage = std::make_unique<StorageManager>(dbPath(name), indexPath(name));
    storage->open();
    return storage;
}


Document makeDocument(const std::string& url, const std::string& title,
                      const std::string& content) {
    Document doc;
    doc.url = url;
    doc.title = title;
    doc.description = "Description of " + title;
    doc.content = content;
    doc.headings = {"First Heading", "Second Heading"};
    doc.links = {"https://example.org/a", "https://example.org/b"};
    doc.canonical_url = url;
    doc.http_status = 200;
    doc.crawled_at = 1730000000;
    return doc;
}

InvertedIndex buildSampleIndex() {
    InvertedIndex index;
    Document d1 = makeDocument("https://example.org/1", "Cpp", "cpp program");
    Document d2 = makeDocument("https://example.org/2", "Python", "python program");
    Document d3 = makeDocument("https://example.org/3", "Sql", "sql database");
    index.indexDocument(1, processor.analyze(d1));
    index.indexDocument(2, processor.analyze(d2));
    index.indexDocument(3, processor.analyze(d3));
    return index;
}

bool sameIndex(const InvertedIndex& a, const InvertedIndex& b) {
    if (a.documentCount() != b.documentCount()) return false;
    if (a.termCount() != b.termCount()) return false;
    if (a.totalPostings() != b.totalPostings()) return false;
    if (a.tokenSum() != b.tokenSum()) return false;
    for (const auto& entry : a.termMap()) {
        const auto* other = b.findTerm(entry.first);
        if (other == nullptr) return false;
        if (other->postings.size() != entry.second.postings.size()) return false;
        for (std::size_t i = 0; i < entry.second.postings.size(); ++i) {
            const minisearch::Posting& p = entry.second.postings[i];
            const minisearch::Posting& q = other->postings[i];
            if (p.doc != q.doc || p.tf_title != q.tf_title || p.tf_heading != q.tf_heading
                || p.tf_body != q.tf_body || p.positions != q.positions) {
                return false;
            }
        }
    }
    for (DocumentId id : a.allDocumentIds()) {
        const auto* ma = a.findDocument(id);
        const auto* mb = b.findDocument(id);
        if (ma == nullptr || mb == nullptr) return false;
        if (ma->title_len != mb->title_len || ma->heading_len != mb->heading_len
            || ma->body_len != mb->body_len || ma->heading_count != mb->heading_count
            || ma->doc_terms != mb->doc_terms) {
            return false;
        }
    }
    return true;
}

void writeGarbageFile(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

// ---------------------------------------------------------------------------
// SQLite document store
// ---------------------------------------------------------------------------

MINI_TEST(storage_open_creates_schema) {
    const auto storagePtr = freshStorage("open");
    auto& storage = *storagePtr;
    MINI_EXPECT_EQ(storage.documentCount(), std::size_t{0});
    MINI_EXPECT_TRUE(storage.lastError().empty());
}

MINI_TEST(upsert_inserts_then_updates_with_stable_id) {
    const auto storagePtr = freshStorage("upsert");
    auto& storage = *storagePtr;

    const auto first = storage.upsertDocument(makeDocument("https://example.org/x", "Old", "old body"));
    MINI_EXPECT_TRUE(first.inserted);
    MINI_EXPECT_EQ(first.id, 1);

    const auto second = storage.upsertDocument(makeDocument("https://example.org/y", "Other", "other"));
    MINI_EXPECT_TRUE(second.inserted);
    MINI_EXPECT_EQ(second.id, 2);

    // Same URL again: update, keep id.
    Document updated = makeDocument("https://example.org/x", "New Title", "new body");
    const auto third = storage.upsertDocument(updated);
    MINI_EXPECT_FALSE(third.inserted);
    MINI_EXPECT_EQ(third.id, first.id);

    MINI_EXPECT_EQ(storage.documentCount(), std::size_t{2});
    const auto loaded = storage.loadDocument(first.id);
    MINI_EXPECT_TRUE(loaded.has_value());
    MINI_EXPECT_EQ(loaded->title, "New Title");
    MINI_EXPECT_EQ(loaded->content, "new body");
}

MINI_TEST(load_document_round_trips_all_fields) {
    const auto storagePtr = freshStorage("roundtrip");
    auto& storage = *storagePtr;
    Document original = makeDocument("https://example.org/full", "Full Doc", "body text here");
    const auto result = storage.upsertDocument(original);
    MINI_EXPECT_TRUE(result.inserted);
    original.id = result.id;  // ids belong to the storage layer

    const auto loaded = storage.loadDocument(result.id);
    MINI_EXPECT_TRUE(loaded.has_value());
    MINI_EXPECT_EQ(*loaded, original);
}

MINI_TEST(load_all_documents_returns_ascending_ids) {
    const auto storagePtr = freshStorage("all");
    auto& storage = *storagePtr;
    storage.upsertDocument(makeDocument("https://example.org/c", "C", "c"));
    storage.upsertDocument(makeDocument("https://example.org/a", "A", "a"));
    storage.upsertDocument(makeDocument("https://example.org/b", "B", "b"));

    const auto documents = storage.loadAllDocuments();
    MINI_EXPECT_EQ(documents.size(), std::size_t{3});
    MINI_EXPECT_EQ(documents[0].url, "https://example.org/c");  // insertion order = id order
    MINI_EXPECT_EQ(documents[1].url, "https://example.org/a");
    MINI_EXPECT_EQ(documents[2].url, "https://example.org/b");
}

MINI_TEST(find_id_by_url_and_delete) {
    const auto storagePtr = freshStorage("find");
    auto& storage = *storagePtr;
    const auto inserted = storage.upsertDocument(makeDocument("https://example.org/z", "Z", "z"));
    const auto found = storage.findIdByUrl("https://example.org/z");
    MINI_EXPECT_TRUE(found.has_value());
    MINI_EXPECT_EQ(*found, inserted.id);
    MINI_EXPECT_FALSE(storage.findIdByUrl("https://example.org/none").has_value());

    MINI_EXPECT_TRUE(storage.deleteDocument(inserted.id));
    MINI_EXPECT_FALSE(storage.deleteDocument(inserted.id));
    MINI_EXPECT_EQ(storage.documentCount(), std::size_t{0});
}

MINI_TEST(meta_counters_persist) {
    const auto storagePtr = freshStorage("meta");
    auto& storage = *storagePtr;
    MINI_EXPECT_EQ(storage.getMeta("pages_crawled"), 0);
    storage.addMeta("pages_crawled", 5);
    storage.addMeta("pages_crawled", 2);
    MINI_EXPECT_EQ(storage.getMeta("pages_crawled"), 7);
    storage.setMeta("pages_crawled", 1);
    MINI_EXPECT_EQ(storage.getMeta("pages_crawled"), 1);
}

// ---------------------------------------------------------------------------
// Serialized inverted index
// ---------------------------------------------------------------------------

MINI_TEST(index_save_load_round_trip) {
    const auto storagePtr = freshStorage("idx");
    auto& storage = *storagePtr;

    InvertedIndex original = buildSampleIndex();
    MINI_EXPECT_TRUE(storage.saveIndex(original));

    InvertedIndex loaded;
    MINI_EXPECT_TRUE(storage.loadIndex(loaded) == StorageManager::LoadStatus::Loaded);
    MINI_EXPECT_TRUE(sameIndex(original, loaded));
    MINI_EXPECT_GT(storage.indexFileSize(), std::uintmax_t{0});
}

MINI_TEST(index_load_missing_file) {
    const auto storagePtr = freshStorage("missing");
    auto& storage = *storagePtr;
    InvertedIndex index;
    MINI_EXPECT_TRUE(storage.loadIndex(index) == StorageManager::LoadStatus::Missing);
}

MINI_TEST(index_load_rejects_garbage) {
    const auto storagePtr = freshStorage("garbage");
    auto& storage = *storagePtr;

    writeGarbageFile(indexPath("garbage"), "not an index at all, just text");
    InvertedIndex index;
    MINI_EXPECT_TRUE(storage.loadIndex(index) == StorageManager::LoadStatus::Corrupted);

    // Right magic, garbage payload.
    writeGarbageFile(indexPath("garbage"), std::string("MSIDX\x01\x00", 7) + "\xDE\xAD\xBE\xEF");
    MINI_EXPECT_TRUE(storage.loadIndex(index) == StorageManager::LoadStatus::Corrupted);
}

MINI_TEST(index_load_rejects_truncated_file) {
    const auto storagePtr = freshStorage("trunc");
    auto& storage = *storagePtr;

    InvertedIndex original = buildSampleIndex();
    MINI_EXPECT_TRUE(storage.saveIndex(original));

    // Truncate the last 10 bytes of a valid file.
    std::ifstream in(indexPath("trunc"), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    writeGarbageFile(indexPath("trunc"), bytes.substr(0, bytes.size() - 10));

    InvertedIndex index;
    MINI_EXPECT_TRUE(storage.loadIndex(index) == StorageManager::LoadStatus::Corrupted);
}

MINI_TEST(index_incremental_update_survives_save_load) {
    const auto storagePtr = freshStorage("incr");
    auto& storage = *storagePtr;

    InvertedIndex index = buildSampleIndex();
    MINI_EXPECT_TRUE(storage.saveIndex(index));

    InvertedIndex reloaded;
    MINI_EXPECT_TRUE(storage.loadIndex(reloaded) == StorageManager::LoadStatus::Loaded);

    // Incremental: index one more document, persist again, reload.
    Document d4 = makeDocument("https://example.org/4", "Java", "java program");
    reloaded.indexDocument(4, processor.analyze(d4));
    MINI_EXPECT_TRUE(storage.saveIndex(reloaded));

    InvertedIndex after;
    MINI_EXPECT_TRUE(storage.loadIndex(after) == StorageManager::LoadStatus::Loaded);
    MINI_EXPECT_EQ(after.documentCount(), std::size_t{4});
    MINI_EXPECT_TRUE(sameIndex(reloaded, after));
    MINI_EXPECT_TRUE(after.findTerm("java") != nullptr);
}

MINI_TEST(index_removal_survives_save_load) {
    const auto storagePtr = freshStorage("removal");
    auto& storage = *storagePtr;

    InvertedIndex index = buildSampleIndex();
    MINI_EXPECT_TRUE(index.removeDocument(2));
    MINI_EXPECT_TRUE(storage.saveIndex(index));

    InvertedIndex loaded;
    MINI_EXPECT_TRUE(storage.loadIndex(loaded) == StorageManager::LoadStatus::Loaded);
    MINI_EXPECT_EQ(loaded.documentCount(), std::size_t{2});
    MINI_EXPECT_TRUE(loaded.findTerm("python") == nullptr);
    MINI_EXPECT_TRUE(sameIndex(index, loaded));
}

MINI_TEST(save_index_is_deterministic) {
    const auto storagePtr = freshStorage("det");
    auto& storage = *storagePtr;

    InvertedIndex a = buildSampleIndex();
    InvertedIndex b = buildSampleIndex();
    MINI_EXPECT_TRUE(storage.saveIndex(a));

    // Read the bytes, save again from an equal index, compare.
    std::ifstream in(indexPath("det"), std::ios::binary);
    const std::string first((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    MINI_EXPECT_TRUE(storage.saveIndex(b));
    std::ifstream in2(indexPath("det"), std::ios::binary);
    const std::string second((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());

    MINI_EXPECT_EQ(first, second);
}

MINI_TEST(wipe_all_clears_documents_and_index) {
    const auto storagePtr = freshStorage("wipe");
    auto& storage = *storagePtr;
    storage.upsertDocument(makeDocument("https://example.org/w", "W", "w"));
    InvertedIndex index = buildSampleIndex();
    storage.saveIndex(index);
    storage.addMeta("pages_crawled", 9);

    MINI_EXPECT_TRUE(storage.wipeAll());
    MINI_EXPECT_EQ(storage.documentCount(), std::size_t{0});
    MINI_EXPECT_EQ(storage.getMeta("pages_crawled"), 0);
    InvertedIndex empty;
    MINI_EXPECT_TRUE(storage.loadIndex(empty) == StorageManager::LoadStatus::Missing);
}

// ---------------------------------------------------------------------------
// Restart semantics: a fresh StorageManager over the same files sees the
// same documents (this is what keeps ids stable across app restarts).
// ---------------------------------------------------------------------------

MINI_TEST(restart_preserves_documents_and_ids) {
    std::error_code ec;
    std::filesystem::remove(dbPath("restart"), ec);
    std::filesystem::remove(indexPath("restart"), ec);
    {
        StorageManager storage(dbPath("restart"), indexPath("restart"));
        MINI_EXPECT_TRUE(storage.open());
        const auto a = storage.upsertDocument(makeDocument("https://example.org/1", "One", "one"));
        const auto b = storage.upsertDocument(makeDocument("https://example.org/2", "Two", "two"));
        InvertedIndex index = buildSampleIndex();
        MINI_EXPECT_TRUE(storage.saveIndex(index));
        MINI_EXPECT_EQ(a.id, 1);
        MINI_EXPECT_EQ(b.id, 2);
    }
    {
        StorageManager storage(dbPath("restart"), indexPath("restart"));
        MINI_EXPECT_TRUE(storage.open());
        MINI_EXPECT_EQ(storage.documentCount(), std::size_t{2});
        const auto found = storage.findIdByUrl("https://example.org/1");
        MINI_EXPECT_TRUE(found.has_value());
        MINI_EXPECT_EQ(*found, 1);

        InvertedIndex index;
        MINI_EXPECT_TRUE(storage.loadIndex(index) == StorageManager::LoadStatus::Loaded);
        MINI_EXPECT_EQ(index.documentCount(), std::size_t{3});
    }
}
