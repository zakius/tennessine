// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! viaduct backend that hands every request to the browser, which sends it
//! with its own network stack (proxy settings, certificates) and blocks this
//! thread until the response arrives.

use std::cell::Cell;
use std::collections::HashMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Arc, OnceLock};

use serde_json::{json, Value};
use viaduct::{ClientSettings, Headers, Request, Response, ViaductError};

/// Called by the browser, before `HttpFn` returns, with the response JSON.
pub type SetFn = extern "C" fn(out: *mut c_void, response_json: *const c_char);
/// `{"method","url","headers":{},"body"?,"timeout_ms"}` in; the browser calls
/// `set(out, json)` with `{"status","headers":{},"body"}` or `{"error"}`.
pub type HttpFn =
    extern "C" fn(ctx: *mut c_void, request_json: *const c_char, out: *mut c_void, set: SetFn);

static HTTP: OnceLock<HttpFn> = OnceLock::new();

thread_local! {
    /// The browser context of the `og_call` running on this thread.
    pub static CTX: Cell<*mut c_void> = const { Cell::new(std::ptr::null_mut()) };
}

pub fn init(http: HttpFn) {
    if HTTP.set(http).is_ok() {
        viaduct::init_backend(Arc::new(Backend));
    }
}

extern "C" fn set_response(out: *mut c_void, response_json: *const c_char) {
    // SAFETY: `out` is the `Option<String>` passed to `HttpFn` below, and the
    // browser calls this before `HttpFn` returns.
    unsafe {
        *(out as *mut Option<String>) =
            Some(CStr::from_ptr(response_json).to_string_lossy().into_owned());
    }
}

struct Backend;

#[async_trait::async_trait]
impl viaduct::Backend for Backend {
    async fn send_request(
        &self,
        request: Request,
        settings: ClientSettings,
    ) -> Result<Response, ViaductError> {
        let http = HTTP.get().ok_or(ViaductError::BackendNotInitialized)?;
        let headers: HashMap<String, String> = request.headers.clone().into();
        // Sync and account bodies are JSON text.
        let body = request.body.as_deref().map(String::from_utf8_lossy);
        let request_json = json!({
            "method": request.method.as_str(),
            "url": request.url.as_str(),
            "headers": headers,
            "body": body,
            "timeout_ms": settings.timeout,
        });
        let request_json = CString::new(request_json.to_string())
            .map_err(|e| ViaductError::BackendError(e.to_string()))?;
        let mut out: Option<String> = None;
        http(
            CTX.get(),
            request_json.as_ptr(),
            &mut out as *mut Option<String> as *mut c_void,
            set_response,
        );
        let response: Value = serde_json::from_str(&out.unwrap_or_default())
            .map_err(|e| ViaductError::NetworkError(format!("bad response from browser: {e}")))?;
        if let Some(error) = response["error"].as_str() {
            return Err(ViaductError::NetworkError(error.to_string()));
        }
        let headers: HashMap<String, String> =
            serde_json::from_value(response["headers"].clone()).unwrap_or_default();
        Ok(Response {
            request_method: request.method,
            url: request.url,
            status: response["status"].as_u64().unwrap_or(0) as u16,
            headers: Headers::try_from_hashmap(headers)?,
            body: response["body"].as_str().unwrap_or("").as_bytes().to_vec(),
        })
    }
}
