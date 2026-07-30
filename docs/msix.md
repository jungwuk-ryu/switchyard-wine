# MSIX and Packaged Desktop Architecture

This document defines the compatibility, security, persistence, and performance
contract for MSIX support in Switchyard Wine. It is an implementation contract,
not a compatibility claim. An application is listed as compatible only after the
exact package and runtime revision have been tested and recorded in
`docs/compatibility.md`.

## Supported product boundary

The initial supported package is a per-user, full-trust desktop package whose
selected payload architecture can run in the current Wine prefix. This includes
ordinary Win32 packages and WinUI 3 applications that use the Windows App SDK,
provided that every required framework package and API is supported.

The following remain unsupported until their separate security and lifecycle
contracts are implemented and tested:

- UWP and AppContainer processes;
- Store licensing, purchase, entitlement, and encrypted packages;
- machine-wide or provisioned packages;
- sparse, optional, resource-only, modification, and related-set packages;
- streaming installation and content groups;
- hosted apps and out-of-process WinRT servers;
- architecture emulation that is not already available in the runtime; and
- manifest extensions or cryptographic algorithms that the parser does not
  explicitly recognize.

Unsupported input must fail with a stable error. It must never be installed
partially, reported as successful, or silently treated as an unpackaged
executable.

## Component boundaries

Package handling is divided into four layers.

1. The deployment layer inspects archives, validates signatures and block maps,
   parses manifests, resolves dependencies, stages payloads, and commits catalog
   transactions. Hostile archive parsing never runs in wineserver.
2. The catalog owns the per-prefix installed-package truth. Public deployment
   and ApplicationModel APIs project immutable snapshots from this catalog.
3. Wineserver owns only a bounded, pointer-free, immutable package graph attached
   to a process. It never parses XML, ZIP, JSON, certificates, or package paths.
4. The loader and WinRT activation code consume a process-local immutable graph
   snapshot. They do not query the catalog or filesystem on the unpackaged path.

The Switchyard application invokes a versioned helper contract. It does not
parse packages, duplicate the installed-package catalog, or launch an extracted
executable directly.

## Package inspection pipeline

Every archive is untrusted. Inspection is completed before a destination path is
created.

1. Resolve and open the input as a regular file. Reject devices, directories,
   pipes, and reparse-point substitutions.
2. Read the ZIP end record and central directory with checked 64-bit arithmetic.
   Reject multi-disk archives, encryption, unsupported flags or methods,
   overlapping records, inconsistent local headers, duplicate records, and
   records outside the file.
3. Enforce limits before decompression:
   - 65,536 entries;
   - 1 MiB per encoded entry name;
   - 256 MiB per metadata document;
   - 16 GiB per payload file;
   - 128 GiB total expanded data;
   - a configurable prefix quota and host free-space floor; and
   - bounded compression ratios and decompressor progress.
4. Decode UTF-8 names strictly. Reject rather than normalize absolute paths,
   drive or UNC paths, device paths, alternate data streams, empty or dot
   components, `..`, mixed separators, trailing dots or spaces, reserved DOS
   names, case-insensitive duplicates, Unicode-normalization collisions, and
   file/directory prefix collisions.
5. Parse `AppxManifest.xml`, `AppxBlockMap.xml`, and
   `[Content_Types].xml` with a pull parser. Network access, DTDs, external
   entities, entity substitution, XInclude, schema loading, recovery mode, and
   unbounded parser modes are prohibited. Element, depth, attribute, name, and
   value limits are enforced by the package parser.
6. Verify the CMS signature and certificate chain, then independently verify the
   AppX package-digest binding. A generic successful `WinVerifyTrust` result is
   not sufficient.
7. Stream each payload through the declared block-map hash and ZIP CRC checks.
   No unchecked payload bytes reach the installed store.
8. Return a bounded inspection record containing identity, publisher and trust
   state, selected architecture, dependencies, applications, capabilities,
   expanded size, and explicit unsupported reasons.

The bundled zlib and libxml2 libraries are used through their streaming APIs.
Cryptographic operations use public BCrypt and Crypt32 APIs; private crypto
implementation details are not linked directly.

## Safe filesystem publication

Extraction uses directory-relative handles rooted in a newly created private
staging directory. Every ancestor and leaf is opened without following reparse
points and is verified after opening. String canonicalization is not a
confinement mechanism.

Installed payloads are immutable and content-addressed. A representative
per-prefix layout is:

```text
drive_c/Program Files/WindowsApps/.wine-msix-store/
  store.lock
  catalog.bin
  catalog.bin.pending-<transaction-id>
  transactions/<transaction-id>.bin
  staging/<transaction-id>/
  payloads/<package-full-name>/<content-id>/
  leases/pending/<launch-id>
  leases/generations/<content-id>
```

Paths in catalog records are store-relative. Callers cannot choose a staging,
payload, transaction, or catalog path. Files are flushed before their containing
directory is flushed. Publication uses a same-directory atomic rename.

`catalog.bin` is the only commit point. A package is installed only when the
active catalog references a complete, verified payload. Registry entries and
directory names are projections, never the source of truth.

## Transaction and recovery contract

Install and update transactions have these durable states:

```text
created -> inspected -> staged -> payload-complete
        -> catalog-prepared -> published -> cleaned
```

Removal transactions have these durable states:

```text
created -> dependency-checked -> catalog-prepared
        -> unpublished -> garbage-collection-pending -> cleaned
```

An update verifies a new generation before replacing the active catalog
reference. It never edits the old generation. Removal first prevents new
queries and launches, then reclaims bytes only after no process lease references
the generation.

Recovery follows the catalog commit point:

- pre-publication staging and pending catalogs are discarded;
- an unreferenced complete payload is quarantined for bounded garbage
  collection;
- after publication, the active catalog wins even if transaction cleanup was
  interrupted;
- a malformed catalog, unknown schema, or broken payload reference fails closed
  and preserves evidence rather than guessing installed state.

Only one writer may publish in a prefix. Readers take a short-lived immutable
catalog snapshot. Long parsing, hashing, extraction, and process launch do not
hold the catalog lock.

Fault-injection tests cover every durable transition, including failure after
file flush and before or after catalog rename.

## Identity and dependency graph

A package graph is a versioned packed record with checked offsets and counts. It
contains:

- the main package identity and installed root;
- the selected application ID and AUMID;
- ordered dependency identities, ranks, and immutable roots;
- in-process WinRT class declarations;
- loader provenance needed by `LoadPackagedLibrary`; and
- a catalog epoch and graph revision.

The authoritative record is created only from a validated catalog snapshot.
Wineserver stores it as an immutable reference-counted object on `struct
process`. A normal child inherits the graph of the effective parent selected by
process creation, including `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS` semantics.
The object is released with the process.

The child obtains a bounded snapshot before its main image is loaded. The local
snapshot is published through
`RTL_USER_PROCESS_PARAMETERS.PackageDependencyData`, with architecture-correct
native and WOW64 representations. Package identity is never inferred from an
executable name, path, publisher, application version, or environment variable.

For an unpackaged process the server reference and local context are null.
Current-process AppModel queries return `APPMODEL_ERROR_NO_PACKAGE` after a
single local state check: no allocation, lock, server request, registry access,
or filesystem access is allowed on that path.

## Loader and WinRT activation

Packaged DLL resolution occurs after API-set/SxS redirection, the loaded-module
list, and KnownDLLs, but before ordinary application, system, and ambient path
search. Initial imports, delay imports, forwarded exports, and explicit loads
use the same graph generation.

`LoadPackagedLibrary` is a graph-only operation. Loaded-module provenance is
retained so a same-basename module loaded from outside the package graph cannot
be misreported as a packaged match. Removing a dependency affects future
resolution but does not unload an already loaded module.

WinRT activation first honors activation-context declarations, then consults
the package graph before ambient registry fallback, subject to Windows
conformance results. A higher-priority package declaration that is malformed or
missing its binary fails; it does not fall through to an unrelated registration.
Out-of-process activation remains unsupported.

Dynamic dependency changes publish a new immutable graph generation. Readers
must not observe mixed generations, use freed nodes, wait under the loader lock,
or accept stale/double-removed handles.

## Public API behavior

Public surfaces are adapters over the same catalog and process state:

- `Windows.Management.Deployment.PackageManager` implements inspect/register,
  add, update, remove, and query operations with real asynchronous completion,
  progress, cancellation, and error results.
- `Windows.ApplicationModel.Package` implements package identity, installed
  location, metadata, status, dependencies, and application list entries.
- AppModel C APIs implement their documented two-call buffer contracts for
  current and remote processes.
- `IAppListEntry::LaunchAsync` resolves the catalog entry by full name and
  application ID, acquires a generation lease, and creates the process with the
  validated package graph attached before its first thread runs.
- A command-line helper exposes inspect, query, install, update, remove, and
  launch as a versioned machine-readable contract. JSON, when requested, is an
  output encoding and not an internal trust boundary.

Synchronous argument errors are returned synchronously. Work accepted for
asynchronous execution always reaches exactly one terminal state. Closing or
cancelling an operation cannot publish a partial transaction.

## Performance contract

Performance changes are measured, not inferred.

- Unpackaged process creation and DLL loading add only predictable null checks.
- Package graphs are parsed once, stored contiguously, and shared immutably
  across ordinary child processes.
- Loader lookup uses a precomputed basename index and performs no catalog,
  registry, XML, or JSON work.
- Catalog writers build replacements off-lock and publish atomically.
- Archive reads, hashing, and decompression are streaming and bounded.
- Package enumeration returns snapshots and does not hold store locks through
  caller callbacks.

Benchmarks record wall time, CPU time, allocations, peak resident memory,
filesystem operations, and graph-size scaling. Regression gates include
unpackaged process startup, unpackaged DLL lookup, packaged cold start, repeated
graph lookup, install throughput, and interrupted-transaction recovery.

## Verification gates

Each implementation slice requires:

- a Windows-observed or specification-backed behavioral expectation;
- positive, negative, boundary, and failure-injection tests;
- malformed-input and resource-limit tests for trust-boundary code;
- architecture and WOW64 coverage where state crosses bitness;
- concurrency tests for update, removal, launch, and graph replacement;
- leak, handle, and cleanup checks;
- performance evidence for process, loader, and filesystem hot paths; and
- an independent review with all material findings resolved.

The end-to-end matrix uses fresh prefixes and covers loose registration, signed
MSIX and bundle install, query, update with rollback, removal while in use,
dependency resolution, full-trust launch, WinUI 3 framework loading, malformed
archives, unsupported packages, and crash recovery. Compatibility is not
claimed from unit tests alone.
