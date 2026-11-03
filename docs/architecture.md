# MiniSearch — Architecture

Version 1.0 (final). Companion documents: `algorithms.md` (exact rules and
formulas), `benchmarks.md` (measured numbers), `README.md` (usage).

## 1. Goals and constraints

Build a real keyword search engine over real crawled webpages:

- crawl → download → parse → analyze → index → query → rank → serve;
- every displayed result traces back to an indexed document;
- the inverted index, TF-IDF, BM25 and query processing are hand-written
  C++17 (spec §21: no existing search engine, no third-party retrieval or
  ranking logic);
- external libraries only for: networking (libcurl), HTML parsing (Gumbo),
  JSON (nlohmann), storage (SQLite), HTTP framework (Crow), front-end
  (vanilla HTML/CSS/JS);
- modular: one header + implementation pair per module, no god-files;
- buildable and tested after every development phase;
- architecture leaves room for later semantic/vector retrieval without
  redesign (spec §19).

## 2. Layering: engine as a library

```
┌──────────────────────────────────────────────────────────────────┐
│ Front-ends (thin, no engine logic)                               │
│  • CLI (src/main.cpp): crawl/search/stats/doc/reindex/wipe/      │
│    benchmark/serve/demo/tokenize                                 │
│  • HTTP API (src/api/SearchAPI.cpp, Crow) + static web UI        │
│    served from frontend/                                         │
├──────────────────────────────────────────────────────────────────┤
│ SearchEngine facade (src/engine)                                 │
│   the ONLY entry point both front-ends call                      │
├──────────────────────────────────────────────────────────────────┤
│ minisearch_core modules                                          │
│  processor │ index │ ranking │ query │ storage │ parser │ crawler │
└──────────────────────────────────────────────────────────────────┘
```

Rationale:

- every algorithm is unit-testable with fixture documents, no network;
- both front-ends call identical core functions, so the web UI can never
  bypass the real engine (no fake results, by construction);
- the future semantic layer plugs in beside the ranking module without
  touching the HTTP surface.

## 3. Pipeline and module inventory (as built)

```
WEBSITES
   │  seeds via CLI / POST /api/crawl
   ▼
WebCrawler ──► HttpDownloader (libcurl, per-worker handles)   robots.txt cache
   │            redirects capped, timeouts, size cap,         per-host politeness
   │            automatic gzip decode
   ▼ PageOutcome{ok, skipped, failed, Document, links}
SearchEngine.onPage
   │  upsert by URL (SQLite row id = stable document id)
   │  analyze → indexDocument (exclusive lock) → save index file
   ▼
InvertedIndex  ◄──────────────►  StorageManager
   │                             SQLite: documents + meta counters
   │                             binary index file: varint+delta postings
   ▼ search()
QueryParser (recursive descent) → QueryProcessor (two-pointer ops,
   │                              phrase position verification)
   ▼ candidates + scoring terms
TFIDFRanker | BM25Ranker (strategies; field boosts 3/2/1)
   ▼ scored, sorted, page-sliced
snippet generation (window around first match) → SearchAPI JSON → Web UI
```

| Module | Files | Key types | Tests |
|---|---|---|---|
| Document model | `common/Document.{h,cpp}` | `Document`, `DocumentId` | 6 |
| Text processor | `processor/TextProcessor.{h,cpp}` | `Token`, `AnalyzedDocument`, config | 29 |
| Inverted index | `index/InvertedIndex.{h,cpp}` | `Posting`, `PostingList`, `DocMeta` | 13 |
| Ranking | `ranking/{Ranker,TFIDF,BM25}.{h,cpp}` | `Ranker`, `FieldWeights`, `CollectionStats` | 13 |
| Query language | `query/{QueryParser,QueryProcessor}.{h,cpp}` | AST, `QueryEvaluation` | 25 |
| Persistence | `storage/StorageManager.{h,cpp}` | upsert, meta counters, binary codec | 15 |
| HTML parser | `parser/HTMLParser.{h,cpp}` | `Result{Document, skippedHrefs}` | 15 |
| URL utils | `crawler/Url.{h,cpp}` | normalize/resolve/hostOf | 12 (with parser file) |
| robots.txt | `crawler/RobotsTxt.{h,cpp}` | `RobotsTxt` | 8 |
| Crawler | `crawler/{WebCrawler,HttpDownloader}.{h,cpp}` | `CrawlConfig`, `PageOutcome`, `ICrawlSink` | 4 unit + integration |
| Engine facade | `engine/SearchEngine.{h,cpp}` | search/stats/suggest/crawl lifecycle | 12 integration |
| API | `api/SearchAPI.{h,cpp}` | Crow routes, static serving | 12 integration |
| Benchmark | `benchmark/Benchmark.{h,cpp}` | 3-way measurement | manual + markdown |

## 4. Data contracts

### 4.1 Document

`{id, url, canonical_url, title, description, content, headings[], links[],
http_status, crawled_at}` — the pipeline's universal cargo. SQLite is the
source of truth (URL is the unique key); the index file is a derived cache
that can always be rebuilt from it.

### 4.2 Tokens, fields, virtual positions

`Token{term, position, field}`, `Field ∈ {Title, Heading, Body}`. Positions
are per-field ordinals; the index maps them onto a **virtual position
stream** per document:

```
[ title tokens | gap | heading₁ | gap | heading₂ | gap | ... | body tokens ]
```

Gaps exist only where a boundary exists (a pure-body document has body
positions equal to ordinals, matching the spec's §8 example). Consequences:

- field-weighted ranking: `tf_weighted = 3·tf_title + 2·tf_heading + 1·tf_body`
  (weights configurable via config.json);
- phrase search verifies consecutive positions, so a phrase can never
  falsely match across a title/body or heading/heading boundary;
- `DocMeta` classifies a position into its field in O(1).

### 4.3 Posting lists

`term → [Posting{doc_id, tf_title, tf_heading, tf_body, positions[]}]`,
always sorted by `doc_id` (binary-search insert on update; appends dominate
during bulk builds). `unordered_map` gives O(1) average term lookup — a
keyword query never scans documents. `DocMeta::doc_terms` (distinct terms
per document) enables O(terms-of-doc) removal.

## 5. Query language

```
expr    := or_expr
or_expr := and_expr ( OR and_expr )*
and_expr := not_expr ( [AND] not_expr )*     // adjacency implies AND
not_expr := primary ( NOT primary )*
primary := '(' expr ')' | '"' phrase '"' | TERM
```

- operators case-insensitive; terms normalized by the same tokenizer as
  indexing (technical-term mapping included);
- phrases keep stop words (positions are indexed for them);
- evaluation: two-pointer intersect/union/subtract over sorted lists;
  `NOT` operands subtract and never contribute to scoring;
- syntax errors (unbalanced quotes/parens, dangling operators, leading NOT,
  empty query) produce structured 400 errors, never silent fallbacks.

## 6. Persistence

- **SQLite**: `documents` table (stable AUTOINCREMENT ids; upsert by URL
  keeps ids stable across re-crawls and restarts) + `meta` counters
  (pages_crawled, pages_failed, crawls_run).
- **Binary index file** (`data/index/inverted.index`): magic `MSIDX` +
  version, sorted vocabulary, per posting varint doc-id deltas, per-field
  tfs, delta-encoded positions; then per-document lens + term lists.
  Deterministic byte layout (identical index → identical file).
- **Atomic saves**: write `*.tmp`, flush, rename; a crash mid-save can
  never destroy a good index.
- **Loading**: header + full structural validation (ascending ids and
  positions, df consistency, postings referencing known documents, no
  trailing bytes); anything wrong → `Corrupted` → honest rebuild from
  SQLite, logged.
- **Startup reconciliation**: documents added by other runs are indexed
  incrementally; postings of deleted documents are dropped.

## 7. Crawler

- BFS frontier `deque<(url, depth)>`; `unordered_set` visited (normalized
  URLs — lowercase scheme/host, no fragment, default ports dropped);
  per-host next-allowed timestamps enforce politeness (max of configured
  delay and robots Crawl-delay).
- robots.txt fetched once per host by the first task; **no second dispatch
  to a host until its robots rules are cached** (correctness under
  concurrency). Unreachable robots = allow-all per RFC 9309.
- Worker pool (default 4) parallelizes across **different** hosts only;
  same-host requests are serialized by the dispatcher. One libcurl easy
  handle per worker.
- Budgets: max pages (attempted fetches), max depth; filters: allowed
  domains (host or suffix), login/admin-like URL blacklist, non-HTML
  content types, duplicate redirect targets, 2 MB size cap.
- Every decision (ok/failed/skipped + reason) is reported to the sink and
  kept in a 50-entry crawl log exposed via `/api/crawl/status`.

## 8. API and frontend

Crow serves: the JSON API (see README for the endpoint table), the static
frontend, and — on **port+1** — the seed corpus as its own site so its
`robots.txt` sits at the site root (the crawler fetches robots at host
root, per RFC). Validation: 400 (bad input), 404 (unknown doc/route), 409
(crawl already running), 500 (generic; internals never leaked).

The frontend is 4 HTML pages + one stylesheet + 4 vanilla JS modules
(~1,100 lines): live suggestions with keyboard navigation, snippet
highlighting, pagination, algorithm/page-size selectors, analytics with
auto-refresh and top-term bars, and a crawler console with a progress bar
and log table. Zero frameworks, zero build step, zero mocked data.

## 9. Threading model

- Searches: shared lock (`std::shared_mutex`) over the in-memory index.
- Crawl indexing: exclusive lock per page (brief — tokenization + posting
  merges + index save; searches may pause for milliseconds during a crawl).
- SQLite: internal mutex in `StorageManager` (one connection).
- Crawler: dispatcher + worker pool as in §7; the engine sink is called
  single-threaded from the dispatcher.
- One crawl job at a time (409 otherwise).

## 10. Testing strategy

| Suite | Count | Covers |
|---|---|---|
| document_tests | 6 | model semantics, debug output |
| text_processor_tests | 29 | tokenization, protected terms, stemmer, stop words, fields |
| index_tests | 13 | spec §8 example verbatim, add/update/remove, sorting, stats, gaps |
| ranking_tests | 13 | hand-computed TF-IDF/BM25 scores, boosts, saturation, guards |
| query_tests | 25 | lexer/parser errors, AND/OR/NOT/phrase semantics, boundaries |
| persistence_tests | 15 | round-trips, corruption, incremental, restart, determinism |
| parser_tests | 15 | RFC 3986 resolution + Gumbo extraction |
| crawler_tests | 4 (+8 robots) | robots rules, offline crawler behavior |
| integration_tests | 12 | real HTTP: boot, crawl corpus, robots, all query types, both algorithms, suggest, stats, document, errors, restart persistence |

The phase-1 harness (`tests/test_framework.h`) mirrors the GoogleTest
macro interface. Decision: keep it rather than vendoring GoogleTest — it
is ~150 dependency-free lines, the migration is mechanical, and the whole
suite runs in under a second (plus ~15 s for integration, which boots a
real server and crawls 21 pages over loopback HTTP).

## 11. Risk register (final status)

| Risk | Outcome |
|---|---|
| Tokenization destroys technical terms | protected-term table + tests — resolved |
| Stemmer hurts recall | documented quirks; configurable; Snowball listed as future work |
| Phrase false-positives across fields | virtual stream gaps — resolved + tested |
| Index corruption on crash | versioned header, structural validation, atomic rename — resolved + tested |
| Crow dependency footprint | vendored via configure-time download, system Boost.Asio — resolved |
| Impolite crawling | robots cache, per-host serialization, crawl-delay honored — resolved + tested live |
| Benchmark dishonesty | fixed queries, warm-ups, live measurements, methodology documented |
| Sandbox DNS blocking unit tests | unit tests use loopback only; integration uses in-process HTTP |
| Unicode text | documented ASCII limitation, pinned by test |

## 12. Future extensions (design room reserved)

`VectorIndex` beside the inverted index (the `Ranker` interface and
`SearchEngine` facade leave room for a hybrid retrieval path), cosine
similarity, HNSW ANN, autocomplete beyond prefixes, server-side result
highlighting, PageRank over the link graph stored in `links`, distributed
crawling (the dispatcher/worker split is already message-shaped), MinHash
duplicate detection, Snowball stemming, UTF-8 tokenization.
