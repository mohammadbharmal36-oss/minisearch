// MiniSearch - command line front-end.
//
// Thin shell over the engine library: crawl, search, stats, document
// inspection, reindex, wipe and benchmark - plus the phase-1 pipeline
// inspection tools (demo/tokenize). The HTTP API (src/api) is the other
// front-end over the identical engine calls.

#include "api/SearchAPI.h"
#include "benchmark/Benchmark.h"
#include "common/Config.h"
#include "common/Document.h"
#include "engine/SearchEngine.h"
#include "processor/TextProcessor.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace minisearch;

void printUsage() {
    std::cout <<
        "MiniSearch 1.0 - a miniature search engine built from scratch\n"
        "\n"
        "Usage:\n"
        "  minisearch version\n"
        "  minisearch demo\n"
        "  minisearch tokenize [--no-stem] [--no-tech-map] [--stopwords] < text\n"
        "\n"
        "  minisearch crawl  --seeds URL[,URL...] | --seed-file FILE\n"
        "      [--max-pages N] [--max-depth N] [--delay-ms N] [--timeout SECONDS]\n"
        "      [--domains DOMAIN,...] [--workers N] [--data-dir DIR] [--config FILE]\n"
        "      Crawl, parse, index and persist pages (blocking, live progress).\n"
        "\n"
        "  minisearch search \"QUERY\" [--algo tfidf|bm25] [--limit N] [--offset N]\n"
        "      [--json] [--data-dir DIR] [--config FILE]\n"
        "      Query syntax: terms, AND / OR / NOT, \"exact phrases\", parentheses.\n"
        "\n"
        "  minisearch stats  [--json] [--data-dir DIR]\n"
        "  minisearch doc ID [--data-dir DIR]\n"
        "  minisearch reindex [--data-dir DIR]     rebuild the index from storage\n"
        "  minisearch benchmark [--runs N] [--markdown PATH] [--data-dir DIR]\n"
        "      brute-force scan vs inverted-index retrieval, with real timings\n"
        "  minisearch wipe [--yes]                 delete all indexed documents\n"
        "  minisearch serve [--port N] [--host H] [--data-dir DIR]\n"
        "      start the HTTP API + web UI (see README for endpoints)\n";
}

struct GlobalOptions {
    std::string dataDir;
    std::string configPath = "config/config.json";
};

// Applies --data-dir to storage paths (both live under <data-dir>/).
EngineConfig loadEngineConfig(const GlobalOptions& options) {
    EngineConfig config;
    if (!options.configPath.empty()) {
        const std::string error = EngineConfig::loadFromFile(options.configPath, config);
        if (!error.empty()) {
            std::fprintf(stderr, "[minisearch] config warning: %s (using defaults)\n",
                         error.c_str());
        }
    }
    if (!options.dataDir.empty()) {
        config.dbPath = options.dataDir + "/database/minisearch.db";
        config.indexPath = options.dataDir + "/index/inverted.index";
    }
    return config;
}

std::unique_ptr<SearchEngine> makeEngine(const GlobalOptions& options) {
    auto engine = std::make_unique<SearchEngine>(loadEngineConfig(options));
    if (!engine->initialize()) {
        std::fprintf(stderr, "[minisearch] engine initialization failed\n");
        return nullptr;
    }
    return engine;
}

// ---------------------------------------------------------------------------
// demo / tokenize (phase 1 inspection tools)
// ---------------------------------------------------------------------------

void printTokenList(const std::string& label, const std::vector<Token>& tokens) {
    std::cout << "  " << label << ":";
    for (const auto& token : tokens) {
        std::cout << ' ' << token.term << '@' << token.position;
    }
    std::cout << '\n';
}

int runDemo() {
    const TextProcessor processor;
    const auto& config = processor.config();

    std::cout << "MiniSearch text-processing pipeline demo\n";
    std::cout << "config: removeStopwords=" << config.removeStopwords
              << " applyStemming=" << config.applyStemming
              << " mapTechnicalTerms=" << config.mapTechnicalTerms << "\n\n";

    std::cout << "1) Protected technical terms (longest match, boundary aware)\n";
    std::cout << "   input : \"C++, C#, Node.js, ASP.NET and plain old C\"\n";
    std::cout << "   terms : ";
    for (const auto& term : processor.tokenizeQuery("C++, C#, Node.js, ASP.NET and plain old C")) {
        std::cout << term << ' ';
    }
    std::cout << "\n";

    std::cout << "\n2) Light stemmer (rule table in docs/algorithms.md)\n";
    for (const auto& word : {"programming", "programs", "program", "running",
                             "runs", "run", "parsed", "parsing", "parse",
                             "languages", "language", "installing", "install"}) {
        std::cout << "   " << word << " -> " << processor.stem(word) << '\n';
    }

    std::cout << "\n3) Document analysis: separate fields, positions restart per field\n";
    Document doc;
    doc.id = 1;
    doc.url = "https://example.org/cpp";
    doc.title = "C++ Programming Language";
    doc.headings = {"Overview", "Usage"};
    doc.content = "C++ is used in competitive programming";
    const auto analyzed = processor.analyze(doc);
    printTokenList("   title  ", analyzed.title);
    for (const auto& headingTokens : analyzed.headings) {
        printTokenList("   heading", headingTokens);
    }
    printTokenList("   body   ", analyzed.body);
    std::cout << "   total tokens: " << analyzed.totalTokenCount() << '\n';
    return 0;
}

int runTokenize(const std::vector<std::string>& flags) {
    TextProcessorConfig config;
    config.applyStemming = true;
    for (const std::string& flag : flags) {
        if (flag == "--no-stem") config.applyStemming = false;
        else if (flag == "--no-tech-map") config.mapTechnicalTerms = false;
        else if (flag == "--stopwords") config.removeStopwords = true;
        else {
            std::cerr << "Unknown option: " << flag << "\n";
            return 1;
        }
    }
    const TextProcessor processor(config);
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    const auto tokens = processor.tokenizeField(buffer.str(), Field::Body);
    for (const auto& token : tokens) {
        std::cout << token.position << '\t' << token.term << '\n';
    }
    std::cerr << "[info] " << tokens.size() << " tokens emitted (field=body)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// crawl
// ---------------------------------------------------------------------------

int runCrawl(const std::vector<std::string>& args) {
    std::vector<std::string> seeds;
    std::string seedFile;
    CrawlConfig crawl;
    GlobalOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&]() -> std::string {
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (arg == "--seeds") {
            std::istringstream stream(value());
            std::string seed;
            while (std::getline(stream, seed, ',')) {
                if (!seed.empty()) seeds.push_back(seed);
            }
        } else if (arg == "--seed-file") {
            seedFile = value();
        } else if (arg == "--max-pages") {
            crawl.maxPages = std::atoi(value().c_str());
        } else if (arg == "--max-depth") {
            crawl.maxDepth = std::atoi(value().c_str());
        } else if (arg == "--delay-ms") {
            crawl.delayMs = std::atoi(value().c_str());
        } else if (arg == "--timeout") {
            crawl.timeoutSeconds = std::atol(value().c_str());
        } else if (arg == "--workers") {
            crawl.workerThreads = std::atoi(value().c_str());
        } else if (arg == "--domains") {
            std::istringstream stream(value());
            std::string domain;
            while (std::getline(stream, domain, ',')) {
                if (!domain.empty()) crawl.allowedDomains.push_back(domain);
            }
        } else if (arg == "--data-dir") {
            options.dataDir = value();
        } else if (arg == "--config") {
            options.configPath = value();
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    if (!seedFile.empty()) {
        std::ifstream file(seedFile);
        if (!file) {
            std::cerr << "cannot read seed file: " << seedFile << "\n";
            return 1;
        }
        std::string seed;
        while (std::getline(file, seed)) {
            while (!seed.empty() && (seed.back() == '\r' || seed.back() == ' ')) seed.pop_back();
            if (!seed.empty() && seed[0] != '#') seeds.push_back(seed);
        }
    }

    if (seeds.empty()) {
        std::cerr << "no seed URLs given (--seeds or --seed-file)\n";
        return 1;
    }

    auto engine = makeEngine(options);
    if (engine == nullptr) return 1;

    const EngineConfig& engineConfig = engine->config();
    crawl.seedUrls = seeds;
    if (crawl.userAgent.empty()) crawl.userAgent = engineConfig.userAgent;

    std::string error;
    const int jobId = engine->startCrawl(crawl, &error);
    if (jobId < 0) {
        std::cerr << "crawl rejected: " << error << "\n";
        return 1;
    }

    std::cout << "crawl job " << jobId << " started (" << seeds.size() << " seed"
              << (seeds.size() == 1 ? "" : "s") << ", max " << crawl.maxPages
              << " pages, depth " << crawl.maxDepth << ", delay " << crawl.delayMs
              << "ms)\n";

    // Live progress until the job finishes.
    while (true) {
        const CrawlStatusSnapshot status = engine->crawlStatus();
        std::printf("\r  queued=%-4d fetched=%-4d ok=%-4d failed=%-4d skipped=%-4d",
                    status.queued, status.fetched, status.succeeded, status.failed,
                    status.skipped);
        std::fflush(stdout);
        if (!status.running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "\n";

    const CrawlStatusSnapshot status = engine->crawlStatus();
    std::cout << "crawl finished: " << status.succeeded << " pages indexed, "
              << status.failed << " failed, " << status.skipped << " skipped\n\n";
    if (!status.recent.empty()) {
        std::cout << "last pages:\n";
        const std::size_t shown = status.recent.size() > 10 ? 10 : status.recent.size();
        for (std::size_t i = status.recent.size() - shown; i < status.recent.size(); ++i) {
            const auto& entry = status.recent[i];
            std::cout << "  [" << entry.kind << "] " << entry.url;
            if (!entry.note.empty()) std::cout << " - " << entry.note;
            std::cout << '\n';
        }
    }

    const EngineStats stats = engine->stats();
    std::cout << "\nindex: " << stats.documents << " documents, " << stats.uniqueTerms
              << " unique terms, " << stats.totalPostings << " postings\n";
    std::cout << "try: ./build/minisearch search \"search engines\"\n";
    return 0;
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

int runSearch(const std::vector<std::string>& args) {
    std::string query;
    std::string algorithm = "bm25";
    int limit = 10;
    int offset = 0;
    bool json = false;
    GlobalOptions options;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&]() -> std::string {
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (arg == "--algo") {
            algorithm = value();
        } else if (arg == "--limit") {
            limit = std::atoi(value().c_str());
        } else if (arg == "--offset") {
            offset = std::atoi(value().c_str());
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--data-dir") {
            options.dataDir = value();
        } else if (arg == "--config") {
            options.configPath = value();
        } else if (query.empty()) {
            query = arg;
        } else {
            std::cerr << "Unexpected argument: " << arg << "\n";
            return 1;
        }
    }

    SearchRequest request;
    request.query = query;
    if (!parseAlgorithm(algorithm, request.algorithm)) {
        std::cerr << "unknown algorithm '" << algorithm << "' (use tfidf or bm25)\n";
        return 1;
    }
    request.limit = limit;
    request.offset = offset;

    auto engine = makeEngine(options);
    if (engine == nullptr) return 1;
    const SearchResponse response = engine->search(request);

    if (!response.ok) {
        std::cerr << "search error: " << response.error << "\n";
        return 1;
    }

    if (json) {
        std::cout << "{\n";
        std::cout << "  \"query\": \"" << response.query << "\",\n";
        std::cout << "  \"algorithm\": \"" << response.algorithm << "\",\n";
        std::cout << "  \"total\": " << response.total << ",\n";
        std::cout << "  \"took_ms\": " << response.tookMs << ",\n";
        std::cout << "  \"items\": [\n";
        for (std::size_t i = 0; i < response.items.size(); ++i) {
            const SearchItem& item = response.items[i];
            std::cout << "    {\"id\": " << item.id << ", \"title\": \"" << item.title
                      << "\", \"url\": \"" << item.url << "\", \"score\": " << item.score
                      << ", \"snippet\": \"" << item.snippet << "\"}"
                      << (i + 1 < response.items.size() ? "," : "") << "\n";
        }
        std::cout << "  ]\n}\n";
        return 0;
    }

    std::cout << response.total << " result" << (response.total == 1 ? "" : "s")
              << " for '" << response.query << "' [" << response.algorithm << ", "
              << response.tookMs << " ms]\n\n";
    if (response.items.empty()) {
        std::cout << "  (no matching documents - try the crawler first)\n";
        return 0;
    }
    for (std::size_t i = 0; i < response.items.size(); ++i) {
        const SearchItem& item = response.items[i];
        std::cout << static_cast<int>(i + 1 + static_cast<std::size_t>(std::max(0, offset)))
                  << ". " << item.title << "\n   " << item.url << "\n   score: "
                  << item.score << "  matched: ";
        for (const std::string& term : item.matchedTerms) std::cout << term << ' ';
        std::cout << "\n   " << item.snippet << "\n\n";
    }
    return 0;
}

// ---------------------------------------------------------------------------
// stats / doc / reindex / wipe
// ---------------------------------------------------------------------------

int runStats(const std::vector<std::string>& args, bool json) {
    GlobalOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data-dir") options.dataDir = args[++i];
        else if (args[i] == "--config") options.configPath = args[++i];
        else if (args[i] == "--json") json = true;
    }

    auto engine = makeEngine(options);
    if (engine == nullptr) return 1;
    const EngineStats stats = engine->stats();

    if (json) {
        std::cout << "{\"documents\": " << stats.documents
                  << ", \"unique_terms\": " << stats.uniqueTerms
                  << ", \"total_postings\": " << stats.totalPostings
                  << ", \"index_size_bytes\": " << stats.indexSizeBytes
                  << ", \"avg_document_tokens\": " << stats.averageDocumentTokens
                  << ", \"avg_search_latency_ms\": " << stats.averageSearchLatencyMs
                  << ", \"search_count\": " << stats.searchCount
                  << ", \"pages_crawled\": " << stats.pagesCrawled
                  << ", \"pages_failed\": " << stats.pagesFailed
                  << ", \"crawls_run\": " << stats.crawlsRun << "}\n";
        return 0;
    }

    std::cout << "MiniSearch index statistics\n";
    std::cout << "  documents indexed:   " << stats.documents << "\n";
    std::cout << "  unique terms:        " << stats.uniqueTerms << "\n";
    std::cout << "  total postings:      " << stats.totalPostings << "\n";
    std::cout << "  index size:          " << stats.indexSizeBytes << " bytes\n";
    std::cout << "  avg document tokens: " << stats.averageDocumentTokens << "\n";
    std::cout << "  searches served:     " << stats.searchCount << " (avg "
              << stats.averageSearchLatencyMs << " ms)\n";
    std::cout << "  pages crawled:       " << stats.pagesCrawled
              << " (failed: " << stats.pagesFailed << ", jobs: " << stats.crawlsRun << ")\n";
    if (!stats.topTerms.empty()) {
        std::cout << "  most common terms:  ";
        for (const auto& [term, df] : stats.topTerms) {
            std::cout << term << '(' << df << ") ";
        }
        std::cout << "\n";
    }
    return 0;
}

int runDoc(const std::vector<std::string>& args) {
    GlobalOptions options;
    DocumentId id = -1;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data-dir") options.dataDir = args[++i];
        else if (args[i] == "--config") options.configPath = args[++i];
        else if (id < 0) id = std::atoll(args[i].c_str());
    }
    if (id < 0) {
        std::cerr << "usage: minisearch doc ID\n";
        return 1;
    }

    auto engine = makeEngine(options);
    if (engine == nullptr) return 1;
    const auto document = engine->document(id);
    if (!document.has_value()) {
        std::cerr << "no document with id " << id << "\n";
        return 1;
    }
    std::cout << document->toDebugString() << "\n";
    std::cout << "headings:";
    for (const std::string& heading : document->headings) {
        std::cout << " [" << heading << "]";
    }
    std::cout << "\ncontent (first 400 chars):\n";
    std::cout << document->content.substr(0, 400) << "\n";
    return 0;
}

int runReindex(const std::vector<std::string>& args) {
    GlobalOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--data-dir") options.dataDir = args[++i];
        else if (args[i] == "--config") options.configPath = args[++i];
    }
    auto engine = makeEngine(options);
    if (engine == nullptr) return 1;
    return engine->reindex() ? 0 : 1;
}

int runWipe(const std::vector<std::string>& args) {
    bool confirmed = false;
    GlobalOptions options;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--yes") confirmed = true;
        else if (args[i] == "--data-dir") options.dataDir = args[++i];
        else if (args[i] == "--config") options.configPath = args[++i];
    }
    if (!confirmed) {
        std::cout << "this deletes every indexed document. pass --yes to confirm\n";
        return 1;
    }
    auto engine = makeEngine(options);
    if (engine == nullptr) return 1;
    return engine->wipe() ? 0 : 1;
}

int runServe(const std::vector<std::string>& args) {
    GlobalOptions options;
    int port = -1;
    std::string host;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&]() -> std::string {
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (arg == "--port") {
            port = std::atoi(value().c_str());
        } else if (arg == "--host") {
            host = value();
        } else if (arg == "--data-dir") {
            options.dataDir = value();
        } else if (arg == "--config") {
            options.configPath = value();
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    EngineConfig config = loadEngineConfig(options);
    if (port > 0) config.port = port;
    if (!host.empty()) config.host = host;

    SearchEngine engine(config);
    if (!engine.initialize()) return 1;

    const EngineStats stats = engine.stats();
    const int corpusPort = config.port + 1;
    std::cout << "MiniSearch server starting\n"
              << "  documents: " << stats.documents
              << "   terms: " << stats.uniqueTerms
              << "   pages crawled so far: " << stats.pagesCrawled << "\n"
              << "  listening on http://" << (config.host == "0.0.0.0" ? "localhost" : config.host)
              << ":" << config.port << "\n"
              << "  pages: /  /search  /analytics  /crawler\n"
              << "  api:  /api/search?q=...  /api/stats  /api/crawl  /api/crawl/status\n"
              << "  seed corpus (own site, robots.txt honored): http://localhost:"
              << corpusPort << "/\n"
              << "  press Ctrl+C to stop\n";

    SearchAPI api(engine, config);
    api.run();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }
    const std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    if (command == "version") {
        std::cout << "MiniSearch 1.0 (inverted index, TF-IDF, BM25, Boolean + phrase "
                     "queries, crawler, HTTP API)\n";
        return 0;
    }
    if (command == "demo") return runDemo();
    if (command == "tokenize") return runTokenize(args);
    if (command == "crawl") return runCrawl(args);
    if (command == "search") return runSearch(args);
    if (command == "stats") return runStats(args, false);
    if (command == "doc") return runDoc(args);
    if (command == "reindex") return runReindex(args);
    if (command == "wipe") return runWipe(args);
    if (command == "benchmark") return minisearch::runBenchmarkCli(args);
    if (command == "serve") return runServe(args);

    std::cerr << "Unknown command: " << command << "\n\n";
    printUsage();
    return 1;
}
