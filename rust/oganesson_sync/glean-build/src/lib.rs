// Copyright 2026 The oganesson Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Stands in for Glean's glean-build, which pip-installs glean_parser into a
//! Python venv at build time; the build container has neither pip nor venv.
//! Only `places` uses it, so `generate` writes the bindings glean_parser
//! made from places' metrics.yaml. Its metrics go through glean-sym, which
//! does nothing without Glean loaded.

use std::path::Path;

#[derive(Default)]
pub struct Builder;

impl Builder {
    pub fn file<S: Into<String>>(&mut self, _file: S) -> &mut Self {
        self
    }

    pub fn format<S: Into<String>>(&mut self, _format: S) -> &mut Self {
        self
    }

    pub fn generate(&self) -> std::io::Result<()> {
        let out = std::env::var("OUT_DIR").expect("OUT_DIR");
        std::fs::write(Path::new(&out).join("glean_metrics.rs"), include_str!("places_metrics.rs"))
    }
}
