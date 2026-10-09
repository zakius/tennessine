// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/firefox_sync/history_adapter.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "components/history/core/browser/history_backend.h"
#include "components/history/core/browser/history_database.h"
#include "components/history/core/browser/history_types.h"
#include "components/prefs/pref_service.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace firefox_sync {

namespace {

// Firefox syncs recent history only.
constexpr base::TimeDelta kWindow = base::Days(30);
// Visits per sync. places uploads up to 5000 pages a sync and drops the
// rest, so a batch never has more.
constexpr int kMaxVisits = 5000;

// Firefox's visit type of a visit the history page shows.
int FirefoxType(ui::PageTransition transition) {
  if (ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_TYPED) ||
      ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_GENERATED) ||
      ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_KEYWORD)) {
    return 2;  // TRANSITION_TYPED
  }
  if (ui::PageTransitionCoreTypeIs(transition,
                                   ui::PAGE_TRANSITION_AUTO_BOOKMARK)) {
    return 3;  // TRANSITION_BOOKMARK
  }
  if (ui::PageTransitionCoreTypeIs(transition, ui::PAGE_TRANSITION_RELOAD)) {
    return 9;  // TRANSITION_RELOAD
  }
  return 1;  // TRANSITION_LINK
}

ui::PageTransition ChromiumTransition(int firefox_type) {
  switch (firefox_type) {
    case 2:
      return ui::PAGE_TRANSITION_TYPED;
    case 3:
      return ui::PAGE_TRANSITION_AUTO_BOOKMARK;
    case 9:
      return ui::PAGE_TRANSITION_RELOAD;
    default:
      return ui::PAGE_TRANSITION_LINK;
  }
}

// On the history sequence: the visits browsed here from `begin` on, oldest
// first, as the history page shows them (no redirect sources, subframes or
// keyword-generated visits).
void ReadVisits(
    base::Time begin,
    base::OnceCallback<void(base::Value, base::Time, bool)> reply,
    history::HistoryBackend*,
    history::URLDatabase* url_db) {
  auto* db = static_cast<history::HistoryDatabase*>(url_db);
  history::VisitVector visits;
  if (!db || !db->GetAllVisitsInRange(begin, base::Time(), std::nullopt,
                                      kMaxVisits, &visits)) {
    std::move(reply).Run(base::Value(), base::Time(), false);
    return;
  }
  // Only visits from elsewhere (sync, extensions, imports) have a source.
  history::VisitSourceMap sources;
  db->GetVisitsSource(visits, &sources);
  base::ListValue list;
  for (const history::VisitRow& visit : visits) {
    ui::PageTransition transition = visit.transition;
    history::URLRow row;
    if (!(transition & ui::PAGE_TRANSITION_CHAIN_END) ||
        !ui::PageTransitionIsMainFrame(transition) ||
        ui::PageTransitionCoreTypeIs(transition,
                                     ui::PAGE_TRANSITION_KEYWORD_GENERATED) ||
        sources.contains(visit.visit_id) ||
        !db->GetURLRow(visit.url_id, &row)) {
      continue;
    }
    list.Append(
        base::DictValue()
            .Set("url", row.url().spec())
            .Set("title", base::UTF16ToUTF8(row.title()))
            .Set("time", static_cast<double>(
                             visit.visit_time.InMillisecondsSinceUnixEpoch()))
            .Set("type", FirefoxType(transition)));
  }
  std::move(reply).Run(
      base::Value(std::move(list)),
      visits.empty() ? base::Time() : visits.back().visit_time,
      visits.size() == static_cast<size_t>(kMaxVisits));
}

// On the history sequence: adds Firefox's visits, skipping those the
// browser has (the same URL and millisecond), such as its own visits coming
// back after places started over.
void AddVisits(base::ListValue visits,
               history::HistoryBackend* backend,
               history::URLDatabase* url_db) {
  auto* db = static_cast<history::HistoryDatabase*>(url_db);
  if (!db) {
    return;
  }
  for (const base::Value& value : visits) {
    const base::DictValue& visit = value.GetDict();
    const std::string* spec = visit.FindString("url");
    std::optional<double> ms = visit.FindDouble("time");
    GURL url(spec ? *spec : std::string());
    if (!ms || !url.is_valid() || !backend->CanAddURL(url)) {
      continue;
    }
    int64_t time = static_cast<int64_t>(*ms);
    history::URLRow row;
    history::VisitVector have;
    if (db->GetRowForURL(url, &row) && db->GetVisitsForURL(row.id(), &have) &&
        std::ranges::any_of(have, [time](const history::VisitRow& v) {
          return v.visit_time.InMillisecondsSinceUnixEpoch() == time;
        })) {
      continue;
    }
    history::HistoryAddPageArgs args;
    args.url = url;
    args.time = base::Time::FromMillisecondsSinceUnixEpoch(time);
    args.transition = ChromiumTransition(visit.FindInt("type").value_or(1));
    args.visit_source = history::SOURCE_SYNCED;
    const std::string* title = visit.FindString("title");
    if (title && !title->empty()) {
      args.title = base::UTF8ToUTF16(*title);
    }
    backend->AddPage(args);
  }
}

}  // namespace

HistoryAdapter::HistoryAdapter(history::HistoryService* history,
                               PrefService* prefs,
                               base::RepeatingClosure local_changed)
    : history_(history),
      prefs_(prefs),
      local_changed_(std::move(local_changed)),
      deletions_(prefs_->GetList(kHistoryDeletionsPref).Clone()) {
  observation_.Observe(history_.get());
}

HistoryAdapter::~HistoryAdapter() = default;

void HistoryAdapter::GetLocal(base::OnceCallback<void(base::Value)> done) {
  if (!history_->BackendLoaded()) {
    std::move(done).Run(base::Value());
    return;
  }
  base::Time begin = std::max(prefs_->GetTime(kHistorySincePref),
                              base::Time::Now() - kWindow);
  history_->ScheduleDBTaskForUI(base::BindOnce(
      &ReadVisits, begin,
      base::BindPostTaskToCurrentDefault(
          base::BindOnce(&HistoryAdapter::OnRead, weak_factory_.GetWeakPtr(),
                         std::move(done)))));
}

void HistoryAdapter::OnRead(base::OnceCallback<void(base::Value)> done,
                            base::Value visits,
                            base::Time until,
                            bool full) {
  if (visits.is_none()) {
    std::move(done).Run(base::Value());
    return;
  }
  sent_until_ = until;
  more_ = full;
  sent_deletions_ = deletions_.size();
  std::move(done).Run(base::Value(base::DictValue()
                                      .Set("visits", std::move(visits))
                                      .Set("deletions", deletions_.Clone())));
}

void HistoryAdapter::Apply(const base::ListValue& ops) {
  deletions_.erase(
      deletions_.begin(),
      deletions_.begin() + static_cast<ptrdiff_t>(sent_deletions_));
  sent_deletions_ = 0;
  prefs_->SetList(kHistoryDeletionsPref, deletions_.Clone());
  if (!sent_until_.is_null()) {
    prefs_->SetTime(kHistorySincePref, sent_until_);
  }
  std::vector<GURL> deleted;
  base::ListValue visits;
  for (const base::Value& value : ops) {
    const base::DictValue* op = value.GetIfDict();
    const std::string* kind = op ? op->FindString("op") : nullptr;
    const std::string* url = op ? op->FindString("url") : nullptr;
    if (kind && url && *kind == "delete") {
      deleted.emplace_back(*url);
    } else if (kind && *kind == "visit") {
      visits.Append(op->Clone());
    }
  }
  if (!deleted.empty()) {
    history_->DeleteURLs(deleted);
  }
  if (!visits.empty()) {
    history_->ScheduleDBTaskForUI(
        base::BindOnce(&AddVisits, std::move(visits)));
  }
  if (more_) {
    local_changed_.Run();
  }
}

void HistoryAdapter::OnHistoryDeletions(
    history::HistoryService* history_service,
    const history::DeletionInfo& deletion_info) {
  // Expiry is local housekeeping.
  if (deletion_info.is_from_expiration()) {
    return;
  }
  const history::DeletionTimeRange& range = deletion_info.time_range();
  const auto& restrict_urls = deletion_info.restrict_urls();
  if (deletion_info.IsAllHistory()) {
    deletions_.Append(base::DictValue().Set("all", true));
  } else if (range.IsValid() && (!restrict_urls || restrict_urls->empty())) {
    // A null end is open.
    base::Time end = range.end().is_null()
                         ? base::Time::Now()
                         : std::min(range.end(), base::Time::Now());
    deletions_.Append(
        base::DictValue()
            .Set("begin", static_cast<double>(
                              range.begin().InMillisecondsSinceUnixEpoch()))
            .Set("end",
                 static_cast<double>(end.InMillisecondsSinceUnixEpoch())));
  } else {
    // Pages with all their visits gone. Deleting some visits of a page
    // stays local.
    for (const history::URLRow& row : deletion_info.deleted_rows()) {
      deletions_.Append(base::DictValue().Set("url", row.url().spec()));
    }
  }
  prefs_->SetList(kHistoryDeletionsPref, deletions_.Clone());
  local_changed_.Run();
}

}  // namespace firefox_sync
