// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FIREFOX_SYNC_BOOKMARKS_ADAPTER_H_
#define CHROME_BROWSER_FIREFOX_SYNC_BOOKMARKS_ADAPTER_H_

#include <map>
#include <string>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/firefox_sync/sync_adapter.h"
#include "components/bookmarks/browser/base_bookmark_model_observer.h"

namespace bookmarks {
class BookmarkModel;
}

namespace firefox_sync {

// `bookmarks`: the profile's local bookmarks (not account storage). Each
// node's Firefox GUID is kept in its meta info. The bookmarks bar, Other
// bookmarks and Mobile bookmarks are Firefox's toolbar, unfiled and mobile
// roots; its menu root is a "Bookmarks Menu" folder, created in Other
// bookmarks when the menu has bookmarks. See bookmarks.rs for the snapshot
// and the ops.
class BookmarksAdapter : public SyncAdapter,
                         public bookmarks::BaseBookmarkModelObserver {
 public:
  BookmarksAdapter(bookmarks::BookmarkModel* model,
                   base::RepeatingClosure local_changed);
  ~BookmarksAdapter() override;

  // SyncAdapter:
  void GetLocal(base::OnceCallback<void(base::Value)> done) override;
  void Apply(const base::ListValue& ops) override;

 private:
  // BaseBookmarkModelObserver:
  void BookmarkModelChanged() override;

  raw_ptr<bookmarks::BookmarkModel> model_;
  base::RepeatingClosure local_changed_;
  // The running sync's snapshot: each node's place and fields by GUID. A
  // node missing from the model that the snapshot had was deleted since, and
  // isn't recreated; one that differs was edited since, and keeps the edit.
  std::map<std::string, std::string> snapshot_;
};

}  // namespace firefox_sync

#endif  // CHROME_BROWSER_FIREFOX_SYNC_BOOKMARKS_ADAPTER_H_
