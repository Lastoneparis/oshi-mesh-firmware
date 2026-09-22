# Contributing to OSHI Mesh

OSHI Mesh is a fork of the [Meshtastic firmware](https://github.com/meshtastic/firmware). Contributions are welcome here for
anything OSHI-specific: the OSHI Mesh Protocol (OMP), custody, the gateway, the router policies in `src/oshi/OshiPolicy.h`,
the specification in `docs/omp/`, tests and tools.

**Fixes to the Meshtastic firmware itself belong upstream.** If a bug is not in OSHI code, please report and fix it in
[meshtastic/firmware](https://github.com/meshtastic/firmware) (their contribution guide and CLA apply there); it will reach
this fork with the next upstream sync.

## Branches

| Branch | Content |
| --- | --- |
| `oshi` (default) | upstream `develop` plus the OSHI Mesh commits. Releases are built from here. |
| `develop` | mirror of `meshtastic/firmware` `develop`, never committed to directly |
| other branches | inherited from upstream, not maintained here |

## How the fork tracks upstream

OSHI code is kept apart so that upstream changes merge cleanly:

- new files only: `src/oshi/`, `src/modules/OshiModule.*`, `test/test_oshi_protocol/`, `tools/oshi/`, `docs/omp/`;
- a small number of hooks in upstream files (`src/mesh/MeshService.{h,cpp}`, `src/mesh/Router.cpp`,
  `src/modules/Modules.cpp`), each behind `#if !MESHTASTIC_EXCLUDE_OSHI`;
- branding only in default names (`NodeDB.cpp`, `main.cpp`), the boot/screen images and `userPrefs.jsonc`.

Syncing (maintainers):

```sh
git remote add upstream https://github.com/meshtastic/firmware.git   # once
git fetch upstream
git push origin upstream/develop:develop       # refresh the mirror
git checkout oshi
git merge upstream/develop                     # merge, never rebase: `oshi` is public, history is not rewritten
pio test -e coverage -f test_oshi_protocol     # Linux; plus tools/oshi/sim_mesh.py
git push origin oshi
```

After a sync, check that the hooks still sit where they must: the phone-side hooks in `MeshService::handleFromRadio` and
`MeshService::handleToRadio`, the duty-cycle hook just before the upstream duty-cycle check in `Router::send`, and the
signature hook in the `COMPATIBLE` branch of `checkXeddsaReceivePolicy`. If upstream changes the behaviour an item in the
README's "Why a fork" table describes, update the table and its line references.

## Changing the protocol

`docs/omp/OMP-v1.md` is a contract with other implementations. A change to any frame layout, constant or state-machine rule
must update the specification and `docs/omp/IMPLEMENTING.md` (including the test vectors) in the same pull request.
Changes that old OMP v1 nodes cannot understand need a new version nibble, not a silent change.

## Pull requests

1. Branch from `oshi`, keep the change focused, and keep OSHI logic in `src/oshi/` as pure, testable code where possible.
2. Add or update unit tests in `test/test_oshi_protocol/`; for behaviour across nodes, a scenario in `tools/oshi/sim_mesh.py`.
3. Say in the description what was tested and how (unit, simulation, which hardware).
4. Format with `trunk fmt`, as upstream does.

By contributing you agree that your contribution is licensed under the GPL-3.0, like the rest of the firmware.
