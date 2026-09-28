# Bug List

This file records known bugs, regressions, and unresolved implementation issues.

For larger or discussion-heavy issues, a GitHub Issue may be created and referenced here.

## Bug Reporting Rules

A bug report should contain enough information to reproduce and investigate the problem.

### Required Information

* Bug ID
* Status
* Symptom
* Reproduction steps
* Expected behavior
* Actual behavior
* Environment, when relevant
* Current suspicion or affected module, when known

### Optional Information

* Logs
* Packet captures
* Screenshots
* Crash dumps
* Related commits
* Related GitHub Issue
* Workaround

### Status

Use one of the following statuses:

| Status          | Meaning                                                                 |
| --------------- | ----------------------------------------------------------------------- |
| `Open`          | Confirmed or reported, not currently being fixed                        |
| `Investigating` | Being investigated                                                      |
| `Fixing`        | A fix is being implemented                                              |
| `Fixed`         | A fix has been implemented and verified                                 |
| `Reopened`      | Previously fixed but reproduced again                                   |
| `Blocked`       | Cannot continue without missing information, hardware, dependency, etc. |
| `Invalid`       | Not considered a bug                                                    |
| `Duplicate`     | Already tracked elsewhere                                               |

Do not create custom status names unless there is a strong reason.

---

## BUG-000 — Sample

**Status:** `Fixed`

**Date:** 2026-09-28

### Symptom

iPhone connects successfully, but video stops after approximately 10 seconds.

### Reproduction

1. Start Windows.
2. Connect iPhone.
3. Wait approximately 10 seconds.
4. Observe the video session.

### Expected

Video playback continues normally.

### Actual

The AirPlay video session disconnects.

### Current Suspicion

NCM keepalive.

### Root Cause

NCM RX thread exited unexpectedly.

### Fix

Fixed by commit `abc123`.

### Verification

Confirmed that the video session remains active after the previous failure interval.

---

## Bug Template

Copy the following template when adding a new bug:

```markdown
## BUG-XXX — Short Description

**Status:** `Open`

**Date:** YYYY-MM-DD

### Symptom

Describe what is wrong.

### Reproduction

1. Step one.
2. Step two.
3. Step three.

### Expected

Describe the expected behavior.

### Actual

Describe the actual behavior.

### Environment

- Windows:
- Hardware:
- iPhone / iOS:
- Connection: Wired / Wireless
- Build / Commit:

### Current Suspicion

Affected module or current hypothesis, if known.

### Logs / Evidence

Add relevant logs, packet captures, screenshots, crash dumps, etc.

### Root Cause

Fill this in after the cause is confirmed.

### Fix

Describe the fix and reference the relevant commit or GitHub Issue.

### Verification

Describe how the fix was verified.

### Related

- GitHub Issue:
- Commit:
- Related Bug:
```

## Guidelines

### 1. Do not present a hypothesis as a confirmed cause

Use:

> Current Suspicion: NCM keepalive

instead of:

> Cause: NCM keepalive

until the cause has been confirmed.

### 2. Keep reproduction steps deterministic

Prefer concrete steps over descriptions such as:

> Sometimes it disconnects after a while.

Record timing, connection type, device, software version, and other conditions when they affect reproduction.

### 3. Record the root cause after confirmation

A bug may initially have several suspected causes. Keep the original hypothesis, but update the report when evidence identifies the actual cause.

### 4. Link external discussion instead of duplicating it

For complex bugs, use a GitHub Issue for discussion and keep the important result in this file.

### 5. Do not delete fixed bugs

Keep historical bugs for regression tracking.

### 6. Bug IDs are permanent

Once assigned, a Bug ID should not be reused for another issue.

---

## Status Legend

```text
Open
  ↓
Investigating
  ↓
Fixing
  ↓
Fixed
```

Possible alternate paths:

```text
Investigating → Blocked
Investigating → Invalid
Investigating → Duplicate
Fixed → Reopened
```
