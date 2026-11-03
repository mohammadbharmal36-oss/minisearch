#pragma once

// MiniSearch - engine configuration.
//
// Defaults are compiled in; config/config.json overrides them; CLI flags
// override the file. The JSON schema mirrors the sections documented in
// config/config.json (which stays the human-readable reference).

#include "processor/TextProcessor.h"
#include "ranking/Ranker.h"

#include <string>

namespace minisearch {

struct EngineConfig {
    // storage
    std::string dbPath = "data/database/minisearch.db";
    std::string indexPath = "data/index/inverted.index";

    // text processing
    TextProcessorConfig text;

    // ranking
    FieldWeights weights;                 // title 3 / heading 2 / body 1
    double bm25K1 = 1.2;
    double bm25B = 0.75;
    std::string defaultAlgorithm = "bm25";  // "bm25" | "tfidf"

    // crawler defaults
    int maxPages = 50;
    int maxDepth = 2;
    long timeoutSeconds = 10;
    int delayMs = 500;
    int workerThreads = 4;
    std::string userAgent = "MiniSearchBot/1.0 (+MiniSearch educational search engine project)";

    // server
    std::string host = "0.0.0.0";
    int port = 8080;
    std::string staticRoot = "frontend";
    std::string corpusRoot = "tools/corpus";

    // Loads a JSON config file over the defaults.
    // Returns "" on success (including "file absent" - defaults kept),
    // or a human-readable error for malformed JSON / wrong types.
    static std::string loadFromFile(const std::string& path, EngineConfig& out);
};

} // namespace minisearch
