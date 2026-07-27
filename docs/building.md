# Building the runtime

Switchyard Wine is built on Apple Silicon as an x86_64 macOS host runtime with i386 and x86_64 PE support. The project leaves one CPU core free by default.

## Requirements

- macOS 14 or later on Apple Silicon
- Xcode Command Line Tools
- Rosetta 2
- Homebrew
- `bison`, `flex`, `pkgconf`, and `mingw-w64`

Install the Homebrew dependencies with:

```sh
brew install bison flex pkgconf mingw-w64
```

## Verify the source history

```sh
./switchyard/verify_source.sh
```

This verifies the pinned upstream ancestry, whitespace, and the absence of obvious proprietary runtime artifacts.

Inspect the source identity that would be written to a runtime manifest with:

```sh
./switchyard/build_runtime.sh --source-info
```

## Build

```sh
./switchyard/build_runtime.sh
```

The builder downloads and verifies Wine Mono, required open-source Homebrew bottles, a pinned x86_64 GnuTLS dependency closure, a pinned i386/x86_64 Mesa Windows OpenGL package, and the pinned redistributable Noto font set into user-local caches. GnuTLS packages come from conda-forge; the legacy libunistring ABI is rebuilt from pinned GNU source so the resulting library can be Developer ID signed and translated reliably by Rosetta. Cached dependency trees are accepted only when their complete file and symbolic-link digest still matches. The runtime is assembled and verified in a sibling staging directory, then atomically swapped into `~/.switchyard/runtimes/`; an interrupted build cannot mutate the active runtime. The resulting `switchyard-runtime.json` records source, dependency, font-asset, Mesa, and core-binary integrity metadata.

The font set supplies regular faces for every Noto family referenced by Wine's DirectWrite fallback table, common bold faces, symbols, and the Japanese, Korean, Simplified Chinese, Traditional Chinese, and Hong Kong faces from Noto Sans CJK. The unmodified files are installed in `share/wine/fonts`, so they are visible to every prefix without copying fonts into `C:\\Windows\\Fonts`. Their pinned URLs and SHA-256 values live in `switchyard/font-assets.tsv`; SIL Open Font License 1.1 notices are copied into the runtime.

Validate the manifest without network access, or verify every downloaded font and its family metadata, with:

```sh
./switchyard/verify_font_assets.sh
./switchyard/verify_font_assets.sh --download
```

Verify the pinned Mesa archive, both PE architectures, hashes, and license notices with:

```sh
./switchyard/build_runtime.sh --verify-mesa
```

An existing custom install prefix is replaced only when it is a child of Switchyard's managed runtime root or contains a valid Switchyard runtime manifest for that exact path. Existing cache directories require a regular Switchyard ownership marker; an intact full-content digest is required for reuse, while an owned cache with damaged content is safely replaced. The builder refuses symbolic-link destinations, unmanaged directories, the managed root itself, the home directory, and dangerous ancestor paths.

The optional `GPTK_PATH` variable may point to a user-selected local Game Porting Toolkit installation. The builder fingerprints and overlays that local input; it never downloads or publishes GPTK.

Set `SWITCHYARD_DISABLE_GPTK_OVERLAY=1` when preparing a redistributable Wine-only build. This explicit mode ignores both `GPTK_PATH` and the path saved in Switchyard preferences, records an empty GPTK path with the `no-gptk` digest, and leaves the Wine-built graphics modules in place. A release workflow must still verify notices, signing, notarization, and LGPL replacement requirements before publishing the resulting runtime.

Useful overrides include:

- `JOBS`: build parallelism; defaults to one fewer than the machine's logical CPU count;
- `WINE_BUILD_DIR`: out-of-tree build directory;
- `WINE_INSTALL_PREFIX`: explicit installation directory;
- `RECONFIGURE=1`: rerun Wine configuration;
- `GPTK_PATH`: user-selected local GPTK path; and
- `SWITCHYARD_DISABLE_GPTK_OVERLAY=1`: force a Wine-only build without reading or copying GPTK;
- `MESA_WINDOWS_CACHE_DIR`: cache for the pinned Mesa Windows package and notices;
- `MESA_WINDOWS_DEPS_PREFIX`: verified staging prefix for the i386/x86_64 Mesa DLLs;
- `FONT_ASSET_DOWNLOAD_CACHE_DIR`: cache for the verified redistributable font files;
- `TLS_PACKAGE_CACHE_DIR`: cache for the pinned, hash-verified x86_64 TLS packages and source archives.

To reuse a complete runtime with exactly matching inputs:

```sh
./switchyard/build_runtime.sh --ensure
```

Plain `build` produces an artifact without changing the Switchyard application's selected Wine path. `--ensure` is the only mode that updates that preference after a complete matching runtime has been verified or built.

Publish a prebuilt Switchyard Wine runtime only after its corresponding source,
dependency notices, packaging, signing, and notarization workflow have been
verified.

For a release build, first build a clean Wine-only runtime and then create a
Developer ID signed, optionally notarized archive:

```sh
SWITCHYARD_DISABLE_GPTK_OVERLAY=1 ./switchyard/build_runtime.sh
./switchyard/release_runtime.sh \
  --runtime ~/.switchyard/runtimes/<runtime-id> \
  --output ~/Desktop/switchyard-runtime-release \
  --identity "Developer ID Application: ..." \
  --notary-profile switchyard-notary
```

The release script refuses dirty source builds, GPTK overlays, missing license
or corresponding-source notices, unexpected signing teams, and runtimes that
cannot start a fresh Wine prefix. The ZIP itself cannot carry a stapled ticket;
the generated release manifest records the accepted notarization submission and
the app verifies the archive digest plus the Developer ID signed Mach-O files.

The destructive-promotion guard has a local macOS regression test:

```sh
./switchyard/tests/directory_safety_test.sh
```

After building, exercise the runtime-local GnuTLS closure through Wine's WinHTTP
implementation with:

```sh
./switchyard/tests/runtime_tls_smoke_test.sh ~/.switchyard/runtimes/<runtime-id>
```

Verify that WoW64 processes receive the upper 2 GiB of virtual address space by
default and retain the legacy limit when `WINE_LARGE_ADDRESS_AWARE=0` is set:

```sh
./switchyard/tests/wow64_large_address_aware_test.sh \
  ~/.switchyard/runtimes/<runtime-id>
```

Verify that handled assertion exceptions remain trace-only while unhandled
assertions retain their error diagnostic:

```sh
./switchyard/tests/assertion_logging_test.sh \
  ~/.switchyard/runtimes/<runtime-id>
```

Verify that heap warning diagnostics remain observational while the heap trace
channel still opts into full validation:

```sh
./switchyard/tests/heap_warning_flags_test.sh \
  ~/.switchyard/runtimes/<runtime-id>
```

Verify that the default Wine OpenGL driver remains unchanged and that the
explicit Mesa selection provides OpenGL 4.3 or newer through llvmpipe for both
PE architectures:

```sh
./switchyard/tests/mesa_opengl_backend_test.sh \
  ~/.switchyard/runtimes/<runtime-id>
```

At launch time, set `WINE_OPENGL_DRIVER=llvmpipe` in a container's
environment overrides to select the software OpenGL backend for its complete
Windows process tree. Use `WINE_OPENGL_DRIVER=wine`, or leave it unset,
to retain Wine's built-in macOS OpenGL path.

GPTK runtimes suppress `MTL_HUD_ENABLED=1` at launch because the closed-source
Metal HUD can fault on an unregistered native GCD worker under Rosetta and
leave the Wine process in a recursive exception. `D3DM_SHOW_HUD_STATS` remains
untouched. Developers investigating the HUD itself can explicitly restore the
unsafe behavior with `SWITCHYARD_ALLOW_UNSAFE_METAL_HUD=1`; production
containers should leave that override unset.

For a Wine-only runtime using a GPTK selected at launch, verify both the D3D11
native-callback path and the Agility-compatible D3D12/D3DMetal path from fresh
prefixes. Run these commands once for each supported GPTK installation:

```sh
./switchyard/tests/gpu_capability_policy_test.sh
./switchyard/tests/graphics_adapter_identity_test.sh \
  ~/.switchyard/runtimes/<wine-only-runtime-id> "$GPTK_PATH"
./switchyard/tests/native_callback_exception_test.sh \
  ~/.switchyard/runtimes/<wine-only-runtime-id> "$GPTK_PATH"
./switchyard/tests/d3dmetal_dxgi_resource_smoke_test.sh \
  ~/.switchyard/runtimes/<wine-only-runtime-id> "$GPTK_PATH"
./switchyard/tests/d3dmetal_d3d12_smoke_test.sh \
  ~/.switchyard/runtimes/<wine-only-runtime-id> "$GPTK_PATH"
./switchyard/tests/atiadl_provider_policy_test.sh \
  ~/.switchyard/runtimes/<wine-only-runtime-id> "$GPTK_PATH"
./switchyard/tests/amd_umd_loader_policy_test.sh \
  ~/.switchyard/runtimes/<wine-only-runtime-id> "$GPTK_PATH"
```

The D3D12 test also starts a Chromium-style GPU probe and requires it to remain
on the Wine graphics fallback. The identity test first verifies that the
launcher leaves D3DMetal's provider-selected DXGI identity untouched, then
verifies that a complete explicit diagnostic identity is applied coherently.
The ADL and UMD tests verify that an incomplete Radeon driver contract remains
unavailable independently of process name or provider-selected compatibility
identity, while preserving an explicit native DLL override for diagnostics.

Verify that expected missing-file and missing-export probes remain available as
trace diagnostics without being reported as warnings:

```sh
./switchyard/tests/lookup_logging_test.sh \
  ~/.switchyard/runtimes/<runtime-id>
```

Verify that the 32-bit APIs hooked by the Steam overlay retain their supported
hotpatch entry sequence:

```sh
./switchyard/tests/steam_overlay_hotpatch_test.sh \
  ~/.switchyard/runtimes/<runtime-id>
```

For a runtime with the GPTK overlay, exercise D3DMetal's shared DXGI resource
callbacks from a fresh prefix with:

```sh
./switchyard/tests/d3dmetal_dxgi_resource_smoke_test.sh \
  ~/.switchyard/runtimes/<runtime-id>
```

For a Wine-only runtime, pass the selected GPTK path as the second argument and
run the same test once for every supported GPTK installation.
