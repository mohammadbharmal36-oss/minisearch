# MiniSearch benchmark results

Produced by `minisearch benchmark --markdown docs/benchmarks.md` - every number below is a live measurement on this machine.

- real index: 21 documents, 750 terms, 1866 postings, 34970 bytes on disk
- full reindex of the real corpus: 2.21265 ms
- retrieval-core scale: 10500 documents (the real corpus replicated 500x, unique ids - affects both sides identically)
- runs per query: 25
- cpu: Intel(R) Xeon(R) Processor

## A) Engine end-to-end

The full user-facing path: query parsing, inverted-index retrieval, BM25
scoring, snippet generation and SQLite metadata reads. Mean over 10 queries: **157.179 us** per search.

## B) Retrieval core: inverted index vs brute-force scan

Same documents, same queries, in-memory, BM25 scoring on both sides. The
brute-force side is given a generous bound: documents arrive pre-analyzed
(tokenization is not timed), but it must still scan every document twice
per query (df counting + scoring).

| query | inverted index (avg) | brute force (avg) |
|---|---|---|
| `search engine` |     2640.6 us |     3955.1 us |
| `inverted index` |     2153.6 us |     4738.6 us |
| `c++ programming` |     1406.2 us |     4573.1 us |
| `bm25 ranking` |      738.9 us |     4665.3 us |
| `python programming language` |      730.8 us |     6743.8 us |
| `web crawler robots` |      435.2 us |     5898.8 us |
| `sql database` |      343.3 us |     4558.8 us |
| `tokenization text processing` |      267.9 us |     5457.4 us |
| `"competitive programming"` |     1746.2 us |     4982.5 us |
| `tfidf OR bm25` |      609.4 us |     3807.1 us |
| **mean** | **    1107.2 us** | **    4938.1 us** |

**Mean speedup: 4.45994x.**
