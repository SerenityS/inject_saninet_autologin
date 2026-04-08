# Ghidra Update Checklist

Use this document whenever a new `XSanity.exe` build ships and auto-login stops working.

## Goal

Recover the internal addresses and field offsets needed by `saninet_autologin` without redoing the entire reverse-engineering process from scratch.

## Current feature model

The current implementation depends on these behaviors:

1. Detect when SaniNet becomes alive/ready.
2. Trigger the internal `0x90` path that opens the login window.
3. Wait for the `SaniNetLoginMenu` screen message.
4. Write the saved PIN into the real login buffer.
5. Call the internal submit function.

That means the most important things to recover after an update are:

- the message handler used by the login screen
- the input handler that accepts internal key events
- the SaniNet singleton/global
- the string helper used to write into the game's string object
- the submit function
- the two SaniNet status checks: alive and login-ready
- the login screen field offsets if they changed

## Current address set

These are the values used by the current working build. Treat them as the baseline to map old -> new.

- `kRvaHandleMessage = 0x006D8F80`
- `kRvaHandleInput = 0x006DA000`
- `kRvaSaniNetGlobal = 0x00B91900`
- `kRvaAssignString = 0x001F6880`
- `kRvaSubmitPin = 0x00740490`
- `kRvaIsAlive = 0x0073AC20`
- `kRvaIsLoginReady = 0x0073AD80`

Known screen offsets in the current build:

- `self + 0x3269` = login active flag
- `self + 0x3270` = PIN string buffer
- `self + 0x3280` = PIN string length

## Fast recovery order

Do these in order. It is the quickest path back to a working build.

1. Recover `SubmitPin`
2. Recover `HandleInput`
3. Recover `HandleMessage`
4. Recover `SaniNetGlobal`
5. Recover `IsAlive` and `IsLoginReady`
6. Re-check the login screen field offsets

## 1. Recover `SubmitPin`

Search strings:

- `LoginState`
- `State`

Look for a function that:

- builds a JSON/object packet
- writes `"State" = 2`
- writes another key containing the PIN string argument
- queues or sends the packet through the SaniNet transport path

That function is the new `SubmitPin`.

## 2. Recover `HandleInput`

Search strings:

- `SaniNetLoginNumPad`
- `SaniNetLoginScreen`

Look for a function that:

- branches on key codes like `0x30..0x39`
- appends digits into a screen-owned string buffer
- clears the buffer on another code
- checks the PIN length
- calls `SubmitPin` once length reaches 4
- treats `0x90` as the login/open trigger path

That function is the new input handler.

This function is also where you re-validate:

- login active flag
- PIN string buffer
- PIN length field

## 3. Recover `HandleMessage`

Search strings:

- `SaniNetLoginMenu`
- `SaniNetPinSuccess`
- `SaniNetPinFailure`

Look for a screen message handler that:

- receives those messages
- updates login UI state
- resets or updates the PIN buffer
- flips the login active flag

That function is the new login screen message handler.

## 4. Recover `SaniNetGlobal`

The easiest way:

- open the new `SubmitPin`
- inspect call sites and first argument usage
- identify the `DAT_xxx` singleton/global passed as the SaniNet object

That global is the new `kRvaSaniNetGlobal`.

## 5. Recover `IsAlive` and `IsLoginReady`

Open the input handler or any SaniNet UI gatekeeper and look for simple boolean checks on the SaniNet global.

Expected pattern:

- one helper says whether SaniNet is connected/alive
- another says whether the login path is ready/openable

In the last known working build these were discovered from the input handler path around the `0x90` handling logic.

## 6. Re-check field offsets

Use the recovered input handler to confirm:

- login active flag offset
- PIN string offset
- PIN length offset

Typical pattern:

- a digit append helper writes into `self + ???`
- a length comparison (`> 3` / `>= 4`) reads from `self + ???`
- login screen open/close logic toggles a nearby boolean flag

Do not assume these offsets stayed the same after a large game update.

## Validation checklist

Before changing code, confirm all of these:

- internal `0x90` still opens the login path
- `SubmitPin` still uses `State = 2`
- `HandleMessage` still receives `SaniNetLoginMenu`
- `HandleInput` still uses the same screen object type
- PIN buffer writes still happen through the same string helper semantics

## Code locations to update

After recovery, update constants in:

- [saninet_autologin.cpp](D:/XSanity/saninet_autologin/saninet_autologin.cpp)

Primary values to update:

- `kRvaHandleMessage`
- `kRvaHandleInput`
- `kRvaSaniNetGlobal`
- `kRvaAssignString`
- `kRvaSubmitPin`
- `kRvaIsAlive`
- `kRvaIsLoginReady`

Re-check screen offsets where used.

## Recommended workflow after an update

1. Open the new `XSanity.exe` in Ghidra.
2. Find `SubmitPin` first.
3. Use it to anchor `SaniNetGlobal`.
4. Find `HandleInput` through `SaniNetLoginNumPad` / `0x90` logic.
5. Find `HandleMessage` through `SaniNetLoginMenu` handling.
6. Update constants in code.
7. Build.
8. Test these cases:
   - cold launch
   - auto-login on first ready state
   - returning to the main menu and auto-login again
   - clean process exit without access violation

## If the game now respawns processes differently

The injector already accounts for a StepMania-style startup where an initial process may hand off to a newer one.

If startup behavior changes again:

- inspect whether `XSanity.exe` respawns itself or launches a child process
- if needed, adjust the injector to attach to the newest stable `XSanity.exe` PID
- do not assume the first observed PID is the final gameplay process

Relevant file:

- [saninet_injector.cpp](D:/XSanity/saninet_autologin/saninet_injector.cpp)

## Long-term hardening

Hardcoded RVAs are fast to update but fragile.

If you want to reduce future maintenance, move these to signature scanning first:

- `HandleMessage`
- `HandleInput`
- `SubmitPin`
- `IsAlive`
- `IsLoginReady`
- `SaniNetGlobal` reference path

That is the highest-value next step once the current build is stable.
