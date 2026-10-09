// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The `history` collection, over application-services' places store and its
//! history engine. places, in a database of its own in the profile folder
//! (bookmarks keep the write connection of theirs for the whole sync), keeps
//! every visit it has seen, so the browser only sends what changed:
//!
//! `{visits: [{url, title, time, type}], deletions: [{all: true} | {begin,
//! end} | {url}]}`, with times in ms since the epoch and Firefox's visit
//! types. Deletions apply first, then the visits; a visit places already has
//! (same URL and time) is skipped, so sending one again is harmless. After
//! the sync, `finish` returns the server's new visits as
//! `{op: "visit", url, title, time, type}` and the pages it deleted as
//! `{op: "delete", url}`.

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{Context, Result};
use places::history_sync::HistorySyncEngine;
use places::storage::history::{
    apply_observation, delete_everything, delete_visits_between, delete_visits_for, url_to_guid,
};
use places::{PlacesApi, PlacesDb, VisitObservation, VisitType};
use serde::Deserialize;
use serde_json::{json, Value};
use sync15::bso::{IncomingBso, OutgoingBso};
use sync15::engine::{CollectionRequest, EngineSyncAssociation, SyncEngine};
use sync15::{telemetry, CollectionName, Guid, ServerTimestamp};
use url::Url;

use crate::bookmarks::{open, set_gen, State};

const STORE: &str = "firefox_sync_history.db";

/// Visit types the browser has: link, typed, bookmark, redirect (to the
/// target) and reload. Embeds, framed links and downloads stay in places.
const SHOWN_TYPES: &str = "1, 2, 3, 5, 6, 9";

#[derive(Deserialize)]
struct Local {
    #[serde(default)]
    visits: Vec<LocalVisit>,
    #[serde(default)]
    deletions: Vec<Deletion>,
}

#[derive(Deserialize)]
struct LocalVisit {
    url: String,
    #[serde(default)]
    title: String,
    time: f64,
    #[serde(rename = "type")]
    kind: u8,
}

/// Everything (only here: Firefox doesn't sync clearing all history), the
/// visits in a time range, or a page.
#[derive(Deserialize, Default)]
#[serde(default)]
struct Deletion {
    all: bool,
    begin: f64,
    end: Option<f64>,
    url: Option<String>,
}

fn store_path(profile: &str) -> PathBuf {
    Path::new(profile).join(STORE)
}

/// Deletes the profile's history database (on sign-out).
pub fn remove_store(profile: &str) {
    crate::remove_db(&store_path(profile));
}

fn has_visit(db: &PlacesDb, url: &Url, time: i64) -> Result<bool> {
    Ok(db.query_row(
        "SELECT EXISTS(SELECT 1 FROM moz_historyvisits v JOIN moz_places h ON h.id = v.place_id
         WHERE h.url_hash = hash(?1) AND h.url = ?1 AND v.visit_date = ?2)",
        (url.as_str(), time),
        |row| row.get(0),
    )?)
}

pub struct HistoryEngine {
    // Dropped in this order: the connections, then the API that owns them.
    engine: HistorySyncEngine,
    db: PlacesDb,
    _api: Arc<PlacesApi>,
    /// The newest visit row before the sync. Newer rows that aren't local
    /// came from the server.
    last_visit: i64,
    /// Pages the server deleted.
    deleted: RefCell<Vec<String>>,
    gen: RefCell<String>,
}

impl HistoryEngine {
    /// From the browser adapter's `{state, local}`: writes the local changes
    /// to places, ready to sync.
    pub fn new(profile: &str, args: &Value) -> Result<Self> {
        let local: Local = serde_json::from_value(args["local"].clone()).context("bad history")?;
        let (api, db, gen) = open(&store_path(profile), args)?;
        for deletion in local.deletions {
            if deletion.all {
                delete_everything(&db)?;
            } else if let Some(url) = deletion.url.and_then(|u| Url::parse(&u).ok()) {
                if let Some(guid) = url_to_guid(&db, &url)? {
                    delete_visits_for(&db, &guid)?;
                }
            } else if let Some(end) = deletion.end {
                delete_visits_between(
                    &db,
                    types::Timestamp(deletion.begin.max(0.0) as u64),
                    types::Timestamp(end.max(0.0) as u64),
                )?;
            }
        }
        for visit in local.visits {
            let (Ok(url), Some(kind)) = (
                Url::parse(&visit.url),
                VisitType::from_primitive(visit.kind),
            ) else {
                continue;
            };
            let time = visit.time as i64;
            if time <= 0 || has_visit(&db, &url, time)? {
                continue;
            }
            let title = (!visit.title.is_empty()).then_some(visit.title);
            let observation = VisitObservation::new(url)
                .with_title(title)
                .with_visit_type(kind)
                .with_at(types::Timestamp(time as u64));
            apply_observation(&db, observation)?;
        }
        let last_visit = db.query_row(
            "SELECT IFNULL(MAX(id), 0) FROM moz_historyvisits",
            [],
            |r| r.get(0),
        )?;
        let engine = HistorySyncEngine::new(api.get_sync_connection()?)?;
        Ok(Self {
            engine,
            db,
            _api: api,
            last_visit,
            deleted: RefCell::default(),
            gen: RefCell::new(gen),
        })
    }
}

impl crate::Collection for HistoryEngine {
    fn finish(self: Box<Self>) -> Result<Value> {
        let mut statement = self.db.prepare(&format!(
            "SELECT h.url, h.title, v.visit_date, v.visit_type
             FROM moz_historyvisits v JOIN moz_places h ON h.id = v.place_id
             WHERE v.id > ?1 AND NOT v.is_local AND v.visit_type IN ({SHOWN_TYPES})"
        ))?;
        let visits = statement.query_map([self.last_visit], |r| {
            Ok(json!({
                "op": "visit",
                "url": r.get::<_, String>(0)?,
                "title": r.get::<_, Option<String>>(1)?.unwrap_or_default(),
                "time": r.get::<_, i64>(2)?,
                "type": r.get::<_, u8>(3)?,
            }))
        })?;
        let mut ops = visits.collect::<Result<Vec<Value>, _>>()?;
        ops.extend(
            self.deleted
                .borrow()
                .iter()
                .map(|url| json!({ "op": "delete", "url": url })),
        );
        let count: i64 = self.db.query_row(
            "SELECT COUNT(DISTINCT place_id) FROM moz_historyvisits",
            [],
            |r| r.get(0),
        )?;
        let state = State {
            gen: self.gen.borrow().clone(),
        };
        Ok(json!({
            "state": serde_json::to_string(&state)?,
            "ops": ops,
            "count": count,
        }))
    }
}

impl SyncEngine for HistoryEngine {
    fn collection_name(&self) -> CollectionName {
        self.engine.collection_name()
    }

    /// places' history engine applies incoming records here, so this is
    /// where places starts having changes the browser hasn't applied yet.
    fn stage_incoming(
        &self,
        inbound: Vec<IncomingBso>,
        telem: &mut telemetry::Engine,
    ) -> Result<()> {
        if inbound.is_empty() {
            return self.engine.stage_incoming(inbound, telem);
        }
        let gen = Guid::random().to_string();
        set_gen(&self.db, &gen)?;
        *self.gen.borrow_mut() = gen;
        for bso in &inbound {
            let tombstone = serde_json::from_str::<Value>(&bso.payload)
                .is_ok_and(|payload| payload["deleted"] == true);
            if !tombstone {
                continue;
            }
            if let Ok(url) = self.db.query_row(
                "SELECT url FROM moz_places WHERE guid = ?1",
                [bso.envelope.id.as_str()],
                |r| r.get::<_, String>(0),
            ) {
                self.deleted.borrow_mut().push(url);
            }
        }
        self.engine.stage_incoming(inbound, telem)
    }

    fn apply(
        &self,
        timestamp: ServerTimestamp,
        telem: &mut telemetry::Engine,
    ) -> Result<Vec<OutgoingBso>> {
        self.engine.apply(timestamp, telem)
    }

    fn set_uploaded(&self, new_timestamp: ServerTimestamp, ids: Vec<Guid>) -> Result<()> {
        self.engine.set_uploaded(new_timestamp, ids)
    }

    fn sync_finished(&self) -> Result<()> {
        self.engine.sync_finished()
    }

    fn get_collection_request(&self, server: ServerTimestamp) -> Result<Option<CollectionRequest>> {
        self.engine.get_collection_request(server)
    }

    fn get_sync_assoc(&self) -> Result<EngineSyncAssociation> {
        self.engine.get_sync_assoc()
    }

    fn reset(&self, assoc: &EngineSyncAssociation) -> Result<()> {
        self.engine.reset(assoc)
    }

    fn wipe(&self) -> Result<()> {
        self.engine.wipe()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Collection;
    use places::history_sync::record::HistoryRecordVisit;
    use places::storage::history::history_sync::apply_synced_visits;

    #[test]
    fn visits_reach_places_once() {
        let dir = std::env::temp_dir().join(format!("og-history-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let profile = dir.to_str().unwrap();
        remove_store(profile);

        let a = json!({"url": "https://a.example/", "title": "A", "time": 1_700_000_000_000.0, "type": 2});
        let args = |state: &str, local: Value| json!({ "state": state, "local": local });
        let first = HistoryEngine::new(
            profile,
            &args("", json!({"visits": [a.clone(), a.clone()]})),
        )
        .unwrap();
        let first = Box::new(first).finish().unwrap();
        assert_eq!(first["count"], 1);
        assert_eq!(first["ops"], json!([]));
        let state = first["state"].as_str().unwrap().to_string();

        // A visit sent again is skipped; a server visit comes back as an op.
        let second = HistoryEngine::new(profile, &args(&state, json!({"visits": [a]}))).unwrap();
        let b = Url::parse("https://b.example/").unwrap();
        let from_server = HistoryRecordVisit {
            date: types::Timestamp(1_700_000_001_000).into(),
            transition: 1,
            unknown_fields: Default::default(),
        };
        apply_synced_visits(
            &second.db,
            &Guid::random(),
            &b,
            &Some("B".into()),
            &[from_server],
            &Default::default(),
        )
        .unwrap();
        let visits: i64 = second
            .db
            .query_row("SELECT COUNT(*) FROM moz_historyvisits", [], |r| r.get(0))
            .unwrap();
        assert_eq!(visits, 2);
        let second = Box::new(second).finish().unwrap();
        assert_eq!(
            second["ops"],
            json!([
                {"op": "visit", "url": "https://b.example/", "title": "B", "time": 1_700_000_001_000_i64, "type": 1},
            ])
        );

        let third = HistoryEngine::new(
            profile,
            &args(
                &state,
                json!({"deletions": [{"url": "https://a.example/"}]}),
            ),
        );
        assert_eq!(Box::new(third.unwrap()).finish().unwrap()["count"], 1);
        remove_store(profile);
    }
}
