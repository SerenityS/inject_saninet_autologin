# saninet_autologin

## Overview

`saninet_autologin` is a DLL + injector pair for XSanity that automatically opens the SaniNet login window and submits the saved 4-digit PIN once the game reaches the ready/online state.

The current build is designed around the current `Program64\XSanity.exe` binary and uses hardcoded internal addresses discovered in Ghidra.

## Expected folder layout

Put these files in the same `Program64` folder:

- `XSanity.exe`
- `saninet_injector.exe`
- `saninet_autologin.dll`
- `pin.txt`

Example:

```text
Program64\
  XSanity.exe
  saninet_injector.exe
  saninet_autologin.dll
  pin.txt
```

## Build

From the project root:

```powershell
.\build.ps1
```

Optional:

```powershell
.\build.ps1 -Configuration Debug
```

## Runtime behavior

### Injector

The injector:

- looks for `pin.txt` in the current working directory first
- falls back to the injector EXE directory when needed
- reuses the saved PIN if it is valid
- prompts once if the PIN is missing or invalid, then saves it back to `pin.txt`
- launches `XSanity.exe` if it is not already running
- waits for the newest `XSanity.exe` process to stabilize before injecting

That last point matters because XSanity behaves like StepMania and may show a loading/bootstrap phase before the final game process settles.

### DLL

The DLL:

- reads `pin.txt`
- polls SaniNet ready/alive state
- sends the internal `0x90` key path to open the login screen
- waits for `SaniNetLoginMenu`
- writes the PIN into the real in-game PIN buffer
- calls the internal submit function

## Usage

From the same folder that contains `XSanity.exe`:

```powershell
.\saninet_injector.exe
```

Optional:

```powershell
.\saninet_injector.exe --pin 1234
.\saninet_injector.exe .\saninet_autologin.dll --pin 1234
```

## Files used at runtime

- `pin.txt`
  - stored in the working folder / game folder
  - accepts either plain `1234` or `pin=1234`
- `saninet_trace.log`
  - created by the DLL in the working folder
  - used for troubleshooting

## Current known-good behavior

- auto login works without pressing F12 manually
- the login window still appears briefly, then the PIN is submitted automatically
- returning to the main menu triggers the login flow again when the game becomes ready
- exit-time access violations were mitigated by restoring the original hook bytes during detach

## Limitations

- this build is tied to the current XSanity executable layout
- if `XSanity.exe` changes, internal addresses may need to be updated
- the injector and DLL assume the game remains 64-bit
- antivirus or OS protections can still interfere with DLL injection

## When the game updates

Use [GHIDRA_UPDATE_CHECKLIST.md](D:/XSanity/saninet_autologin/GHIDRA_UPDATE_CHECKLIST.md).

The minimum values that usually need updating are:

- `kRvaHandleMessage`
- `kRvaHandleInput`
- `kRvaSaniNetGlobal`
- `kRvaAssignString`
- `kRvaSubmitPin`
- `kRvaIsAlive`
- `kRvaIsLoginReady`

## Troubleshooting

### Injector says the game executable was not found

Run the injector from the same folder that contains `XSanity.exe`, or place the injector in `Program64` and launch it there.

### Injector launches the game but injection does not work

This can happen if the game respawns itself during startup. The current injector waits for the newest `XSanity.exe` PID to stabilize before injecting, which is the intended fix for that behavior.

### The game updates and auto-login stops working

The internal addresses likely changed. Re-run the update workflow in [GHIDRA_UPDATE_CHECKLIST.md](D:/XSanity/saninet_autologin/GHIDRA_UPDATE_CHECKLIST.md).

### Login succeeds but the game crashes on exit

That usually means the detour or detach path regressed. Check the current hook cleanup logic in [saninet_autologin.cpp](D:/XSanity/saninet_autologin/saninet_autologin.cpp).
