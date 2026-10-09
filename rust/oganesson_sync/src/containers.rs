// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Container definitions, synced the way Firefox syncs them: through the
//! `storage.sync` area of Mozilla's Multi-Account Containers add-on
//! (`@testpilot-containers`), which Firefox Sync carries in the
//! `extension-storage` collection. application-services' webext-storage
//! store is the staging store; only that add-on's area is read or written.
//!
//! The add-on's format (its `src/js/background/sync.js`): one
//! `identity@@_<uuid>` key per container (`name`, `icon`, `color`,
//! `macAddonUUID`), a `deletedIdentityList` of uuids, and a
//! `MACinstance:<id>` key per install listing the uuids it has, which keeps
//! deletions around until every install has seen them. Site assignments
//! (`siteContainerMap@@_*`) are left alone: oganesson has none.
//!
//! The browser sends its containers, `[{id, name, icon, color}]` with icon
//! the index of oganesson's icon and color `#rrggbb`. A container uploads
//! under its own id as uuid, except when its name matched a Firefox
//! container's first, as the add-on matches them; the state keeps those
//! pairs. `finish` returns `{op: "upsert", id, name, icon, color}` for every
//! synced container and `{op: "delete", id}` for the deleted ones.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};
use sync15::bso::{IncomingBso, OutgoingBso, OutgoingEnvelope};
use sync15::engine::{CollSyncIds, CollectionRequest, EngineSyncAssociation, SyncEngine};
use sync15::{telemetry, CollectionName, ServerTimestamp};
use sync_guid::Guid;
use webext_storage::{WebExtStorageBridgedEngine, WebExtStorageStore};

use crate::{remove_db, Collection};

const STORE: &str = "firefox_sync_extension_storage.db";
const EXT_ID: &str = "@testpilot-containers";
const COLLECTION: &str = "extension-storage";
const IDENTITY: &str = "identity@@_";
const DELETED: &str = "deletedIdentityList";
/// The add-on drops installs it hasn't heard from in 30 days; refresh ours
/// well before that even without changes.
const INSTANCE_REFRESH_MS: i64 = 7 * 24 * 3600 * 1000;

/// oganesson's icons, by index (`containers::mojom::Icon`), as Firefox's.
/// Firefox icons without a counterpart map to the star; see `icon_for_sync`.
const ICONS: [&str; 11] = [
    "fingerprint", // kPersonal
    "briefcase",   // kWork
    "cart",        // kShopping
    "chill",       // kSocial
    "gift",        // kEvents
    "dollar",      // kBanking
    "circle",      // kStar
    "vacation",    // kTravel
    "tree",        // kSchool
    "fence",       // kPrivate
    "circle",      // kMessaging
];
const STAR: usize = 6;

/// Firefox's container colors, under the legacy names the add-on syncs.
const COLORS: [(&str, u32); 9] = [
    ("blue", 0x37adff),
    ("turquoise", 0x00c79a),
    ("green", 0x51cd00),
    ("yellow", 0xffcb00),
    ("orange", 0xff9f00),
    ("red", 0xff613d),
    ("pink", 0xff4bda),
    ("purple", 0xaf51f5),
    ("toolbar", 0x7c7c7d),
];

fn icon_from_sync(name: &str) -> usize {
    ICONS.iter().position(|i| *i == name).unwrap_or(STAR)
}

/// Firefox's icon for a local one. `current` is the synced icon, kept when
/// it already means `icon` here, so lossy mappings don't overwrite Firefox.
fn icon_for_sync(icon: usize, current: Option<&str>) -> String {
    match current {
        Some(c) if icon_from_sync(c) == icon => c.into(),
        _ => ICONS.get(icon).copied().unwrap_or("fingerprint").into(),
    }
}

fn color_from_sync(name: &str) -> String {
    let rgb = COLORS.iter().find(|(n, _)| *n == name).map_or(0x37adff, |(_, rgb)| *rgb);
    format!("#{rgb:06x}")
}

fn parse_color(hex: &str) -> Option<u32> {
    u32::from_str_radix(hex.trim_start_matches('#'), 16).ok()
}

/// The nearest Firefox color, or `current` when it already means `color`.
fn color_for_sync(color: &str, current: Option<&str>) -> String {
    if let Some(c) = current {
        if color_from_sync(c).eq_ignore_ascii_case(color) {
            return c.into();
        }
    }
    let rgb = parse_color(color).unwrap_or(0x37adff);
    let channels = |c: u32| [(c >> 16) as i64 & 255, (c >> 8) as i64 & 255, c as i64 & 255];
    let distance = |c: u32| {
        channels(c).iter().zip(channels(rgb)).map(|(a, b)| (a - b) * (a - b)).sum::<i64>()
    };
    COLORS.iter().min_by_key(|(_, c)| distance(*c)).map(|(n, _)| (*n).into()).unwrap()
}

#[derive(Deserialize, Clone)]
struct Local {
    id: String,
    name: String,
    icon: usize,
    color: String,
}

#[derive(Serialize, Deserialize, Default)]
struct State {
    /// meta/global's sync ID, which webext-storage's engine doesn't keep.
    #[serde(default)]
    global: String,
    /// Local container id -> the add-on's uuid, when they differ.
    #[serde(default)]
    uuids: HashMap<String, String>,
    /// This install's `MACinstance:` key.
    #[serde(default)]
    instance: String,
    /// The uuids the browser had after the last `finish`: one missing from
    /// the browser now was deleted here.
    #[serde(default)]
    had: HashSet<String>,
    /// `[name, icon, color]` the last `finish` gave the browser, by uuid. A
    /// local container that still has them wasn't edited here, even when the
    /// store moved on since: a sync whose ops the browser never applied (it
    /// failed, or the browser quit) downloaded Firefox's edits into the store.
    #[serde(default)]
    synced: HashMap<String, (String, usize, String)>,
}

fn store_path(profile: &str) -> PathBuf {
    Path::new(profile).join(STORE)
}

pub fn remove_store(profile: &str) {
    remove_db(&store_path(profile));
}

/// The add-on's area, as an object.
fn area(store: &WebExtStorageStore) -> Result<Map<String, Value>> {
    match store.get(EXT_ID, Value::Null)? {
        Value::Object(map) => Ok(map),
        _ => Ok(Map::new()),
    }
}

fn deleted_list(area: &Map<String, Value>) -> Vec<String> {
    area.get(DELETED)
        .and_then(Value::as_array)
        .map(|l| l.iter().filter_map(|v| v.as_str().map(String::from)).collect())
        .unwrap_or_default()
}

/// The add-on's identities by uuid.
fn identities(area: &Map<String, Value>) -> HashMap<String, &Map<String, Value>> {
    area.iter()
        .filter_map(|(key, value)| {
            let uuid = key.strip_prefix(IDENTITY)?;
            Some((uuid.to_string(), value.as_object()?))
        })
        .collect()
}

pub struct ContainersEngine {
    store: Arc<WebExtStorageStore>,
    bridged: Arc<WebExtStorageBridgedEngine>,
    local: Vec<Local>,
    state: RefCell<State>,
}

impl ContainersEngine {
    /// From the browser adapter's `{state, local}`: writes the local changes
    /// to the store, ready to sync.
    pub fn new(profile: &str, args: &Value) -> Result<Self> {
        let local: Vec<Local> =
            serde_json::from_value(args["local"].clone()).context("bad containers")?;
        let mut state: State = match args["state"].as_str() {
            Some(s) if !s.is_empty() => serde_json::from_str(s).context("bad engine state")?,
            _ => State::default(),
        };
        if state.instance.is_empty() {
            state.instance = format!("MACinstance:oganesson-{}", Guid::random());
        }
        let store = Arc::new(WebExtStorageStore::new(store_path(profile))?);
        let bridged = store.clone().bridged_engine();
        // Nothing goes up before Firefox's containers came down once, so a
        // container Firefox has under the same name isn't uploaded twice.
        if bridged.last_sync()? > 0 {
            write_local_changes(&store, &local, &mut state, now_ms())?;
        }
        Ok(Self { store, bridged, local, state: RefCell::new(state) })
    }
}

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_millis() as i64)
}

/// Local additions, edits and deletions since the store last matched the
/// browser (the previous `finish`, whose ops the browser applied).
fn write_local_changes(
    store: &WebExtStorageStore,
    local: &[Local],
    state: &mut State,
    now: i64,
) -> Result<()> {
    let area = area(store)?;
    let remote = identities(&area);
    let mut deleted = deleted_list(&area);
    let mut set = Map::new();
    let mut remove = vec![];

    let mut local_uuids = HashSet::new();
    for container in local {
        let uuid = state.uuids.get(&container.id).cloned().unwrap_or(container.id.clone());
        if deleted.contains(&uuid) {
            continue; // Deleted in Firefox; `finish` deletes it here.
        }
        local_uuids.insert(uuid.clone());
        let current = remote.get(&uuid);
        let field = |name: &str| current.and_then(|c| c.get(name)).and_then(Value::as_str);
        let record = json!({
            "name": container.name,
            "icon": icon_for_sync(container.icon, field("icon")),
            "color": color_for_sync(&container.color, field("color")),
            "macAddonUUID": uuid,
        });
        let edited = state.synced.get(&uuid).is_none_or(|(name, icon, color)| {
            *name != container.name
                || *icon != container.icon
                || !color.eq_ignore_ascii_case(&container.color)
        });
        let changed = edited
            && match current {
                None => true,
                Some(c) => ["name", "icon", "color"]
                    .iter()
                    .any(|f| c.get(*f) != record.get(*f)),
            };
        if changed {
            // Keep what the add-on stored that oganesson doesn't have.
            let mut merged = current.map(|c| (*c).clone()).unwrap_or_default();
            merged.extend(record.as_object().unwrap().clone());
            set.insert(format!("{IDENTITY}{uuid}"), Value::Object(merged));
        }
    }
    // Synced containers the browser no longer has were deleted here.
    for uuid in remote.keys() {
        if state.had.contains(uuid) && !local_uuids.contains(uuid) {
            remove.push(format!("{IDENTITY}{uuid}"));
            if !deleted.contains(uuid) {
                deleted.push(uuid.clone());
            }
            state.uuids.retain(|_, v| v != uuid);
        }
    }
    if deleted != deleted_list(&area) {
        set.insert(DELETED.into(), json!(deleted));
    }

    let mut uuids: Vec<&String> = local_uuids.iter().collect();
    uuids.sort();
    let instance = area.get(&state.instance);
    let listed = instance.map(|i| i["identities"].clone()).unwrap_or(Value::Null);
    let stamp = instance.and_then(|i| i["timestamp"].as_i64()).unwrap_or(0);
    if listed != json!(uuids) || now - stamp > INSTANCE_REFRESH_MS {
        set.insert(
            state.instance.clone(),
            json!({ "timestamp": now, "identities": uuids, "siteAssignments": [] }),
        );
    }
    if !set.is_empty() {
        store.set(EXT_ID, Value::Object(set))?;
    }
    if !remove.is_empty() {
        store.remove(EXT_ID, json!(remove))?;
    }
    Ok(())
}

/// The ops that make the browser's containers match the store.
fn ops(store: &WebExtStorageStore, local: &[Local], state: &mut State) -> Result<(Vec<Value>, usize)> {
    let area = area(store)?;
    let remote = identities(&area);
    let deleted = deleted_list(&area);
    let uuid_of = |state: &State, id: &str| state.uuids.get(id).cloned().unwrap_or(id.into());
    let mut ops = vec![];
    let mut synced = HashMap::new();
    let mut taken: HashSet<String> = HashSet::new();
    let mut uuids: Vec<&String> = remote.keys().collect();
    uuids.sort();
    for uuid in uuids {
        if deleted.contains(uuid) {
            continue;
        }
        let record = remote[uuid];
        let name = record.get("name").and_then(Value::as_str).unwrap_or_default();
        // As the add-on does: the same uuid, else the same name.
        let id = match local.iter().find(|c| uuid_of(state, &c.id) == *uuid) {
            Some(c) => c.id.clone(),
            None => match local.iter().find(|c| {
                c.name == name
                    && !taken.contains(&c.id)
                    && !remote.contains_key(&uuid_of(state, &c.id))
            }) {
                Some(c) => {
                    state.uuids.insert(c.id.clone(), uuid.clone());
                    c.id.clone()
                }
                None => uuid.clone(),
            },
        };
        taken.insert(id.clone());
        let field = |f: &str| record.get(f).and_then(Value::as_str).unwrap_or_default();
        let (icon, color) = (icon_from_sync(field("icon")), color_from_sync(field("color")));
        synced.insert(uuid.clone(), (name.to_string(), icon, color.clone()));
        ops.push(json!({
            "op": "upsert",
            "id": id,
            "name": name,
            "icon": icon,
            "color": color,
        }));
    }
    for container in local {
        if deleted.contains(&uuid_of(state, &container.id)) {
            ops.push(json!({ "op": "delete", "id": container.id }));
            state.uuids.remove(&container.id);
        }
    }
    // After the ops, the browser has the synced containers and the local ones
    // this install uploaded.
    state.had = remote.keys().filter(|u| !deleted.contains(*u)).cloned().collect();
    state.synced = synced;
    let count = state.had.len();
    Ok((ops, count))
}

impl Collection for ContainersEngine {
    fn finish(self: Box<Self>) -> Result<Value> {
        let mut state = self.state.into_inner();
        let (ops, count) = ops(&self.store, &self.local, &mut state)?;
        Ok(json!({
            "state": serde_json::to_string(&state)?,
            "ops": ops,
            "count": count,
        }))
    }
}

/// webext-storage's engine through its bridged API (its `SyncEngine` isn't
/// public), plus the global sync ID it leaves to the caller.
impl SyncEngine for ContainersEngine {
    fn collection_name(&self) -> CollectionName {
        COLLECTION.into()
    }

    fn sync_started(&self) -> Result<()> {
        self.bridged.sync_started()
    }

    fn stage_incoming(&self, inbound: Vec<IncomingBso>, _telem: &mut telemetry::Engine) -> Result<()> {
        // The bridged API takes BSOs as the server's JSON.
        let inbound = inbound
            .into_iter()
            .map(|bso| {
                json!({
                    "id": bso.envelope.id,
                    "modified": bso.envelope.modified.as_millis() as f64 / 1000.0,
                    "sortindex": bso.envelope.sortindex,
                    "ttl": bso.envelope.ttl,
                    "payload": bso.payload,
                })
                .to_string()
            })
            .collect();
        self.bridged.store_incoming(inbound)
    }

    fn apply(&self, timestamp: ServerTimestamp, _telem: &mut telemetry::Engine) -> Result<Vec<OutgoingBso>> {
        self.bridged
            .apply(timestamp.as_millis())?
            .iter()
            .map(|s| {
                let bso: Value = serde_json::from_str(s)?;
                Ok(OutgoingBso {
                    envelope: OutgoingEnvelope {
                        id: bso["id"].as_str().unwrap_or_default().into(),
                        sortindex: bso["sortindex"].as_i64().map(|i| i as i32),
                        ttl: bso["ttl"].as_u64().map(|t| t as u32),
                    },
                    payload: bso["payload"].as_str().unwrap_or_default().into(),
                })
            })
            .collect()
    }

    fn set_uploaded(&self, new_timestamp: ServerTimestamp, ids: Vec<Guid>) -> Result<()> {
        self.bridged
            .set_uploaded(new_timestamp.as_millis(), ids.into_iter().map(|g| g.to_string()).collect())
    }

    fn sync_finished(&self) -> Result<()> {
        self.bridged.sync_finished()
    }

    fn get_collection_request(&self, server: ServerTimestamp) -> Result<Option<CollectionRequest>> {
        let since = ServerTimestamp::from_millis(self.bridged.last_sync()?);
        Ok((since != server).then(|| CollectionRequest::new(COLLECTION.into()).full().newer_than(since)))
    }

    fn get_sync_assoc(&self) -> Result<EngineSyncAssociation> {
        let global = self.state.borrow().global.clone();
        Ok(match self.bridged.sync_id()? {
            Some(coll) if !global.is_empty() => EngineSyncAssociation::Connected(CollSyncIds {
                global: global.into(),
                coll: coll.into(),
            }),
            _ => EngineSyncAssociation::Disconnected,
        })
    }

    fn reset(&self, assoc: &EngineSyncAssociation) -> Result<()> {
        self.bridged.reset()?;
        let mut state = self.state.borrow_mut();
        match assoc {
            EngineSyncAssociation::Disconnected => state.global.clear(),
            EngineSyncAssociation::Connected(ids) => {
                self.bridged.ensure_current_sync_id(ids.coll.as_str())?;
                state.global = ids.global.to_string();
            }
        }
        Ok(())
    }

    fn wipe(&self) -> Result<()> {
        self.bridged.wipe()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn container(id: &str, name: &str, icon: usize, color: &str) -> Local {
        Local { id: id.into(), name: name.into(), icon, color: color.into() }
    }

    #[test]
    fn mappings_keep_what_firefox_had() {
        assert_eq!(icon_for_sync(1, None), "briefcase");
        // "food" has no counterpart: the star here, and kept as food.
        assert_eq!(icon_from_sync("food"), STAR);
        assert_eq!(icon_for_sync(STAR, Some("food")), "food");
        assert_eq!(icon_for_sync(0, Some("food")), "fingerprint");
        assert_eq!(color_from_sync("red"), "#ff613d");
        assert_eq!(color_for_sync("#ff613d", Some("red")), "red");
        assert_eq!(color_for_sync("#ee5544", None), "red");
        assert_eq!(color_for_sync("#3070ff", Some("purple")), "blue");
    }

    #[test]
    fn local_changes_and_firefox_changes() -> Result<()> {
        let dir = std::env::temp_dir().join(format!("og-containers-{}", std::process::id()));
        std::fs::create_dir_all(&dir)?;
        let store = WebExtStorageStore::new(dir.join(STORE))?;
        let mut state = State { instance: "MACinstance:test".into(), ..Default::default() };

        // Firefox has Work (uuid W) and Food (uuid F); oganesson has Work
        // (id w) and Personal (id p).
        store.set(EXT_ID, json!({
            "identity@@_W": {"name": "Work", "icon": "briefcase", "color": "orange",
                             "macAddonUUID": "W", "cookieStoreId": "firefox-container-2"},
            "identity@@_F": {"name": "Food", "icon": "food", "color": "green",
                             "macAddonUUID": "F"},
        }))?;
        let mut local = vec![container("w", "Work", 1, "#ff9f00")];
        // Before a first finish, a local container named like a Firefox one
        // isn't uploaded under its own id yet: `ops` pairs them first.
        let (ops1, count) = ops(&store, &local, &mut state)?;
        assert_eq!(count, 2);
        assert_eq!(state.uuids["w"], "W");
        assert_eq!(ops1[0], json!({"op": "upsert", "id": "F", "name": "Food", "icon": STAR,
                                   "color": "#51cd00"}));
        assert_eq!(ops1[1]["id"], "w");

        // The browser applies them and adds Personal.
        local.push(container("F", "Food", STAR, "#51cd00"));
        local.push(container("p", "Personal", 0, "#37adff"));
        write_local_changes(&store, &local, &mut state, 1000)?;
        let a = area(&store)?;
        assert_eq!(a["identity@@_p"], json!({"name": "Personal", "icon": "fingerprint",
                                             "color": "blue", "macAddonUUID": "p"}));
        // Food keeps its Firefox icon; Work keeps the add-on's own fields.
        assert_eq!(a["identity@@_F"]["icon"], "food");
        assert_eq!(a["identity@@_W"]["cookieStoreId"], "firefox-container-2");
        assert_eq!(a["MACinstance:test"]["identities"], json!(["F", "W", "p"]));

        // A local edit and a local deletion.
        local[0].name = "Job".into();
        local.retain(|c| c.id != "F");
        write_local_changes(&store, &local, &mut state, 2000)?;
        let a = area(&store)?;
        assert_eq!(a["identity@@_W"]["name"], "Job");
        assert!(!a.contains_key("identity@@_F"));
        assert_eq!(a[DELETED], json!(["F"]));

        // Firefox deletes Personal: the browser gets a delete.
        store.set(EXT_ID, json!({ DELETED: ["F", "p"] }))?;
        store.remove(EXT_ID, json!(["identity@@_p"]))?;
        let (ops3, _) = ops(&store, &local, &mut state)?;
        assert!(ops3.contains(&json!({"op": "delete", "id": "p"})), "{ops3:?}");
        drop(store);
        remove_db(&dir.join(STORE));
        Ok(())
    }
}
