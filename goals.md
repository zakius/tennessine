# Goals

What tennessine is for is in the [README](README.md#what-its-for). This file lists what is planned beyond it.

## Parked

Planned, not started, in this order. Decided 2026-10-20, together with oganesson's [parked goals](https://github.com/zakius/oganesson/blob/main/goals.md#parked).

1. **Containers**, as shared code in dubna. On Android this is a large job: Chromium creates a tab's WebContents without a storage partition, so the tab model, session restore and link inheritance need work. Firefox for Android has no containers to copy. The first version stays minimal: each tab shows its container's color, a link's long-press menu gets "Open in container", and Multi-Account Containers' popup does the rest.
2. **Firefox add-ons, uBO first.** Upstream uBlock Origin's Firefox build from AMO replaces the Helium build the browser installs today, with a settings migration. Then Multi-Account Containers. The patches are shared with oganesson; tennessine is GPL-2.0-only, so code taken from Helium (GPL-3.0) must be written fresh.
