// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The `passwords` collection, over application-services' logins store and
//! its sync engine. As for addresses (autofill_sync.rs), the browser's
//! password store stays the source of truth and the logins store, a database
//! in the profile folder, stages it: each sync the browser sends all its
//! logins (in Firefox's record shape), the difference from the store goes
//! into the store as local changes, and after the merge `finish` returns
//! what changed as ops.
//!
//! A login is identified by Firefox's duplicate key (hostname, httpRealm,
//! username) on both sides; the browser has no GUIDs for logins. Merging and
//! conflicts are the logins engine's, as in Firefox.
//!
//! Passwords are only in memory in the clear: the store encrypts them with a
//! key kept in the state, which the browser stores encrypted.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{bail, Context, Result};
use logins::{LoginEntry, LoginEntryWithMeta, LoginMeta, LoginStore, LoginsApiError, ValidateAndFixup};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sync15::bso::{IncomingBso, OutgoingBso};
use sync15::engine::{CollectionRequest, EngineSyncAssociation, SyncEngine};
use sync15::{telemetry, CollectionName, Guid, ServerTimestamp};

/// A Firefox `passwords` record, as the browser sends and applies it.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Login {
    pub hostname: String,
    #[serde(rename = "formSubmitURL", default)]
    pub form_submit_url: Option<String>,
    #[serde(default)]
    pub http_realm: Option<String>,
    #[serde(default)]
    pub username: String,
    pub password: String,
    #[serde(default)]
    pub username_field: String,
    #[serde(default)]
    pub password_field: String,
    #[serde(default, deserialize_with = "number")]
    pub time_created: i64,
    #[serde(default, deserialize_with = "number")]
    pub time_password_changed: i64,
    #[serde(default, deserialize_with = "number")]
    pub time_last_used: i64,
    #[serde(default, deserialize_with = "number")]
    pub times_used: i64,
}

/// Accepts any JSON number (the browser writes 64-bit times as doubles) or null.
fn number<'de, D: serde::Deserializer<'de>>(d: D) -> Result<i64, D::Error> {
    Ok(Option::<f64>::deserialize(d)?.unwrap_or(0.0) as i64)
}

/// What identifies a login on both sides, as in Firefox's duplicate check.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq, Hash)]
#[serde(rename_all = "camelCase")]
pub struct Key {
    pub hostname: String,
    pub http_realm: Option<String>,
    pub username: String,
}

impl Login {
    pub fn key(&self) -> Key {
        Key {
            hostname: self.hostname.clone(),
            http_realm: self.http_realm.clone(),
            username: self.username.clone(),
        }
    }

    /// The store's form of it; `None` if the store rejects it.
    fn entry(&self) -> Option<LoginEntry> {
        LoginEntry {
            origin: self.hostname.clone(),
            form_action_origin: self.form_submit_url.clone(),
            http_realm: self.http_realm.clone(),
            username_field: self.username_field.clone(),
            password_field: self.password_field.clone(),
            username: self.username.clone(),
            password: self.password.clone(),
        }
        .fixup()
        .ok()
    }

    fn from_store(l: &logins::Login) -> Self {
        Login {
            hostname: l.origin.clone(),
            form_submit_url: l.form_action_origin.clone(),
            http_realm: l.http_realm.clone(),
            username: l.username.clone(),
            password: l.password.clone(),
            username_field: l.username_field.clone(),
            password_field: l.password_field.clone(),
            time_created: l.time_created,
            time_password_changed: l.time_password_changed,
            time_last_used: l.time_last_used,
            times_used: l.times_used,
        }
    }

    /// Whether the browser can store it: a web origin, a password, and
    /// exactly one of formSubmitURL (a form login) and httpRealm (HTTP auth).
    fn usable(&self) -> bool {
        let web = url::Url::parse(&self.hostname)
            .map(|u| matches!(u.scheme(), "http" | "https"))
            .unwrap_or(false);
        web && !self.password.is_empty()
            && self.form_submit_url.is_some() != self.http_realm.is_some()
    }
}

/// A change for the browser to make to its password store.
#[derive(Serialize, Debug, PartialEq)]
#[serde(tag = "op", rename_all = "camelCase")]
pub enum Op {
    /// Update the logins with `replaces` (or the login's own key) or add it.
    Upsert { login: Login, replaces: Option<Key> },
    /// Remove the logins with this key.
    Delete { key: Key },
}

/// Persisted by the browser (encrypted): the generation of the store it last
/// applied (see bookmarks.rs) and the key the store encrypts logins with.
#[derive(Serialize, Deserialize, Default)]
struct State {
    #[serde(default)]
    gen: String,
    #[serde(default)]
    key: String,
}

/// Refuse to upload more deletions than this in one sync when they are also
/// most of the synced logins: a snapshot that lost the local logins must not
/// empty the user's Firefox.
const MASS_DELETE_MIN: usize = 10;

fn store_path(profile: &str) -> PathBuf {
    Path::new(profile).join("firefox_sync_passwords.db")
}

/// Deletes the profile's logins store (on sign-out).
pub fn remove_store(profile: &str) {
    crate::remove_db(&store_path(profile));
}

fn open(path: &Path, key: &str) -> Result<Arc<LoginStore>> {
    let encdec = logins::create_managed_encdec(logins::create_static_key_manager(key.into()));
    Ok(Arc::new(LoginStore::new(path, encdec)?))
}

fn get_gen(store: &LoginStore) -> Result<String> {
    Ok(store
        .lock_db()?
        .query_row("SELECT value FROM loginsSyncMeta WHERE key = 'oganesson_gen'", [], |r| r.get(0))
        .unwrap_or_default())
}

/// The store's logins by key, one each: the most recently changed when
/// several share a key (same site and user, different form actions).
fn by_key(store: &LoginStore) -> Result<HashMap<Key, logins::Login>> {
    let mut logins: HashMap<Key, logins::Login> = HashMap::new();
    for l in store.list()? {
        let key = Login::from_store(&l).key();
        let newer = logins.get(&key).is_none_or(|have| {
            (l.time_password_changed, l.time_created) > (have.time_password_changed, have.time_created)
        });
        if newer {
            logins.insert(key, l);
        }
    }
    Ok(logins)
}

/// Skips a login the store rejects; it stays local.
fn skip_invalid<T>(result: logins::ApiResult<T>) -> Result<()> {
    match result {
        Ok(_) | Err(LoginsApiError::InvalidRecord { .. }) => Ok(()),
        Err(e) => Err(e.into()),
    }
}

pub struct PasswordsEngine {
    key: String,
    engine: Box<dyn SyncEngine>,
    store: Arc<LoginStore>,
    /// The snapshot, one login per key as in `by_key`, in the store's form.
    local: HashMap<Key, LoginEntry>,
    /// The key of each login the store had after the local changes, to
    /// follow logins the merge changes the key of.
    staged: HashMap<String, Key>,
    gen: std::cell::RefCell<String>,
}

impl PasswordsEngine {
    /// From the browser adapter's `{state, local}`: writes the local changes
    /// to the store, ready to sync.
    pub fn new(profile: &str, args: &Value) -> Result<Self> {
        let mut state: State = match args["state"].as_str() {
            Some(s) if !s.is_empty() => serde_json::from_str(s).context("bad engine state")?,
            _ => State::default(),
        };
        let snapshot: Vec<Login> =
            serde_json::from_value(args["local"].clone()).context("bad logins")?;

        let path = store_path(profile);
        if state.key.is_empty() {
            state.key = db_crypto::create_key()?;
            crate::remove_db(&path);
        }
        let mut store = open(&path, &state.key)?;
        if get_gen(&store)? != state.gen {
            // Starts over, merging the browser's logins with the server's.
            drop(store);
            crate::remove_db(&path);
            store = open(&path, &state.key)?;
        }

        let mut newest: HashMap<Key, Login> = HashMap::new();
        for login in snapshot {
            let newer = newest.get(&login.key()).is_none_or(|l| {
                (login.time_password_changed, login.time_created)
                    > (l.time_password_changed, l.time_created)
            });
            if newer {
                newest.insert(login.key(), login);
            }
        }
        let mut local = HashMap::new();
        for (key, login) in &newest {
            if let Some(entry) = login.entry() {
                local.insert(key.clone(), entry);
            }
        }

        let before = by_key(&store)?;
        let synced = before.values().filter(|l| Login::from_store(l).usable()).count();
        // Logins the browser can't store (other schemes, from Firefox) stay.
        let deletions: Vec<&Key> = before
            .iter()
            .filter(|(key, l)| !newest.contains_key(*key) && Login::from_store(l).usable())
            .map(|(key, _)| key)
            .collect();
        if deletions.len() > MASS_DELETE_MIN && deletions.len() * 2 > synced {
            bail!("refusing to delete {} of {} synced logins from the server", deletions.len(), synced);
        }
        for (key, entry) in &local {
            match before.get(key) {
                Some(have) if have.entry() == *entry => {}
                Some(have) => skip_invalid(store.update(&have.id, entry.clone()))?,
                None => {
                    let login = &newest[key];
                    skip_invalid(store.add_with_meta(LoginEntryWithMeta {
                        entry: entry.clone(),
                        meta: LoginMeta {
                            id: Guid::random().to_string(),
                            time_created: login.time_created,
                            time_password_changed: login.time_password_changed,
                            time_last_used: login.time_last_used,
                            times_used: login.times_used,
                            time_last_breach_alert_dismissed: None,
                        },
                    }))?
                }
            }
        }
        // Every login with the key: the ones `by_key` didn't pick too.
        let deleted: HashSet<&Key> = deletions.into_iter().collect();
        for l in store.list()? {
            if deleted.contains(&Login::from_store(&l).key()) {
                store.delete(&l.id)?;
            }
        }

        let staged = store.list()?.iter().map(|l| (l.id.clone(), Login::from_store(l).key())).collect();
        let engine = store.clone().create_logins_sync_engine()?;
        Ok(Self { key: state.key, engine, store, local, staged, gen: std::cell::RefCell::new(state.gen) })
    }
}

impl crate::Collection for PasswordsEngine {
    fn finish(self: Box<Self>) -> Result<Value> {
        let merged: HashMap<Key, logins::Login> = by_key(&self.store)?
            .into_iter()
            .filter(|(_, l)| Login::from_store(l).usable())
            .collect();
        let mut ops = vec![];
        let mut replaced = HashSet::new();
        for (key, l) in &merged {
            if self.local.get(key) == Some(&l.entry()) {
                continue;
            }
            // A login whose key the merge changed (Firefox renamed the user)
            // replaces the browser's login under the old key.
            let replaces = self
                .staged
                .get(&l.id)
                .filter(|old| *old != key && self.local.contains_key(*old) && !merged.contains_key(*old))
                .cloned();
            replaced.extend(replaces.clone());
            ops.push(Op::Upsert { login: Login::from_store(l), replaces });
        }
        let staged: HashSet<&Key> = self.staged.values().collect();
        for key in self.local.keys() {
            if staged.contains(key) && !merged.contains_key(key) && !replaced.contains(key) {
                ops.push(Op::Delete { key: key.clone() });
            }
        }
        let state = State { gen: self.gen.take(), key: self.key.clone() };
        Ok(json!({ "state": serde_json::to_string(&state)?, "ops": ops, "count": merged.len() }))
    }
}

impl SyncEngine for PasswordsEngine {
    fn collection_name(&self) -> CollectionName {
        self.engine.collection_name()
    }

    fn sync_started(&self) -> Result<()> {
        self.engine.sync_started()
    }

    fn stage_incoming(&self, inbound: Vec<IncomingBso>, telem: &mut telemetry::Engine) -> Result<()> {
        self.engine.stage_incoming(inbound, telem)
    }

    fn apply(&self, timestamp: ServerTimestamp, telem: &mut telemetry::Engine) -> Result<Vec<OutgoingBso>> {
        let gen = Guid::random().to_string();
        self.store.lock_db()?.execute(
            "INSERT OR REPLACE INTO loginsSyncMeta (key, value) VALUES ('oganesson_gen', ?1)",
            [&gen],
        )?;
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

    fn login(host: &str, user: &str, pass: &str, changed: i64) -> Login {
        Login {
            hostname: host.into(),
            form_submit_url: Some(host.into()),
            username: user.into(),
            password: pass.into(),
            time_password_changed: changed,
            ..Default::default()
        }
    }

    fn sync(profile: &str, state: &str, local: &[Login]) -> Value {
        let args = json!({ "state": state, "local": local });
        Box::new(PasswordsEngine::new(profile, &args).unwrap()).finish().unwrap()
    }

    fn profile(name: &str) -> String {
        nss::ensure_initialized();
        let dir = std::env::temp_dir().join(format!("og-{name}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let profile = dir.to_str().unwrap().to_string();
        remove_store(&profile);
        profile
    }

    #[test]
    fn local_changes_reach_the_store() {
        let profile = profile("passwords");
        let a = login("https://a.example", "ann", "secret-pa", 10);
        let first = sync(&profile, "", &[a.clone()]);
        assert_eq!((first["ops"].clone(), first["count"].clone()), (json!([]), json!(1)));
        let state = first["state"].as_str().unwrap();

        // Passwords aren't in the clear on disk.
        let path = store_path(&profile).into_os_string();
        for suffix in ["", "-wal"] {
            let mut file = path.clone();
            file.push(suffix);
            let bytes = std::fs::read(file).unwrap_or_default();
            assert!(!bytes.windows(9).any(|w| w == b"secret-pa"));
        }

        // An edit stays in the store; an unchanged snapshot changes nothing.
        let edited = login("https://a.example", "ann", "new", 20);
        let second = sync(&profile, state, &[edited.clone()]);
        assert_eq!((second["ops"].clone(), second["count"].clone()), (json!([]), json!(1)));
        let store = open(&store_path(&profile), &serde_json::from_str::<State>(state).unwrap().key).unwrap();
        assert_eq!(store.list().unwrap()[0].password, "new");
        drop(store);

        // A deletion leaves it empty; a generation the store doesn't have
        // starts over without deleting anything.
        let third = sync(&profile, second["state"].as_str().unwrap(), &[]);
        assert_eq!(third["count"], 0);
        let key = serde_json::from_str::<State>(state).unwrap().key;
        let fourth = sync(&profile, &json!({"gen": "elsewhere", "key": key}).to_string(), &[]);
        assert_eq!(fourth["count"], 0);
        remove_store(&profile);
    }

    #[test]
    fn refuses_mass_deletion() {
        let profile = profile("passwords-mass");
        let many: Vec<Login> = (0..20).map(|i| login(&format!("https://{i}.example"), "u", "p", 1)).collect();
        let first = sync(&profile, "", &many);
        assert_eq!(first["count"], 20);
        let args = json!({ "state": first["state"], "local": [] });
        assert!(PasswordsEngine::new(&profile, &args).is_err());
        remove_store(&profile);
    }

    #[test]
    fn times_accept_doubles_and_null() {
        let l: Login = serde_json::from_value(json!({"hostname": "https://a.example", "password": "p",
            "timeCreated": 1727000000000.0, "timeLastUsed": null})).unwrap();
        assert_eq!((l.time_created, l.time_last_used), (1727000000000, 0));
    }

    #[test]
    fn record_crypto_matches_known_vector() {
        // From application-services' sync15 key_bundle tests.
        nss::ensure_initialized();
        let key = sync15::KeyBundle::from_base64(
            "9K/wLdXdw+nrTtXo4ZpECyHFNr4d7aYHqeg3KW9+m6Q=",
            "MMntEfutgLTc8FlTLQFms8/xMPmCldqPlq/QQXEjx70=",
        )
        .unwrap();
        let ciphertext = concat!(
            "NMsdnRulLwQsVcwxKW9XwaUe7ouJk5Wn80QhbD80l0HEcZGCynh45qIbeYBik0lgcHbK",
            "mlIxTJNwU+OeqipN+/j7MqhjKOGIlvbpiPQQLC6/ffF2vbzL0nzMUuSyvaQzyGGkSYM2",
            "xUFt06aNivoQTvU2GgGmUK6MvadoY38hhW2LCMkoZcNfgCqJ26lO1O0sEO6zHsk3IVz6",
            "vsKiJ2Hq6VCo7hu123wNegmujHWQSGyf8JeudZjKzfi0OFRRvvm4QAKyBWf0MgrW1F8S",
            "FDnVfkq8amCB7NhdwhgLWbN+21NitNwWYknoEWe1m6hmGZDgDT32uxzWxCV8QqqrpH/Z",
            "ggViEr9uMgoy4lYaWqP7G5WKvvechc62aqnsNEYhH26A5QgzmlNyvB+KPFvPsYzxDnSC",
            "jOoRSLx7GG86wT59QZw=",
        );
        let clear = key
            .decrypt(ciphertext, "GX8L37AAb2FZJMzIoXlX8w==",
                     "b1e6c18ac30deb70236bc0d65a46f7a4dce3b8b0e02cf92182b914e3afa5eebc")
            .unwrap();
        assert!(clear.starts_with(r#"{"id":"5qRsgXWRJZXr","histUri":"#));

        // A login payload survives an encrypt/decrypt round trip.
        let l = login("https://a.example", "ann", "pa", 10);
        let (enc, iv, hmac) = key.encrypt_rand_iv(&serde_json::to_string(&l).unwrap()).unwrap();
        let back: Login = serde_json::from_str(&key.decrypt(&enc, &iv, &hmac).unwrap()).unwrap();
        assert_eq!(back, l);
    }
}
