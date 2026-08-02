# MSIX and Packaged Desktop Support

This document describes the MSIX behavior that is implemented in Switchyard
Wine. It is not a promise of complete Windows package compatibility. An
application is compatible only after its exact package and runtime revision
have been tested and recorded in `docs/compatibility.md`.

## Current scope

The current target is a signed, per-prefix, full-trust desktop MSIX/AppX
package. Supported application declarations are the full-trust,
`packagedClassicApp`, and `win32App` forms accepted by the manifest parser, with
the `runFullTrust` capability. Standalone framework packages can be installed
and resolved as static manifest dependencies.

This work can provide the package identity, dependency graph, DLL search, and
in-process WinRT activation needed by some Win32 and WinUI 3 applications. It
does not imply that every Windows App SDK or application API used by such a
package is implemented.

The following are explicitly outside the current target:

- UWP, AppContainer process creation, and AppContainer isolation;
- Store purchase, entitlement, licensing, and encrypted package support;
- machine-wide provisioning, multi-user deployment, and a shared system
  package database;
- sparse, optional, modification, resource-only, related-set, and streaming
  deployment;
- hosted applications and out-of-process WinRT activation;
- loose or development-mode registration;
- dynamic package dependencies or runtime package-graph mutation; and
- CPU emulation not already provided by the selected Wine runtime.

Unsupported manifests, package kinds, options, and architectures fail instead
of being treated as unpackaged applications.

## Implemented components

Package support is divided between these components:

1. `appxsvc.dll` contains the private archive, manifest, catalog, deployment,
   recovery, graph-building, and launch implementation. It runs in the calling
   Wine process; it is not a privileged or separately isolated service.
2. `appxdeploymentclient.dll` exposes the implemented subset of
   `Windows.Management.Deployment.PackageManager` over the private deployment
   entry points.
3. `wineappx.exe` exposes inspection, extraction, store administration, and a
   private full-trust launch command.
4. Wineserver accepts only a bounded, pointer-free package graph and the
   handles needed to bind it to a process. It does not parse ZIP, XML,
   certificates, manifests, or the installed catalog.
5. Ntdll, KernelBase, the loader, `windows.applicationmodel.dll`, and combase
   consume the immutable graph attached during process creation.

The installed catalog is the per-prefix source of package state. Process
identity is not inferred from an executable path, filename, environment
variable, or registry entry.

## Inspection and extraction

`wineappx inspect` and `wineappx unpack` accept signed MSIX/AppX packages and
supported bundles. Inspection verifies the archive layout, signature and signed
package digest, content types, manifest, block map, payload hashes, and ZIP CRCs.
Bundle inspection selects one compatible application payload and rejects
ambiguous, optional, resource, encrypted, unsupported-type, unsupported
qualifier, and incompatible-architecture payloads.

Archive entry names are validated before extraction. Absolute, drive, UNC,
device, alternate-stream, dot-component, traversal, reserved-name,
case-collision, Unicode-normalization-collision, and file/directory-prefix
collision cases are rejected. XML parsing disables external entities, DTD
loading, network access, XInclude, and recovery behavior. Archive, metadata,
entry, expansion, and parser limits are enforced.

Extraction creates a new destination and uses directory-relative handles
without following reparse points. `unpack` only verifies and extracts content;
it does not stage or register the resulting directory.

## Deployment store

The default store is:

```text
C:\Program Files\WindowsApps\.wine-msix-store
```

`wineappx --store` can select another drive-absolute store path. The public
`PackageManager` projection always uses the default path.

The implemented store contains:

```text
store.lock
catalog.bin
catalog.bin.pending
transactions/
staging/
payloads/
quarantine/
records/
record-staging/
record-quarantine/
leases/pending/
leases/generations/
```

Install and update verify and extract a new payload generation before publishing
it. The updater does not edit an active generation in place. Catalog
publication uses a pending catalog and same-directory rename; `catalog.bin` is
the package-state commit point. Paths held by the catalog are store-relative.

Removal first publishes a catalog that no longer exposes the package. Payload
and deployment-record reclamation is deferred while a process holds a validated
generation lease. The recovery command reconciles interrupted journals around
the catalog commit point, and garbage collection reclaims bounded,
unreferenced generations and quarantined records. A store writer uses
`store.lock`; catalog readers consume validated snapshots.

## Full-trust launch and process graph

The implemented launch entry point is private
`appx_deployment_launch`, exposed by:

```text
wineappx launch PACKAGE_FULL_NAME APP_ID
```

This path is distinct from `IAppListEntry::LaunchAsync`, which is not
implemented.

Launch performs these checks before `CreateProcessW`:

1. Open and validate the selected store and catalog.
2. Resolve an exact active package full name and exact application ID.
3. Reopen the declared executable below the published payload generation and
   verify its stored file identity and content digest.
4. Resolve the static manifest dependency graph for the selected target
   architecture.
5. Reopen and verify the loader and in-process WinRT files recorded for every
   graph package.
6. Open one validated generation-marker lease for every graph package.
7. Build an immutable graph containing package identities and roots, the
   application ID and AUMID, executable identity, dependency ordering, loader
   inventory, loader search ranks, and in-process WinRT class declarations.
8. Pass the graph, executable handle, and lease handles through the private
   process-thread attribute before the child starts.

Wineserver validates the graph shape, every generation marker, and the exact
main-image filesystem identity. It verifies the mapped executable again before
process initialization completes. The server retains the generation leases
until the process's last thread exits.

A normal packaged child can inherit a graph only when the creating process
already owns the exact same server graph object. Selecting an unrelated
packaged process as the nominal parent does not borrow that process's package
identity. An explicitly supplied graph must carry a complete, independently
validated image binding and lease set.

The local, architecture-appropriate graph pointer is published through
`RTL_USER_PROCESS_PARAMETERS.PackageDependencyData`. A process gets one
immutable startup graph. There is currently no supported API that adds,
removes, or replaces graph packages while the process is running.

## Loader and in-process WinRT behavior

The native loader validates the graph before using it. Packaged resolution
participates after API-set and activation-context handling, loaded-module
matching, and KnownDLL handling, and before ordinary ambient search where the
Windows search path requires it.

Each registered package DLL is bound to its exact package-relative path and
filesystem object identity, change time, and size. If a PE file exists below a
graph package root but is not registered at that exact graph-relative path, an
ordinary load through that root fails with `STATUS_INVALID_IMAGE_HASH` instead
of accepting the file through current-directory or basename fallback. A missing
package-root candidate still permits the applicable system fallback, and an
explicit absolute path outside every graph root retains ordinary loader
behavior.

`LoadPackagedLibrary` is graph-only. It does not fall back to arbitrary files or
the registry and reports an unpackaged process as having no package.

`RoGetActivationFactory` checks activation-context declarations first, then the
package graph, followed by the existing builtin and registry paths. A graph
class is loaded with `LoadPackagedLibrary`, so its module must be in the
verified loader inventory. Only in-process WinRT server declarations are
represented; out-of-process activation is not implemented.

## Public API status

### `Windows.Management.Deployment.PackageManager`

The implemented deployment operations return real asynchronous operations with
progress, completion, cancellation, and a `DeploymentResult`:

- `AddPackageAsync` accepts a local drive-absolute `file:` URI, an empty
  dependency iterable, and `DeploymentOptions_None`.
- `UpdatePackageAsync` has the same URI and dependency restrictions and accepts
  only `DeploymentOptions_None` or
  `DeploymentOptions_ForceUpdateFromAnyVersion`.
- `RemovePackageAsync` removes one exact package full name.
- `IPackageManager2::RemovePackageWithOptionsAsync` accepts only
  `RemovalOptions_None`.

Catalog-backed enumeration and lookup are implemented for all packages, exact
full name, family name, and name plus publisher. The user-SID variants accept
only the current user SID. Package-type filters accept the implemented main,
framework, and resource flags; this does not add resource-package deployment
support.

The following operations return `E_NOTIMPL`:

- `StagePackageAsync` and `StagePackageWithOptionsAsync`;
- `RegisterPackageAsync` and `RegisterPackageByFullNameAsync`;
- user-data staging and cleanup; and
- package-user enumeration and package-state mutation.

Development-mode, external-location, deferred-registration, and other
unimplemented deployment options are rejected. Dependency URI iterables must
be empty; dependency packages are installed separately and resolved from their
static manifest declarations.

### `Windows.ApplicationModel.Package`

Catalog and current-process package projections implement the package identity,
installed location, framework/resource/development flags, and the dependency
views that can be represented by the catalog or startup graph. The projection
is intentionally partial.

Display metadata, description, logo, package status, installed date, and
application-list entries are not implemented where the required data is not
present. `IPackage3::GetAppListEntriesAsync` returns `E_NOTIMPL`; no
`IAppListEntry` object or `IAppListEntry::LaunchAsync` implementation is
provided.

### AppModel C APIs

The startup graph backs the implemented current-process identity, family, full
name, package ID, package path, application user model ID, and package-info
queries. Remote-process full-name and family-name queries obtain an immutable
graph snapshot from wineserver rather than reading the target PEB. Installed
catalog queries and package-name/family conversion helpers are also implemented
where exported by KernelBase.

Unpackaged current-process queries take the no-graph path and return the
appropriate `APPMODEL_ERROR_NO_PACKAGE` or
`APPMODEL_ERROR_NO_APPLICATION`. A malformed graph fails as package-runtime
corruption.

The Windows dynamic-dependency creation, add, remove, and delete APIs are not
implemented. Static manifest dependencies in the startup graph are not dynamic
dependencies.

## Command-line contract

`wineappx` currently provides:

```text
inspect PACKAGE
unpack PACKAGE DESTINATION
initialize
install PACKAGE
update PACKAGE
remove PACKAGE_FULL_NAME
query PACKAGE_FULL_NAME
list
launch PACKAGE_FULL_NAME APP_ID
recover
gc
```

Successful standard output is stable escaped `key=value` data. Diagnostics are
written to standard error. There is no JSON output mode, JSON registration
format, or loose-registration command.

The helper also exposes store, target-architecture, downgrade, weak-durability,
archive-size, expanded-size, free-space-floor, and launch-wait options. These
are private helper controls, not additional public Windows deployment APIs.

## Security boundary

The implementation assumes one trusted Unix user owns and operates a Wine
prefix. The private deployment code runs with that user's normal host
permissions. It is not a privileged package broker and does not provide
security isolation from another process running as the same Unix UID with
access to the prefix.

The ntdll-to-wineserver graph handoff narrows this same-UID boundary. Its backing
file must be a regular file owned by `getuid()`, mode `0600`, exact-sized,
read-only at the server, and unlinked before it becomes process state. The
server similarly requires current-UID, regular, read-only, single-link image
and generation-marker files, validates marker content against each graph
package, rejects duplicate lease objects and leases that permit write or delete
sharing, and binds the graph to the exact executable object.

These checks enforce object consistency and stop graph borrowing through an
unrelated nominal parent in the normal process-creation path. They do not
authenticate package identity against a hostile process running under the same
Unix UID. They are not a cross-user authorization system, a machine-wide
package trust service, or an AppContainer sandbox. A launched application is
full trust and has the normal permissions of its Wine process.

## Architecture boundary

The public deployment projection and `wineappx` default the target architecture
from `GetNativeSystemInfo`. A direct package or static dependency is compatible
only when it is neutral or exactly matches the selected target architecture.
Bundle deployment maps concrete x86, x64, ARM, and ARM64 targets and rejects a
target it cannot map.

`wineappx --arch` changes package and bundle selection; it does not provide an
emulator or make an otherwise unrunnable executable valid. Native and WOW64
process setup use architecture-correct graph representations, but the selected
executable must still be runnable by the current Wine prefix and host runtime.

## Compatibility claims

Unit and conformance tests establish API and trust-boundary behavior, not
application compatibility. Record an application result in
`docs/compatibility.md` only after testing the exact package, Switchyard Wine
revision, host environment, and launch path.
