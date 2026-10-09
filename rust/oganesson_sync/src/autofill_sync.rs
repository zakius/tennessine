// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The `addresses` and `creditcards` collections, over application-services'
//! autofill store and its sync engines. As for bookmarks (bookmarks.rs), the
//! browser's data stays the source of truth and the store, one database per
//! collection in the profile folder, stages it: each sync the browser sends
//! all its records, the difference from the store goes into the store as
//! local changes, and after the merge `finish` returns what changed as ops.
//!
//! A record is `{id, <field>: value}` with Firefox's field names; `id` is the
//! browser's GUID, a UUID. Firefox's GUIDs are 12 characters. The state maps
//! them to the browser's; a browser record not in it yet uses the first 12 hex
//! digits of its UUID, and a Firefox record gets a new UUID.
//!
//! Card numbers are only in memory in the clear: the store encrypts them with
//! a key kept in the state, which the browser stores encrypted.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{bail, Context, Result};
use autofill::db::models::address::{
    AddressMeta, UpdatableAddressFields, UpdatableAddressFieldsWithMeta,
};
use autofill::db::models::credit_card::{
    CreditCardMeta, UpdatableCreditCardFields, UpdatableCreditCardFieldsWithMeta,
};
use autofill::db::store::Store;
use autofill::db::AutofillDb;
use autofill::encryption::EncryptorDecryptor;
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};
use sync15::bso::{IncomingBso, OutgoingBso};
use sync15::engine::{CollectionRequest, EngineSyncAssociation, SyncEngine};
use sync15::{telemetry, CollectionName, Guid, ServerTimestamp};

/// See passwords.rs: a snapshot that lost the records must not empty the
/// user's Firefox.
const MASS_DELETE_MIN: usize = 10;

#[derive(Clone, Copy)]
pub enum Kind {
    Addresses,
    CreditCards,
}

impl Kind {
    fn name(self) -> &'static str {
        match self {
            Kind::Addresses => "addresses",
            Kind::CreditCards => "creditcards",
        }
    }

    fn table(self) -> &'static str {
        match self {
            Kind::Addresses => "addresses_data",
            Kind::CreditCards => "credit_cards_data",
        }
    }

    fn fields(self) -> &'static [&'static str] {
        match self {
            Kind::Addresses => &[
                "name", "organization", "street-address", "address-level3", "address-level2",
                "address-level1", "postal-code", "country", "tel", "email",
            ],
            Kind::CreditCards => {
                &["cc-name", "cc-number", "cc-exp-month", "cc-exp-year", "cc-type"]
            }
        }
    }
}

/// Persisted by the browser (encrypted): the generation of the store it last
/// applied (see bookmarks.rs), the browser GUID of each Firefox GUID, and for
/// cards the key the store encrypts their numbers with.
#[derive(Serialize, Deserialize, Default)]
struct State {
    #[serde(default)]
    gen: String,
    #[serde(default)]
    ids: HashMap<String, String>,
    #[serde(default)]
    key: String,
}

fn store_path(profile: &str, kind: Kind) -> PathBuf {
    Path::new(profile).join(format!("firefox_sync_{}.db", kind.name()))
}

/// Deletes the profile's autofill stores (on sign-out).
pub fn remove_stores(profile: &str) {
    for kind in [Kind::Addresses, Kind::CreditCards] {
        crate::remove_db(&store_path(profile, kind));
    }
}

fn get_gen(db: &AutofillDb) -> String {
    db.query_row("SELECT value FROM moz_meta WHERE key = 'oganesson_gen'", [], |r| r.get(0))
        .unwrap_or_default()
}

fn set_gen(db: &AutofillDb, gen: &str) -> Result<()> {
    db.execute("INSERT OR REPLACE INTO moz_meta (key, value) VALUES ('oganesson_gen', ?1)", [gen])?;
    Ok(())
}

/// Row IDs by GUID. Merging can change a record's GUID (a local record that
/// duplicates a Firefox one, or a conflict forking it), never its row.
fn row_ids(db: &AutofillDb, kind: Kind) -> Result<HashMap<String, i64>> {
    let mut statement = db.prepare(&format!("SELECT guid, rowid FROM {}", kind.table()))?;
    let rows = statement.query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?;
    let ids = rows.collect::<Result<HashMap<String, i64>, _>>()?;
    Ok(ids)
}

/// A record's fields, each present, typed as the store has them.
fn normalize(kind: Kind, record: &Value) -> Value {
    let fields: Map<String, Value> = kind
        .fields()
        .iter()
        .map(|&f| {
            let value = match f {
                "cc-exp-month" | "cc-exp-year" => {
                    json!(record[f].as_f64().unwrap_or_default() as i64)
                }
                _ => json!(record[f].as_str().unwrap_or_default()),
            };
            (f.to_string(), value)
        })
        .collect();
    Value::Object(fields)
}

fn address(fields: &Value) -> UpdatableAddressFields {
    let f = |name: &str| fields[name].as_str().unwrap_or_default().to_string();
    UpdatableAddressFields {
        name: f("name"),
        organization: f("organization"),
        street_address: f("street-address"),
        address_level3: f("address-level3"),
        address_level2: f("address-level2"),
        address_level1: f("address-level1"),
        postal_code: f("postal-code"),
        country: f("country"),
        tel: f("tel"),
        email: f("email"),
    }
}

fn card(fields: &Value, key: &str) -> Result<UpdatableCreditCardFields> {
    let number = fields["cc-number"].as_str().unwrap_or_default();
    let chars: Vec<char> = number.chars().collect();
    Ok(UpdatableCreditCardFields {
        cc_name: fields["cc-name"].as_str().unwrap_or_default().into(),
        cc_number_enc: EncryptorDecryptor::new(key)?.encrypt(number)?,
        cc_number_last_4: chars[chars.len().saturating_sub(4)..].iter().collect(),
        cc_exp_month: fields["cc-exp-month"].as_i64().unwrap_or_default(),
        cc_exp_year: fields["cc-exp-year"].as_i64().unwrap_or_default(),
        cc_type: fields["cc-type"].as_str().unwrap_or_default().into(),
    })
}

/// The store's records: fields by Firefox GUID.
fn records(store: &Store, kind: Kind, key: &str) -> Result<HashMap<String, Value>> {
    Ok(match kind {
        Kind::Addresses => store
            .get_all_addresses()?
            .into_iter()
            .map(|a| {
                let fields = json!({
                    "name": a.name, "organization": a.organization,
                    "street-address": a.street_address, "address-level3": a.address_level3,
                    "address-level2": a.address_level2, "address-level1": a.address_level1,
                    "postal-code": a.postal_code, "country": a.country, "tel": a.tel,
                    "email": a.email,
                });
                (a.guid, fields)
            })
            .collect(),
        Kind::CreditCards => {
            let crypt = EncryptorDecryptor::new(key)?;
            let mut cards = HashMap::new();
            for c in store.get_all_credit_cards()? {
                let fields = json!({
                    "cc-name": c.cc_name, "cc-number": crypt.decrypt(&c.cc_number_enc)?,
                    "cc-exp-month": c.cc_exp_month, "cc-exp-year": c.cc_exp_year,
                    "cc-type": c.cc_type,
                });
                cards.insert(c.guid, fields);
            }
            cards
        }
    })
}

/// Adds a local record, as changed so it uploads.
fn add(store: &Store, kind: Kind, key: &str, guid: &str, fields: &Value) -> Result<()> {
    let now = types::Timestamp::now().as_millis_i64();
    match kind {
        Kind::Addresses => {
            store.add_address_with_meta(UpdatableAddressFieldsWithMeta {
                fields: address(fields),
                meta: AddressMeta {
                    guid: guid.into(),
                    time_created: now,
                    time_last_modified: now,
                    sync_change_counter: 1,
                    ..Default::default()
                },
            })?;
        }
        Kind::CreditCards => {
            store.add_credit_card_with_meta(UpdatableCreditCardFieldsWithMeta {
                fields: card(fields, key)?,
                meta: CreditCardMeta {
                    guid: guid.into(),
                    time_created: now,
                    time_last_modified: now,
                    sync_change_counter: 1,
                    ..Default::default()
                },
            })?;
        }
    }
    Ok(())
}

fn update(store: &Store, kind: Kind, key: &str, guid: &str, fields: &Value) -> Result<()> {
    match kind {
        Kind::Addresses => store.update_address(guid.into(), address(fields))?,
        Kind::CreditCards => store.update_credit_card(guid.into(), card(fields, key)?)?,
    }
    Ok(())
}

fn delete(store: &Store, kind: Kind, guid: &str) -> Result<()> {
    match kind {
        Kind::Addresses => store.delete_address(guid.into())?,
        Kind::CreditCards => store.delete_credit_card(guid.into())?,
    };
    Ok(())
}

/// A new browser GUID: a random (version 4) UUID.
fn new_uuid() -> Result<String> {
    let mut b = [0u8; 16];
    rc_crypto::rand::fill(&mut b)?;
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    let hex: String = b.iter().map(|x| format!("{x:02x}")).collect();
    Ok(format!("{}-{}-{}-{}-{}", &hex[..8], &hex[8..12], &hex[12..16], &hex[16..20], &hex[20..]))
}

pub struct AutofillEngine {
    kind: Kind,
    key: String,
    engine: Box<dyn SyncEngine>,
    store: Arc<Store>,
    /// A second connection, for what the store's API doesn't offer.
    db: AutofillDb,
    /// The snapshot: browser GUID and fields by Firefox GUID.
    local: HashMap<String, (String, Value)>,
    /// Browser GUIDs by row, to follow GUID changes.
    rows: HashMap<i64, String>,
    gen: RefCell<String>,
}

impl AutofillEngine {
    /// From the browser adapter's `{state, local}`: writes the local changes
    /// to the store, ready to sync.
    pub fn new(kind: Kind, profile: &str, args: &Value) -> Result<Self> {
        let mut state: State = match args["state"].as_str() {
            Some(s) if !s.is_empty() => serde_json::from_str(s).context("bad engine state")?,
            _ => State::default(),
        };
        let snapshot = args["local"].as_array().context("bad records")?;

        let path = store_path(profile, kind);
        let mut db = AutofillDb::new(&path)?;
        if get_gen(&db) != state.gen || state.key.is_empty() {
            // Starts over, merging the browser's records with the server's.
            // A new key comes with a new store, so it can read every number.
            drop(db);
            crate::remove_db(&path);
            db = AutofillDb::new(&path)?;
            if state.key.is_empty() {
                state.key = EncryptorDecryptor::create_key()?;
            }
        }
        let store = Arc::new(Store::new(&path)?);
        let key = state.key.clone();

        let guids: HashMap<&str, &str> =
            state.ids.iter().map(|(guid, id)| (id.as_str(), guid.as_str())).collect();
        let mut local = HashMap::new();
        for record in snapshot {
            let Some(id) = record["id"].as_str() else { continue };
            let guid = match guids.get(id) {
                Some(guid) => guid.to_string(),
                None => id.replace('-', "").chars().take(12).collect(),
            };
            if !local.contains_key(&guid) {
                local.insert(guid, (id.to_string(), normalize(kind, record)));
            }
        }

        let before = records(&store, kind, &key)?;
        let deletions: Vec<&String> = before.keys().filter(|g| !local.contains_key(*g)).collect();
        if deletions.len() > MASS_DELETE_MIN && deletions.len() * 2 > before.len() {
            bail!(
                "refusing to delete {} of {} synced {} from the server",
                deletions.len(),
                before.len(),
                kind.name()
            );
        }
        for (guid, (_, fields)) in &local {
            match before.get(guid) {
                Some(have) if have == fields => {}
                Some(_) => update(&store, kind, &key, guid, fields)?,
                None => add(&store, kind, &key, guid, fields)?,
            }
        }
        for guid in deletions {
            delete(&store, kind, guid)?;
        }

        let rows = row_ids(&db, kind)?
            .into_iter()
            .filter_map(|(guid, row)| Some((row, local.get(&guid)?.0.clone())))
            .collect();
        let engine = match kind {
            Kind::Addresses => store.clone().create_addresses_sync_engine(),
            Kind::CreditCards => {
                let mut engine = store.clone().create_credit_cards_sync_engine();
                engine.set_local_encryption_key(&key)?;
                engine
            }
        };
        Ok(Self { kind, key, engine, store, db, local, rows, gen: RefCell::new(state.gen) })
    }
}

impl crate::Collection for AutofillEngine {
    /// Ops: `{op: "upsert", id, <field>: value}` for each record the browser
    /// lacks or has with other fields, and `{op: "delete", id}` for snapshot
    /// records the store no longer has.
    fn finish(self: Box<Self>) -> Result<Value> {
        let merged = records(&self.store, self.kind, &self.key)?;
        let rows = row_ids(&self.db, self.kind)?;
        let had: HashMap<&str, &Value> =
            self.local.values().map(|(id, fields)| (id.as_str(), fields)).collect();
        let mut ops = vec![];
        let mut ids = HashMap::new();
        for (guid, mut fields) in merged {
            let id = match rows.get(&guid).and_then(|row| self.rows.get(row)) {
                Some(id) => id.clone(),
                None => new_uuid()?,
            };
            if had.get(id.as_str()) != Some(&&fields) {
                fields["op"] = json!("upsert");
                fields["id"] = json!(id);
                ops.push(fields);
            }
            ids.insert(guid, id);
        }
        let kept: HashSet<&String> = ids.values().collect();
        for (id, _) in self.local.values() {
            if !kept.contains(id) {
                ops.push(json!({ "op": "delete", "id": id }));
            }
        }
        let count = ids.len();
        let state = State { gen: self.gen.take(), ids, key: self.key.clone() };
        Ok(json!({ "state": serde_json::to_string(&state)?, "ops": ops, "count": count }))
    }
}

impl SyncEngine for AutofillEngine {
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

    fn sync(kind: Kind, profile: &str, state: &str, local: Value) -> Value {
        let args = json!({ "state": state, "local": local });
        let engine = AutofillEngine::new(kind, profile, &args).unwrap();
        Box::new(engine).finish().unwrap()
    }

    #[test]
    fn local_changes_reach_the_store() {
        nss::ensure_initialized();
        let dir = std::env::temp_dir().join(format!("og-autofill-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let profile = dir.to_str().unwrap();
        remove_stores(profile);

        let id = "0123abcd-4567-4890-8abc-def012345678";
        let ann = json!({"id": id, "name": "Ann", "country": "US"});
        let first = sync(Kind::Addresses, profile, "", json!([ann]));
        assert_eq!(first["ops"], json!([]));
        assert_eq!(first["count"], 1);
        let state: State = serde_json::from_str(first["state"].as_str().unwrap()).unwrap();
        assert_eq!(state.ids["0123abcd4567"], id);

        // An edit stays in the store; a deletion leaves it empty.
        let edited = json!([{"id": id, "name": "Anna", "country": "US"}]);
        let second = sync(Kind::Addresses, profile, first["state"].as_str().unwrap(), edited);
        assert_eq!((second["ops"].clone(), second["count"].clone()), (json!([]), json!(1)));
        let third = sync(Kind::Addresses, profile, second["state"].as_str().unwrap(), json!([]));
        assert_eq!(third["count"], 0);

        // A generation the store doesn't have starts over: no deletions.
        let fourth = sync(Kind::Addresses, profile, r#"{"gen":"elsewhere"}"#, json!([]));
        assert_eq!(fourth["count"], 0);
        remove_stores(profile);
    }

    #[test]
    fn card_numbers_are_encrypted_at_rest() {
        nss::ensure_initialized();
        let dir = std::env::temp_dir().join(format!("og-cards-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let profile = dir.to_str().unwrap();
        remove_stores(profile);

        let number = "4111111111111111";
        let card = json!([{"id": "0123abcd-4567-4890-8abc-def012345678", "cc-name": "Ann",
            "cc-number": number, "cc-exp-month": 12.0, "cc-exp-year": 2030.0, "cc-type": "visa"}]);
        let first = sync(Kind::CreditCards, profile, "", card.clone());
        assert_eq!((first["ops"].clone(), first["count"].clone()), (json!([]), json!(1)));
        let path = store_path(profile, Kind::CreditCards).into_os_string();
        for suffix in ["", "-wal"] {
            let mut file = path.clone();
            file.push(suffix);
            let bytes = std::fs::read(file).unwrap_or_default();
            assert!(!bytes.windows(number.len()).any(|w| w == number.as_bytes()));
        }

        // The state's key reads the number back: the card is unchanged.
        let second = sync(Kind::CreditCards, profile, first["state"].as_str().unwrap(), card);
        assert_eq!(second["ops"], json!([]));
        remove_stores(profile);
    }
}
