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

## Guest threads are fibers

`guest_threads.c`. Lifted code keeps the CPU in process globals, so a second
OS thread running it would corrupt the first. Each guest thread is instead a
fiber on the one host thread, with its own simulated stack, TIB (so its own SEH
chain) and saved copy of the registers, x87 stack, MMX and `fs` base.

Switching is cooperative, at the calls that would block: `Sleep`,
`WaitForSingleObject`, `select`. Each polls, yields round-robin, and throttles
with a 1 ms real wait per round. With no other runnable fiber they block for
real, so a single-threaded run behaves exactly as before.

- `TlsAlloc`/`TlsGetValue`/… map to FLS, which Windows keeps per fiber.
- The thread handle the client holds is a manual-reset event set on exit, so
  `WaitForSingleObject(thread)` and `GetExitCodeThread` work unchanged.
- The native passthrough's TEB swap uses the current fiber's stack bounds.
- Critical sections stay real. All fibers share one OS thread, so they never
  contend. Mutual exclusion holds unless a fiber yields while holding one.
- `recvfrom` on a blocking socket would still block every fiber. The client
  calls `select` first.
- `RaiseException` accepts only `0x406D1388` (MSVC's SetThreadName idiom at
  `0x623719E8`) and reports anything else.

First run: thread 1 (`0x62371AC0`) is created suspended during render init,
resumed, runs, and exits with code 0.

## Where init stops now: the login mediator

`InitClientDLL` returns **-7** from `0x62003380`. `CNetMgr::Init` succeeds
(`WSAStartup` is its transport's first call), and then the step requires
`[0x629DF7F0]`, bound to **`ILTLoginMediator.Default`**, to exist and its
`vt[4]` to return true. That object is argument 5 of `InitClientDLL`, and the
host passes NULL.

`launcher.exe` uses the same interface database. A static initialiser at
`0x00494AB0` binds its own `[0x4D2C58]` to `"ILTLoginMediator.Default"` (tag 1),
and the launcher calls it (`vt[7]`, `vt[9]`, `vt[2]`) before it loads
`client.dll`. So login, and the TCP auth the emulators speak, lives in the
launcher, and the client receives the result as this object.
`InitClientDLL` also calls its `vt[4]`, `vt[22]`, `vt[23]` and `vt[24]` for the
startup banner.

## The login mediator stand-in

`login_mediator.c`. The launcher's `CLTLoginMediator` (static object
`0x004D4E38`, vtable `0x004B01C8`, 101 slots) is almost entirely accessors on
one large state object. The stand-in is that object with the launcher's
layout, zeroed, plus a table that transcribes each accessor the client calls
(byte, dword, address, a read through the selected-world record at `+0x680`,
or out-parameter-and-return). Each table entry names the launcher method it
mirrors. Slots with logic, most of which go through a login state machine at
`+0x10`, report their number and stop when first reached.

The initial state is "launched by the launcher" (`+4 = 1`), with an empty
world record and no state machine. Slot 0 returns the class name, as every
class's slot 0 does (`CNetMgr`'s returns `"CNetMgr"`). With it in place, the
-7 is gone.

## Whole DLLs bound natively

`bind_native_dlls()` in `host.c` writes real export addresses into the IAT for
the DLLs listed in `MXO_NATIVE_DLLS` (default `binkw32.dll`), loaded from the
game directory. Calls then go through the native passthrough. Nothing else
works for Bink: the client passes `BinkOpenDirectSound`, read from the IAT, to
`BinkSetSoundSystem` as a callback. `CoCreateInstance` is forwarded the same
way. The client creates DirectMusic's loader and performance and one more
audio class; each CLSID is logged.

## Windowed

The game's own switch: `Windowed = 1` in `autoexec.cfg` (shipped commented
out). Verified at `CreateDevice`: `D3DPRESENT_PARAMETERS.Windowed = 1`.

## A lifter bug: `repe cmpsb` never set CF

MSVC inlines `memcmp` as `repe cmpsb` followed by `sbb ebx, ebx; sbb ebx, -1`
(the -1/0/+1 sign idiom), and the `sbb` reads CF directly. The toolbox lifter
(`pcrecomp/tools/lift/lift32.py`) recorded the last compared pair for the lazy
flags but never assigned `_cf`, so the `sbb` saw whatever the previous
instruction had left, usually 0. Every pair of differing strings then compared
as "greater" and never "less", and an STLport `map<string, …>` returned its
first node for **any** key.

How it showed up: the UI layout lookup returned the same widget for
`MT_Image`, `MT_Title` and `MT_Description`, and the MouseText cursor later
dereferenced a widget member that was never a real widget. The fix adds
`_cf = CMP_B(a, b)` to the `cmps`/`scas` block. It affects **540** sites in the
final client. The toolbox's differential tester (Unicorn as reference) had
`repe cmps` cases but none that followed the loop with an `sbb`. It now has
two, and without the fix they fail:

```
              ebx  lifted 00000000   cpu FFFFFFFF
               CF  lifted        0   cpu        1
2/3 match hardware, 0 known divergences, 1 failures
```

With the fix: `115/125 match hardware, 10 known divergences, 0 failures`.

## Preemption points, and critical sections owned per fiber

Yielding only at blocking calls starved every guest thread: a game loop paces
itself by polling the clock and the message queue. In a 41M-call init the
resource prefetch thread (`CRezMgr_PreFetch_Loader_Thread`, as it logs itself)
and both audio streamers never ran once. `timeGetTime`, `GetTickCount`,
`QueryPerformanceCounter` and `PeekMessageW` now yield, at most once every
2 ms.

That makes the real `CRITICAL_SECTION` useless: every fiber is the same OS
thread, and a critical section is reentrant for its owner. So ownership is
tracked per fiber in the client's own struct (`OwningThread` = fiber + 1,
`RecursionCount`), and a contended `Enter` yields until the section is free.
The real API is never called.

## Diagnostics worth knowing

| switch | what it gives |
|---|---|
| `LogToFile = 8` in `autoexec.cfg` | the client's full log in `ClientErrors.txt` (its own console variable, level ≥ 7 logs everything) |
| `MXO_FILE_TRACE=1` | every `fopen`/`_fsopen` and whether it succeeded |
| `MXO_NATIVE_TRACE=1` | every native call, with the argument count it popped |
| `MXO_RING=N` (traced build) | the last N functions entered at a fault. This is entry order across all fibers, not a call stack |
| fault report | the vtable behind each pointer register, which names the object |

A temporary `printf` in the generated C of one function is also fair game: the
tree is regenerated by every lift. It is how the lookup bug was pinned down.

## Guest entry through DEP faults: native code calling the client

`guest_call.c` is the counterpart of the native passthrough. A real DLL calls
a function pointer the client gave it: Python calls the client's extension
functions and type slots, and msacm32 calls an enumeration callback. Those
are guest addresses, which the CPU can't run. Instead of rewriting every
such pointer, the guest image is mapped **non-executable** after loading. A
native jump into it raises a DEP fault, and a vectored handler redirects to a
thunk. The thunk copies the caller's arguments onto the simulated stack
(starting 256 KB below the real ESP, the stack-collision rule), runs the lifted
function, and pops however many arguments that function's own `ret N` popped.

With that in place, `pythonMXO.dll` and `msacm32.dll` are bound natively
alongside `binkw32.dll`, and the game's own rule scripts run in the real
interpreter:

```
SP: Abilities System Functioning
-- Loaded CLHooks
  [mediator] listener 629DDFC8 registered
  [mediator] listener 6298A760 registered
returned, eax=0x00000001  (esp=0x0E3FFF00, 62263380 indirect calls)
```

**`InitClientDLL` returns 1.**

## Two more lifter bugs, both in MSVC's float-compare idioms

Both hung the frame loop inside the same texture-scroll function
(`0x624AA400`), which wraps UV offsets into [0, 1) with
`while (x >= 1) x -= 1` and `while (x < 0) x += 1`. The watchdog showed it:
`cur_func` stayed at `0x624AA400` and the indirect-call count stopped moving.

- **Unordered x87 compares.** A NaN operand sets C3, C2 **and** C0
  (`0x4500`). The lifter modelled only the three ordered outcomes, so NaN read
  as "equal" (`0x4000`). `FPU_CMP()` now returns 2 for unordered: `fnstsw`
  maps it to `0x4500`, and `fcomi`/`sahf` conditions see ZF=PF=CF=1. This
  affects 3,911 compare sites.
- **Parity was a constant.** `#define CMP_P(a, b) 0 /* TODO: parity */`.
  MSVC tests `x < 0.0` as `fnstsw ax; test ah, 5; jp`, so `jp` was never taken
  and `jnp` always was, and `while (x < 0) x += 1` never exited. PF is now the
  parity of the result's low byte, rebuilt per setter from the captured operands.
  `_flag_capture` records the width because it left-aligns narrow operands.
  This affects 2,685 sites.

Each has a differential test that fails without the fix and passes with it.
Toolbox suite: **118/129 match hardware, 11 known divergences, 0 failures**.
The one new known divergence is documented: the *materialized* EFLAGS still
rebuild PF without the width, which only a `pushfd` would see.

## TCP exists

`WS2_32` imports `connect`/`send`/`recv`; earlier notes looked only at
`WSOCK32` and concluded UDP-only. The frame loop's first network act is an
HTTP fetch from `ad.thematrixonline.net:80` over a non-blocking socket. The
shims redirect it to the loopback stand-in, log it, and wait cooperatively in
`recv`. With nothing listening, the client keeps retrying `connect` and carries
on.

## Watchdog and deadline

- `MXO_WATCHDOG=N` starts a real OS thread that prints `cur_func`, `esp`, the
  indirect-call count and the last native target every N seconds. A frozen
  count with a fixed `cur_func` is a loop in lifted code.
- `MXO_RUN_SECONDS=N` ends a run after N seconds with the normal report.
  `RunClientDLL` never returns on its own.

## What is on screen

The window is black, and `MXO_NATIVE_TRACE` shows why. Counted by
`IDirect3DDevice9` slot over a 90 s `--boot`:

| phase | Present (17) | Begin/EndScene (41/42) | DrawPrimitiveUP (83) |
|---|---:|---:|---:|
| `InitClientDLL` (splash, intro) | 945 | 949 | 1,117 |
| `RunClientDLL` frame loop | 281 (a second object) | 0 | 0 |

So D3D9 works end to end and the intro draws through it. The frame loop then
presents empty frames at about 4–5 per second. The client log stops at
"Splash View Stepped 16 times.", so the front end is waiting to leave the
splash. The likely trigger is login state: the mediator stand-in has no login
state machine at `+0x10`, and the launcher's version drives the client
forward. Next thread.

## Running

Run from the game directory — resources are found relative to it, and with the
repo root as the working directory init fails with "Initialization of the
resource manager has failed!":

```
cd game_final && ../build/mxo_final.exe client.dll --init
```

When it faults, the host flushes every open stream first (so `ClientErrors.txt`
survives) and prints the first word behind each pointer register. That word is
usually a vtable, which identifies the object.
