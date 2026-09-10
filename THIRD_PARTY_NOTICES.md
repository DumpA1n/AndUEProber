# Third-party provenance

Inventory baseline: `74b7c356a0a80719968fcba6fefdfd1cebcc6c76`. This records the checked-in snapshot, not a complete release SBOM. Dependency revisions, URLs and build inputs are unchanged.

## Git submodules

| Path | Pinned commit | Configured source |
|---|---|---|
| `external/AndSwapChainHook` | `61e022a850ade7aa173e252a32e4c0059d2a5134` | `https://github.com/DumpA1n/AndSwapChainHook.git` |
| `external/AndUEDumper` | `5db6c1f8b4f6a7e567f8fbd0c35ddf3c3ded52d0` | `git@github.com:DumpA1n/AndUEDumper.git` |

Branch labels in .gitmodules do not replace the gitlink pin. Nested dependencies and license files must be resolved at those exact commits for a release. AndUEProber pins its own AndSwapChainHook snapshot; the adjacent checkout is not its build input.

## Source attribution gaps

Directory names identify components but do not prove the upstream revision or local patch set. The inventory does not infer a legal incompatibility from missing files. Before distributing binaries, resolve source/build provenance and include the required notices for all linked and nested components. Existing MIT licenses in public repositories remain unchanged.
