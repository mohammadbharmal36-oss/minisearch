#include "parser/HTMLParser.h"

#include "crawler/Url.h"

#include <gumbo.h>

#include <cctype>

namespace minisearch {

namespace {

bool isSkippedTag(GumboTag tag) {
    switch (tag) {
        case GUMBO_TAG_SCRIPT:
        case GUMBO_TAG_STYLE:
        case GUMBO_TAG_NOSCRIPT:
        case GUMBO_TAG_TEMPLATE:
        case GUMBO_TAG_NAV:
        case GUMBO_TAG_ASIDE:
        case GUMBO_TAG_FOOTER:
        case GUMBO_TAG_SVG:
        case GUMBO_TAG_IFRAME:
        case GUMBO_TAG_FORM:
        case GUMBO_TAG_CANVAS:
        case GUMBO_TAG_SELECT:
        case GUMBO_TAG_OBJECT:
        case GUMBO_TAG_EMBED:
            return true;
        default:
            return false;
    }
}

bool isBlockTag(GumboTag tag) {
    switch (tag) {
        case GUMBO_TAG_P:
        case GUMBO_TAG_DIV:
        case GUMBO_TAG_UL:
        case GUMBO_TAG_OL:
        case GUMBO_TAG_LI:
        case GUMBO_TAG_TABLE:
        case GUMBO_TAG_TR:
        case GUMBO_TAG_BR:
        case GUMBO_TAG_HR:
        case GUMBO_TAG_BLOCKQUOTE:
        case GUMBO_TAG_PRE:
        case GUMBO_TAG_DL:
        case GUMBO_TAG_DT:
        case GUMBO_TAG_DD:
        case GUMBO_TAG_H3:
        case GUMBO_TAG_H4:
        case GUMBO_TAG_H5:
        case GUMBO_TAG_H6:
        case GUMBO_TAG_SECTION:
        case GUMBO_TAG_ARTICLE:
        case GUMBO_TAG_MAIN:
        case GUMBO_TAG_FIGURE:
        case GUMBO_TAG_FIGCAPTION:
        case GUMBO_TAG_ADDRESS:
        case GUMBO_TAG_DETAILS:
        case GUMBO_TAG_SUMMARY:
            return true;
        default:
            return false;
    }
}

std::string collapseWhitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pendingSpace = true;
        } else {
            if (pendingSpace && !out.empty()) out.push_back(' ');
            pendingSpace = false;
            out.push_back(c);
        }
    }
    return out;
}

std::string trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string toLowerString(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

const char* attributeValue(const GumboVector& attributes, const char* name) {
    for (unsigned int i = 0; i < attributes.length; ++i) {
        const auto* attribute = static_cast<const GumboAttribute*>(attributes.data[i]);
        if (std::string(attribute->name) == name) return attribute->value;
    }
    return nullptr;
}

// Case-insensitive attribute lookup (HTML attributes are case-insensitive).
const char* attributeValueCi(const GumboVector& attributes, const char* name) {
    const std::string loweredName(name);
    for (unsigned int i = 0; i < attributes.length; ++i) {
        const auto* attribute = static_cast<const GumboAttribute*>(attributes.data[i]);
        if (toLowerString(attribute->name) == loweredName) return attribute->value;
    }
    return nullptr;
}

bool containsTokenCi(const std::string& value, const char* token) {
    const std::string lowered = toLowerString(value);
    std::size_t pos = 0;
    const std::string tokenStr(token);
    while (pos <= lowered.size()) {
        const std::size_t found = lowered.find(tokenStr, pos);
        if (found == std::string::npos) return false;
        const std::size_t after = found + tokenStr.size();
        const bool leftOk = found == 0 || lowered[found - 1] == ' '
                            || lowered[found - 1] == '\t';
        const bool rightOk = after == lowered.size() || lowered[after] == ' '
                             || lowered[after] == '\t';
        if (leftOk && rightOk) return true;
        pos = after;
    }
    return false;
}

// Concatenates all text below an element (used for title/headings).
std::string collectText(const GumboNode* node) {
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_CDATA) {
        return node->v.text.text != nullptr ? node->v.text.text : "";
    }
    if (node->type != GUMBO_NODE_ELEMENT) return "";
    std::string out;
    const GumboVector& children = node->v.element.children;
    for (unsigned int i = 0; i < children.length; ++i) {
        out += collectText(static_cast<const GumboNode*>(children.data[i]));
    }
    return out;
}

struct ExtractionState {
    std::string title;
    std::string description;
    std::string canonicalHref;
    std::vector<std::string> headings;
    std::vector<std::string> rawHrefs;
    std::vector<std::string> skippedHrefs;
    std::string currentLine;
    std::vector<std::string> contentLines;

    void appendText(const std::string& text) {
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!currentLine.empty() && currentLine.back() != ' ') currentLine.push_back(' ');
            } else {
                currentLine.push_back(c);
            }
        }
    }

    void flushLine() {
        const std::string line = trim(currentLine);
        if (!line.empty()) contentLines.push_back(line);
        currentLine.clear();
    }
};

void walk(const GumboNode* node, ExtractionState& state) {
    if (node->type == GUMBO_NODE_TEXT || node->type == GUMBO_NODE_CDATA) {
        state.appendText(node->v.text.text != nullptr ? node->v.text.text : "");
        return;
    }
    if (node->type == GUMBO_NODE_WHITESPACE) {
        state.appendText(" ");
        return;
    }
    if (node->type != GUMBO_NODE_ELEMENT) {
        return;  // comments and other oddities are dropped
    }

    const GumboElement& element = node->v.element;

    switch (element.tag) {
        case GUMBO_TAG_TITLE: {
            const std::string text = trim(collapseWhitespace(collectText(node)));
            if (!text.empty() && state.title.empty()) state.title = text;
            return;  // title text never lands in content
        }
        case GUMBO_TAG_META: {
            const char* name = attributeValueCi(element.attributes, "name");
            const char* property = attributeValueCi(element.attributes, "property");
            const char* content = attributeValueCi(element.attributes, "content");
            if (content != nullptr && state.description.empty()) {
                const bool isDescription = (name != nullptr && toLowerString(name) == "description")
                    || (property != nullptr && toLowerString(property) == "og:description");
                if (isDescription) {
                    state.description = trim(collapseWhitespace(content));
                }
            }
            return;
        }
        case GUMBO_TAG_LINK: {
            const char* rel = attributeValueCi(element.attributes, "rel");
            const char* href = attributeValueCi(element.attributes, "href");
            if (rel != nullptr && href != nullptr && state.canonicalHref.empty()
                && containsTokenCi(rel, "canonical")) {
                state.canonicalHref = href;
            }
            return;
        }
        case GUMBO_TAG_H1:
        case GUMBO_TAG_H2: {
            const std::string text = trim(collapseWhitespace(collectText(node)));
            if (!text.empty()) state.headings.push_back(text);
            state.flushLine();
            return;  // heading text lives in its own field
        }
        case GUMBO_TAG_A: {
            const char* href = attributeValue(element.attributes, "href");
            if (href != nullptr) state.rawHrefs.push_back(href);
            break;  // anchor text still flows into content
        }
        default:
            break;
    }

    if (isSkippedTag(element.tag)) return;

    const bool block = isBlockTag(element.tag);
    if (block) state.flushLine();

    const GumboVector& children = element.children;
    for (unsigned int i = 0; i < children.length; ++i) {
        walk(static_cast<const GumboNode*>(children.data[i]), state);
    }

    if (block) state.flushLine();
}

} // namespace

HTMLParser::Result HTMLParser::parse(const std::string& html, const std::string& baseUrl) {
    Result result;
    const auto normalizedBase = url::normalize(baseUrl);
    if (!normalizedBase.has_value()) {
        result.error = "invalid base URL: " + baseUrl;
        return result;
    }

    GumboOutput* output = gumbo_parse(html.empty() ? "" : html.c_str());
    if (output == nullptr) {
        result.error = "gumbo parser failed";
        return result;
    }

    ExtractionState state;
    walk(output->root, state);
    state.flushLine();  // trailing inline content before </body>
    gumbo_destroy_output(&kGumboDefaultOptions, output);

    Document& document = result.document;
    document.url = *normalizedBase;
    document.title = state.title;
    document.description = state.description;
    document.headings = state.headings;
    document.content.clear();
    for (std::size_t i = 0; i < state.contentLines.size(); ++i) {
        if (i > 0) document.content.push_back('\n');
        document.content += state.contentLines[i];
    }
    document.links.reserve(state.rawHrefs.size());
    for (const std::string& href : state.rawHrefs) {
        const std::string trimmed = trim(href);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        auto resolved = url::resolve(*normalizedBase, trimmed);
        if (resolved.has_value()) {
            document.links.push_back(std::move(*resolved));
        } else {
            result.skippedHrefs.push_back(trimmed);
        }
    }

    if (!state.canonicalHref.empty()) {
        auto canonical = url::resolve(*normalizedBase, state.canonicalHref);
        if (canonical.has_value()) document.canonical_url = *canonical;
    }

    result.ok = true;
    return result;
}

} // namespace minisearch
