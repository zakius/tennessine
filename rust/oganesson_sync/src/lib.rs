// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Firefox Sync for passwords, bookmarks, history, addresses and payment methods, over Mozilla's application-services crates.
//! The browser loads this library and talks to it through three C functions:
//! `og_init` (once), `og_call` (JSON in, JSON out, blocking; call it off the
//! UI thread) and `og_free`.

mod autofill_sync;
mod bookmarks;
mod containers;
mod history;
mod http;
mod passwords;
mod tabs;

use std::collections::{HashMap, HashSet};
use std::ffi::{c_char, c_void, CStr, CString};
use std::path::Path;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use anyhow::{anyhow, bail, Result};
use fxa_client::{DeviceConfig, FirefoxAccount, FxaConfig, FxaError, FxaEvent, FxaServer, FxaState};
use serde_json::{json, Value};
use sync15::client::{sync_multiple_with_command_processor, MemoryCachedState, ServiceStatus, Sync15StorageClientInit};
use sync15::clients_engine::{Command, CommandProcessor, CommandStatus, Settings};
use sync15::KeyBundle;

use sync15::engine::SyncEngine;

// The Mozilla account setup, all in one place. Firefox desktop's client ID
// (5882386c6d801776) only accepts the WebChannel redirect, which needs
// Firefox's chrome-privileged messaging; this client redirects to a page
// the browser can watch for, and is the one application-services' own
// command-line tools sign in with.
//
// The accounts server is the one the user set in settings, whose
// /.well-known/fxa-client-configuration gives the other servers, as for
// Firefox's identity.fxaccounts.autoconfig.uri. A self-hosted sync storage
// server is a separate setting, the token server, as in Fenix's "Custom Sync
// server"; the account keeps it once signed in. OGANESSON_FXA_SERVER (the
// accounts server's content URL) and OGANESSON_SYNC_TOKEN_SERVER win over
// it, for tools/fake_firefox_sync.py.
const CLIENT_ID: &str = "3c49430b43dfba77";
const REDIRECT_URI: &str = "https://accounts.firefox.com/oauth/success/3c49430b43dfba77";

const SYNC_SCOPE: &str = "https://identity.mozilla.com/apps/oldsync";
const PROFILE_SCOPE: &str = "profile";
const DEVICE_NAME: &str = "oganesson";

fn config(server: Option<&str>, token_server: Option<&str>) -> Result<FxaConfig> {
    let env = |name| std::env::var(name).ok().filter(|v: &String| !v.is_empty());
    let server = match (env("OGANESSON_FXA_SERVER"), server.filter(|s| !s.is_empty())) {
        (Some(url), _) => FxaServer::Custom { url },
        (None, Some(url)) => FxaServer::from(&url::Url::parse(url)?),
        (None, None) => bail!("no sync server set"),
    };
    Ok(FxaConfig {
        server,
        client_id: CLIENT_ID.into(),
        redirect_uri: REDIRECT_URI.into(),
        token_server_url_override: env("OGANESSON_SYNC_TOKEN_SERVER")
            .or_else(|| token_server.filter(|s| !s.is_empty()).map(String::from)),
    })
}

/// Accounts by browser profile, kept between calls: an OAuth flow in
/// progress only lives in memory.
static ACCOUNTS: Mutex<Option<HashMap<String, Arc<FirefoxAccount>>>> = Mutex::new(None);

fn initialize(account: &FirefoxAccount) -> Result<FxaState> {
    Ok(account.process_event(FxaEvent::Initialize {
        device_config: DeviceConfig {
            name: DEVICE_NAME.into(),
            device_type: sync15::DeviceType::Desktop,
            capabilities: vec![],
        },
    })?)
}

/// The profile's account, restored from the browser's saved state if it
/// isn't loaded yet.
fn account(profile: &str, saved: Option<&str>) -> Result<Arc<FirefoxAccount>> {
    let mut accounts = ACCOUNTS.lock().unwrap();
    let accounts = accounts.get_or_insert_with(HashMap::new);
    if let Some(account) = accounts.get(profile) {
        return Ok(account.clone());
    }
    let saved = saved.ok_or_else(|| anyhow!("not signed in"))?;
    let account = Arc::new(FirefoxAccount::from_json(saved)?);
    initialize(&account)?;
    accounts.insert(profile.into(), account.clone());
    Ok(account)
}

fn forget(profile: &str) -> Option<Arc<FirefoxAccount>> {
    CACHED.lock().unwrap().as_mut().map(|cached| cached.remove(profile));
    ACCOUNTS.lock().unwrap().as_mut()?.remove(profile)
}

/// sync15's state between syncs, by browser profile: among others, the
/// servers' backoff (`Retry-After`, `X-Weave-Backoff`), until which no sync
/// contacts them.
static CACHED: Mutex<Option<HashMap<String, MemoryCachedState>>> = Mutex::new(None);

/// A sync the servers asked to wait for, until the time it holds.
#[derive(Debug)]
struct Backoff(SystemTime);
impl std::fmt::Display for Backoff {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Firefox Sync's server asked to wait before syncing again")
    }
}
impl std::error::Error for Backoff {}

fn millis(time: SystemTime) -> f64 {
    time.duration_since(UNIX_EPOCH).map_or(0.0, |d| d.as_millis() as f64)
}

fn str_arg<'a>(args: &'a Value, name: &str) -> Result<&'a str> {
    args[name].as_str().ok_or_else(|| anyhow!("missing {name}"))
}

/// Returns the sign-in page and the redirect to watch for. `server` is the
/// accounts server's URL, `token_server` the sync server's, if not the one the
/// accounts server names.
fn begin_signin(args: &Value) -> Result<Value> {
    let profile = str_arg(args, "profile")?;
    let server = args["server"].as_str();
    let account =
        Arc::new(FirefoxAccount::new(config(server, args["token_server"].as_str())?));
    initialize(&account)?;
    let state = account.process_event(FxaEvent::BeginOAuthFlow {
        service: String::new(),
        scopes: vec![SYNC_SCOPE.into(), PROFILE_SCOPE.into()],
        entrypoint: "oganesson".into(),
    })?;
    let FxaState::Authenticating { oauth_url, .. } = state else {
        // The state machine logs the cause and falls back to Disconnected;
        // the usual one is a server without /.well-known/fxa-client-configuration.
        bail!(
            "{} isn't a Mozilla accounts server, or can't be reached. A self-hosted \
             sync storage server goes in Sync server.",
            server.unwrap_or("The accounts server")
        );
    };
    ACCOUNTS.lock().unwrap().get_or_insert_with(HashMap::new).insert(profile.into(), account);
    Ok(json!({ "url": oauth_url, "redirect": REDIRECT_URI }))
}

/// Finishes sign-in with the code and state from the redirect.
fn complete_signin(args: &Value) -> Result<Value> {
    let profile = str_arg(args, "profile")?;
    let account = account(profile, None)?;
    account.process_event(FxaEvent::CompleteOAuthFlow {
        code: str_arg(args, "code")?.into(),
        state: str_arg(args, "state")?.into(),
    })?;
    if account.get_access_token(SYNC_SCOPE, true)?.key.is_none() {
        bail!("the account gave no sync key");
    }
    let email = account.get_profile(false).map(|p| p.email).unwrap_or_default();
    Ok(json!({ "account": account.to_json()?, "email": email }))
}

fn signout(args: &Value) -> Result<Value> {
    let profile = str_arg(args, "profile")?;
    let account = match forget(profile) {
        Some(account) => Some(account),
        None => args["account"].as_str().and_then(|s| FirefoxAccount::from_json(s).ok()).map(Arc::new),
    };
    // Revokes the refresh token on the server.
    if let Some(account) = account {
        account.disconnect();
    }
    passwords::remove_store(profile);
    bookmarks::remove_store(profile);
    history::remove_store(profile);
    autofill_sync::remove_stores(profile);
    tabs::remove_store(profile);
    containers::remove_store(profile);
    Ok(json!({}))
}

/// Deletes an SQLite database and its WAL files.
fn remove_db(path: &Path) {
    for suffix in ["", "-wal", "-shm"] {
        let mut file = path.as_os_str().to_owned();
        file.push(suffix);
        let _ = std::fs::remove_file(file);
    }
}

/// A collection's engine. The browser has one adapter per collection that
/// sends `{state, local}` (the engine's saved state and a snapshot of the
/// browser's data, in whatever shape the engine defines) and applies what
/// `finish` returns: `{state, ops, count}`.
trait Collection: SyncEngine {
    fn finish(self: Box<Self>) -> Result<Value>;
}

/// The collections this library syncs, by Sync collection name. `profile`
/// is the browser profile's folder.
fn collection(name: &str, profile: &str, args: &Value) -> Result<Box<dyn Collection>> {
    match name {
        "passwords" => Ok(Box::new(passwords::PasswordsEngine::new(profile, args)?)),
        "bookmarks" => Ok(Box::new(bookmarks::BookmarksEngine::new(profile, args)?)),
        "history" => Ok(Box::new(history::HistoryEngine::new(profile, args)?)),
        "tabs" => Ok(Box::new(tabs::TabsEngine::new(profile, args)?)),
        "extension-storage" => Ok(Box::new(containers::ContainersEngine::new(profile, args)?)),
        "addresses" => Ok(Box::new(autofill_sync::AutofillEngine::new(
            autofill_sync::Kind::Addresses,
            profile,
            args,
        )?)),
        "creditcards" => Ok(Box::new(autofill_sync::AutofillEngine::new(
            autofill_sync::Kind::CreditCards,
            profile,
            args,
        )?)),
        _ => bail!("unsupported collection {name}"),
    }
}

/// One sync of the enabled collections. In: the saved account and global
/// state, and `engines: {name: {state, local}}`. Out: both states and
/// `engines: {name: {state, ops, count} or {error}}`.
fn sync(args: &Value) -> Result<Value> {
    let profile = str_arg(args, "profile")?;
    let mut cached = CACHED
        .lock()
        .unwrap()
        .get_or_insert_with(HashMap::new)
        .remove(profile)
        .unwrap_or_default();
    let result = sync_with(profile, args, &mut cached);
    let retry_after = cached.get_next_sync_after();
    CACHED.lock().unwrap().get_or_insert_with(HashMap::new).insert(profile.into(), cached);
    match (result, retry_after) {
        (Ok(mut value), Some(after)) => {
            value["retry_after"] = json!(millis(after));
            Ok(value)
        }
        (Err(e), Some(after)) if !e.is::<AuthError>() && !e.is::<Backoff>() => {
            Err(e.context(Backoff(after)))
        }
        (result, _) => result,
    }
}

fn sync_with(profile: &str, args: &Value, cached: &mut MemoryCachedState) -> Result<Value> {
    if let Some(after) = cached.get_next_sync_after() {
        if after > SystemTime::now() {
            bail!(Backoff(after));
        }
    }
    let account = account(profile, args["account"].as_str())?;
    let token = match account.get_access_token(SYNC_SCOPE, true) {
        Err(FxaError::Authentication) => bail!(AuthError),
        token => token?,
    };
    let key = token.key.ok_or_else(|| anyhow!("no sync key"))?;
    let storage_init = Sync15StorageClientInit {
        key_id: key.kid.clone(),
        access_token: token.token.clone(),
        tokenserver_url: url::Url::parse(&account.get_token_server_endpoint_url()?)?,
    };
    let root_key = KeyBundle::from_ksync_bytes(&key.key_bytes()?)?;

    let mut global_state: Option<String> = args["global"].as_str().map(Into::into);
    let empty = serde_json::Map::new();
    let requested = args["engines"].as_object().unwrap_or(&empty);
    let mut engines: Vec<(String, Box<dyn Collection>)> = vec![];
    let mut results = serde_json::Map::new();
    for (name, engine_args) in requested {
        match collection(name, profile, engine_args) {
            Ok(engine) => engines.push((name.clone(), engine)),
            Err(e) => {
                results.insert(name.clone(), json!({ "error": format!("{e:#}") }));
            }
        }
    }
    let refs: Vec<&dyn SyncEngine> = engines.iter().map(|(_, e)| e.as_ref() as &dyn SyncEngine).collect();
    let processor = if requested.contains_key("tabs") {
        Some(TabCommands {
            settings: Settings {
                fxa_device_id: account.get_current_device_id()?,
                device_name: DEVICE_NAME.into(),
                device_type: sync15::DeviceType::Desktop,
            },
        })
    } else {
        None
    };
    let result = sync_multiple_with_command_processor(
        processor.as_ref().map(|p| p as &dyn CommandProcessor),
        &refs,
        &mut global_state,
        cached,
        &storage_init,
        &root_key,
        &interrupt_support::NeverInterrupts,
        None,
    );
    drop(refs);
    if matches!(result.service_status, ServiceStatus::AuthenticationError) {
        // A stale token; the next attempt gets a new one.
        account.clear_access_token_cache();
    }
    result.result?;
    for (name, engine) in engines {
        let outcome = match result.engine_results.get(&name) {
            Some(Err(e)) => json!({ "error": e.to_string() }),
            _ => engine.finish().unwrap_or_else(|e| json!({ "error": format!("{e:#}") })),
        };
        results.insert(name, outcome);
    }
    Ok(json!({
        "account": account.to_json()?,
        "global": global_state,
        "engines": results,
    }))
}

struct TabCommands {
    settings: Settings,
}

impl CommandProcessor for TabCommands {
    fn settings(&self) -> &Settings { &self.settings }
    fn fetch_outgoing_commands(&self) -> Result<HashSet<Command>> { Ok(HashSet::new()) }
    fn apply_incoming_command(&self, _command: Command) -> Result<CommandStatus> {
        Ok(CommandStatus::Unsupported)
    }
}

#[derive(Debug)]
struct AuthError;
impl std::fmt::Display for AuthError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("the Mozilla account needs to sign in again")
    }
}
impl std::error::Error for AuthError {}

fn dispatch(method: &str, args: &Value) -> Result<Value> {
    match method {
        "version" => Ok(json!(concat!("oganesson_sync ", env!("CARGO_PKG_VERSION"),
                                      " (application-services v158.0)"))),
        "begin_signin" => begin_signin(args),
        "complete_signin" => complete_signin(args),
        "sync" => sync(args),
        "signout" => signout(args),
        _ => bail!("unknown method {method}"),
    }
}

/// Registers the browser's HTTP callback and initializes NSS (a no-op if
/// the browser already did). Call once.
#[no_mangle]
pub extern "C" fn og_init(http: http::HttpFn) {
    nss::ensure_initialized();
    http::init(http);
}

/// Runs `method` with JSON `args`. Returns `{"ok": ...}` or
/// `{"error": "...", "auth": bool}`; free it with `og_free`. `ctx` is passed
/// back to the HTTP callback for requests this call makes.
///
/// # Safety
/// `method` and `args` are NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn og_call(
    ctx: *mut c_void,
    method: *const c_char,
    args: *const c_char,
) -> *mut c_char {
    let method = CStr::from_ptr(method).to_string_lossy();
    let args = CStr::from_ptr(args).to_string_lossy();
    http::CTX.set(ctx);
    let result = catch_unwind(AssertUnwindSafe(|| {
        let args: Value = serde_json::from_str(&args)?;
        dispatch(&method, &args)
    }));
    http::CTX.set(std::ptr::null_mut());
    let out = match result {
        Ok(Ok(value)) => json!({ "ok": value }),
        Ok(Err(e)) => json!({
            "error": format!("{e:#}"),
            "auth": e.is::<AuthError>(),
            "retry_after": e.downcast_ref::<Backoff>().map(|b| millis(b.0)),
        }),
        Err(_) => json!({ "error": "internal error (panic)", "auth": false }),
    };
    CString::new(out.to_string()).unwrap_or_default().into_raw()
}

/// # Safety
/// `s` came from `og_call`.
#[no_mangle]
pub unsafe extern "C" fn og_free(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}
