#pragma once

// MiniSearch - URL utilities (crawler/parser support).
//
// Hand-written, minimal RFC 3986 handling for the crawler's needs:
//   - normalize: lowercase scheme/host, strip fragments, drop default
//     ports, keep path+query (used as the visited-set key)
//   - resolve: merge a relative reference against a base URL
//   - hostOf:  lowercase host for per-domain politeness
//
// This is deliberately small and well-tested; it is *not* a full URI
// library (no IRIs, no percent-decoding - normalization never rewrites
// encoded characters, so fetches use the original spelling).

#include <optional>
#include <string>

namespace minisearch::url {

// http/https only.
bool isHttpScheme(const std::string& url);

// Canonical form, or nullopt for non-http(s) / unparseable input.
// Never touches percent-encoded characters; fragments are dropped.
std::optional<std::string> normalize(const std::string& url);

// RFC 3986 section 5 reference resolution (path merging + dot-segment
// removal). Returns nullopt when the result is not http/https.
std::optional<std::string> resolve(const std::string& base, const std::string& ref);

// Lowercased host (no port, no userinfo); "" when unparseable.
std::string hostOf(const std::string& url);

// RFC 3986 section 5.2.4 - exposed for unit tests.
std::string removeDotSegments(const std::string& path);

} // namespace minisearch::url
