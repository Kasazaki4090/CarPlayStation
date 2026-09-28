# AI-Assisted Development Guidelines

## Scope and Coordination

1. Confirm the target project, repository, and task scope before making changes.
2. Inspect the relevant code, documentation, tests, and repository status before editing. Do not overwrite or revert unrelated changes.
3. Keep changes focused on the task. Do not modify unrelated projects or combine unrelated work.
4. When multiple tasks or contributors are active, avoid editing files owned by another task. Coordinate before changing shared files or interfaces.
5. Report progress clearly, including completed work, remaining work, changed files, blockers, and tests run.

## Design and Implementation

6. Search for existing implementations before adding functionality.
7. Prefer reusing existing modules and patterns.
8. Keep changes as small as practical. Do not introduce a new framework for a minor feature.
9. Do not add managers, services, or handlers without a clear need.
10. Give each module one clear responsibility.
11. Before changing a public interface, describe the affected callers, modules, and projects, as well as any compatibility implications.
12. Do not remove functionality merely to resolve compilation errors.

## Reliability and Verification

13. Do not silently ignore or swallow errors.
14. When protocol behavior or requirements are unclear, report the uncertainty and ask for clarification rather than guessing.
15. Run the relevant builds and tests when possible. Clearly state which checks were run and disclose any that could not be run.