# The host: what `client.dll` needs from the process around it

`client.dll` is the game; `launcher.exe` is the process that loads it. Our host
(`src/runtime/host.c`) stands in for the launcher. This page is what that job
turned out to require, found on the final client (`MxO_7.6005`, base
`0x62000000`). Retail addresses differ; the mechanisms are the same.

## The contract: `InitClientDLL` takes eight arguments

Read from the launcher's call at `0x0040A57A`, cdecl, returns **1 on success**,
0 or a negative code on failure (`-1` bad command line, `-2`… per subsystem).

| # | launcher source | what the client does with it |
|---|---|---|
| 0 | `[0x4D2C5C]` filtered argc | command-line parse (`0x622A3D10`) |
| 1 | `[0x4D2C60]` filtered argv | same |
| 2 | `LoadLibrary("client.dll")` | window `hInstance` |
| 3 | `LoadLibrary("cres.dll")` | `LoadStringW` for the window title |
| 4 | `[0x4D6304]`, a 0xB4-byte launcher object | stored at `0x62B073E4`, used by the frame loop |
| 5 | `[0x4D2C58]`, the launcher's login object | `register(obj, "ILTLoginMediator.Default", 1)` |
| 6 | packed word from launcher fields `+0xA8`/`+0xAC` | unknown |
| 7 | flag byte `[0x4D2C69]` | unknown |

**eax=0 is not success.** Earlier work read it as LithTech's `LT_OK` and
concluded init completed. It had failed on its fifth check with "Could not
create application window." — the reason is in the buffer at `0x629DDE88`
(`ErrorClientDLL` returns it), and the client also writes `ClientErrors.txt`
in the working directory. **Read both after every run.**

`SetMasterDatabase` is exported but the launcher never calls it (the string is
present, unreferenced).

## The interface database binds at registration

`0x629F14A0` is a singleton with three intrusive smart-pointer slots; `+0x18`
is the live database. `register(obj, name, tag)` (`0x6229D2A0`) calls
`db->vt[4]`, which finds or creates the entry for `(name, tag)`, appends `obj`,
and **notifies every listener** on that entry. Consumers are listeners built by
static constructors (e.g. `0x627C3A10` binds `&g_pNetMgr` at `0x629DF7D0` to
`"INetMgr.Default"`). So binding is immediate, in either order.

The "28 holders" in `analysis/engine_interfaces.json` are mostly **the client's
own implementations** — `0x62999968` is a static `CNetMgr` (class name follows
its vtable at `0x628AF9D0`). They looked empty because their `Init` had not
run, not because an engine was missing. `MXO_IFACES` should stay unset.

## The window comes from MSLU, not the import table

The client statically links the Microsoft Layer for Unicode. Each W-function
is a function-pointer variable whose initial value is a stub:

```
push fallback ; push [x] ; push &ptrvar ; push "CreateWindowExW" ; push "user32.dll" ; call loader
```

The loader walks the real DLL's export table by hand, via the `GetModuleHandleA`
/ `LoadLibraryA` we decline — so every variable resolved to the failure stub.
`shims_mslu.c` scans for that byte pattern at startup (36 found: 21 user32,
15 kernel32) and pre-fills each variable with a synthetic VA (`0x7D000000`+)
that routes to a host shim. Unshimmed names report and stop.

The window procedure is a guest address, so `RegisterClassExW` registers a
host thunk (`guest_wndproc`) that pushes the four arguments and re-enters the
lifted procedure.

## Native passthrough: real COM on the simulated stack

`native_call.c`. When lifted code calls a code address inside a module Windows
loaded (a D3D9 vtable slot, a `GetProcAddress` result), point the real ESP at
the simulated stack and `call` it. The arguments are already where the callee
expects them, and a stdcall callee's `ret N` pops them — **the ESP it returns
with measures the arity**. No SDK table, no per-method shim.

```
  [native] -> 6ADE5430 this=08F1D0E0 slot=10 esp=0E3FFA40
  [native] <- eax=00000000 popped=7          # CheckDeviceFormat: this + 6
```

`RajkoD3DCreate9` (the community `r3d9.dll`'s name for `Direct3DCreate9`,
called with `0x20`) and `DirectInput8Create` forward to the system DLLs this
way. `MXO_NATIVE_TRACE=1` logs every such call.

### The stack-collision rule

While a native call runs, **the real stack is the simulated stack**. If it
calls back into guest code (D3D's `CreateDevice` and `Release` send window
messages synchronously), the guest frame must start well below the real ESP:

- Pushing at `g_esp` put guest frames on top of D3D's → `/GS` cookie →
  `0xC0000409`, no output.
- A 0x400 gap was not enough: lifted C runs on the real stack too, and
  `DefWindowProcW` under it went deep enough to overwrite the guest's saved
  EBX/ESI/EBP (`WM_STYLECHANGED`, esi `0BBF7AC0 -> 00000004`, then a write
  through `this=4`). The gap is now 256 KB per nesting level.

`guest_wndproc` reports any callee-saved register that changes across a
callback. That report is what found this.

## CRT forwards and the MSVCR71 differences

- **`time_t` is 32 bits in MSVCR71, 64 in the host CRT.** `_ftime` wrote a
  16-byte `__timeb64` into a 12-byte `_timeb`; `dstflag = 1` landed on a saved
  EBP (`ebp=0E3F0001`). All time shims use the `_32` forms.
- **UCRT fail-fasts on invalid parameters** (`0xC0000409`, bypasses the VEH,
  loses buffered output). The host installs an invalid-parameter handler and
  runs stdout unbuffered.
- `_vsnprintf` is not forwarded: the `va_list` is a guest pointer the shared
  formatter walks directly. Counted forms return **-1 on truncation**; the
  client's wrapper at `0x62005B40` depends on it.

## Running

Run from the game directory — resources are found relative to it, and with the
repo root as the working directory init fails with "Initialization of the
resource manager has failed!":

```
cd game_final && ../build/mxo_final.exe client.dll --init
```
