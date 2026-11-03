#pragma once

// MiniSearch - benchmark harness (spec module 15).
//
// Compares, on the currently indexed corpus:
//   A. brute-force document scanning (no index: tokenize + df-count +
//      score every document for every query)
//   B. the hand-built inverted index (the real engine search path,
//      including snippets and metadata reads)
//
// Everything is measured with std::chrono on the live machine; numbers
// are printed (and optionally written as Markdown). Nothing is simulated.

#include <string>
#include <vector>

namespace minisearch {

// CLI entry: minisearch benchmark [--runs N] [--markdown PATH]
//           [--data-dir DIR] [--config FILE]
int runBenchmarkCli(const std::vector<std::string>& args);

} // namespace minisearch
