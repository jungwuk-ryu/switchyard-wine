# Source provenance

Switchyard Wine is a downstream Wine branch based on upstream revision `0c1585cf5bb9a29a5c480ee04d5529b8fc236044` from [WineHQ](https://gitlab.winehq.org/wine/wine).

The recorded upstream base is a provenance anchor rather than a commitment to track WineHQ continuously. Switchyard may remain on a known-working base, selectively cherry-pick or adapt upstream changes, and retain project-specific changes downstream. The base is advanced only when the resulting runtime can preserve the launcher and game behavior that Switchyard already supports.

The original Switchyard change queue was imported in order as ordinary Git commits. Each imported commit retains its original author, message, rationale, upstream status, and test notes where present. Git history is now the canonical record; the repository does not maintain a parallel numbered change ledger.

The mechanical migration was verified by comparing the Git tree produced by sequentially applying the former queue with the tree produced by the imported commit range. The compositor unification change then received publication-review corrections for visibility, same-HWND DComp plane composition, foreign-surface lifetime, and owner-generation validation. `switchyard/verify_source.sh` enforces pinned upstream ancestry, whitespace checks, and source-only repository policy on future changes.

Wine and the downstream modifications in this repository are licensed under the GNU Lesser General Public License, version 2.1 or later. See `LICENSE` and `COPYING.LIB`. Anyone distributing a binary built from this repository must continue to satisfy the LGPL source, relinking, notice, and replacement requirements applicable to that distribution.

This repository contains no Apple Game Porting Toolkit binaries or source, no launcher binaries or assets, and no user credentials. References to GPTK, Steam, Battle.net, Chrome, or other software describe compatibility work and local interoperability testing only.
