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

This verifies the pinned upstream ancestry, imported patch trailers, whitespace, and the absence of obvious proprietary runtime artifacts.

Inspect the source identity that would be written to a runtime manifest with:

```sh
./switchyard/build_runtime.sh --source-info
```

## Build

```sh
./switchyard/build_runtime.sh
```

The builder downloads and verifies Wine Mono and required open-source Homebrew bottles into user-local caches. Cached dependency trees are accepted only when their complete file and symbolic-link digest still matches. The runtime is assembled and verified in a sibling staging directory, then atomically swapped into `~/.switchyard/runtimes/`; an interrupted build cannot mutate the active runtime. The resulting `switchyard-runtime.json` records source, dependency, and core-binary integrity metadata.

An existing custom install prefix is replaced only when it is a child of Switchyard's managed runtime root or contains a valid Switchyard runtime manifest for that exact path. Existing cache directories require a regular Switchyard ownership marker; an intact full-content digest is required for reuse, while an owned cache with damaged content is safely replaced. The builder refuses symbolic-link destinations, unmanaged directories, the managed root itself, the home directory, and dangerous ancestor paths.

The optional `GPTK_PATH` variable may point to a user-selected local Game Porting Toolkit installation. The builder fingerprints and overlays that local input; it never downloads or publishes GPTK.

Useful overrides include:

- `JOBS`: build parallelism; defaults to one fewer than the machine's logical CPU count;
- `WINE_BUILD_DIR`: out-of-tree build directory;
- `WINE_INSTALL_PREFIX`: explicit installation directory;
- `RECONFIGURE=1`: rerun Wine configuration;
- `GPTK_PATH`: user-selected local GPTK path; and
- `SWITCHYARD_TLS_SOURCE_PREFIX`: user-local x86_64 Wine prefix containing a compatible GnuTLS dependency closure.

To reuse a complete runtime with exactly matching inputs:

```sh
./switchyard/build_runtime.sh --ensure
```

Plain `build` produces an artifact without changing the Switchyard application's selected Wine path. `--ensure` is the only mode that updates that preference after a complete matching runtime has been verified or built.

No prebuilt Switchyard Wine runtime is published until its corresponding source, dependency notices, packaging, signing, and notarization workflow have been verified.

The destructive-promotion guard has a local macOS regression test:

```sh
./switchyard/tests/directory_safety_test.sh
```
