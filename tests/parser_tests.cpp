// Unit tests for URL utilities (crawler/Url) and the Gumbo-based HTML
// extraction (parser/HTMLParser).

#include "crawler/Url.h"
#include "parser/HTMLParser.h"
#include "test_framework.h"

#include <string>
#include <vector>

namespace {

using minisearch::HTMLParser;
using minisearch::url::hostOf;
using minisearch::url::normalize;
using minisearch::url::removeDotSegments;
using minisearch::url::resolve;

std::string join(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ' ';
        out += value;
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// URL normalization
// ---------------------------------------------------------------------------

MINI_TEST(normalize_lowercases_scheme_and_host) {
    MINI_EXPECT_EQ(*normalize("HTTP://Example.COM/Path"), "http://example.com/Path");
    MINI_EXPECT_EQ(*normalize("https://Mixed.Case.Host.Net/"), "https://mixed.case.host.net/");
}

MINI_TEST(normalize_strips_fragments_and_default_ports) {
    MINI_EXPECT_EQ(*normalize("http://example.com/page#section"), "http://example.com/page");
    MINI_EXPECT_EQ(*normalize("http://example.com:80/page"), "http://example.com/page");
    MINI_EXPECT_EQ(*normalize("https://example.com:443/x"), "https://example.com/x");
    MINI_EXPECT_EQ(*normalize("http://example.com:8080/page"), "http://example.com:8080/page");
}

MINI_TEST(normalize_keeps_query_and_empty_path_becomes_root) {
    MINI_EXPECT_EQ(*normalize("http://example.com/search?q=cpp&page=2"),
                   "http://example.com/search?q=cpp&page=2");
    MINI_EXPECT_EQ(*normalize("http://example.com"), "http://example.com/");
}

MINI_TEST(normalize_rejects_non_http_schemes) {
    MINI_EXPECT_FALSE(normalize("mailto:user@example.com").has_value());
    MINI_EXPECT_FALSE(normalize("ftp://example.com/file").has_value());
    MINI_EXPECT_FALSE(normalize("javascript:void(0)").has_value());
    MINI_EXPECT_FALSE(normalize("example.com/no-scheme").has_value());
    MINI_EXPECT_FALSE(normalize("").has_value());
}

MINI_TEST(normalize_handles_userinfo) {
    MINI_EXPECT_EQ(*normalize("http://user:pass@example.com/x"), "http://example.com/x");
}

MINI_TEST(hostof_extracts_lowercase_host) {
    MINI_EXPECT_EQ(hostOf("http://Example.com:8080/path?q=1"), "example.com");
    MINI_EXPECT_EQ(hostOf("https://sub.domain.org/"), "sub.domain.org");
    MINI_EXPECT_EQ(hostOf("not a url"), "");
}

// ---------------------------------------------------------------------------
// Dot-segment removal (RFC 3986 5.2.4)
// ---------------------------------------------------------------------------

MINI_TEST(remove_dot_segments_rfc_examples) {
    MINI_EXPECT_EQ(removeDotSegments("/a/b/c/./../../g"), "/a/g");
    MINI_EXPECT_EQ(removeDotSegments("mid/content=5/../6"), "mid/6");
    MINI_EXPECT_EQ(removeDotSegments("/./"), "/");
    MINI_EXPECT_EQ(removeDotSegments("/../"), "/");
    MINI_EXPECT_EQ(removeDotSegments("/a/b/.."), "/a/");
    MINI_EXPECT_EQ(removeDotSegments("/a/b/."), "/a/b/");
}

// ---------------------------------------------------------------------------
// Reference resolution (RFC 3986 5.4, base = http://a/b/c/d;p?q)
// ---------------------------------------------------------------------------

MINI_TEST(resolve_rfc_3986_normal_examples) {
    const std::string base = "http://a/b/c/d;p?q";
    MINI_EXPECT_EQ(*resolve(base, "g"), "http://a/b/c/g");
    MINI_EXPECT_EQ(*resolve(base, "./g"), "http://a/b/c/g");
    MINI_EXPECT_EQ(*resolve(base, "g/"), "http://a/b/c/g/");
    MINI_EXPECT_EQ(*resolve(base, "/g"), "http://a/g");
    MINI_EXPECT_EQ(*resolve(base, "//g"), "http://g/");
    MINI_EXPECT_EQ(*resolve(base, "?y"), "http://a/b/c/d;p?y");
    MINI_EXPECT_EQ(*resolve(base, "g?y"), "http://a/b/c/g?y");
    MINI_EXPECT_EQ(*resolve(base, "#s"), "http://a/b/c/d;p?q");
    MINI_EXPECT_EQ(*resolve(base, ""), "http://a/b/c/d;p?q");
    MINI_EXPECT_EQ(*resolve(base, "g#s"), "http://a/b/c/g");
    MINI_EXPECT_EQ(*resolve(base, "g?y#s"), "http://a/b/c/g?y");
    MINI_EXPECT_EQ(*resolve(base, ";x"), "http://a/b/c/;x");
    MINI_EXPECT_EQ(*resolve(base, "g;x"), "http://a/b/c/g;x");
    MINI_EXPECT_EQ(*resolve(base, "g;x?y#s"), "http://a/b/c/g;x?y");
}

MINI_TEST(resolve_rfc_3986_abnormal_examples) {
    const std::string base = "http://a/b/c/d;p?q";
    MINI_EXPECT_EQ(*resolve(base, "../../../g"), "http://a/g");
    MINI_EXPECT_EQ(*resolve(base, "../../../../g"), "http://a/g");
    MINI_EXPECT_EQ(*resolve(base, "/./g"), "http://a/g");
    MINI_EXPECT_EQ(*resolve(base, "/../g"), "http://a/g");
    MINI_EXPECT_EQ(*resolve(base, "g/../h"), "http://a/b/c/h");
}

MINI_TEST(resolve_absolute_references) {
    MINI_EXPECT_EQ(*resolve("http://a/b/", "https://other.org/x/y"),
                   "https://other.org/x/y");
    // Non-http schemes are rejected (nullopt) so the crawler skips them.
    MINI_EXPECT_FALSE(resolve("http://a/b/", "mailto:someone@example.org").has_value());
    MINI_EXPECT_FALSE(resolve("http://a/b/", "javascript:void(0)").has_value());
}

MINI_TEST(resolve_case_insensitive_scheme) {
    MINI_EXPECT_EQ(*resolve("HTTP://a/b/", "HTTPS://other.org/x"), "https://other.org/x");
}

// ---------------------------------------------------------------------------
// HTML parser
// ---------------------------------------------------------------------------

MINI_TEST(parser_extracts_full_page) {
    const std::string html =
        "<!DOCTYPE html>"
        "<html><head>"
        "<title>C++ Programming Guide</title>"
        "<meta name=\"description\" content=\"Learn modern C++ from first principles\">"
        "<link rel=\"canonical\" href=\"https://example.org/cpp-guide\">"
        "</head><body>"
        "<nav><a href=\"/ads\">Sponsored</a></nav>"
        "<script>var ignored = 'script text';</script>"
        "<style>.body { color: red; }</style>"
        "<h1>C++ Programming Guide</h1>"
        "<p>C++ is a general purpose programming language created by Bjarne Stroustrup.</p>"
        "<h2>Memory Management</h2>"
        "<p>Smart pointers help with <b>memory</b> safety in modern C++.</p>"
        "<a href=\"compilation.html\">Compilation notes</a>"
        "<a href=\"https://other.org/external\">External link</a>"
        "<a href=\"mailto:hi@example.org\">Mail me</a>"
        "<a href=\"#top\">Back to top</a>"
        "<!-- this comment must vanish -->"
        "</body></html>";

    const auto result = HTMLParser::parse(html, "http://example.org/docs/cpp.html");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.document.title, "C++ Programming Guide");
    MINI_EXPECT_EQ(result.document.description, "Learn modern C++ from first principles");
    MINI_EXPECT_EQ(result.document.canonical_url, "https://example.org/cpp-guide");
    MINI_EXPECT_EQ(join(result.document.headings),
                   "C++ Programming Guide Memory Management");
    MINI_EXPECT_EQ(result.document.links.size(), std::size_t{2});
    MINI_EXPECT_EQ(result.document.links[0], "http://example.org/docs/compilation.html");
    MINI_EXPECT_EQ(result.document.links[1], "https://other.org/external");
    MINI_EXPECT_EQ(result.skippedHrefs.size(), std::size_t{1});  // mailto:
    // "#top" is dropped silently: a pure fragment link cannot leave the page.

    const std::string& content = result.document.content;
    MINI_EXPECT_TRUE(content.find("general purpose programming language") != std::string::npos);
    MINI_EXPECT_TRUE(content.find("memory safety") != std::string::npos);  // <b> inline
    MINI_EXPECT_TRUE(content.find("script text") == std::string::npos);    // scripts removed
    MINI_EXPECT_TRUE(content.find("color: red") == std::string::npos);     // styles removed
    MINI_EXPECT_TRUE(content.find("Sponsored") == std::string::npos);      // nav removed
    MINI_EXPECT_TRUE(content.find("comment must vanish") == std::string::npos);
    MINI_EXPECT_TRUE(content.find("C++ Programming Guide") == std::string::npos);  // title/h1 out
    MINI_EXPECT_TRUE(content.find("Compilation notes") != std::string::npos);     // anchor text
}

MINI_TEST(parser_missing_title) {
    const auto result = HTMLParser::parse("<html><body><p>body only</p></body></html>",
                                          "http://example.org/x");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_TRUE(result.document.title.empty());
    MINI_EXPECT_EQ(result.document.content, "body only");
}

MINI_TEST(parser_empty_html) {
    const auto result = HTMLParser::parse("", "http://example.org/empty");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_TRUE(result.document.title.empty());
    MINI_EXPECT_TRUE(result.document.content.empty());
    MINI_EXPECT_TRUE(result.document.headings.empty());
}

MINI_TEST(parser_malformed_html_is_tolerated) {
    const auto result = HTMLParser::parse("<p>unclosed <b>bold<i>both",
                                          "http://example.org/malformed");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.document.content, "unclosed boldboth");
}

MINI_TEST(parser_nested_elements_collect_text_once) {
    const auto result = HTMLParser::parse(
        "<div><p>outer <span>inner <em>deep</em></span> tail</p></div>",
        "http://example.org/nested");
    MINI_EXPECT_TRUE(result.ok);
    MINI_EXPECT_EQ(result.document.content, "outer inner deep tail");
}

MINI_TEST(parser_multiple_paragraphs_become_lines) {
    const auto result = HTMLParser::parse("<p>first paragraph</p><p>second paragraph</p>",
                                          "http://example.org/two");
    MINI_EXPECT_EQ(result.document.content, "first paragraph\nsecond paragraph");
}

MINI_TEST(parser_decodes_entities) {
    const auto result = HTMLParser::parse(
        "<p>AT&amp;T uses &lt;tags&gt; and caf&eacute;</p>", "http://example.org/ent");
    // Gumbo decodes named entities; the UTF-8 e-acute survives byte-wise.
    MINI_EXPECT_TRUE(result.document.content.find("AT&T") != std::string::npos);
    MINI_EXPECT_TRUE(result.document.content.find("<tags>") != std::string::npos);
}

MINI_TEST(parser_collapses_whitespace) {
    const auto result = HTMLParser::parse(
        "<p>lots      of\n\nwhitespace\t\tand   spaces</p>", "http://example.org/ws");
    MINI_EXPECT_EQ(result.document.content, "lots of whitespace and spaces");
}

MINI_TEST(parser_relative_link_resolution_variants) {
    const std::string html =
        "<a href='/root.html'>1</a>"
        "<a href='sub/page.html'>2</a>"
        "<a href='../up.html'>3</a>"
        "<a href='//other.net/proto.html'>4</a>"
        "<a href='?query=1'>5</a>";
    const auto result = HTMLParser::parse(html, "http://example.org/dir/page.html");
    MINI_EXPECT_EQ(result.document.links.size(), std::size_t{5});
    MINI_EXPECT_EQ(result.document.links[0], "http://example.org/root.html");
    MINI_EXPECT_EQ(result.document.links[1], "http://example.org/dir/sub/page.html");
    MINI_EXPECT_EQ(result.document.links[2], "http://example.org/up.html");
    MINI_EXPECT_EQ(result.document.links[3], "http://other.net/proto.html");
    MINI_EXPECT_EQ(result.document.links[4], "http://example.org/dir/page.html?query=1");
}

MINI_TEST(parser_og_description_fallback) {
    const auto result = HTMLParser::parse(
        "<meta property=\"og:description\" content=\"Open Graph description\">",
        "http://example.org/og");
    MINI_EXPECT_EQ(result.document.description, "Open Graph description");
}

MINI_TEST(parser_headings_keep_document_order) {
    const std::string html =
        "<h1>First H1</h1><h2>Alpha</h2><p>text</p><h2>Beta</h2><h1>Second H1</h1>";
    const auto result = HTMLParser::parse(html, "http://example.org/h");
    MINI_EXPECT_EQ(join(result.document.headings), "First H1 Alpha Beta Second H1");
}

MINI_TEST(parser_invalid_base_url_is_reported) {
    const auto result = HTMLParser::parse("<p>x</p>", "not a url");
    MINI_EXPECT_FALSE(result.ok);
    MINI_EXPECT_FALSE(result.error.empty());
}
