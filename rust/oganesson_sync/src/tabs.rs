use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{anyhow, Result};
use serde_json::{json, Value};
use sync15::engine::{CollectionRequest, EngineSyncAssociation, SyncEngine};
use sync15::{telemetry, ClientData, CollectionName, ServerTimestamp};
use sync_guid::Guid;
use tabs::{RemoteTabRecord, TabsStore};

use crate::{remove_db, Collection};

fn store_path(profile: &str) -> PathBuf {
    Path::new(profile).join("firefox_sync_tabs.db")
}

pub fn remove_store(profile: &str) {
    remove_db(&store_path(profile));
}

// A browser tab, or None for one that stays private (not http or https).
fn local_tab(tab: &Value) -> Result<Option<RemoteTabRecord>> {
    let url = tab["url"]
        .as_str()
        .ok_or_else(|| anyhow!("tab missing url"))?;
    let parsed = url::Url::parse(url)?;
    if !matches!(parsed.scheme(), "http" | "https") {
        return Ok(None);
    }
    Ok(Some(RemoteTabRecord {
        title: tab["title"].as_str().unwrap_or(url).into(),
        url_history: vec![url.into()],
        // Milliseconds; the browser writes them as a JSON double.
        last_used: tab["lastUsed"].as_f64().unwrap_or(0.0) as i64,
        pinned: tab["pinned"].as_bool().unwrap_or(false),
        ..Default::default()
    }))
}

pub struct TabsEngine {
    inner: tabs::TabsEngine,
    store: Arc<TabsStore>,
    count: usize,
}

impl TabsEngine {
    pub fn new(profile: &str, args: &Value) -> Result<Self> {
        let local = args["local"]
            .as_array()
            .ok_or_else(|| anyhow!("missing local tabs"))?;
        let local: Vec<RemoteTabRecord> = local
            .iter()
            .map(local_tab)
            .collect::<Result<Vec<_>>>()?
            .into_iter()
            .flatten()
            .collect();
        let count = local.len();
        let store = Arc::new(TabsStore::new(store_path(profile)));
        store.set_local_tabs(local);
        let inner = tabs::TabsEngine::new(store.clone());
        Ok(Self {
            inner,
            store,
            count,
        })
    }
}

impl Collection for TabsEngine {
    fn finish(self: Box<Self>) -> Result<Value> {
        let devices: Vec<Value> = self
            .store
            .get_all()
            .into_iter()
            .filter_map(|device| {
                let tabs: Vec<Value> = device
                    .remote_tabs
                    .into_iter()
                    .filter_map(|tab| {
                        let url = tab.url_history.into_iter().next()?;
                        let parsed = url::Url::parse(&url).ok()?;
                        if !matches!(parsed.scheme(), "http" | "https") {
                            return None;
                        }
                        let title = if tab.title.is_empty() {
                            &url
                        } else {
                            &tab.title
                        };
                        Some(json!({"url": url, "title": title}))
                    })
                    .collect();
                if tabs.is_empty() {
                    return None;
                }
                Some(json!({"name": device.client_name, "tabs": tabs}))
            })
            .collect();
        Ok(json!({"state": "", "ops": devices, "count": self.count}))
    }
}

impl SyncEngine for TabsEngine {
    fn collection_name(&self) -> CollectionName {
        self.inner.collection_name()
    }
    fn last_sync(&self) -> Result<Option<ServerTimestamp>> {
        self.inner.last_sync()
    }
    fn reset_last_sync(&self) -> Result<()> {
        self.inner.reset_last_sync()
    }
    fn set_clients(&self, clients: &dyn Fn() -> ClientData) -> Result<()> {
        self.inner.set_clients(clients)
    }
    fn get_collection_request(&self, server: ServerTimestamp) -> Result<Option<CollectionRequest>> {
        self.inner.get_collection_request(server)
    }
    fn stage_incoming(
        &self,
        incoming: Vec<sync15::bso::IncomingBso>,
        telem: &mut telemetry::Engine,
    ) -> Result<()> {
        self.inner.stage_incoming(incoming, telem)
    }
    fn apply(
        &self,
        timestamp: ServerTimestamp,
        telem: &mut telemetry::Engine,
    ) -> Result<Vec<sync15::bso::OutgoingBso>> {
        self.inner.apply(timestamp, telem)
    }
    fn set_uploaded(&self, timestamp: ServerTimestamp, ids: Vec<Guid>) -> Result<()> {
        self.inner.set_uploaded(timestamp, ids)
    }
    fn reset(&self, assoc: &EngineSyncAssociation) -> Result<()> {
        self.inner.reset(assoc)
    }
    fn wipe(&self) -> Result<()> {
        self.inner.wipe()
    }
    fn get_sync_assoc(&self) -> Result<EngineSyncAssociation> {
        self.inner.get_sync_assoc()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn local_tabs_exclude_private_schemes() {
        assert!(TabsEngine::new("/tmp", &json!({"local": null})).is_err());
        assert!(TabsEngine::new("/tmp", &json!({"local": [{}]})).is_err());
        let tabs = TabsEngine::new(
            "/tmp",
            &json!({"local": [
                {"url": "https://example.com", "title": "Example"},
                {"url": "file:///private.txt", "title": "Private"},
                {"url": "chrome://settings", "title": "Settings"}
            ]}),
        )
        .unwrap();
        assert_eq!(tabs.count, 1);
    }

    #[test]
    fn last_used_from_a_double() {
        // base::WriteJson writes the browser's double with ".0" (#41).
        for last_used in [json!(1700000000000.0), json!(1700000000000i64)] {
            let tab = local_tab(&json!({"url": "https://tab.example/", "lastUsed": last_used}))
                .unwrap()
                .unwrap();
            assert_eq!(tab.last_used, 1700000000000);
        }
    }
}
