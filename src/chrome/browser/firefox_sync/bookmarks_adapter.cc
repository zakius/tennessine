// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/firefox_sync/bookmarks_adapter.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/location.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/bookmarks/common/bookmark_metrics.h"
#include "url/gurl.h"

namespace firefox_sync {

namespace {

using bookmarks::BookmarkModel;
using bookmarks::BookmarkNode;
using bookmarks::metrics::BookmarkEditSource;

constexpr char kGuidKey[] = "oganesson.firefox_sync.guid";
constexpr char kMenu[] = "menu________";

using Nodes = std::map<std::string, const BookmarkNode*, std::less<>>;

// A node's Firefox GUID: the one in its meta info, else (not synced yet, or
// a copy of a synced node) one from its UUID, stable so that a failed sync
// doesn't make it a new item twice.
std::string GuidOf(const BookmarkNode* node, const Nodes& nodes) {
  std::string guid;
  if (node->GetMetaInfo(kGuidKey, &guid) && guid.size() == 12 &&
      !nodes.contains(guid)) {
    return guid;
  }
  base::RemoveChars(node->uuid().AsLowercaseString(), "-", &guid);
  return guid.substr(0, 12);
}

// `folder`'s children as snapshot nodes. Adds every node to `nodes`, and
// sets `menu` to the menu folder, which is left out.
base::ListValue Walk(const BookmarkNode* folder,
                     Nodes& nodes,
                     const BookmarkNode*& menu) {
  base::ListValue list;
  for (const auto& child : folder->children()) {
    const BookmarkNode* node = child.get();
    std::string guid = GuidOf(node, nodes);
    nodes[guid] = node;
    if (guid == kMenu && node->is_folder()) {
      menu = node;
      continue;
    }
    base::DictValue entry;
    entry.Set("guid", guid);
    entry.Set("title", base::UTF16ToUTF8(node->GetTitle()));
    entry.Set("added", node->date_added().InMillisecondsFSinceUnixEpoch());
    if (node->is_url()) {
      entry.Set("url", node->url().spec());
      const std::u16string keyword = bookmarks::GetBookmarkKeyword(node);
      if (!keyword.empty()) {
        entry.Set("keyword", base::UTF16ToUTF8(keyword));
      }
    } else {
      entry.Set("children", Walk(node, nodes, menu));
    }
    list.Append(std::move(entry));
  }
  return list;
}

// The snapshot, by Firefox root. Fills `nodes` with every node by GUID.
base::DictValue Collect(BookmarkModel* model, Nodes& nodes) {
  const std::pair<const char*, const BookmarkNode*> roots[] = {
      {"toolbar_____", model->bookmark_bar_node()},
      {"unfiled_____", model->other_node()},
      {"mobile______", model->mobile_node()},
  };
  for (const auto& [guid, root] : roots) {
    nodes[guid] = root;
  }
  const BookmarkNode* menu = nullptr;
  base::DictValue snapshot;
  for (const auto& [guid, root] : roots) {
    snapshot.Set(guid, Walk(root, nodes, menu));
  }
  if (menu) {
    snapshot.Set(kMenu, Walk(menu, nodes, menu));
  }
  return snapshot;
}

// The node before `node` in its folder, not counting the menu folder.
const BookmarkNode* PreviousSibling(const BookmarkNode* node,
                                    const BookmarkNode* menu) {
  const BookmarkNode* parent = node->parent();
  for (size_t i = parent->GetIndexOf(node).value(); i > 0; --i) {
    const BookmarkNode* previous = parent->children()[i - 1].get();
    if (previous != menu) {
      return previous;
    }
  }
  return nullptr;
}

// Each node's place and fields, by GUID: a node whose print differs between
// two calls changed in between (or, missing from the first, is new).
std::map<std::string, std::string> Prints(const Nodes& nodes) {
  std::map<const BookmarkNode*, std::string_view> guids;
  for (const auto& [guid, node] : nodes) {
    guids[node] = guid;
  }
  auto menu = nodes.find(kMenu);
  std::map<std::string, std::string> prints;
  for (const auto& [guid, node] : nodes) {
    if (node->is_permanent_node()) {
      continue;
    }
    const BookmarkNode* previous = PreviousSibling(
        node, menu != nodes.end() ? menu->second : nullptr);
    prints[guid] = base::StrCat(
        {guids[node->parent()], "\n", previous ? guids[previous] : "", "\n",
         base::UTF16ToUTF8(node->GetTitle()), "\n",
         node->is_url() ? node->url().spec() : "", "\n",
         base::UTF16ToUTF8(bookmarks::GetBookmarkKeyword(node))});
  }
  return prints;
}

}  // namespace

BookmarksAdapter::BookmarksAdapter(bookmarks::BookmarkModel* model,
                                   base::RepeatingClosure local_changed)
    : model_(model), local_changed_(std::move(local_changed)) {
  model_->AddObserver(this);
}

BookmarksAdapter::~BookmarksAdapter() {
  model_->RemoveObserver(this);
}

void BookmarksAdapter::GetLocal(base::OnceCallback<void(base::Value)> done) {
  if (!model_->loaded()) {
    // Syncing without the local bookmarks would read as deleting them.
    std::move(done).Run(base::Value());
    return;
  }
  Nodes nodes;
  base::DictValue snapshot = Collect(model_, nodes);
  snapshot_ = Prints(nodes);
  std::move(done).Run(base::Value(std::move(snapshot)));
}

void BookmarksAdapter::Apply(const base::ListValue& ops) {
  if (!model_->loaded()) {
    return;
  }
  Nodes nodes;
  Collect(model_, nodes);
  auto find = [&nodes](std::string_view guid) -> const BookmarkNode* {
    auto it = nodes.find(guid);
    return it != nodes.end() ? it->second : nullptr;
  };
  // Nodes added or edited here since the snapshot: the ops, merged from the
  // snapshot, would undo the edits, so these keep their local state and the
  // next sync (the edit scheduled one) takes them up.
  std::vector<const BookmarkNode*> edited;
  for (const auto& [guid, print] : Prints(nodes)) {
    auto it = snapshot_.find(guid);
    if (it == snapshot_.end() || it->second != print) {
      edited.push_back(find(guid));
    }
  }
  // Whether `node` or a node in it is in `edited`.
  auto touched = [&edited](const BookmarkNode* node) {
    return std::ranges::any_of(edited, [node](const BookmarkNode* e) {
      return e->HasAncestor(node);
    });
  };
  std::vector<const BookmarkNode*> doomed;
  size_t added = 0, updated = 0, removed = 0;
  for (const base::Value& value : ops) {
    const base::DictValue* op = value.GetIfDict();
    const std::string* kind = op ? op->FindString("op") : nullptr;
    const std::string* guid = op ? op->FindString("guid") : nullptr;
    if (!kind || !guid) {
      continue;
    }
    const std::string* old = op->FindString("old");
    const BookmarkNode* node = find(old ? *old : *guid);
    if (node && (*kind == "delete" ? touched(node)
                                   : std::ranges::contains(edited, node))) {
      if (*kind == "upsert") {
        // The edit stays, but the node takes the GUID a first sync's
        // deduplication gives it, so the ops for its children find it.
        std::string meta;
        if (!node->GetMetaInfo(kGuidKey, &meta) || meta != *guid) {
          model_->SetNodeMetaInfo(node, kGuidKey, *guid);
        }
        nodes[*guid] = node;
      }
      continue;
    }
    if (*kind == "delete") {
      if (node && !node->is_permanent_node()) {
        doomed.push_back(node);
      }
      continue;
    }
    if (*kind != "upsert") {
      continue;
    }
    const std::string* parent_guid = op->FindString("parent");
    const BookmarkNode* parent = parent_guid ? find(*parent_guid) : nullptr;
    if (!parent && parent_guid && *parent_guid == kMenu) {
      const BookmarkNode* other = model_->other_node();
      parent = model_->AddFolder(other, other->children().size(),
                                 u"Bookmarks Menu");
      model_->SetNodeMetaInfo(parent, kGuidKey, kMenu);
      nodes[kMenu] = parent;
    }
    const std::string* url_spec = op->FindString("url");
    GURL url(url_spec ? *url_spec : std::string());
    if (!parent || !parent->is_folder() || (url_spec && !url.is_valid())) {
      continue;
    }
    const std::string* title_utf8 = op->FindString("title");
    std::u16string title = base::UTF8ToUTF16(title_utf8 ? *title_utf8 : "");
    // Right after `after`, or first if null.
    const std::string* after_guid = op->FindString("after");
    const BookmarkNode* after = after_guid ? find(*after_guid) : nullptr;
    size_t index = 0;
    if (after_guid) {
      index = after && after->parent() == parent
                  ? parent->GetIndexOf(after).value() + 1
                  : parent->children().size();
    }
    if (!node) {
      if (snapshot_.contains(old ? *old : *guid)) {
        continue;  // Deleted here during the sync.
      }
      node = url_spec ? model_->AddURL(parent, index, title, url)
                      : model_->AddFolder(parent, index, title);
      ++added;
    } else {
      if (node->is_permanent_node() || node->is_url() != !!url_spec ||
          parent->HasAncestor(node)) {
        continue;
      }
      bool changed = false;
      if (node->GetTitle() != title) {
        model_->SetTitle(node, title, BookmarkEditSource::kOther);
        changed = true;
      }
      if (url_spec && node->url() != url) {
        model_->SetURL(node, url, BookmarkEditSource::kOther);
        changed = true;
      }
      if (node->parent() != parent ||
          PreviousSibling(node, find(kMenu)) != after) {
        model_->Move(node, parent, index);
        changed = true;
      }
      updated += changed;
    }
    // Firefox keeps a keyword per URL, so every bookmark of the URL gets it.
    const std::string* keyword = op->FindString("keyword");
    if (url_spec && base::UTF16ToUTF8(bookmarks::GetBookmarkKeyword(node)) !=
                        (keyword ? *keyword : std::string())) {
      if (keyword) {
        model_->SetNodeMetaInfo(node, bookmarks::kKeywordMetaInfoKey, *keyword);
      } else {
        model_->DeleteNodeMetaInfo(node, bookmarks::kKeywordMetaInfoKey);
      }
    }
    std::string meta;
    if (!node->GetMetaInfo(kGuidKey, &meta) || meta != *guid) {
      model_->SetNodeMetaInfo(node, kGuidKey, *guid);
    }
    nodes[*guid] = node;
  }
  // Removing a folder frees the nodes in it, so remove only the topmost ones.
  // The menu folder stays, wherever the user put it.
  const BookmarkNode* menu = find(kMenu);
  std::set<const BookmarkNode*> doomed_set(doomed.begin(), doomed.end());
  std::vector<const BookmarkNode*> topmost;
  for (const BookmarkNode* node : doomed) {
    bool inside = false;
    for (const BookmarkNode* ancestor = node->parent(); ancestor && !inside;
         ancestor = ancestor->parent()) {
      inside = doomed_set.contains(ancestor);
    }
    if (!inside && !(menu && menu->HasAncestor(node))) {
      topmost.push_back(node);
    }
  }
  for (const BookmarkNode* node : topmost) {
    model_->Remove(node, BookmarkEditSource::kOther, FROM_HERE);
    ++removed;
  }
  snapshot_.clear();
  VLOG(1) << "Firefox Sync: bookmarks: " << added << " added, " << updated
          << " updated, " << removed << " removed";
}

void BookmarksAdapter::BookmarkModelChanged() {
  local_changed_.Run();
}

}  // namespace firefox_sync
