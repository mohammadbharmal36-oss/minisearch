#pragma once

// MiniSearch - HTML parsing module (spec module 2).
//
// Converts raw HTML into a Document using Gumbo (a conformant HTML5
// parsing library - we do NOT regex-strip tags). Extraction rules:
//   title            <title> text (first non-empty)
//   description      <meta name="description"> or og:description
//   canonical_url    <link rel="canonical">
//   headings         h1 and h2 text, in document order
//   content          visible text with script/style/noscript/template/
//                    nav/aside/footer/svg/iframe/form/canvas removed and
//                    whitespace collapsed; block elements become newlines
//   links            <a href> resolved against the base URL, http(s) only
//
// The title and h1/h2 texts are intentionally excluded from `content`
// (they live in their own fields and get their own index weights).

#include "common/Document.h"

#include <string>
#include <vector>

namespace minisearch {

class HTMLParser {
public:
    struct Result {
        bool ok = false;
        std::string error;
        Document document;                   // url, title, description,
                                             // canonical_url, headings,
                                             // content, links
        std::vector<std::string> skippedHrefs;  // mailto:/javascript:/... (debug)
    };

    // baseUrl: the URL the HTML was downloaded from (after redirects);
    // relative links are resolved against it.
    static Result parse(const std::string& html, const std::string& baseUrl);
};

} // namespace minisearch
