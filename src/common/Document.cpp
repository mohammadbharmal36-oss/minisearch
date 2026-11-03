#include "common/Document.h"

#include <ostream>
#include <sstream>

namespace minisearch {

namespace {

// Keep debug output readable: truncate long free-text fields.
std::string truncate(const std::string& text, std::size_t maxLength) {
    if (text.size() <= maxLength) return text;
    return text.substr(0, maxLength) + "...";
}

} // namespace

std::string Document::toDebugString() const {
    std::ostringstream out;
    out << "Document{id=" << id
        << ", url='" << truncate(url, 80) << "'"
        << ", title='" << truncate(title, 60) << "'"
        << ", description='" << truncate(description, 60) << "'"
        << ", headings=" << headings.size()
        << ", outgoing_links=" << outgoingLinkCount()
        << ", http_status=" << http_status
        << ", crawled_at=" << crawled_at
        << "}";
    return out.str();
}

bool operator==(const Document& lhs, const Document& rhs) {
    return lhs.id == rhs.id
        && lhs.url == rhs.url
        && lhs.canonical_url == rhs.canonical_url
        && lhs.title == rhs.title
        && lhs.description == rhs.description
        && lhs.content == rhs.content
        && lhs.headings == rhs.headings
        && lhs.links == rhs.links
        && lhs.http_status == rhs.http_status
        && lhs.crawled_at == rhs.crawled_at;
}

bool operator!=(const Document& lhs, const Document& rhs) {
    return !(lhs == rhs);
}

std::ostream& operator<<(std::ostream& stream, const Document& document) {
    return stream << document.toDebugString();
}

} // namespace minisearch
