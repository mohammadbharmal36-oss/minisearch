#!/usr/bin/env python3
"""Generates the MiniSearch seed corpus (tools/corpus/).

A small, honest corpus of self-written documentation pages about the
topics this search engine covers (search engines, ranking, crawling,
programming languages). Pages interlink so the crawler's BFS has real
work to do, and robots.txt disallows tools/corpus/private/ so robots
compliance is exercised end-to-end. Re-run with:

    python3 tools/generate_corpus.py
"""

import html
import os
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent / "tools" / "corpus"


def page(title, description, h1, sections, links, meta_robots=None):
    """Renders one corpus page. sections: list of (h2, [paragraphs])."""
    parts = []
    parts.append("<!DOCTYPE html>")
    parts.append('<html lang="en">')
    parts.append("<head>")
    parts.append(f"<title>{html.escape(title)}</title>")
    parts.append(f'<meta name="description" content="{html.escape(description)}">')
    if meta_robots:
        parts.append(f'<meta name="robots" content="{meta_robots}">')
    parts.append('<link rel="canonical" href="">')  # left empty on purpose
    parts.append('<link rel="stylesheet" href="">')
    parts.append("</head>")
    parts.append("<body>")
    parts.append(f"<h1>{html.escape(h1)}</h1>")
    for heading, paragraphs in sections:
        parts.append(f"<h2>{html.escape(heading)}</h2>")
        for paragraph in paragraphs:
            parts.append(f"<p>{paragraph}</p>")
    parts.append("<h2>See also</h2>")
    parts.append("<ul>")
    for url, label in links:
        parts.append(f'<li><a href="{url}">{html.escape(label)}</a></li>')
    parts.append("</ul>")
    parts.append("</body>")
    parts.append("</html>")
    return "\n".join(parts) + "\n"


# Each entry: filename -> (title, description, h1, sections, links)
PAGES = {
    "index.html": (
        "MiniSearch Corpus - Programming and Search Documentation",
        "A documentation corpus about search engines, information retrieval and programming.",
        "Programming and Search Documentation",
        [
            ("Overview", [
                "This small website is a corpus of self-written documentation pages about "
                "search engines, information retrieval, and several programming languages. "
                "The MiniSearch crawler explores it over plain HTTP, parses the HTML, "
                "tokenizes the text and builds an inverted index.",
                "Every page is plain semantic HTML with a title, a meta description, "
                "headings, paragraphs and hyperlinks to related pages. There is nothing "
                "hidden behind JavaScript: what the crawler downloads is what the "
                "parser sees.",
            ]),
            ("How to read this corpus", [
                "Follow the See also links to travel the graph the way the crawler does. "
                "The pages about the inverted index, TF-IDF and BM25 describe the "
                "algorithms this engine implements in C++. The pages about crawling and "
                "robots.txt describe the polite fetching behavior you can observe in "
                "the crawler console.",
                "One page is deliberately hidden: the robots.txt file disallows the "
                "private directory, and the crawler must respect that.",
            ]),
        ],
        [
            ("search-engines.html", "How search engines work"),
            ("inverted-index.html", "The inverted index"),
            ("c-plus-plus.html", "C++ programming language"),
            ("python.html", "Python programming language"),
            ("glossary.html", "Glossary of search terms"),
        ],
    ),
    "search-engines.html": (
        "How Search Engines Work",
        "The crawl, parse, index and rank pipeline behind every web search engine.",
        "How Search Engines Work",
        [
            ("The pipeline", [
                "A web search engine is a pipeline. A crawler discovers and downloads "
                "pages. A parser turns raw HTML into clean text and links. A text "
                "processor tokenizes that text into terms. An indexer builds the data "
                "structures that map terms to documents. A query processor evaluates "
                "user queries against those structures, and a ranking function scores "
                "the matching documents so the best answers appear first.",
                "MiniSearch implements this entire pipeline from scratch in C++: the "
                "inverted index, the TF-IDF and BM25 ranking functions, the Boolean "
                "and phrase query processing, and the crawler are all hand-written.",
            ]),
            ("Why ranking matters", [
                "Retrieval without ranking is a sorted list of everything that matches. "
                "Ranking is what turns a match set into an answer. Classical ranking "
                "functions combine how often a term appears in a document (term "
                "frequency) with how rare the term is across the whole collection "
                "(inverse document frequency).",
                "Modern engines add hundreds of signals, but TF-IDF and BM25 remain "
                "the readable, teachable core of text ranking, and they remain "
                "competitive baselines for keyword search.",
            ]),
        ],
        [
            ("inverted-index.html", "The inverted index"),
            ("tf-idf.html", "TF-IDF ranking"),
            ("bm25.html", "BM25 ranking algorithm"),
            ("web-crawlers.html", "Web crawlers"),
            ("/", "Corpus home"),
        ],
    ),
    "inverted-index.html": (
        "The Inverted Index",
        "How a term-to-document mapping enables sub-millisecond keyword retrieval.",
        "The Inverted Index",
        [
            ("From documents to terms", [
                "An inverted index maps every term to the list of documents that "
                "contain it, together with term frequencies and word positions. With "
                "three documents - one about C++ programming, one about Python "
                "programming and one about competitive programming in C++ - the term "
                "programming points at all three documents, while python points at "
                "only one.",
                "Because posting lists are sorted by document id, Boolean operations "
                "become cheap two-pointer merges: intersection for AND, union for OR, "
                "and subtraction for NOT.",
            ]),
            ("Positions enable phrases", [
                "Storing the position of every occurrence makes exact phrase search "
                "possible. To match the phrase competitive programming, the engine "
                "checks whether the two terms appear in adjacent positions in the "
                "correct order within the same field of a document.",
                "MiniSearch stores a virtual position stream per document that spans "
                "title, headings and body, with deliberate gaps at field boundaries "
                "so a phrase can never falsely match across two different fields.",
            ]),
            ("Complexity", [
                "Looking up one term costs one hash lookup plus a scan of its posting "
                "list: proportional to the number of documents containing the term, "
                "not to the total collection size. Brute-force scanning instead "
                "touches every document for every query; the benchmark command in "
                "MiniSearch measures the difference on real data.",
            ]),
        ],
        [
            ("tf-idf.html", "TF-IDF ranking"),
            ("bm25.html", "BM25 ranking algorithm"),
            ("boolean-queries.html", "Boolean queries"),
            ("phrase-search.html", "Phrase search"),
            ("search-engines.html", "How search engines work"),
        ],
    ),
    "tf-idf.html": (
        "TF-IDF Ranking Explained",
        "Term frequency times inverse document frequency, the classic relevance score.",
        "TF-IDF Ranking",
        [
            ("Term frequency", [
                "Term frequency measures how often a query term appears in a document, "
                "normalized by the document length so long documents do not win simply "
                "by being long. If a term occurs three times in a document of one "
                "hundred tokens, its normalized term frequency is low; three "
                "occurrences in a document of ten tokens is strong evidence the "
                "document is about that term.",
            ]),
            ("Inverse document frequency", [
                "Inverse document frequency measures how selective a term is. A term "
                "that appears in every document of the collection cannot distinguish "
                "anything, so its IDF approaches zero after smoothing. A term that "
                "appears in a single document is maximally selective.",
                "MiniSearch uses the smoothed IDF formula log((N + 1) / (df + 1)) + 1, "
                "which stays strictly positive and avoids degenerate cases when a "
                "term occurs in every document.",
            ]),
            ("Field weighting", [
                "Words in a title describe a page more precisely than words buried in "
                "a paragraph. MiniSearch therefore weights matches per field: title "
                "matches count triple, heading matches count double, and body matches "
                "count once. The weights are configurable, and they are applied at "
                "ranking time, never at index time.",
            ]),
        ],
        [
            ("bm25.html", "BM25 ranking algorithm"),
            ("inverted-index.html", "The inverted index"),
            ("ranking.html", "Ranking functions compared"),
            ("search-engines.html", "How search engines work"),
        ],
    ),
    "bm25.html": (
        "BM25 Ranking Algorithm",
        "The Okapi BM25 scoring function: saturation and length normalization.",
        "BM25 Ranking",
        [
            ("The formula", [
                "BM25 scores a document for a query by summing, over query terms, an "
                "IDF factor multiplied by a saturated term frequency factor. The "
                "saturation is controlled by k1: after a handful of occurrences, "
                "repeating the term adds less and less score, which matches the "
                "intuition that relevance is not linear in repetition.",
                "The parameter b controls document length normalization. At b equal "
                "to one, long documents are penalized fully relative to the average "
                "document length; at b equal to zero, length is ignored.",
            ]),
            ("Why BM25 usually beats TF-IDF", [
                "Raw TF-IDF keeps rewarding repetition linearly, so a page that "
                "mentions a word a hundred times outranks a page that mentions it "
                "once, even when the repetition is spam. BM25 saturates instead. "
                "It also normalizes document length more carefully than a simple "
                "division, which keeps medium-length focused documents competitive "
                "against both thin and bloated pages.",
                "BM25 has been the default ranking baseline of keyword search "
                "engines for decades, and it remains a strong, interpretable "
                "starting point before any learning-to-rank machinery.",
            ]),
        ],
        [
            ("tf-idf.html", "TF-IDF ranking"),
            ("ranking.html", "Ranking functions compared"),
            ("inverted-index.html", "The inverted index"),
            ("glossary.html", "Glossary of search terms"),
        ],
    ),
    "ranking.html": (
        "Ranking Functions Compared",
        "TF-IDF and BM25 side by side on the same queries.",
        "Ranking Functions Compared",
        [
            ("Same candidates, different orders", [
                "TF-IDF and BM25 receive exactly the same candidate documents from "
                "the query processor; they only differ in how they score. On simple "
                "queries their rankings often agree at the top and drift lower down. "
                "On queries with repeated terms or on collections with mixed "
                "document lengths, BM25 tends to prefer focused, medium-sized "
                "documents while TF-IDF favors repetition and short documents.",
            ]),
            ("Choosing in MiniSearch", [
                "The search interface and the API both accept an algorithm parameter "
                "with the values tfidf and bm25, so the same query can be run through "
                "both functions and the results compared directly. The analytics "
                "page reports the average search latency across whichever function "
                "was used.",
                "Both implementations are hand-written C++ over the same posting "
                "lists; no third-party ranking library is involved anywhere in the "
                "engine.",
            ]),
        ],
        [
            ("tf-idf.html", "TF-IDF ranking"),
            ("bm25.html", "BM25 ranking algorithm"),
            ("query-processing.html", "Query processing"),
        ],
    ),
    "query-processing.html": (
        "Query Processing in a Search Engine",
        "From a raw query string to a ranked result list.",
        "Query Processing",
        [
            ("Parsing", [
                "A query is parsed by a small recursive descent parser. Operators are "
                "AND, OR and NOT, quoted phrases match exact word order, and "
                "parentheses group expressions. Adjacent terms imply AND, so the "
                "query python programming behaves like python AND programming.",
                "The parser normalizes terms with the same tokenizer the indexer "
                "uses, including the technical term mapping that preserves "
                "expressions like C++ and Node.js as single searchable terms.",
            ]),
            ("Evaluation", [
                "Evaluation walks the parse tree and combines posting lists with "
                "two-pointer set operations over sorted document ids. A phrase node "
                "verifies adjacent positions using the stored word positions. NOT "
                "subtracts the operand's documents from the positive side instead of "
                "scoring it.",
            ]),
        ],
        [
            ("boolean-queries.html", "Boolean queries"),
            ("phrase-search.html", "Phrase search"),
            ("tokenization.html", "Tokenization and text processing"),
            ("inverted-index.html", "The inverted index"),
        ],
    ),
    "boolean-queries.html": (
        "Boolean Queries",
        "AND, OR and NOT over posting lists.",
        "Boolean Queries",
        [
            ("Semantics", [
                "The query cpp AND programming returns documents containing both "
                "terms. The query cpp OR python returns documents containing either "
                "term. The query programming NOT python returns documents about "
                "programming that do not mention python. Parentheses make "
                "combinations explicit, and operator words are recognized "
                "case-insensitively.",
            ]),
            ("Efficiency", [
                "Because posting lists are sorted by document id, intersection and "
                "union run in time linear in the combined list sizes with two "
                "cursors, never with nested loops or hash rehashing. Subtraction "
                "for NOT works the same way.",
            ]),
        ],
        [
            ("query-processing.html", "Query processing"),
            ("phrase-search.html", "Phrase search"),
            ("inverted-index.html", "The inverted index"),
        ],
    ),
    "phrase-search.html": (
        "Exact Phrase Search",
        "Matching words in order using stored positions.",
        "Phrase Search",
        [
            ("How it works", [
                "A quoted query such as \"competitive programming\" asks for "
                "documents where the two words appear next to each other, in that "
                "order. The engine takes the posting lists of both terms, "
                "intersects the document sets, and then checks the stored positions: "
                "the phrase matches a document if some position p holds the first "
                "term and position p plus one holds the second.",
                "Stop words are kept inside phrases, so \"state of the art\" matches "
                "the actual four-word sequence rather than three unrelated words.",
            ]),
            ("Field boundaries", [
                "MiniSearch inserts gaps between fields in its virtual position "
                "stream. A document titled Python Guide whose body starts with the "
                "word python does not match the phrase guide python, because the "
                "last title position and the first body position are deliberately "
                "not adjacent.",
            ]),
        ],
        [
            ("query-processing.html", "Query processing"),
            ("boolean-queries.html", "Boolean queries"),
            ("inverted-index.html", "The inverted index"),
        ],
    ),
    "tokenization.html": (
        "Tokenization and Text Processing",
        "Turning raw text into indexable terms without destroying technical words.",
        "Tokenization and Text Processing",
        [
            ("Tokenization", [
                "Tokenization splits text into terms: lowercase, split on "
                "punctuation, keep numbers and version-like tokens intact. "
                "Programming languages make this subtle. A naive tokenizer that "
                "strips punctuation turns C++ into a bare c and Node.js into node, "
                "which makes those terms unsearchable.",
                "MiniSearch therefore recognizes protected technical terms first, "
                "with longest-match and word-boundary checks, and maps them to "
                "canonical index terms: C++ and cplusplus both become cpp, C# "
                "becomes csharp, Node.js becomes nodejs.",
            ]),
            ("Stemming and stop words", [
                "A light rule-based stemmer unifies common inflections, so "
                "programming, programs and program converge to one index term. "
                "Stop words such as the and of are kept at indexing time because "
                "phrases need their positions; ranking simply gives them the tiny "
                "weight their ubiquity deserves.",
            ]),
        ],
        [
            ("query-processing.html", "Query processing"),
            ("inverted-index.html", "The inverted index"),
            ("html-parsing.html", "HTML parsing"),
        ],
    ),
    "html-parsing.html": (
        "HTML Parsing for Text Extraction",
        "From raw HTML to clean, indexable document text.",
        "HTML Parsing",
        [
            ("What to keep", [
                "A search engine does not index markup. The parser walks the HTML "
                "tree and extracts the title, meta description, canonical URL, "
                "heading text for h1 and h2, hyperlink targets, and the visible "
                "body text. Scripts, styles, templates, navigation, asides and "
                "footers are skipped because they are chrome, not content.",
            ]),
            ("Why a real parser", [
                "Stripping tags with regular expressions fails on nested quotes, "
                "comments containing tag-like text, and malformed markup. A "
                "conformant HTML5 parser recovers from errors the same way browsers "
                "do. MiniSearch uses Gumbo, Google's HTML5 parsing library, for "
                "exactly this reason; the extraction rules, however, are its own.",
            ]),
        ],
        [
            ("search-engines.html", "How search engines work"),
            ("tokenization.html", "Tokenization and text processing"),
            ("web-crawlers.html", "Web crawlers"),
        ],
    ),
    "web-crawlers.html": (
        "Web Crawlers and Politeness",
        "How a crawler discovers pages without becoming a nuisance.",
        "Web Crawlers",
        [
            ("The frontier", [
                "A crawler starts from seed URLs and follows hyperlinks breadth "
                "first. The frontier is the queue of URLs waiting to be fetched; a "
                "visited set prevents loops; a depth budget stops the crawl from "
                "wandering into the entire web; and a page budget caps the total "
                "work per job.",
            ]),
            ("Politeness", [
                "A polite crawler respects robots.txt, sends a descriptive "
                "User-Agent, and spaces requests to the same host. MiniSearch "
                "serializes requests per host with a configurable delay and only "
                "parallelizes across different hosts. It also refuses "
                "login-looking URLs, because authenticated content is neither "
                "public nor polite to scrape.",
            ]),
        ],
        [
            ("robots-txt.html", "The robots.txt protocol"),
            ("http-basics.html", "HTTP protocol basics"),
            ("html-parsing.html", "HTML parsing"),
            ("search-engines.html", "How search engines work"),
        ],
    ),
    "robots-txt.html": (
        "The robots.txt Protocol",
        "How websites tell crawlers what they may fetch.",
        "robots.txt",
        [
            ("The file", [
                "robots.txt is a plain text file at the root of a site. It groups "
                "Allow and Disallow path prefixes under User-agent names, and a "
                "crawler applies the group that matches its own product token, "
                "falling back to the wildcard group addressed to everyone. An "
                "empty Disallow means everything is allowed.",
                "This corpus ships a robots.txt that disallows the private "
                "directory; the MiniSearch crawler skips those pages, and the crawl "
                "log records them as skipped.",
            ]),
            ("Longest prefix wins", [
                "When both an Allow and a Disallow prefix match a path, the longer, "
                "more specific prefix wins. Ties go to Allow. A Crawl-delay line "
                "asks crawlers to wait between requests; MiniSearch honors it by "
                "raising its per-host delay to at least the requested value.",
            ]),
        ],
        [
            ("web-crawlers.html", "Web crawlers"),
            ("http-basics.html", "HTTP protocol basics"),
            ("urls.html", "URLs and links"),
        ],
    ),
    "http-basics.html": (
        "HTTP Protocol Basics",
        "Status codes, redirects and timeouts as a crawler sees them.",
        "HTTP Protocol Basics",
        [
            ("Requests and responses", [
                "The crawler speaks plain HTTP. It issues GET requests with a "
                "User-Agent header, honors redirects up to a small cap, records the "
                "final response code, and enforces connect and total timeouts so a "
                "silent server can never stall a crawl job indefinitely.",
            ]),
            ("Status codes", [
                "A 200 response with an HTML content type is indexed. Client "
                "errors such as 404 and 410 are recorded as failures and never "
                "retried within the same job. Server errors are also recorded; a "
                "future scheduler could re-try them later with backoff. Redirect "
                "chains are followed, and the final URL is what gets indexed.",
            ]),
        ],
        [
            ("web-crawlers.html", "Web crawlers"),
            ("urls.html", "URLs and links"),
            ("robots-txt.html", "The robots.txt protocol"),
        ],
    ),
    "urls.html": (
        "URLs and Links",
        "Normalization, relative references and deduplication.",
        "URLs and Links",
        [
            ("Normalization", [
                "Two spellings of the same address should be fetched once. "
                "Normalization lowercases the scheme and host, strips the fragment, "
                "drops default ports, and keeps the path and query intact. The "
                "normalized form is the key in the crawler's visited set.",
            ]),
            ("Relative references", [
                "HTML links may be absolute, root-relative, or relative to the "
                "current page. Resolving them follows the standard reference "
                "resolution algorithm, including dot-segment removal, so a link to "
                "../bm25.html from the ranking page lands on the right document.",
            ]),
        ],
        [
            ("http-basics.html", "HTTP protocol basics"),
            ("web-crawlers.html", "Web crawlers"),
            ("html-parsing.html", "HTML parsing"),
        ],
    ),
    "c-plus-plus.html": (
        "C++ Programming Language",
        "A systems programming language with zero-overhead abstractions.",
        "C++ Programming Language",
        [
            ("Overview", [
                "C++ is a general-purpose programming language created by Bjarne "
                "Stroustrup. It compiles to native code, gives the programmer "
                "direct control over memory and hardware, and pairs that control "
                "with zero-overhead abstractions such as templates, move semantics "
                "and the standard template library.",
                "MiniSearch itself is written in modern C++17: the inverted index, "
                "the ranking functions, the query parser and the crawler are all "
                "C++ code built with CMake.",
            ]),
            ("Memory and the STL", [
                "Smart pointers make ownership explicit; RAII ties resource "
                "lifetimes to scopes; and the standard template library provides "
                "the hash maps, vectors and algorithms the index is built on. A "
                "search engine is a memory-bound program, so cache-friendly "
                "contiguous posting lists matter more than clever pointer "
                "structures.",
            ]),
            ("Competitive programming", [
                "C++ is used in competitive programming because the standard "
                "template library ships battle-tested data structures and the "
                "language rewards precise control of complexity. The classic "
                "phrase competitive programming appears verbatim in this corpus "
                "for phrase search demonstrations.",
            ]),
        ],
        [
            ("stl.html", "The standard template library"),
            ("competitive-programming.html", "Competitive programming"),
            ("python.html", "Python programming language"),
            ("javascript.html", "JavaScript and Node.js"),
            ("/", "Corpus home"),
        ],
    ),
    "python.html": (
        "Python Programming Language",
        "A readable, batteries-included dynamic language.",
        "Python Programming Language",
        [
            ("Overview", [
                "Python is an interpreted programming language that emphasizes "
                "readable source code and a comprehensive standard library. It "
                "dominates data science, scripting and education, and it is a "
                "popular glue language for systems written in C and C++.",
                "Python is a programming language that appears frequently in "
                "search engine query logs, which makes it a good corpus citizen "
                "for testing multi-term queries.",
            ]),
            ("Language features", [
                "Significant whitespace, first-class functions, generators and a "
                "rich ecosystem of packages are the language's hallmarks. The "
                "classic description of its philosophy is that Python code reads "
                "like pseudocode that happens to execute.",
            ]),
        ],
        [
            ("c-plus-plus.html", "C++ programming language"),
            ("javascript.html", "JavaScript and Node.js"),
            ("sql-and-sqlite.html", "SQL and SQLite databases"),
            ("/", "Corpus home"),
        ],
    ),
    "javascript.html": (
        "JavaScript and Node.js",
        "The language of the web, on both ends of the wire.",
        "JavaScript and Node.js",
        [
            ("In the browser", [
                "JavaScript is the programming language of the web platform. This "
                "corpus is deliberately static HTML with no scripts, because a "
                "crawler that indexes what a server returns should not depend on "
                "executing client-side code. The MiniSearch web interface, "
                "however, is vanilla JavaScript fetching JSON from the search API.",
            ]),
            ("On the server with Node.js", [
                "Node.js brought the language to the server with an event loop "
                "and a vast package registry. Writing a search engine in C++ while "
                "its web front-end is a few kilobytes of JavaScript is a "
                "deliberate contrast: the heavy lifting stays native, the "
                "interface stays simple.",
            ]),
        ],
        [
            ("c-plus-plus.html", "C++ programming language"),
            ("python.html", "Python programming language"),
            ("http-basics.html", "HTTP protocol basics"),
        ],
    ),
    "stl.html": (
        "The C++ Standard Template Library",
        "Containers, iterators and algorithms for index building.",
        "The Standard Template Library",
        [
            ("Containers for search", [
                "The STL provides exactly the containers an inverted index needs: "
                "unordered_map for term lookup, vector for posting lists, and "
                "sort plus lower_bound for keeping lists ordered by document id. "
                "Reserving capacity and keeping postings contiguous keeps the "
                "index cache friendly.",
            ]),
            ("Algorithms", [
                "Standard algorithms such as sort, stable_sort, set operations and "
                "binary search implement most of the index's mechanics in a few "
                "lines each, which is why the hand-written core stays readable "
                "and auditable.",
            ]),
        ],
        [
            ("c-plus-plus.html", "C++ programming language"),
            ("inverted-index.html", "The inverted index"),
            ("sql-and-sqlite.html", "SQL and SQLite databases"),
        ],
    ),
    "competitive-programming.html": (
        "Competitive Programming",
        "Algorithms under time pressure.",
        "Competitive Programming",
        [
            ("The discipline", [
                "Competitive programming is solving algorithmic puzzles under "
                "strict time limits. Contestants reach for C++ because the "
                "standard template library provides hash maps, priority queues "
                "and graph primitives that compile to fast native code.",
            ]),
            ("Relevance to search engines", [
                "The habits transfer directly: choose the data structure that "
                "makes the common operation cheap, know the complexity of every "
                "merge you write, and measure instead of guessing. An inverted "
                "index is, in the end, one long algorithms problem about "
                "sorted lists.",
            ]),
        ],
        [
            ("c-plus-plus.html", "C++ programming language"),
            ("stl.html", "The standard template library"),
            ("bm25.html", "BM25 ranking algorithm"),
        ],
    ),
    "sql-and-sqlite.html": (
        "SQL and SQLite Databases",
        "Relational storage for document metadata.",
        "SQL and SQLite",
        [
            ("Why SQL here", [
                "A search engine needs durable, queryable metadata: which URL was "
                "crawled when, with what title and description. SQLite provides "
                "a complete SQL database in a single file with no server, which "
                "suits a self-contained engine perfectly.",
            ]),
            ("Schema of MiniSearch", [
                "MiniSearch stores one documents table with the URL as a unique "
                "key, plus text columns for the title, description, extracted "
                "content, headings and outgoing links. The inverted index itself "
                "lives in a compact serialized binary file that loads in "
                "milliseconds; SQLite is the source of truth the index is "
                "rebuilt from when needed.",
            ]),
        ],
        [
            ("storage-and-persistence.html", "Persistence and restarts"),
            ("stl.html", "The standard template library"),
            ("c-plus-plus.html", "C++ programming language"),
        ],
    ),
    "storage-and-persistence.html": (
        "Persistence and Restarts",
        "Making an index survive process shutdowns.",
        "Persistence",
        [
            ("What persists", [
                "MiniSearch persists two things: the SQLite database with every "
                "document's metadata and content, and a binary serialization of "
                "the inverted index with delta-encoded posting lists. Saving is "
                "atomic - a temporary file is fully written and then renamed - so "
                "a crash mid-save can never destroy a good index.",
            ]),
            ("Startup", [
                "On startup the engine loads the index file and reconciles it "
                "with the database: new documents are indexed incrementally, "
                "postings of deleted documents are dropped, and a corrupted index "
                "file is detected by its header and structural checks and rebuilt "
                "honestly rather than trusted.",
            ]),
        ],
        [
            ("sql-and-sqlite.html", "SQL and SQLite"),
            ("inverted-index.html", "The inverted index"),
            ("search-engines.html", "How search engines work"),
        ],
    ),
    "performance.html": (
        "Search Engine Performance",
        "Measuring before optimizing.",
        "Search Engine Performance",
        [
            ("Measure first", [
                "The benchmark command compares brute-force document scanning "
                "with inverted-index retrieval on the same corpus and queries, "
                "reporting per-query latency for both. Only after such "
                "measurements do optimizations earn their place: contiguous "
                "posting lists, reserved hash capacity, and serialization that "
                "loads in one pass.",
            ]),
            ("Latency budget", [
                "A keyword search over thousands of documents should complete in "
                "well under a millisecond: one hash lookup per term, a posting "
                "list scan bounded by document frequency, and a small heap for "
                "the top results. The expensive parts of a real search page - "
                "snippets and metadata reads - are exactly what the benchmark "
                "keeps visible instead of hiding.",
            ]),
        ],
        [
            ("inverted-index.html", "The inverted index"),
            ("bm25.html", "BM25 ranking algorithm"),
            ("storage-and-persistence.html", "Persistence and restarts"),
        ],
    ),
    "glossary.html": (
        "Glossary of Search Terms",
        "Short definitions of the vocabulary used by this corpus.",
        "Glossary",
        [
            ("Terms", [
                "Crawler: the program that downloads pages. Frontier: the queue of "
                "URLs awaiting fetch. Posting: one term's occurrence record in one "
                "document, with frequencies and positions. Posting list: the sorted "
                "list of postings for one term. Token: one normalized word. Term: "
                "the canonical index key a token maps to.",
                "TF-IDF: term frequency times inverse document frequency. BM25: the "
                "saturated, length-normalized ranking function. Inverted index: the "
                "term-to-documents mapping. Snippet: the short excerpt shown under "
                "a search result. robots.txt: the site-level crawl permission file.",
            ]),
        ],
        [
            ("/", "Corpus home"),
            ("search-engines.html", "How search engines work"),
            ("inverted-index.html", "The inverted index"),
        ],
    ),
    "private/secret.html": (
        "Internal Draft - Not For Crawling",
        "A page disallowed by robots.txt, used to verify crawler compliance.",
        "Internal Draft",
        [
            ("Why this page exists", [
                "This page sits inside the private directory, which the corpus "
                "robots.txt disallows for all crawlers. A compliant crawler must "
                "skip it even though other pages link here. The word zanzibar "
                "appears only on this page, so searching for zanzibar should "
                "return no results after a crawl - a direct test that robots "
                "rules were honored.",
            ]),
        ],
        [
            ("/", "Corpus home"),
        ],
    ),
}


def main():
    (ROOT / "private").mkdir(parents=True, exist_ok=True)

    # The hub page links to the private page so robots compliance is
    # actually exercised (the crawler must *want* to fetch it, then refuse).
    hub = PAGES["index.html"]
    sections = list(hub[3])
    sections.append(("A note on privacy", [
        "There is also a <a href='private/secret.html'>private page</a> that "
        "robots.txt forbids crawling. It contains the word zanzibar and nothing "
        "else of value. If you can search for zanzibar and find it, something is "
        "wrong with the crawler's manners.",
    ]))
    PAGES["index.html"] = (hub[0], hub[1], hub[2], sections, hub[4])

    for filename, spec in PAGES.items():
        target = ROOT / filename
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(page(*spec), encoding="utf-8")
        print(f"wrote {target.relative_to(ROOT.parent.parent)}")

    robots = ROOT / "robots.txt"
    robots.write_text(
        "# MiniSearch seed corpus robots.txt\n"
        "User-agent: *\n"
        "Disallow: /private/\n"
        "Crawl-delay: 0\n",
        encoding="utf-8",
    )
    print(f"wrote {robots.relative_to(ROOT.parent.parent)}")


if __name__ == "__main__":
    main()
