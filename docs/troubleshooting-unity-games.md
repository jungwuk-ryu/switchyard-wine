# Unity game crash troubleshooting notes

This is a short case study for future Switchyard Wine investigations. A Unity
game exposed two independent failures: an early DbgHelp access violation and a
later D3DMetal host lock abort. The useful lesson is to build a timeline from
several logs instead of treating every disappearance as the same crash.

## Start by isolating the prefix

Record the exact Wine prefix, runtime, game directory, and launch environment
before changing anything. A second copy of the game may be running through
Heroic or another launcher in a different prefix.

- Use read-only process inspection first.
- Never use a broad `pkill wine`, `pkill xdt`, or launcher-name match.
- If a restart is required, target only the intended prefix:

  ```sh
  WINEPREFIX="$prefix" "$runtime/bin/wineserver" -k
  ```

Capture the latest modification time and tail of these files before each run:

- Steam `logs/gameprocess_log.txt` and `logs/console_log.txt`;
- Unity `Player.log`;
- Unity's `Crashes/` directory; and
- the Wine or runner stderr stream.

Steam's process log is especially useful when a game appears to hang or vanish.
Exit `-1073741819` is `0xc0000005`, not a clean exit. In this case,
`Player.log` repeatedly stopped at `TableData.BeforeInit`.

## Failure signatures from this investigation

### D3DMetal native callback failure

The later Unity crash reported `Illegal Instruction (0xc000001d)` in
`libsystem_platform` at `_os_unfair_lock_unowned_abort`. Symbolication led
through `objc_storeWeak` and `IOGPUMetalBuffer` destruction. The relevant
call entered D3DMetal through `ID3D11DeviceChild::SetPrivateData`.

The top-level D3D11 and DXGI objects were already bridged, but objects returned
by `Create*`, `GetBuffer`, and `QueryInterface` kept native child vtables.
Their methods could therefore run with Wine's Windows TEB installed instead of
the native pthread TSD base. Wrap returned COM interfaces recursively, including
the four `ID3D11DeviceChild` methods.

Native Mach-O `__DATA` vtables may not exist in Wine's virtual-memory map.
`NtProtectVirtualMemory` failing does not prove that the host mapping is
read-only. A page-fault-protected direct pointer store is a valid fallback when
the host mapping is writable.

### DbgHelp symbol search failure

The early crash was `0xc0000005` inside
`pe_load_debug_info -> path_find_symbol_file`. Two conditions mattered:

1. module enumeration can present a PE module without `DFI_PE` format data;
2. a managed runtime directory plus an enumerated DLL name can exceed
   `MAX_PATH`, even when the directory alone fits.

The overwritten nonvolatile registers contained UTF-16 filename fragments such
as `er.d` and `ll`. That is a strong signal for a stack string overwrite.
Checking only the initial path was insufficient: `do_searchW` also appended a
wildcard and each directory entry without checking the destination capacity.

## A productive diagnostic loop

1. Establish a fresh launch timestamp and identify which failure happens first.
2. For a Wine exception, run the game once from its own directory while Steam is
   alive and retain only useful SEH lines:

   ```sh
   WINEPREFIX="$prefix" WINEDEBUG=-all,+seh \
     "$runtime/bin/switchyard-wine" ./Game.exe 2>&1 |
     awk '/code=c0000005|Unhandled page fault/{n=30} n>0 {print; n--}'
   ```

3. Subtract the loaded module base from the fault address and resolve the PE
   virtual address against the exact built DLL with
   `x86_64-w64-mingw32-addr2line` and `objdump`.
4. After every patch, reproduce again. A moved fault address often means that
   the first defect was fixed and a second defect is now visible.
5. Confirm that the process loaded the rebuilt binary. Many Wine builtins are
   copied into the prefix:

   ```sh
   shasum -a 256 \
     "$runtime/lib/wine/x86_64-windows/dbghelp.dll" \
     "$prefix/drive_c/windows/system32/dbghelp.dll"
   ```

   Updating only the runtime copy did not update this prefix. Run
   `wineboot -u` with the final runtime, then compare hashes. `ntdll` is
   loaded from the runtime and behaves differently from copied builtins.
6. Validate past both observed windows: at least 30 seconds for the DbgHelp
   failure, at least five minutes for the Metal failure, and preferably ten
   minutes. Require all of the following:

   - the game process still consumes CPU;
   - Steam has no new “no longer tracking” entry;
   - `Player.log` progressed beyond the old stopping point; and
   - no new Unity crash directory appeared.

## False leads and avoidable detours

- The apparent hang was an access violation; Steam's process log exposed it.
- LLDB can intercept intentional Wine/FAudio SEH probes and produce a misleading
  Unity crash directory. Prefer postmortem symbolication and filtered
  `WINEDEBUG=+seh` before attaching a debugger.
- The first DbgHelp null guard moved the crash into path search. That was
  progress, not evidence that the guard caused the failure.
- Guarding a long source path alone did not help. The actual overflow happened
  when a long filename was appended during directory enumeration.
- A newly built runtime DLL may not be active while the prefix still contains
  an older copied builtin.
- `wineserver -w` can wait indefinitely while prefix services remain alive.
  Do not use it as a generic completion check.

## Code map

- `dlls/ntdll/loader.c`: bridge native D3D11/DXGI callbacks and recursively
  wrap returned COM vtables.
- `dlls/dbghelp/pe_module.c`: reject symbol loading without PE format data.
- `dlls/dbghelp/path.c`: bound wildcard and directory-entry appends.

Keep crash artifacts and runtime paths out of the repository. Record only the
signature, source location, relevant hashes, and validation duration.
