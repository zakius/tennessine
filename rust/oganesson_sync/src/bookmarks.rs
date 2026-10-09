// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The `bookmarks` collection, over application-services' places store and
//! its bookmark engine (dogear tree merge). The browser's bookmarks stay the
//! source of truth; places, in the profile folder, is the staging store
//! between them and the server.
//!
//! Each sync gets a snapshot of the browser's bookmarks: `{root: [node]}` by
//! Firefox root GUID, a node being `{guid, title, added, url, keyword?}` or
//! `{guid, title, added, children}`. Places holds what the browser had after
//! the last sync, so the difference between the two is the local changes,
//! which go into places (bumping its change counters) before the merge. After
//! the merge, `finish` returns places' tree as ops for the browser to apply.
//!
//! Separators, queries (`place:` URLs) and tags live only in places; the
//! browser never sees them and they are kept in place. Keywords belong to a
//! URL in places (as in Firefox) and to a bookmark in the browser: every
//! bookmark of a URL gets the URL's keyword.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{bail, Context, Result};
use places::bookmark_sync::BookmarksSyncEngine;
use places::storage::bookmarks::fetch::{fetch_tree, Item as PlacesItem};
use places::storage::bookmarks::{
    delete_bookmark, insert_bookmark, update_bookmark, BookmarkPosition, InsertableBookmark,
    InsertableFolder, InsertableItem, UpdatableBookmark, UpdatableFolder, UpdatableItem,
    UpdateTreeLocation,
};
use places::{ConnectionType, PlacesApi, PlacesDb};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sync15::bso::{IncomingBso, OutgoingBso};
use sync15::engine::{CollectionRequest, EngineSyncAssociation, SyncEngine};
use sync15::{telemetry, CollectionName, Guid, ServerTimestamp};
use url::Url;

/// The places database, in the browser profile's folder.
const STORE: &str = "firefox_sync_places.db";

/// The roots the browser has, in snapshot order: toolbar, unfiled (Other
/// bookmarks), mobile, and menu (a folder in Other bookmarks).
const ROOTS: [&str; 4] = ["toolbar_____", "unfiled_____", "mobile______", "menu________"];

/// See passwords.rs: a snapshot that lost the bookmarks must not empty the
/// user's Firefox.
const MASS_DELETE_MIN: usize = 10;

/// A browser snapshot node.
#[derive(Deserialize)]
struct LocalNode {
    guid: String,
    #[serde(default)]
    title: String,
    #[serde(default)]
    added: f64,
    #[serde(default)]
    url: Option<String>,
    #[serde(default)]
    keyword: Option<String>,
    #[serde(default)]
    children: Vec<LocalNode>,
}

/// An item of a tree as the browser sees it: a folder without `url`. `after`
/// is its previous sibling, which is how order is compared.
struct Entry {
    parent: String,
    after: Option<String>,
    title: String,
    url: Option<Url>,
    keyword: Option<String>,
    added: f64,
}

/// Items by GUID, and their GUIDs in pre-order.
#[derive(Default)]
struct Tree {
    order: Vec<String>,
    items: HashMap<String, Entry>,
}

impl Tree {
    fn add(&mut self, guid: String, entry: Entry) {
        self.order.push(guid.clone());
        self.items.insert(guid, entry);
    }

    fn from_local(mut roots: HashMap<String, Vec<LocalNode>>) -> Tree {
        fn walk(tree: &mut Tree, parent: &str, nodes: Vec<LocalNode>) {
            let mut after: Option<String> = None;
            for node in nodes {
                let url = match node.url.map(|u| Url::parse(&u)) {
                    Some(Ok(url)) if url.scheme() != "place" => Some(url),
                    Some(_) => continue,
                    None => None,
                };
                let guid = node.guid;
                if !Guid::from(guid.as_str()).is_valid_for_places()
                    || ROOTS.contains(&guid.as_str())
                    || tree.items.contains_key(&guid)
                {
                    continue;
                }
                let folder = url.is_none();
                let entry = Entry {
                    parent: parent.into(),
                    after: after.replace(guid.clone()),
                    title: node.title,
                    keyword: node.keyword.filter(|k| url.is_some() && !k.is_empty()),
                    url,
                    added: node.added,
                };
                tree.add(guid.clone(), entry);
                if folder {
                    walk(tree, &guid, node.children);
                }
            }
        }
        let mut tree = Tree::default();
        for root in ROOTS {
            walk(&mut tree, root, roots.remove(root).unwrap_or_default());
        }
        tree
    }

    fn from_places(db: &PlacesDb) -> Result<Tree> {
        fn walk(
            tree: &mut Tree,
            keywords: &HashMap<String, String>,
            parent: &str,
            items: Vec<PlacesItem>,
        ) {
            let mut after: Option<String> = None;
            for item in items {
                let (guid, title, url, children) = match item {
                    PlacesItem::Bookmark { b } if b.url.scheme() != "place" => {
                        (b.guid, b.title, Some(b.url), vec![])
                    }
                    PlacesItem::Folder { f } => {
                        (f.guid, f.title, None, f.child_nodes.unwrap_or_default())
                    }
                    _ => continue,
                };
                let guid = guid.to_string();
                let entry = Entry {
                    parent: parent.into(),
                    after: after.replace(guid.clone()),
                    title: title.unwrap_or_default(),
                    keyword: url.as_ref().and_then(|u| keywords.get(u.as_str()).cloned()),
                    url,
                    added: 0.0,
                };
                tree.add(guid.clone(), entry);
                walk(tree, keywords, &guid, children);
            }
        }
        let keywords = keywords(db)?;
        let mut tree = Tree::default();
        for root in ROOTS {
            if let Some(PlacesItem::Folder { f }) = fetch_tree(db, &Guid::from(root))? {
                walk(&mut tree, &keywords, root, f.child_nodes.unwrap_or_default());
            }
        }
        Ok(tree)
    }
}

/// Persisted by the browser: the generation of places it last applied.
#[derive(Serialize, Deserialize, Default)]
pub(crate) struct State {
    #[serde(default)]
    pub(crate) gen: String,
}

fn store_path(profile: &str) -> PathBuf {
    Path::new(profile).join(STORE)
}

/// Deletes the profile's places database (on sign-out, or to start over).
pub fn remove_store(profile: &str) {
    crate::remove_db(&store_path(profile));
}

/// A new generation is written whenever a merge may change places, and the
/// browser keeps the one it applied. When they differ, places has changes
/// the browser never got (or another account's data), so comparing it with
/// the browser's bookmarks would read them as local changes.
fn get_gen(db: &PlacesDb) -> String {
    db.query_row("SELECT value FROM moz_meta WHERE key = 'oganesson_gen'", [], |r| r.get(0))
        .unwrap_or_default()
}

pub(crate) fn set_gen(db: &PlacesDb, gen: &str) -> Result<()> {
    db.execute("INSERT OR REPLACE INTO moz_meta (key, value) VALUES ('oganesson_gen', ?1)", [gen])?;
    Ok(())
}

/// Opens the places database at `path` for a sync, with the engine state in
/// `args`: a database at another generation than the state's starts over.
/// Returns the state's generation.
pub(crate) fn open(path: &Path, args: &Value) -> Result<(Arc<PlacesApi>, PlacesDb, String)> {
    let state: State = match args["state"].as_str() {
        Some(s) if !s.is_empty() => serde_json::from_str(s).context("bad engine state")?,
        _ => State::default(),
    };
    let api = PlacesApi::new(path)?;
    let db = api.open_connection(ConnectionType::ReadWrite)?;
    if get_gen(&db) == state.gen {
        return Ok((api, db, state.gen));
    }
    drop(db);
    drop(api);
    crate::remove_db(path);
    let api = PlacesApi::new(path)?;
    let db = api.open_connection(ConnectionType::ReadWrite)?;
    Ok((api, db, state.gen))
}

/// Keywords by URL.
fn keywords(db: &PlacesDb) -> Result<HashMap<String, String>> {
    let mut statement = db.prepare(
        "SELECT h.url, k.keyword FROM moz_keywords k JOIN moz_places h ON h.id = k.place_id",
    )?;
    let rows = statement.query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?;
    Ok(rows.collect::<Result<HashMap<String, String>, _>>()?)
}

/// Sets or removes the keyword of `url` (a bookmarked URL), and marks the
/// bookmarks whose keyword changes for upload, as Firefox desktop does.
fn set_keyword(db: &PlacesDb, url: &str, keyword: Option<&str>) -> Result<()> {
    let Ok(place) = db.query_row(
        "SELECT id FROM moz_places WHERE url_hash = hash(?1) AND url = ?1",
        [url],
        |r| r.get::<_, i64>(0),
    ) else {
        return Ok(());
    };
    // A keyword names one URL: take it from any other URL first.
    db.execute(
        "UPDATE moz_bookmarks SET syncChangeCounter = syncChangeCounter + 1
         WHERE fk = ?1 OR fk IN (SELECT place_id FROM moz_keywords WHERE keyword = ?2)",
        (place, keyword),
    )?;
    db.execute("DELETE FROM moz_keywords WHERE place_id = ?1 OR keyword = ?2", (place, keyword))?;
    if let Some(keyword) = keyword {
        db.execute("INSERT INTO moz_keywords (keyword, place_id) VALUES (?1, ?2)", (keyword, place))?;
    }
    Ok(())
}

/// Places row IDs by GUID. Merging can change an item's GUID (deduping on the
/// first sync), never its row.
fn row_ids(db: &PlacesDb) -> Result<HashMap<String, i64>> {
    let mut statement = db.prepare("SELECT guid, id FROM moz_bookmarks")?;
    let rows = statement.query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?;
    let ids = rows.collect::<Result<HashMap<String, i64>, _>>()?;
    Ok(ids)
}

/// An item's parent GUID, position, and previous sibling the browser sees.
fn location(db: &PlacesDb, guid: &str) -> Result<(String, u32, Option<String>)> {
    let (parent_id, parent, position): (i64, String, u32) = db.query_row(
        "SELECT b.parent, p.guid, b.position FROM moz_bookmarks b
         JOIN moz_bookmarks p ON p.id = b.parent WHERE b.guid = ?1",
        [guid],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
    )?;
    let previous = db
        .query_row(
            "SELECT b.guid FROM moz_bookmarks b LEFT JOIN moz_places h ON h.id = b.fk
             WHERE b.parent = ?1 AND b.position < ?2 AND b.type != 3
               AND (h.url IS NULL OR h.url NOT LIKE 'place:%')
             ORDER BY b.position DESC LIMIT 1",
            (parent_id, position),
            |r| r.get(0),
        )
        .ok();
    Ok((parent, position, previous))
}

/// Where to put an item so it follows `want.after`: places' final index, for
/// an item now at `current` (parent, position), if any.
fn target(db: &PlacesDb, want: &Entry, current: Option<(&str, u32)>) -> Result<BookmarkPosition> {
    let Some(after) = &want.after else {
        return Ok(BookmarkPosition::Specific { pos: 0 });
    };
    let (_, after_pos, _) = location(db, after)?;
    let pos = match current {
        Some((parent, pos)) if parent == want.parent && pos < after_pos => after_pos,
        _ => after_pos + 1,
    };
    Ok(BookmarkPosition::Specific { pos })
}

fn insert(db: &PlacesDb, guid: &str, want: &Entry) -> Result<()> {
    let parent_guid = Guid::from(want.parent.as_str());
    let position = target(db, want, None)?;
    let date_added = (want.added >= 1.0).then(|| types::Timestamp(want.added as u64));
    let title = (!want.title.is_empty()).then(|| want.title.clone());
    let guid = Some(Guid::from(guid));
    let item: InsertableItem = match &want.url {
        Some(url) => InsertableBookmark {
            parent_guid,
            position,
            date_added,
            last_modified: None,
            guid,
            url: url.clone(),
            title,
        }
        .into(),
        None => InsertableFolder {
            parent_guid,
            position,
            date_added,
            last_modified: None,
            guid,
            title,
            children: vec![],
        }
        .into(),
    };
    insert_bookmark(db, item)?;
    Ok(())
}

fn update(db: &PlacesDb, guid: &str, have: &Entry, want: &Entry) -> Result<()> {
    if have.url.is_some() != want.url.is_some() {
        bail!("{guid} changed type");
    }
    let id = Guid::from(guid);
    let title = (have.title != want.title).then(|| want.title.clone());
    let url = if have.url != want.url { want.url.clone() } else { None };
    if title.is_some() || url.is_some() {
        let item: UpdatableItem = match want.url {
            Some(_) => UpdatableBookmark { location: UpdateTreeLocation::None, url, title }.into(),
            None => UpdatableFolder { location: UpdateTreeLocation::None, title }.into(),
        };
        update_bookmark(db, &id, &item)?;
    }
    let (parent, pos, previous) = location(db, guid)?;
    if parent == want.parent && previous == want.after {
        return Ok(());
    }
    let moved = UpdateTreeLocation::Parent {
        guid: Guid::from(want.parent.as_str()),
        pos: target(db, want, Some((parent.as_str(), pos)))?,
    };
    let item: UpdatableItem = match want.url {
        Some(_) => UpdatableBookmark { location: moved, ..Default::default() }.into(),
        None => UpdatableFolder { location: moved, ..Default::default() }.into(),
    };
    update_bookmark(db, &id, &item)?;
    Ok(())
}

/// Writes the browser's keyword changes to places. Before the browser has
/// received keywords from places once (see `finish`), a bookmark without one
/// is not a removal: it may predate keyword support.
fn sync_keywords(db: &PlacesDb, local: &Tree) -> Result<()> {
    let delivered = db
        .query_row("SELECT value FROM moz_meta WHERE key = 'oganesson_keywords'", [], |r| {
            r.get::<_, i64>(0)
        })
        .is_ok();
    let have = keywords(db)?;
    // A URL's keyword is its first bookmark's that has one, in pre-order.
    let mut want: HashMap<&str, Option<&str>> = HashMap::new();
    for guid in &local.order {
        let entry = &local.items[guid];
        if let Some(url) = &entry.url {
            let keyword = want.entry(url.as_str()).or_insert(None);
            if keyword.is_none() {
                *keyword = entry.keyword.as_deref();
            }
        }
    }
    for (url, keyword) in want {
        let current = have.get(url).map(String::as_str);
        if current != keyword && (keyword.is_some() || delivered) {
            set_keyword(db, url, keyword)?;
        }
    }
    Ok(())
}

pub struct BookmarksEngine {
    // Dropped in this order: the connections, then the API that owns them.
    engine: BookmarksSyncEngine,
    db: PlacesDb,
    _api: Arc<PlacesApi>,
    /// The snapshot, less items places refused.
    local: Tree,
    /// Snapshot GUIDs by places row, to follow GUID changes.
    rows: HashMap<i64, String>,
    gen: RefCell<String>,
}

impl BookmarksEngine {
    /// From the browser adapter's `{state, local}`: writes the local changes
    /// to places, ready to sync.
    pub fn new(profile: &str, args: &Value) -> Result<Self> {
        let roots = serde_json::from_value(args["local"].clone()).context("bad bookmarks")?;
        let mut local = Tree::from_local(roots);
        let (api, db, gen) = open(&store_path(profile), args)?;

        let before = Tree::from_places(&db)?;
        let deletions: Vec<&String> =
            before.order.iter().filter(|g| !local.items.contains_key(*g)).collect();
        if deletions.len() > MASS_DELETE_MIN && deletions.len() * 2 > before.items.len() {
            bail!(
                "refusing to delete {} of {} synced bookmarks from the server",
                deletions.len(),
                before.items.len()
            );
        }
        // In pre-order, so parents and previous siblings are in place first.
        // Refused items (a URL places rejects) map to the sibling before them.
        let mut refused: HashMap<String, Option<String>> = HashMap::new();
        for guid in &local.order {
            let want = local.items.get_mut(guid).expect("in order");
            if let Some(skip) = want.after.as_deref().and_then(|a| refused.get(a)).cloned() {
                want.after = skip;
            }
            let result = match before.items.get(guid) {
                Some(have) => update(&db, guid, have, want),
                None => insert(&db, guid, want),
            };
            if result.is_err() {
                refused.insert(guid.clone(), want.after.clone());
            }
        }
        // After the moves, so children moved out of a deleted folder stay.
        for guid in deletions {
            delete_bookmark(&db, &Guid::from(guid.as_str()))?;
        }
        // Neither synced nor deleted: the next sync tries again.
        for guid in refused.keys() {
            local.items.remove(guid);
        }
        sync_keywords(&db, &local)?;

        let rows = row_ids(&db)?
            .into_iter()
            .filter(|(guid, _)| local.items.contains_key(guid))
            .map(|(guid, id)| (id, guid))
            .collect();
        let engine = BookmarksSyncEngine::new(api.get_sync_connection()?)?;
        Ok(Self { engine, db, _api: api, local, rows, gen: RefCell::new(gen) })
    }
}

impl crate::Collection for BookmarksEngine {
    /// Ops, in order: `{op: "upsert", guid, old?, parent, after, title, url?,
    /// keyword?}`
    /// for every item in places, in pre-order (`old` is the item's snapshot
    /// GUID when merging changed it; the browser creates, updates or moves it
    /// to follow `after`, or to be first if null), then `{op: "delete", guid}`
    /// for snapshot items places no longer has.
    fn finish(self: Box<Self>) -> Result<Value> {
        let merged = Tree::from_places(&self.db)?;
        let ids = row_ids(&self.db)?;
        let mut ops = vec![];
        let mut kept = HashSet::new();
        for guid in &merged.order {
            let item = &merged.items[guid];
            let mut op = json!({
                "op": "upsert",
                "guid": guid,
                "parent": item.parent,
                "after": item.after,
                "title": item.title,
            });
            if let Some(url) = &item.url {
                op["url"] = json!(url.as_str());
            }
            if let Some(keyword) = &item.keyword {
                op["keyword"] = json!(keyword);
            }
            match ids.get(guid).and_then(|id| self.rows.get(id)) {
                Some(old) if old != guid => {
                    op["old"] = json!(old);
                    kept.insert(old.clone());
                }
                _ => {
                    kept.insert(guid.clone());
                }
            }
            ops.push(op);
        }
        for guid in &self.local.order {
            if self.local.items.contains_key(guid) && !kept.contains(guid) {
                ops.push(json!({ "op": "delete", "guid": guid }));
            }
        }
        self.db.execute(
            "INSERT OR REPLACE INTO moz_meta (key, value) VALUES ('oganesson_keywords', 1)",
            [],
        )?;
        let state = State { gen: self.gen.borrow().clone() };
        Ok(json!({
            "state": serde_json::to_string(&state)?,
            "ops": ops,
            "count": merged.items.len(),
        }))
    }
}

impl SyncEngine for BookmarksEngine {
    fn collection_name(&self) -> CollectionName {
        self.engine.collection_name()
    }

    fn stage_incoming(&self, inbound: Vec<IncomingBso>, telem: &mut telemetry::Engine) -> Result<()> {
        self.engine.stage_incoming(inbound, telem)
    }

    fn apply(&self, timestamp: ServerTimestamp, telem: &mut telemetry::Engine) -> Result<Vec<OutgoingBso>> {
        let gen = Guid::random().to_string();
        set_gen(&self.db, &gen)?;
        *self.gen.borrow_mut() = gen;
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

    fn sync(profile: &str, state: &str, local: Value) -> Value {
        let engine = BookmarksEngine::new(profile, &json!({ "state": state, "local": local })).unwrap();
        Box::new(engine).finish().unwrap()
    }

    /// The upserts' [guid, parent, after].
    fn upserts(result: &Value) -> Vec<Value> {
        result["ops"].as_array().unwrap().iter()
            .filter(|op| op["op"] == "upsert")
            .map(|op| json!([op["guid"], op["parent"], op["after"]]))
            .collect()
    }

    #[test]
    fn local_changes_reach_places() {
        let dir = std::env::temp_dir().join(format!("og-bookmarks-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let profile = dir.to_str().unwrap();
        remove_store(profile);

        let a = json!({"guid": "aaaaaaaaaaaa", "title": "A", "url": "https://a.example/",
                       "keyword": "ak"});
        let b = json!({"guid": "bbbbbbbbbbbb", "title": "B", "url": "https://b.example/"});
        let folder = json!({"guid": "ffffffffffff", "title": "F", "children": [b.clone()]});
        let first = sync(profile, "", json!({"toolbar_____": [a.clone(), folder]}));
        assert_eq!(upserts(&first), vec![
            json!(["aaaaaaaaaaaa", "toolbar_____", null]),
            json!(["ffffffffffff", "toolbar_____", "aaaaaaaaaaaa"]),
            json!(["bbbbbbbbbbbb", "ffffffffffff", null]),
        ]);
        assert_eq!(first["count"], 3);
        assert_eq!(first["ops"][0]["keyword"], "ak");
        let state = first["state"].as_str().unwrap();

        // B moves out of the folder to before A, the folder is deleted, and A
        // loses its keyword.
        let mut a = a;
        a.as_object_mut().unwrap().remove("keyword");
        let second = sync(profile, state, json!({"toolbar_____": [b, a]}));
        assert_eq!(upserts(&second), vec![
            json!(["bbbbbbbbbbbb", "toolbar_____", null]),
            json!(["aaaaaaaaaaaa", "toolbar_____", "bbbbbbbbbbbb"]),
        ]);
        assert_eq!(second["count"], 2);
        assert_eq!(second["ops"][1]["keyword"], Value::Null);

        // A generation places doesn't have starts over: no deletions.
        let third = sync(profile, r#"{"gen":"elsewhere"}"#, json!({}));
        assert_eq!(third["count"], 0);
        remove_store(profile);
    }
}
