#pragma once

// MiniSearch - persistent storage (spec module 8).
//
// Two complementary stores:
//   1. SQLite      - document metadata (the durable source of truth:
//                    url, title, description, content, headings, links,
//                    crawl bookkeeping). Document ids are SQLite row ids,
//                    so they stay stable across restarts.
//   2. Binary file - the serialized inverted index (a derived cache):
//                    varint + delta-encoded posting lists, written
//                    atomically (temp file + rename), guarded by a
//                    magic/version header with structural validation.
//
// On startup the engine loads the index file; if it is missing or
// corrupted, it is honestly rebuilt from the SQLite documents (the
// tokenizer is fast). Incremental updates add postings for documents
// the index does not know yet.

#include "common/Document.h"
#include "index/InvertedIndex.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace minisearch {

class StorageManager {
public:
    StorageManager(std::string dbPath, std::string indexPath);

    ~StorageManager();

    StorageManager(const StorageManager&) = delete;
    StorageManager& operator=(const StorageManager&) = delete;

    // Creates parent directories, opens the database, ensures the schema.
    bool open();

    const std::string& lastError() const noexcept { return lastError_; }

    // --- documents ---------------------------------------------------------

    struct UpsertResult {
        DocumentId id = -1;
        bool inserted = false;  // false = existing row updated (same URL)
    };

    // Insert or update by URL; keeps the existing stable id on update.
    UpsertResult upsertDocument(const Document& document);

    std::optional<Document> loadDocument(DocumentId id) const;
    std::optional<DocumentId> findIdByUrl(const std::string& url) const;
    std::vector<Document> loadAllDocuments() const;  // ascending id
    bool deleteDocument(DocumentId id);
    std::size_t documentCount() const;

    // --- engine counters ----------------------------------------------------

    std::int64_t getMeta(const std::string& key, std::int64_t fallback = 0) const;
    void setMeta(const std::string& key, std::int64_t value);
    void addMeta(const std::string& key, std::int64_t delta);

    // --- inverted index persistence ------------------------------------------

    enum class LoadStatus { Loaded, Missing, Corrupted };

    bool saveIndex(const InvertedIndex& index) const;   // atomic
    LoadStatus loadIndex(InvertedIndex& index) const;   // clears on corruption
    bool deleteIndexFile() const;
    std::uintmax_t indexFileSize() const;

    // Removes every document row and the index file (CLI 'wipe').
    bool wipeAll();

private:
    bool execute(const std::string& sql);
    bool prepare(const std::string& sql, sqlite3_stmt** stmt) const;

    std::string dbPath_;
    std::string indexPath_;
    sqlite3* db_ = nullptr;
    mutable std::string lastError_;
    mutable std::mutex mutex_;  // sqlite connection is single-threaded
};

} // namespace minisearch
