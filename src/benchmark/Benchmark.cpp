#include "benchmark/Benchmark.h"

#include "common/Config.h"
#include "engine/SearchEngine.h"
#include "processor/TextProcessor.h"
#include "query/QueryParser.h"
#include "query/QueryProcessor.h"
#include "ranking/BM25.h"
#include "ranking/TFIDF.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace minisearch {

namespace {

using Clock = std::chrono::steady_clock;

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct BruteDocument {
    std::unordered_map<std::string, std::array<std::uint32_t, 3>> termCounts;  // t/h/b
    std::uint32_t titleLen = 0;
    std::uint32_t headingLen = 0;
    std::uint32_t bodyLen = 0;
};

const char* kQueries[] = {
    "search engine",
    "inverted index",
    "c++ programming",
    "bm25 ranking",
    "python programming language",
    "web crawler robots",
    "sql database",
    "tokenization text processing",
    "\"competitive programming\"",
    "tfidf OR bm25",
};

std::string readCpuModel() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.compare(0, 10, "model name") == 0) {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string model = line.substr(colon + 1);
                while (!model.empty() && model.front() == ' ') model.erase(model.begin());
                return model;
            }
        }
    }
    return "unknown";
}

std::string formatMicros(double milliseconds) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%10.1f us", milliseconds * 1000.0);
    return buffer;
}

// Brute-force scan over pre-analyzed documents: df pass + scoring pass.
// (Documents are tokenized once *outside* the timed region - an index-less
// engine would have to tokenize per query, so this is the generous bound.)
double bruteForceQuery(const std::vector<BruteDocument>& corpus,
                       const std::vector<std::string>& terms,
                       const std::vector<std::size_t>& termDf,
                       std::vector<DocumentId>& hits) {
    hits.clear();
    double best = 0.0;
    const double totalDocs = static_cast<double>(corpus.size());
    for (std::size_t d = 0; d < corpus.size(); ++d) {
        const BruteDocument& document = corpus[d];
        const double weightedLength =
            3.0 * static_cast<double>(document.titleLen)
            + 2.0 * static_cast<double>(document.headingLen)
            + 1.0 * static_cast<double>(document.bodyLen);
        if (weightedLength <= 0.0) continue;

        bool any = false;
        double score = 0.0;
        for (std::size_t t = 0; t < terms.size(); ++t) {
            const auto it = document.termCounts.find(terms[t]);
            if (it == document.termCounts.end()) continue;
            any = true;
            const auto& counts = it->second;
            const double tf =
                (3.0 * static_cast<double>(counts[0])
                 + 2.0 * static_cast<double>(counts[1])
                 + 1.0 * static_cast<double>(counts[2]))
                / weightedLength;
            score += tf * TFIDFRanker::idf(static_cast<std::size_t>(totalDocs),
                                           static_cast<std::uint32_t>(termDf[t]));
        }
        if (any) {
            hits.push_back(static_cast<DocumentId>(d));
            if (score > best) best = score;
    }
    }
    return best;
}

} // namespace

int runBenchmarkCli(const std::vector<std::string>& args) {
    int runs = 20;
    int scale = 500;  // corpus replicas for the retrieval-core comparison
    std::string markdownPath;
    EngineConfig config;
    std::string dataDir;
    std::string configPath = "config/config.json";

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto value = [&]() -> std::string {
            return i + 1 < args.size() ? args[++i] : std::string();
        };
        if (arg == "--runs") {
            runs = std::max(1, std::atoi(value().c_str()));
        } else if (arg == "--scale") {
            scale = std::max(1, std::atoi(value().c_str()));
        } else if (arg == "--markdown") {
            markdownPath = value();
        } else if (arg == "--data-dir") {
            dataDir = value();
        } else if (arg == "--config") {
            configPath = value();
        } else {
            std::fprintf(stderr, "unknown benchmark option: %s\n", arg.c_str());
            return 1;
        }
    }

    const std::string configError = EngineConfig::loadFromFile(configPath, config);
    if (!configError.empty()) {
        std::fprintf(stderr, "[minisearch] config warning: %s\n", configError.c_str());
    }
    if (!dataDir.empty()) {
        config.dbPath = dataDir + "/database/minisearch.db";
        config.indexPath = dataDir + "/index/inverted.index";
    }

    SearchEngine engine(config);
    if (!engine.initialize()) return 1;

    const EngineStats stats = engine.stats();
    if (stats.documents == 0) {
        std::fprintf(stderr,
                     "the index is empty - crawl something first "
                     "(e.g. `minisearch crawl --seeds http://...`)\n");
        return 1;
    }

    const TextProcessor& processor = engine.processor();
    const std::vector<Document> realDocuments = engine.allDocumentsForBenchmark();

    // ------------------------------------------------------------------
    // Part 1: engine end-to-end on the real index (retrieval + BM25 +
    // snippets + metadata reads - what a user actually experiences).
    // ------------------------------------------------------------------
    std::vector<double> enginePerQuery;
    double engineTotalMs = 0.0;
    for (const char* queryText : kQueries) {
        SearchRequest request;
        request.query = queryText;
        request.algorithm = SearchRequest::Algorithm::BM25;
        request.limit = 10;
        engine.search(request);  // warm-up
        const auto start = Clock::now();
        for (int r = 0; r < runs; ++r) engine.search(request);
        const double ms = millisecondsSince(start) / static_cast<double>(runs);
        enginePerQuery.push_back(ms);
        engineTotalMs += ms;
    }

    // ------------------------------------------------------------------
    // Part 2: retrieval-core comparison at scale, in memory only.
    // The real corpus is replicated `scale` times (unique document ids),
    // which affects both sides identically. The inverted-index side runs
    // the real QueryProcessor + BM25 scorer; the brute-force side scans
    // every pre-analyzed document (df pass + scoring pass).
    // ------------------------------------------------------------------
    struct ScaledRow {
        std::string query;
        double indexMs = 0.0;
        double bruteMs = 0.0;
    };
    std::vector<ScaledRow> scaledRows;
    double indexTotalMs = 0.0;
    double bruteTotalMs = 0.0;
    std::size_t scaledDocuments = 0;

    {
        // Build the scaled corpora (untimed).
        InvertedIndex scaledIndex;
        std::vector<BruteDocument> bruteCorpus;
        DocumentId nextId = 1;
        for (int copy = 0; copy < scale; ++copy) {
            for (const Document& document : realDocuments) {
                const AnalyzedDocument analyzed = processor.analyze(document);
                scaledIndex.indexDocument(nextId, analyzed);

                BruteDocument brute;
                brute.titleLen = static_cast<std::uint32_t>(analyzed.title.size());
                for (const auto& heading : analyzed.headings) {
                    brute.headingLen += static_cast<std::uint32_t>(heading.size());
                    for (const Token& token : heading) {
                        brute.termCounts[token.term][1] += 1;
                    }
                }
                for (const Token& token : analyzed.title) {
                    brute.termCounts[token.term][0] += 1;
                }
                for (const Token& token : analyzed.body) {
                    brute.termCounts[token.term][2] += 1;
                }
                bruteCorpus.push_back(std::move(brute));
                ++nextId;
            }
        }
        scaledDocuments = bruteCorpus.size();

        const query::QueryParser parser(processor);
        const QueryProcessor evaluator(processor);
        const BM25Ranker ranker(config.bm25K1, config.bm25B, config.weights);
        CollectionStats collection;
        collection.totalDocs = scaledIndex.documentCount();
        double weightedSum = 0.0;
        for (const auto& entry : scaledIndex.documentMap()) {
            weightedSum += weightedDocumentLength(entry.second, config.weights);
        }
        collection.avgWeightedDocLength =
            collection.totalDocs > 0 ? weightedSum / static_cast<double>(collection.totalDocs)
                                     : 0.0;

        for (const char* queryText : kQueries) {
            ScaledRow row;
            row.query = queryText;

            const auto parsed = parser.parse(queryText);
            if (!parsed.ok) continue;
            std::vector<std::string> terms;
            for (const std::string& term : evaluator.evaluate(*parsed.root, scaledIndex).scoringTerms) {
                if (!processor.isStopword(term)) terms.push_back(term);
            }
            if (terms.empty()) continue;

            // Inverted index side: evaluate + BM25-score candidates.
            const auto indexRun = [&]() {
                const QueryEvaluation evaluation = evaluator.evaluate(*parsed.root, scaledIndex);
                double top = 0.0;
                for (DocumentId id : evaluation.candidates) {
                    const DocMeta* meta = scaledIndex.findDocument(id);
                    if (meta == nullptr) continue;
                    double score = 0.0;
                    for (const std::string& term : terms) {
                        const PostingList* list = scaledIndex.findTerm(term);
                        if (list == nullptr) continue;
                        const Posting* posting = scaledIndex.findPosting(term, id);
                        if (posting == nullptr) continue;
                        score += ranker.scoreTerm(term, *list, *posting, *meta, collection);
                    }
                    if (score > top) top = score;
                }
                return top;
            };
            indexRun();  // warm-up
            const auto indexStart = Clock::now();
            for (int r = 0; r < runs; ++r) indexRun();
            row.indexMs = millisecondsSince(indexStart) / static_cast<double>(runs);

            // Brute side: pre-compute df per term (a scan - that's the
            // price of having no index), then the scoring scan.
            const auto bruteRun = [&]() {
                std::vector<std::size_t> termDf(terms.size(), 0);
                for (std::size_t t = 0; t < terms.size(); ++t) {
                    for (const BruteDocument& document : bruteCorpus) {
                        if (document.termCounts.count(terms[t]) > 0) ++termDf[t];
                    }
                }
                std::vector<DocumentId> hits;
                return bruteForceQuery(bruteCorpus, terms, termDf, hits);
            };
            bruteRun();  // warm-up
            const auto bruteStart = Clock::now();
            for (int r = 0; r < runs; ++r) bruteRun();
            row.bruteMs = millisecondsSince(bruteStart) / static_cast<double>(runs);

            indexTotalMs += row.indexMs;
            bruteTotalMs += row.bruteMs;
            scaledRows.push_back(std::move(row));
        }
    }

    // ------------------------------------------------------------------
    // Rebuild timing (real engine, real index).
    // ------------------------------------------------------------------
    const auto reindexStart = Clock::now();
    engine.reindex();
    const double reindexMs = millisecondsSince(reindexStart);

    // ------------------------------------------------------------------
    // Report
    // ------------------------------------------------------------------
    const double engineAvg = engineTotalMs / static_cast<double>(enginePerQuery.size());
    const double indexAvg = indexTotalMs / static_cast<double>(scaledRows.size());
    const double bruteAvg = bruteTotalMs / static_cast<double>(scaledRows.size());
    const double coreSpeedup = indexAvg > 0.0 ? bruteAvg / indexAvg : 0.0;
    const std::string cpu = readCpuModel();

    std::printf("MiniSearch benchmark (all numbers measured live)\n\n");
    std::printf("index: %zu documents, %zu terms, %zu postings, %llu bytes\n",
                stats.documents, stats.uniqueTerms, stats.totalPostings,
                static_cast<unsigned long long>(stats.indexSizeBytes));
    std::printf("full reindex of the real corpus: %.2f ms\n\n", reindexMs);

    std::printf("A) Engine end-to-end (real index; retrieval + BM25 + snippets + SQLite)\n");
    std::printf("   queries: %zu   runs each: %d   mean: %.1f us\n\n",
                enginePerQuery.size(), runs, engineAvg * 1000.0);

    std::printf("B) Retrieval core at scale (%zu documents = real corpus x %d, in memory)\n",
                scaledDocuments, scale);
    std::printf("%-38s %14s %14s\n", "query", "inverted idx", "brute force");
    std::printf("%-38s %14s %14s\n", "--------------------------------------",
                "--------------", "--------------");
    for (const ScaledRow& row : scaledRows) {
        std::printf("%-38s %14s %14s\n", row.query.c_str(),
                    formatMicros(row.indexMs).c_str(), formatMicros(row.bruteMs).c_str());
    }
    std::printf("%-38s %14s %14s\n", "mean", formatMicros(indexAvg).c_str(),
                formatMicros(bruteAvg).c_str());
    std::printf("\nspeedup (brute force / inverted index): %.1fx\n", coreSpeedup);
    std::printf("runs per query: %d   cpu: %s\n", runs, cpu.c_str());

    if (!markdownPath.empty()) {
        std::ofstream out(markdownPath);
        if (!out) {
            std::fprintf(stderr, "cannot write markdown report: %s\n", markdownPath.c_str());
            return 1;
        }
        out << "# MiniSearch benchmark results\n\n";
        out << "Produced by `minisearch benchmark --markdown " << markdownPath
            << "` - every number below is a live measurement on this machine.\n\n";
        out << "- real index: " << stats.documents << " documents, " << stats.uniqueTerms
            << " terms, " << stats.totalPostings << " postings, " << stats.indexSizeBytes
            << " bytes on disk\n";
        out << "- full reindex of the real corpus: " << reindexMs << " ms\n";
        out << "- retrieval-core scale: " << scaledDocuments
            << " documents (the real corpus replicated " << scale
            << "x, unique ids - affects both sides identically)\n";
        out << "- runs per query: " << runs << "\n";
        out << "- cpu: " << cpu << "\n\n";
        out << "## A) Engine end-to-end\n\n";
        out << "The full user-facing path: query parsing, inverted-index retrieval, BM25\n";
        out << "scoring, snippet generation and SQLite metadata reads. Mean over "
            << enginePerQuery.size() << " queries: **" << engineAvg * 1000.0 << " us** per search.\n\n";
        out << "## B) Retrieval core: inverted index vs brute-force scan\n\n";
        out << "Same documents, same queries, in-memory, BM25 scoring on both sides. The\n";
        out << "brute-force side is given a generous bound: documents arrive pre-analyzed\n";
        out << "(tokenization is not timed), but it must still scan every document twice\n";
        out << "per query (df counting + scoring).\n\n";
        out << "| query | inverted index (avg) | brute force (avg) |\n";
        out << "|---|---|---|\n";
        for (const ScaledRow& row : scaledRows) {
            out << "| `" << row.query << "` | " << formatMicros(row.indexMs) << " | "
                << formatMicros(row.bruteMs) << " |\n";
        }
        out << "| **mean** | **" << formatMicros(indexAvg) << "** | **"
            << formatMicros(bruteAvg) << "** |\n";
        out << "\n**Mean speedup: " << coreSpeedup << "x.**\n";
        std::printf("markdown report written to %s\n", markdownPath.c_str());
    }
    return 0;
}

} // namespace minisearch
