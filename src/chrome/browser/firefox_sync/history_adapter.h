// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_FIREFOX_SYNC_HISTORY_ADAPTER_H_
#define CHROME_BROWSER_FIREFOX_SYNC_HISTORY_ADAPTER_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "chrome/browser/firefox_sync/sync_adapter.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_service_observer.h"

class PrefService;

namespace firefox_sync {

// The time of the newest visit synced, registered and cleared by
// FirefoxSyncService.
inline constexpr char kHistorySincePref[] =
    "oganesson.firefox_sync.history_since";

// History deletions not yet synced, so they survive the process ending before
// the next sync (Android ends it often). Registered and cleared by
// FirefoxSyncService.
inline constexpr char kHistoryDeletionsPref[] =
    "oganesson.firefox_sync.history_deletions";

// `history`: the profile's history. Each sync sends the visits browsed here
// since the last one (at most 30 days back, in batches), and the deletions
// made since; visits from Firefox are added with the synced visit source, so
// they never go back up. See history.rs for the snapshot and the ops.
class HistoryAdapter : public SyncAdapter,
                       public history::HistoryServiceObserver {
 public:
  HistoryAdapter(history::HistoryService* history,
                 PrefService* prefs,
                 base::RepeatingClosure local_changed);
  ~HistoryAdapter() override;

  // SyncAdapter:
  void GetLocal(base::OnceCallback<void(base::Value)> done) override;
  void Apply(const base::ListValue& ops) override;

 private:
  void OnRead(base::OnceCallback<void(base::Value)> done,
              base::Value visits,
              base::Time until,
              bool full);

  // HistoryServiceObserver:
  void OnHistoryDeletions(history::HistoryService* history_service,
                          const history::DeletionInfo& deletion_info) override;

  raw_ptr<history::HistoryService> history_;
  raw_ptr<PrefService> prefs_;
  base::RepeatingClosure local_changed_;
  // Deletions not synced yet, oldest first; the running sync sent the first
  // `sent_deletions_`.
  base::ListValue deletions_;
  size_t sent_deletions_ = 0;
  // The newest visit the running sync read, and whether it left newer ones
  // for the next sync.
  base::Time sent_until_;
  bool more_ = false;
  base::ScopedObservation<history::HistoryService,
                          history::HistoryServiceObserver>
      observation_{this};
  base::WeakPtrFactory<HistoryAdapter> weak_factory_{this};
};

}  // namespace firefox_sync

#endif  // CHROME_BROWSER_FIREFOX_SYNC_HISTORY_ADAPTER_H_
