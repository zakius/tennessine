// Copyright 2026 The oganesson Authors
// You can use, redistribute, and/or modify this source code under
// the terms of the GPL-3.0 license that can be found in the LICENSE file.

#include "components/bookmarks/browser/bookmark_utils.h"

#include <string>
#include <utility>

#include "base/i18n/case_conversion.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "ui/base/models/tree_node_iterator.h"
#include "url/gurl.h"
#include "base/strings/escape.h"

namespace bookmarks {

namespace {

std::u16string NormalizeKeyword(const std::u16string& keyword) {
  std::u16string normalized;
  base::RemoveChars(base::i18n::ToLower(keyword), base::kWhitespaceUTF16,
                    &normalized);
  return normalized;
}

}  // namespace

std::u16string GetBookmarkKeyword(const BookmarkNode* node) {
  std::string keyword;
  return node->GetMetaInfo(kKeywordMetaInfoKey, &keyword)
             ? base::UTF8ToUTF16(keyword)
             : std::u16string();
}

void SetBookmarkKeyword(BookmarkModel* model,
                        const BookmarkNode* node,
                        const std::u16string& keyword) {
  const std::u16string normalized = NormalizeKeyword(keyword);
  // Without a keyword before or after, there's nothing to change, and the
  // URL's other bookmarks keep theirs.
  if (normalized.empty() && GetBookmarkKeyword(node).empty()) {
    return;
  }
  // Firefox keeps one keyword per URL (and Firefox Sync gives it to every
  // bookmark of the URL), so the keyword moves off other URLs and applies to
  // every bookmark of this one, also when it is unchanged but the bookmark's
  // URL changed.
  ui::TreeNodeIterator<const BookmarkNode> iterator(model->root_node());
  while (iterator.has_next()) {
    const BookmarkNode* other = iterator.Next();
    const bool same_url =
        other == node || (node->is_url() && other->is_url() &&
                          other->url() == node->url());
    if (same_url && !normalized.empty()) {
      model->SetNodeMetaInfo(other, kKeywordMetaInfoKey,
                             base::UTF16ToUTF8(normalized));
    } else if (same_url || (!normalized.empty() &&
                            GetBookmarkKeyword(other) == normalized)) {
      model->DeleteNodeMetaInfo(other, kKeywordMetaInfoKey);
    }
  }
}

std::pair<const BookmarkNode*, GURL> ResolveBookmarkKeyword(
    const BookmarkModel* model,
    const std::u16string& text) {
  std::u16string_view trimmed =
      base::TrimWhitespace(std::u16string_view(text), base::TRIM_ALL);
  const size_t space = trimmed.find_first_of(base::kWhitespaceUTF16);
  const std::u16string keyword =
      base::i18n::ToLower(std::u16string(trimmed.substr(0, space)));
  const std::string terms =
      space == std::u16string_view::npos
          ? std::string()
          : base::UTF16ToUTF8(base::TrimWhitespace(trimmed.substr(space),
                                                   base::TRIM_LEADING));
  if (keyword.empty() || !model->loaded()) {
    return {nullptr, GURL()};
  }
  // ponytail: scans every bookmark on each keystroke; keep an index of
  // keywords if large bookmark trees make typing slow.
  ui::TreeNodeIterator<const BookmarkNode> iterator(model->root_node());
  while (iterator.has_next()) {
    const BookmarkNode* node = iterator.Next();
    if (!node->is_url() || GetBookmarkKeyword(node) != keyword) {
      continue;
    }
    std::string spec = node->url().spec();
    if (!terms.empty() && spec.find("%s") == std::string::npos &&
        spec.find("%S") == std::string::npos) {
      return {nullptr, GURL()};
    }
    base::ReplaceSubstringsAfterOffset(
        &spec, 0, "%s", base::EscapeQueryParamValue(terms, /*use_plus=*/false));
    base::ReplaceSubstringsAfterOffset(&spec, 0, "%S", terms);
    GURL url(spec);
    if (!url.is_valid()) {
      return {nullptr, GURL()};
    }
    return {node, url};
  }
  return {nullptr, GURL()};
}

}  // namespace bookmarks
