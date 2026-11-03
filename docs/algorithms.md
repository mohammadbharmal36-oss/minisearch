# MiniSearch — Algorithms and Data Structures

Version 1.0 (final). Everything below describes code that exists and is
tested; see the test files for executable proofs of each claim.

## 1. Tokenization (processor/TextProcessor)

### 1.1 Character classes (ASCII)

| Class | Characters | Role |
|---|---|---|
| Word characters | `A–Z a–z 0–9 _` | build generic tokens |
| Boundary characters | everything else | separate tokens |
| Special | `.` | may *start* a protected term (`.net`); joins digits (`3.14`) |

Non-ASCII bytes act as separators. **Known limitation** (`café → caf`),
pinned by `unicode_degrades_to_ascii_fragments`.

### 1.2 Extraction — single O(n) scan

At every potential token start:

1. **Protected-term match** (if enabled): technical-term table
   longest-surface-first; a match requires both sides to be token
   boundaries (`cppreference` is NOT matched as `cpp`; `foo.net` is NOT
   `.NET`; `asp.net` beats `.net` by length).
2. `.` alone (no protected match): punctuation, skipped.
3. **Generic token**: maximal run of word characters plus internal
   apostrophes (`don't → dont`) and digit-joining dots (`1.2.3`).

Hyphens separate (`state-of-the-art` → 4 tokens) except inside protected
surfaces (`objective-c`). Colons separate (`std::vector`). Underscores
join (`std_vector`).

### 1.3 Canonicalization

lowercase → strip apostrophes → protected map lookup. Junk (empty,
`_`/`.`-only) dropped. Canonical table:

| Surface | Canonical | | Surface | Canonical |
|---|---|---|---|---|
| `c++`, `cpp`, `cplusplus` | `cpp` | | `node.js`, `nodejs` | `nodejs` |
| `c#`, `csharp`, `c-sharp` | `csharp` | | `asp.net`, `aspnet` | `aspnet` |
| `f#`, `fsharp` | `fsharp` | | `vb.net`, `vbnet` | `vbnet` |
| `.net`, `dotnet` | `dotnet` | | `objective-c`, `objectivec` | `objectivec` |
| `golang` | `golang` | | `sqlite`, `redis` | `sqlite`, `redis` |

Querying `c++` or `cpp` hits the same postings — exactly the spec's §8
example vocabulary.

### 1.4 Stop words

~70 English function words. **Index time: kept** (positions must stay
intact for phrases; IDF de-values them anyway). Removal is a config flag;
when enabled, **ordinals are not renumbered** — positions always refer to
the pre-filter sequence. At ranking time the engine drops stop words from
scoring terms unless they are the entire query.

### 1.5 Fields and positions

`Token{term, position, field}`; positions restart at 0 per field;
headings are tokenized per-heading. The index composes the **virtual
position stream** (see §2.1).

## 2. Inverted index (index/InvertedIndex)

### 2.1 Virtual position stream

```
title tokens:      positions [0, title_len)
gap:               1 position (only if title non-empty)
heading h:         starts at headingStart + Σ previous lens + h
gap after each:    1 position
body:              starts at headingEnd (gap already included)
```

`DocMeta::{headingStartVirtual, headingEndVirtual, bodyStartVirtual}`
derive all boundaries from four stored lengths in O(1); a pure-body
document gets positions == ordinals. **Invariant:** no two adjacent
positions ever cross a field or heading boundary → phrase matches are
field-correct.

### 2.2 Structures

```
unordered_map<string term, PostingList>
PostingList = vector<Posting>          // sorted by doc_id
Posting    = {doc_id, tf_title, tf_heading, tf_body, vector<uint32_t> positions}
unordered_map<DocumentId, DocMeta>
DocMeta    = {lens, heading_count, doc_terms}   // doc_terms enables removal
```

Statistics maintained incrementally: document count, term count, total
postings, token sum (→ average document length).

### 2.3 Operations and complexity

| Operation | Complexity | Notes |
|---|---|---|
| term lookup | O(1) average | hash map |
| indexDocument | O(tokens + log df) | local map build + sorted insert per term |
| removeDocument | O(distinct terms × log df) | via doc_terms |
| posting scan | O(df) | never O(N documents) |
| phrase match | O(df ∩) | position-set probes per candidate |
| suggestions | O(V) per keystroke | prefix scan, top-df; fine at this scale |

Update semantics: re-indexing an existing id removes its old postings
first — no duplicates, ever (tested).

## 3. Ranking (ranking/)

Both rankers implement `Ranker::scoreTerm(term, list, posting, meta, stats)`
and are summed over the query's positive scoring terms (stop words dropped
unless the whole query is stop words).

### 3.1 TF-IDF (hand-written)

```
tf(t,d)   = weighted_tf / weighted_len
weighted_tf  = 3·tf_title + 2·tf_heading + 1·tf_body     (configurable)
weighted_len = 3·len_title + 2·len_heading + 1·len_body  (same weights)
idf(t)    = ln((N + 1) / (df + 1)) + 1                   (smoothed, > 0)
score     = Σ_t tf·idf
```

Natural log; empty document (weighted_len = 0) scores 0, no division by
zero anywhere. Hand-computed values are pinned in `ranking_tests.cpp`
(e.g. `0.6438410362258904` for the fixture corpus).

### 3.2 BM25 (hand-written)

```
idf(t)    = ln(1 + (N − df + 0.5) / (df + 0.5))          (Lucene-style, > 0)
score(t,D)= idf · f·(k1+1) / (f + k1·(1 − b + b·|D|/avgdl))
f = weighted_tf, |D| = weighted_len, avgdl over the collection
k1 = 1.2, b = 0.75 (both configurable)
```

Guards: zero tf → 0; degenerate avgdl → treated as |D|; the +1 inside the
log keeps IDF positive when df = N (pinned by test).

**Why both:** the spec requires both, and they make a genuinely useful
pair — BM25's saturation and length normalization fix TF-IDF's linear
repetition reward. The API/CLI select between them per query; tests pin
saturation (`2·tf < 2·score`) and length normalization (`b = 0` removes
the penalty).

### 3.3 Field boosting

Weights apply at ranking time only (never at index time — the index stores
raw per-field tfs). Equal-weighted-length fixtures prove the boost is the
*field*, not the length (title match scores 3× the body match under
TF-IDF; see `field_boost_makes_title_matches_outrank_body_matches`).

## 4. Query language (query/)

Grammar and semantics are in `docs/architecture.md` §5. Implementation:

- **Lexer**: words, quoted phrases, parens; operators recognized
  case-insensitively as standalone words; unbalanced quotes reported.
- **Recursive-descent parser**: OR < AND(implicit) < NOT precedence;
  `A NOT B` becomes `AND(A, NOT(B))`; leading NOT / dangling operators /
  empty queries are structured errors.
- **Evaluator**: Term → postings (multi-term surface forms OR-composed);
  Phrase → candidate intersection then position verification
  (`∃p: ∀i: term_i @ p+i`, in the same field thanks to stream gaps);
  And → two-pointer intersection with NOT-children subtracted; Or →
  two-pointer union. All set operations are linear merges over ascending
  vectors — no nested loops.

Missing terms simply contribute empty lists (AND with a missing term →
empty result, not an error).

## 5. Persistence (storage/StorageManager)

### 5.1 SQLite schema

```sql
documents(id INTEGER PRIMARY KEY AUTOINCREMENT, url TEXT UNIQUE NOT NULL,
          canonical_url, title, description, content, headings, links,
          http_status, crawled_at)
meta(key TEXT PRIMARY KEY, value INTEGER)
```

Upsert-by-URL keeps ids stable across re-crawls and restarts (tested).

### 5.2 Binary index format (little endian)

```
"MSIDX" | u8 major=1 | u8 minor=0
u32 termCount
  per term (sorted): u16 len, bytes, u32 df
    per posting: varint ΔdocId | varint tf_t | varint tf_h | varint tf_b
                 | varint posCount | varint pos₀, varint Δpos…
u32 docCount
  per doc (ascending id): varint id | varint lens(×4) | u32 termCount
                           | per term u16 len + bytes
```

Deterministic output (equal index ⇒ equal bytes — tested). Load-time
validation: magic/version, ascending ids/positions, df consistency,
postings referencing known documents, zero trailing bytes → any violation
is `Corrupted` and triggers an honest rebuild. Saves are atomic
(tmp + rename).

## 6. Crawler (crawler/)

- **Politeness**: dispatcher holds `host → next-allowed time`
  (configurable delay, raised to robots Crawl-delay when larger);
  workers run concurrently but a host's requests serialize; robots are
  fetched by the first task and cached before any second dispatch.
- **robots.txt subset** (RFC 9309): User-agent groups (product-token
  match beats `*`), Allow/Disallow path prefixes, longest prefix wins,
  ties → Allow, empty Disallow → allow all, Crawl-delay honored;
  unreachable robots → allow all.
- **Frontier**: BFS by depth; dedup via normalized URLs (fragments
  dropped, host lowercased, default ports removed); login/admin-like URL
  blacklist; allowed-domain (host or suffix) filter; redirect-target
  dedup; non-HTML content skipped.
- **Downloader**: one libcurl handle per worker; FOLLOWLOCATION (≤5),
  connect + total timeouts, 2 MB cap, automatic gzip/deflate decoding,
  honest error strings.

## 7. Snippets (engine/SearchEngine)

The stored content is split into whitespace words; each word is
canonicalized through the same tokenizer as the query; the first word
whose stem is a scoring term anchors a ±12/+18-word window, capped at 280
characters with ellipses. Front-end highlighting wraps words that prefix-
match query terms.

## 8. Benchmarks (benchmark/Benchmark)

Three measurements, all live (see `docs/benchmarks.md` for real numbers):

1. **Engine end-to-end**: the user-facing path (parse → retrieve → score →
   snippet → SQLite reads).
2. **Retrieval core at scale**: the real corpus replicated ×N in memory —
   the real `QueryProcessor` + `BM25Ranker` vs a brute-force double scan
   (df pass + scoring pass) over pre-analyzed documents. Replication
   affects both sides identically and is worst-case for the index (common
   terms land in every document → maximal posting lists).
3. **Full reindex** of the real corpus.

Nothing is simulated; the command re-measures on whatever machine runs it.
