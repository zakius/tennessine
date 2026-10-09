// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FIREFOX_SYNC_SYNC_ADAPTER_H_
#define CHROME_BROWSER_FIREFOX_SYNC_SYNC_ADAPTER_H_

#include "base/functional/callback.h"
#include "base/values.h"

namespace firefox_sync {

// The browser side of one Firefox Sync collection. The library has the
// matching engine (see `collection()` in rust/oganesson_sync/src/lib.rs);
// the two agree on the JSON of the local snapshot and of the ops. Adapters
// report local changes through the closure they are created with, and the
// service syncs soon after.
class SyncAdapter {
 public:
  virtual ~SyncAdapter() = default;

  // The engine's `local`: a snapshot of the browser's data. A none Value
  // means the data couldn't be read, and a string explains why the user's
  // settings rule it out; both skip this collection.
  virtual void GetLocal(base::OnceCallback<void(base::Value)> done) = 0;

  // Applies the engine's ops to the browser's data.
  virtual void Apply(const base::ListValue& ops) = 0;
};

}  // namespace firefox_sync

#endif  // CHROME_BROWSER_FIREFOX_SYNC_SYNC_ADAPTER_H_
