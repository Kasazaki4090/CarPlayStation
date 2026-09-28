# TODO

This file tracks planned work for CarPlayStation.

TODO items should be derived from:

1. `PROJECT.md` — project goals and scope
2. `ARCHITECTURE.md` — current architecture
3. `DECISIONS.md` — accepted design decisions
4. `BUGS.md` — known problems and regressions
5. `Reference/` — protocol and implementation research
6. Current source code — implemented and missing functionality

Do not blindly add tasks based on assumptions.

## Rules

### 1. Keep tasks actionable

Prefer:

```text
- [ ] Implement USB transport initialization
```

over:

```text
- [ ] USB
```

### 2. Do not duplicate BUGS.md

Known bugs belong in `BUGS.md`.

A TODO may reference a bug when fixing it requires planned development work.

### 3. Do not treat research assumptions as implementation requirements

If something is not yet understood, record it as a research task first:

```text
- [ ] Investigate wireless CarPlay discovery flow
```

rather than prematurely defining an implementation.

### 4. Keep priorities visible

Use the following priority levels:

* `P0` — Required for the basic receiver to work
* `P1` — Required for first usable release
* `P2` — Important improvement
* `P3` — Optional / future work

Example:

```text
- [ ] P0 — Implement USB transport
- [ ] P0 — Implement iAP2 session
- [ ] P1 — Implement touch input
```

### 5. Remove completed work

Completed implementation should normally disappear from TODO rather than becoming a historical checklist.

Historical information belongs in:

* Git history
* `DECISIONS.md`
* `BUGS.md`

---

## Current TODO

The initial TODO list should be generated and maintained after reviewing the project documentation, source code, and reference implementations.

At project initialization, do not assume this list is complete.

```text
- [ ] Review project architecture
- [ ] Analyze Reference projects
- [ ] Identify required CarPlay protocol components
- [ ] Identify required transport components
- [ ] Identify unknowns and research tasks
- [ ] Generate implementation plan
```

---

## AI Project Review

When an AI agent starts work on the project, it should first review:

```text
PROJECT.md
ARCHITECTURE.md
DECISIONS.md
BUGS.md
TODO.md
Reference/
Core/
UI/
Lib/
```

After reviewing the project, the AI should:

1. Identify missing implementation areas.
2. Identify assumptions that require verification.
3. Identify missing research tasks.
4. Identify architectural inconsistencies.
5. Update `TODO.md` with concrete tasks.
6. Avoid modifying project goals without documenting the decision in `DECISIONS.md`.
