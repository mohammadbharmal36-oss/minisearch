#include "storage/StorageManager.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>

namespace minisearch {

namespace {

// ---------------------------------------------------------------------------
// Index file format (little endian):
//   magic  "MSIDX" | u8 major | u8 minor          (7 bytes)
//   u32 termCount
//   per term (terms sorted for deterministic files):
//     u16 termLength, bytes
//     u32 df (== number of postings that follow)
//     per posting: varint docIdDelta | varint tf_title | varint tf_heading
//                  | varint tf_body | varint positionCount
//                  | varint firstPosition, varint positionDeltas...
//   u32 docCount
//   per doc (ascending id):
//     varint id | varint title_len | varint heading_len | varint body_len
//     | varint heading_count | u32 docTermCount
//     | per doc term: u16 termLength, bytes
// ---------------------------------------------------------------------------

constexpr char kMagic[] = "MSIDX";
constexpr std::uint8_t kMajorVersion = 1;
constexpr std::uint8_t kMinorVersion = 0;

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

// Columns are NOT NULL DEFAULT '' - this is defense in depth.
std::string readText(sqlite3_stmt* stmt, int column) {
    const unsigned char* text = sqlite3_column_text(stmt, column);
    return text != nullptr ? std::string(reinterpret_cast<const char*>(text)) : std::string();
}

Document documentFromColumns(sqlite3_stmt* stmt) {
    Document doc;
    doc.id = sqlite3_column_int64(stmt, 0);
    doc.url = readText(stmt, 1);
    doc.canonical_url = readText(stmt, 2);
    doc.title = readText(stmt, 3);
    doc.description = readText(stmt, 4);
    doc.content = readText(stmt, 5);
    doc.headings = splitLines(readText(stmt, 6));
    doc.links = splitLines(readText(stmt, 7));
    doc.http_status = static_cast<int>(sqlite3_column_int(stmt, 8));
    doc.crawled_at = sqlite3_column_int64(stmt, 9);
    return doc;
}

void appendVarint(std::string& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void appendU16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void appendU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

// Byte-level reader with bounds checking; throws std::runtime_error on any
// inconsistency, which the loader translates into LoadStatus::Corrupted.
class ByteReader {
public:
    explicit ByteReader(const std::string& data) : data_(data) {}

    std::size_t remaining() const { return data_.size() - pos_; }

    void require(std::size_t bytes) const {
        if (bytes > data_.size() - pos_) throw std::runtime_error("unexpected end of index");
    }

    std::uint8_t readU8() {
        require(1);
        return static_cast<std::uint8_t>(data_[pos_++]);
    }

    std::uint16_t readU16() {
        require(2);
        const std::uint16_t lo = static_cast<std::uint16_t>(
            static_cast<std::uint8_t>(data_[pos_++]));
        const std::uint16_t hi = static_cast<std::uint16_t>(
            static_cast<std::uint8_t>(data_[pos_++]));
        return static_cast<std::uint16_t>(lo | (hi << 8));
    }

    std::uint32_t readU32() {
        require(4);
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data_[pos_++])) << shift;
        }
        return value;
    }

    std::uint64_t readVarint() {
        std::uint64_t value = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            require(1);
            const std::uint8_t byte = static_cast<std::uint8_t>(data_[pos_++]);
            value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return value;
            shift += 7;
        }
        throw std::runtime_error("malformed varint");
    }

    std::string readString(std::uint16_t length) {
        require(length);
        std::string out(data_, pos_, length);
        pos_ += length;
        return out;
    }

private:
    const std::string& data_;
    std::size_t pos_ = 0;
};

// RAII statement guard.
class StatementGuard {
public:
    explicit StatementGuard(sqlite3_stmt* stmt) : stmt_(stmt) {}
    ~StatementGuard() {
        if (stmt_ != nullptr) sqlite3_finalize(stmt_);
    }
    StatementGuard(const StatementGuard&) = delete;
    StatementGuard& operator=(const StatementGuard&) = delete;
    sqlite3_stmt* get() const noexcept { return stmt_; }

private:
    sqlite3_stmt* stmt_;
};

const char* kSelectColumns =
    "SELECT id, url, canonical_url, title, description, content, headings, links,"
    " http_status, crawled_at FROM documents";

} // namespace

StorageManager::StorageManager(std::string dbPath, std::string indexPath)
    : dbPath_(std::move(dbPath)), indexPath_(std::move(indexPath)) {}

StorageManager::~StorageManager() {
    if (db_ != nullptr) sqlite3_close(db_);
}

bool StorageManager::open() {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_.clear();

    for (const std::string& path : {dbPath_, indexPath_}) {
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    }

    const int rc = sqlite3_open(dbPath_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        lastError_ = db_ != nullptr ? sqlite3_errmsg(db_) : "cannot open database";
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    const char* schema =
        "CREATE TABLE IF NOT EXISTS documents ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " url TEXT NOT NULL UNIQUE,"
        " canonical_url TEXT NOT NULL DEFAULT '',"
        " title TEXT NOT NULL DEFAULT '',"
        " description TEXT NOT NULL DEFAULT '',"
        " content TEXT NOT NULL DEFAULT '',"
        " headings TEXT NOT NULL DEFAULT '',"
        " links TEXT NOT NULL DEFAULT '',"
        " http_status INTEGER NOT NULL DEFAULT 0,"
        " crawled_at INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS meta ("
        " key TEXT PRIMARY KEY,"
        " value INTEGER NOT NULL"
        ");";
    return execute(schema);
}

bool StorageManager::execute(const std::string& sql) {
    char* errorMessage = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errorMessage);
    if (rc != SQLITE_OK) {
        lastError_ = errorMessage != nullptr ? errorMessage : "sqlite exec failed";
        if (errorMessage != nullptr) sqlite3_free(errorMessage);
        return false;
    }
    return true;
}

bool StorageManager::prepare(const std::string& sql, sqlite3_stmt** stmt) const {
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

StorageManager::UpsertResult StorageManager::upsertDocument(const Document& document) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastError_.clear();
    UpsertResult result;

    sqlite3_stmt* findStmt = nullptr;
    if (!prepare("SELECT id FROM documents WHERE url = ?", &findStmt)) return result;
    StatementGuard findGuard(findStmt);
    sqlite3_bind_text(findStmt, 1, document.url.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(findStmt) == SQLITE_ROW) {
        result.id = sqlite3_column_int64(findStmt, 0);
    }

    const std::string headings = joinLines(document.headings);
    const std::string links = joinLines(document.links);

    if (result.id >= 0) {
        sqlite3_stmt* updateStmt = nullptr;
        if (!prepare("UPDATE documents SET canonical_url = ?, title = ?, description = ?,"
                     " content = ?, headings = ?, links = ?, http_status = ?,"
                     " crawled_at = ? WHERE id = ?",
                     &updateStmt)) {
            return result;
        }
        StatementGuard guard(updateStmt);
        sqlite3_bind_text(updateStmt, 1, document.canonical_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateStmt, 2, document.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateStmt, 3, document.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateStmt, 4, document.content.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateStmt, 5, headings.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(updateStmt, 6, links.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(updateStmt, 7, document.http_status);
        sqlite3_bind_int64(updateStmt, 8, document.crawled_at);
        sqlite3_bind_int64(updateStmt, 9, result.id);
        if (sqlite3_step(updateStmt) != SQLITE_DONE) {
            lastError_ = sqlite3_errmsg(db_);
            return result;
        }
        return result;  // stable id kept, inserted = false
    }

    sqlite3_stmt* insertStmt = nullptr;
    if (!prepare("INSERT INTO documents (url, canonical_url, title, description, content,"
                 " headings, links, http_status, crawled_at)"
                 " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                 &insertStmt)) {
        return result;
    }
    StatementGuard guard(insertStmt);
    sqlite3_bind_text(insertStmt, 1, document.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 2, document.canonical_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 3, document.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 4, document.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 5, document.content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 6, headings.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insertStmt, 7, links.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insertStmt, 8, document.http_status);
    sqlite3_bind_int64(insertStmt, 9, document.crawled_at);
    if (sqlite3_step(insertStmt) != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return result;
    }
    result.id = sqlite3_last_insert_rowid(db_);
    result.inserted = true;
    return result;
}

std::optional<Document> StorageManager::loadDocument(DocumentId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare(std::string(kSelectColumns) + " WHERE id = ?", &stmt)) {
        return std::nullopt;
    }
    StatementGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_ROW) return std::nullopt;
    return documentFromColumns(stmt);
}

std::optional<DocumentId> StorageManager::findIdByUrl(const std::string& url) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT id FROM documents WHERE url = ?", &stmt)) return std::nullopt;
    StatementGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) return std::nullopt;
    return sqlite3_column_int64(stmt, 0);
}

std::vector<Document> StorageManager::loadAllDocuments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Document> documents;
    sqlite3_stmt* stmt = nullptr;
    if (!prepare(std::string(kSelectColumns) + " ORDER BY id", &stmt)) return documents;
    StatementGuard guard(stmt);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        documents.push_back(documentFromColumns(stmt));
    }
    return documents;
}

bool StorageManager::deleteDocument(DocumentId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("DELETE FROM documents WHERE id = ?", &stmt)) return false;
    StatementGuard guard(stmt);
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        lastError_ = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

std::size_t StorageManager::documentCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT COUNT(*) FROM documents", &stmt)) return 0;
    StatementGuard guard(stmt);
    if (sqlite3_step(stmt) != SQLITE_ROW) return 0;
    return static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
}

// ---------------------------------------------------------------------------
// Counters
// ---------------------------------------------------------------------------

std::int64_t StorageManager::getMeta(const std::string& key, std::int64_t fallback) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("SELECT value FROM meta WHERE key = ?", &stmt)) return fallback;
    StatementGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) return fallback;
    return sqlite3_column_int64(stmt, 0);
}

void StorageManager::setMeta(const std::string& key, std::int64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (!prepare("INSERT INTO meta (key, value) VALUES (?, ?)"
                 " ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                 &stmt)) {
        return;
    }
    StatementGuard guard(stmt);
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, value);
    sqlite3_step(stmt);
}

void StorageManager::addMeta(const std::string& key, std::int64_t delta) {
    setMeta(key, getMeta(key) + delta);
}

// ---------------------------------------------------------------------------
// Index persistence
// ---------------------------------------------------------------------------

bool StorageManager::saveIndex(const InvertedIndex& index) const {
    // Sorted vocabulary -> deterministic, diff-friendly files.
    std::vector<const std::pair<const std::string, PostingList>*> termEntries;
    termEntries.reserve(index.termMap().size());
    for (const auto& entry : index.termMap()) termEntries.push_back(&entry);
    std::sort(termEntries.begin(), termEntries.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
    for (const auto* entry : termEntries) {
        if (entry->first.size() > 0xFFFF) {
            lastError_ = "term too long to serialize: '" + entry->first + "'";
            return false;
        }
    }

    std::string buffer;
    buffer.reserve(4096);
    buffer.append(kMagic, 5);
    buffer.push_back(static_cast<char>(kMajorVersion));
    buffer.push_back(static_cast<char>(kMinorVersion));

    appendU32(buffer, static_cast<std::uint32_t>(termEntries.size()));
    for (const auto* entry : termEntries) {
        const std::string& term = entry->first;
        appendU16(buffer, static_cast<std::uint16_t>(term.size()));
        buffer.append(term);
        appendU32(buffer, entry->second.df());
        DocumentId previousDoc = 0;
        for (const Posting& posting : entry->second.postings) {
            appendVarint(buffer, static_cast<std::uint64_t>(posting.doc - previousDoc));
            appendVarint(buffer, posting.tf_title);
            appendVarint(buffer, posting.tf_heading);
            appendVarint(buffer, posting.tf_body);
            appendVarint(buffer, static_cast<std::uint64_t>(posting.positions.size()));
            bool firstPosition = true;
            std::uint32_t previousPos = 0;
            for (const std::uint32_t pos : posting.positions) {
                if (firstPosition) {
                    appendVarint(buffer, pos);
                    firstPosition = false;
                } else {
                    appendVarint(buffer, static_cast<std::uint64_t>(pos - previousPos));
                }
                previousPos = pos;
            }
            previousDoc = posting.doc;
        }
    }

    const std::vector<DocumentId> docIds = index.allDocumentIds();
    appendU32(buffer, static_cast<std::uint32_t>(docIds.size()));
    for (DocumentId id : docIds) {
        const DocMeta* meta = index.findDocument(id);
        appendVarint(buffer, static_cast<std::uint64_t>(id));
        appendVarint(buffer, meta->title_len);
        appendVarint(buffer, meta->heading_len);
        appendVarint(buffer, meta->body_len);
        appendVarint(buffer, meta->heading_count);
        appendU32(buffer, static_cast<std::uint32_t>(meta->doc_terms.size()));
        for (const std::string& term : meta->doc_terms) {
            appendU16(buffer, static_cast<std::uint16_t>(term.size()));
            buffer.append(term);
        }
    }

    // Atomic write: temp file, flush, rename over the previous index.
    const std::string tmpPath = indexPath_ + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        out.flush();
        if (!out) {
            lastError_ = "failed writing index file: " + tmpPath;
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmpPath, indexPath_, ec);
    if (ec) {
        // Rename can fail across filesystems; fall back to copy+remove.
        std::filesystem::copy_file(tmpPath, indexPath_,
                                    std::filesystem::copy_options::overwrite_existing, ec);
        std::error_code removeEc;
        std::filesystem::remove(tmpPath, removeEc);
        if (ec) {
            lastError_ = "failed replacing index file: " + ec.message();
            return false;
        }
    }
    return true;
}

StorageManager::LoadStatus StorageManager::loadIndex(InvertedIndex& index) const {
    std::ifstream in(indexPath_, std::ios::binary);
    if (!in) return LoadStatus::Missing;

    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 7 || std::memcmp(data.data(), kMagic, 5) != 0
        || static_cast<std::uint8_t>(data[5]) != kMajorVersion) {
        return LoadStatus::Corrupted;
    }

    index.clear();
    try {
        ByteReader reader(data);
        for (int i = 0; i < 5; ++i) reader.readU8();  // magic (already checked)
        if (reader.readU8() != kMajorVersion) throw std::runtime_error("version mismatch");
        (void)reader.readU8();  // minor version - informational

        std::unordered_map<std::string, PostingList> terms;
        const std::uint32_t termCount = reader.readU32();
        terms.reserve(termCount);
        for (std::uint32_t t = 0; t < termCount; ++t) {
            const std::string term = reader.readString(reader.readU16());
            const std::uint32_t df = reader.readU32();
            PostingList list;
            list.postings.resize(df);
            DocumentId previousDoc = 0;
            for (std::uint32_t p = 0; p < df; ++p) {
                Posting& posting = list.postings[p];
                const std::uint64_t delta = reader.readVarint();
                posting.doc = (p == 0) ? static_cast<DocumentId>(delta)
                                       : previousDoc + static_cast<DocumentId>(delta);
                if (posting.doc < 0 || (p > 0 && posting.doc <= previousDoc)) {
                    throw std::runtime_error("postings not ascending");
                }
                previousDoc = posting.doc;
                posting.tf_title = static_cast<std::uint32_t>(reader.readVarint());
                posting.tf_heading = static_cast<std::uint32_t>(reader.readVarint());
                posting.tf_body = static_cast<std::uint32_t>(reader.readVarint());
                const std::uint64_t positionCount = reader.readVarint();
                posting.positions.resize(static_cast<std::size_t>(positionCount));
                std::uint32_t previousPos = 0;
                for (std::uint64_t q = 0; q < positionCount; ++q) {
                    const std::uint64_t value = reader.readVarint();
                    const std::uint32_t pos =
                        (q == 0) ? static_cast<std::uint32_t>(value)
                                 : previousPos + static_cast<std::uint32_t>(value);
                    if (q > 0 && pos <= previousPos) {
                        throw std::runtime_error("positions not ascending");
                    }
                    posting.positions[static_cast<std::size_t>(q)] = pos;
                    previousPos = pos;
                }
            }
            if (!terms.emplace(std::move(term), std::move(list)).second) {
                throw std::runtime_error("duplicate term entry");
            }
        }

        std::unordered_map<DocumentId, DocMeta> docs;
        const std::uint32_t docCount = reader.readU32();
        docs.reserve(docCount);
        for (std::uint32_t d = 0; d < docCount; ++d) {
            DocMeta meta;
            meta.id = static_cast<DocumentId>(reader.readVarint());
            meta.title_len = static_cast<std::uint32_t>(reader.readVarint());
            meta.heading_len = static_cast<std::uint32_t>(reader.readVarint());
            meta.body_len = static_cast<std::uint32_t>(reader.readVarint());
            meta.heading_count = static_cast<std::uint32_t>(reader.readVarint());
            const std::uint32_t docTermCount = reader.readU32();
            meta.doc_terms.resize(docTermCount);
            for (std::uint32_t k = 0; k < docTermCount; ++k) {
                meta.doc_terms[k] = reader.readString(reader.readU16());
            }
            if (meta.id < 0 || !docs.emplace(meta.id, std::move(meta)).second) {
                throw std::runtime_error("duplicate document entry");
            }
        }

        if (reader.remaining() != 0) throw std::runtime_error("trailing bytes in index file");

        // Structural cross-checks before accepting the file.
        std::size_t totalPostings = 0;
        for (const auto& entry : terms) {
            totalPostings += entry.second.postings.size();
            for (const Posting& posting : entry.second.postings) {
                if (docs.count(posting.doc) == 0) {
                    throw std::runtime_error("posting references unknown document");
                }
            }
        }
        std::uint64_t tokenSum = 0;
        for (const auto& entry : docs) tokenSum += entry.second.totalTokens();

        index.restore(std::move(terms), std::move(docs), totalPostings, tokenSum);
    } catch (const std::exception&) {
        index.clear();
        return LoadStatus::Corrupted;
    }
    return LoadStatus::Loaded;
}

bool StorageManager::deleteIndexFile() const {
    std::error_code ec;
    const bool removed = std::filesystem::remove(indexPath_, ec);
    return !ec && removed;
}

std::uintmax_t StorageManager::indexFileSize() const {
    std::error_code ec;
    const auto size = std::filesystem::file_size(indexPath_, ec);
    return ec ? 0 : size;
}

bool StorageManager::wipeAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!execute("DELETE FROM documents; DELETE FROM meta;")) return false;
    }
    deleteIndexFile();
    return true;
}

} // namespace minisearch
