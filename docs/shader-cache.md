# D3D12 application shader cache

Switchyard's built-in vkd3d provider implements the public
`ID3D12Device9::CreateShaderCacheSession` key/value API in both memory and disk
modes. This is an application-managed cache. It is not a transparent pipeline
state object (PSO) cache, and its contents are opaque to Wine and vkd3d.

The implementation follows Microsoft's
[public API contract](https://microsoft.github.io/DirectX-Specs/d3d/ShaderCache.html)
and the documented
[`D3D12_SHADER_CACHE_SESSION_DESC`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_shader_cache_session_desc)
semantics:

- `Identifier` names a process-local cache. Sessions with the same identifier
  share the first session's resolved description. Driver-versioned sessions on
  different adapters remain side-by-side as required by that flag. A different
  `Version` is rejected with `DXGI_ERROR_ALREADY_EXISTS` while any matching
  session remains open; otherwise the old cache is cleared before the new
  version opens.
- Zero memory limits resolve to 1 KiB and 128 entries. A zero maximum value size
  resolves to 128 MiB. Values larger than the resolved maximum fail with
  `DXGI_ERROR_CACHE_FULL`; a requested maximum above 1 GiB is invalid.
- `FindValue` supports the size-query call, reports the required size with
  `DXGI_ERROR_MORE_DATA`, and distinguishes not-found from a valid hash
  collision. `StoreValue` never overwrites: an exact duplicate returns
  `DXGI_ERROR_ALREADY_EXISTS` and a different key with the same digest returns
  `DXGI_ERROR_CACHE_HASH_COLLISION`.
- `D3D12_SHADER_CACHE_FLAG_DRIVER_VERSIONED` adds the Vulkan vendor, device,
  driver/API versions, and pipeline cache UUID to the identity. Without this
  opt-in the application-visible data remains portable as the D3D12 contract
  requires. `D3D12_SHADER_CACHE_FLAG_USE_WORKING_DIR` places the root below the
  current directory captured when the cache is first opened.
- `SetDeleteOnDestroy` is shared by all matching sessions and processes. The
  namespace is cleared after the final lifecycle holder closes it. A durable
  marker makes a process crash conservative: the next exclusive opener
  completes the deletion.
- Disabling application-managed caches process-wide makes create, find, and
  store return `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`. Clear affects all active
  application sessions, including sessions created from other devices, and
  inactive default-location disk caches, but deliberately does not discover
  inactive working-directory caches.

Invalid pointers, zero-length keys or values, a null identifier, unknown mode
or flags, and mutually exclusive control flags return the public contract's
argument errors. Filesystem errors are not hidden: disk full, read-only media,
ACL denial, and sharing failures are returned as their Win32-derived HRESULTs.

## Location and identity

The default location is:

```text
<user temporary directory>/vkd3d-cache/<SHA-256 of full executable path>/
```

With `USE_WORKING_DIR`, it is:

```text
<captured current directory>/vkd3d-cache/
```

Each cache below that root uses a 64-character lowercase hexadecimal directory
name. The namespace digest covers a tagged identity record containing the full
executable path, application identifier, flags, and the driver identity when
`DRIVER_VERSIONED` is selected. The namespace manifest also records the cache
format ABI, Switchyard/Wine provider version, session version, and namespace
digest. A mismatch cannot reuse old bytes: it causes an exclusive reset or an
`ALREADY_EXISTS` result when another lifecycle holder makes a reset unsafe.

The implementation hashes opaque identifiers and keys; it never places their
text in a path. Every created or opened component is checked as a non-reparse
directory or regular file. Open directory handles deny delete sharing while a
session is active, closing rename/replacement races. Enumeration accepts only
fixed known metadata names and exact 64-character hexadecimal names. Cleanup
never recursively follows or deletes unknown objects. Directories and files
inherit the caller's user cache or working-directory ACL and umask; the cache
does not weaken those permissions.

Cache-owned metadata is written to a newly created file and atomically renamed
over its destination. It never truncates a pre-existing path, so an untrusted
hard link at a metadata name is replaced inside the cache rather than modifying
the linked file. Finalized entry readers deny write sharing for the complete
checksum and copy lifetime, closing mutation races between validation and the
second-pass read of a large value.

## On-disk format

All integers are little-endian. All digests are SHA-256. Reserved fields must
be zero. A format, size, ABI, identity, length, or checksum mismatch quarantines
the affected entry rather than returning its value.

### Entry version 1

An entry filename is the 64-character hexadecimal SHA-256 of its key. Its
contents are a 176-byte header followed by the exact key bytes and exact value
bytes.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic `VKD3DC01` |
| 8 | 4 | Format version (`1`) |
| 12 | 4 | Header size (`176`) |
| 16 | 4 | Cache ABI version |
| 20 | 4 | Flags, currently zero |
| 24 | 8 | Key size |
| 32 | 8 | Value size |
| 40 | 8 | Session version |
| 48 | 32 | Namespace digest |
| 80 | 32 | Key digest |
| 112 | 32 | Digest of key followed by value |
| 144 | 32 | Digest of header bytes 0 through 143 |

The file length must equal `176 + key size + value size`. Full retrieval and
duplicate detection compare the stored key and validate the complete payload
checksum, which prevents a malformed collision record from bypassing integrity
checking. A size-only lookup validates the header and key but deliberately does
not read a potentially 1 GiB value; a later retrieval validates the payload
before returning any bytes. Values too large for the memory tier are verified
in a streaming pass and then read again into the application buffer, so corrupt
data is never copied to that buffer before its checksum succeeds.

### Namespace metadata version 1

The 128-byte manifest has magic `VKD3DM01`. It covers the format and ABI
versions, session version, namespace digest, and provider version, and ends in
a checksum over its first 96 bytes.

The 104-byte namespace quota record has magic `VKD3DQ01`. It records a dirty
bit, logical value bytes, physical bytes, and entry count, with a checksum over
its first 72 bytes. The dirty bit is flushed before publishing an entry. A
missing, corrupt, impossible, or dirty record is rebuilt by a bounded scan.

### Root aggregate metadata version 1

The root state has magic `VKD3DR01` and a fixed size of 10,848 bytes:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 64 | Versioned header, counts, 4 GiB cap, committed bytes, serial |
| 64 | 7,168 | 128 fixed 56-byte namespace records |
| 7,232 | 3,584 | 32 fixed 112-byte reservation records |
| 10,816 | 32 | SHA-256 of bytes 0 through 10,815 |

A namespace record contains its digest, charged entry-file bytes, last-use
time, and committed entry count. A live reservation contains namespace, entry,
and temporary-file digests, charged bytes, and flags. Each reservation slot has
its own byte-range lock in the root lock file. The state is written to a fixed
temporary name, flushed, and atomically replaced with write-through enabled.

## Publication, recovery, and deletion

Store takes the namespace operation lock, checks for an existing key, reserves
bounded capacity, and marks quota metadata dirty. It writes a uniquely hashed
temporary file with write-through enabled, flushes the complete header/key/value
stream, and publishes it with a write-through atomic rename. Only then is clean
quota metadata committed. Manifest replacement uses the same temporary-write,
flush, and atomic-rename sequence.

This policy uses the strongest ordering available through the Win32 interface
implemented by Wine. It does not claim a POSIX directory `fsync` guarantee that
the API cannot portably provide. A sudden power loss may discard the newest
cache entry, which is acceptable for disposable cache data; it cannot make an
unchecked partial value visible. Startup removes only validated hexadecimal
temporary files. Partial entries, bad lengths, bad checksums, and stale dirty
metadata are isolated and recovered independently.

Delete-on-destroy and explicit clear acquire the exclusive operation lock.
They delete only known entry and temporary files and known metadata, then
remove empty known directories. A reparse point or unknown object stops that
part of cleanup with an error instead of expanding the deletion boundary.

## Resource bounds and eviction

The requested in-memory byte and entry limits are hard bounds for cached
payloads. The in-process tier uses LRU eviction and is protected by a short
mutex; disk I/O never occurs while that mutex is held.

`MaximumValueFileSizeBytes` limits each disk value independently and is ignored
by memory mode. A disk key has an implementation hard limit of 128 MiB. The
root as a whole stores at most 65,536 committed plus reserved entries, 128
namespaces, 32 concurrent durable reservations, and 4 GiB of charged entry-file
bytes. The charge includes each entry header, key, and value. Filesystem
allocation-unit and directory metadata overhead is not measurable through this
portable Win32 path, but its object growth is bounded by the entry and namespace
caps.

Normal stores use checksummed quota counters. Recovery is also bounded when the
root is malformed: at most 264 root objects (including root metadata and dot
entries, where exposed) and 256 recognized namespace candidates are considered,
and at most 66,049 entry-directory objects and
65,536 recognized entry candidates are considered across the root. A normal
entry mutation scans at most 65,538 objects, while stale-temporary cleanup scans
at most 34. Exceeding a bound fails closed with `DXGI_ERROR_CACHE_FULL`.

Every disk-session open also performs a bounded inventory of root namespace
objects. Missing or untracked namespace directories force reconstruction before
a new directory can be created, preventing repeated failed opens from growing
unaccounted directories. The inventory reserves room for the root-state
temporary file; unsafe or excessive objects fail closed rather than expanding
the cache boundary.

The root aggregate quota includes outstanding cross-process reservations. A
store durably reserves aggregate space before publishing its entry, then
commits or releases that reservation. Recovery uses the reservation byte-range
locks to distinguish live writers from stale records and discards stale
reservations only after reconstructing actual usage. When necessary it tries
to evict the oldest inactive namespace; a fail-fast exclusive lifecycle lock
ensures an active namespace is never removed. Lookup does not acquire the root
lock.

Lock ordering is root aggregate, namespace lifecycle, namespace operation, then
the in-process memory mutex. The root lock is never acquired while a namespace
operation lock is held; post-write reservation finalization happens after the
operation lock is released. This avoids global serialization of lookup and
payload I/O while maintaining cross-process accounting.

## Provider PSO boundary

Switchyard does not advertise automatic-disk-cache or driver-managed-cache
support from this work. The built-in vkd3d Vulkan pipeline cache is currently
an in-process provider optimization and has no established persistent blob
compatibility, measured host benefit, or complete invalidation identity in this
tree. D3DMetal/GPTK is a closed, user-selected provider reached through the
Agility proxy; Switchyard neither guesses at nor modifies its internal cache.

Consequently, application cache sessions are supported by the built-in vkd3d
path, while transparent provider PSO persistence remains disabled. Enabling it
later requires a provider-owned serialization contract, identity including the
provider ABI, GPU/driver, macOS and provider versions, crash tests, and measured
cold/warm compilation benefit.

## Validation

The focused Wine test is part of `dlls/d3d12/tests/d3d12.c`. Its normal pass
covers memory and disk defaults, persistence, overwrite and collision results,
two-call lookup, version changes, delete lifetime, per-value limits, LRU,
same-process races, working-directory control scope, and malformed arguments.
Interactive mode expands the deterministic fuzz-style pass to 10,000 entries.

The external probe builds and runs as follows:

```sh
./switchyard/tests/d3d12_shader_cache_test.sh /path/to/switchyard-runtime
```

It measures cold store, new-process warm hit and miss, multithread throughput,
peak working set and disk bytes. It also exercises 10,000 disk entries, eight
concurrent processes storing one key, a checksummed synthetic hash collision,
payload corruption, finalized-entry write exclusion, forced termination after
observing a partial 64 MiB temporary write, delete on final
close, version invalidation, and an unsafe cache-root object. Exit status 77 is
an explicit capability skip; it is never reported as a pass.

### Development measurements and limitations

The implementation was measured on 2026-08-03 on an Apple M5 Pro host running
macOS 26.5.2. A GPU-independent backend harness used the same compiled cache
object through Switchyard Wine. Three 10,000-entry runs produced these results;
times are medians, with ranges in parentheses:

| Workload | Result |
| --- | ---: |
| Cold durable stores | 47,388 ms (47,202-48,321 ms) |
| New-session warm hits | 3,192 ms (2,841-3,458 ms) |
| Warm misses | 835 ms (784-849 ms) |
| Eight threads, 16,000 memory-tier hits | 19.3 ms (18.7-20.2 ms) |
| Peak working set | 40.4 MiB (40.2-41.9 MiB) |
| Cache-owned disk bytes after 10,000 stores | 2,539,970 bytes |

The deliberately strong per-entry flush and metadata ordering makes the cold
10,000-entry durability stress expensive; the warm path avoids that work and
normal lookup remains root-lock-free. A four-process, 40,000-store contention
run over 10,000 shared keys produced exactly 10,000 committed entries and
30,000 `ALREADY_EXISTS` results. A deterministic interrupted 256 MiB backend
write recovered with the incomplete key missing, and a 16 MiB value larger
than its memory tier passed the streaming-checksum and second-read path.

The original memory-only implementation was also retained as a comparison
binary and interleaved over seven runs. Median results for old versus current
were 2.860 versus 3.035 ms for 10,000 stores, 1.271 versus 1.438 ms for 10,000
hits, 15.460 versus 16.362 ms for 16,000 eight-thread hits, and 35.25 versus
35.68 MiB peak working set. Memory mode keeps the original 64-bit FNV tree
key; SHA-256 is used only for persistent identities and disk-session tiers.

The configured public D3D12 runtime on this host returned `E_INVALIDARG` from
`D3D12CreateDevice`, so the committed API probe correctly exited 77. Therefore
these measurements do not claim end-to-end shader compilation or PSO stall
improvement, nor real-application compatibility, and `compatibility.md` is not
changed. The pre-change disk mode was unsupported, so there is no valid disk
baseline. ASan, UBSan, and TSan runtimes were unavailable for the MinGW PE
target, and Instruments could not exercise a D3D12 device. Warning-strict
dual-architecture builds, GCC `-fanalyzer`, bounded fuzz-style tests,
multi-process stress, large-value verification, forced interruption, and
manual leak/error-unwind and lock-order audits were used as the strongest
available substitutes. Transparent provider PSO persistence remains disabled
because no safe provider blob contract or measured compilation benefit was
available.
