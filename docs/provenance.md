# Source provenance

Switchyard Wine is a downstream Wine branch based on upstream revision `0c1585cf5bb9a29a5c480ee04d5529b8fc236044` from [WineHQ](https://gitlab.winehq.org/wine/wine).

The original Switchyard patch queue was imported in order, one patch per commit. Each imported commit retains its original author, message, rationale, upstream status, and test notes where present, plus a stable `Switchyard-Patch` trailer. The historical ledger in `docs/patch-history.md` preserves the former patch identifiers and validation notes.

The mechanical migration was verified by comparing the Git tree produced by sequentially applying the former queue with the tree produced by the imported commit range. The final `0109` commit then received publication-review corrections for compositor visibility, same-HWND DComp plane composition, foreign-surface lifetime, and owner-generation validation. `switchyard/verify_source.sh` enforces the ancestry and trailer invariants on future changes.

Wine and the downstream modifications in this repository are licensed under the GNU Lesser General Public License, version 2.1 or later. See `LICENSE` and `COPYING.LIB`. Anyone distributing a binary built from this repository must continue to satisfy the LGPL source, relinking, notice, and replacement requirements applicable to that distribution.

This repository contains no Apple Game Porting Toolkit binaries or source, no launcher binaries or assets, and no user credentials. References to GPTK, Steam, Battle.net, Chrome, or other software describe compatibility work and local interoperability testing only.
