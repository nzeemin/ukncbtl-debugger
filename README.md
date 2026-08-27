# ukncbtl-debugger

A console (command-line) debugger for [UKNCBTL](https://github.com/nzeemin/ukncbtl), an emulator
of the Soviet UKNC (УКНЦ, Электроника МС 0511) educational computer. It links against UKNCBTL's
`emubase` emulation core directly, with no GUI dependency, so it builds and runs anywhere a C++17
compiler does — Linux, macOS, and Windows.

The command set is a console port of the table-driven command dispatcher from the WinAPI GUI
debugger, reworked for a plain stdin/stdout session, plus commands that only make sense in a
scriptable tool: save/load state, floppy image attach/detach, ROM cartridge attach/detach,
screenshot export, and screen OCR to text.

The UKNC has two PDP-11-style processors — a central processor (CPU) and a peripheral processor
(PPU). The debugger drives both; the `p` command switches which one the register/step/disasm
commands act on, and the prompt shows the current one (`CPU:` or `PPU:`).

## Why

The GUI debugger is great for interactive work, but it's Windows-only and not scriptable. This
tool exists for the cases where you want to:

- build and run the emulator in a CI pipeline, a container, or any non-Windows environment,
- drive a debugging or reverse-engineering session from a script (feed it commands over stdin,
  capture state, compare runs),
- automate things like "boot this disk image for N frames and read what's on screen" without a
  display — the `screentext` command OCRs the framebuffer back into text.

## Building

Requires a C++17 compiler (GCC and Clang on Linux/macOS; MSVC/Visual Studio 2022 project files
are also included for Windows).

```sh
make            # release build (default) -> build/release/ukncbtldebug
make debug      # debug build -> build/debug/ukncbtldebug
make run        # build (release) and run
make run-debug  # build (debug) and run
make clean      # remove build artifacts
```

On Windows, open `ukncbtldebug.sln` in Visual Studio 2022.

### ROM file

The emulator boots from the UKNC system ROM, expected as `uknc_rom.bin` (32 KB) in the directory
you run the debugger from. If the file is missing or short, the debugger reports it and fails to
initialize rather than crashing.

## Running

```sh
./ukncbtldebug
```

On startup the debugger drops into a command prompt showing the current processor and its PC in
octal, e.g. `PPU:000000>`. Type `h` for the command list.

### Example: run for a while, then read the screen

```
$ ./ukncbtldebug
BKBTL emulator console debugger [...]
Use 'h' command to show help.
PPU:000000> disk1 attach rt11.dsk
Attached disk1: rt11.dsk
PPU:000000> cf400
 Stopped at 104134 (no breakpoint hit after 400 frames)
PPU:104134> i floppy
Floppy engine: ON
  disk1: attached, read-write
  disk2: not attached
  disk3: not attached
  disk4: not attached
PPU:104134> screentext
...recognized screen text...
PPU:104134> screen boot.png
Saved screenshot boot.png
```

## Commands

Single-letter commands never take a space before their argument (`d100260`, `r0=123`); full-word
commands always do (`disasm 100260`, `continue frames 10`). Numeric arguments are octal
throughout, **except** the frame count in `continue frames N`/`cfN`, which is decimal.

Commands that change state (registers, memory, breakpoints, reset, file/disk/cartridge
operations) print a short confirmation line. `disasm`/`d`/`D` and `memory`/`m` page their output:
after a page, the prompt becomes a `-- more --` prompt; pressing Enter shows the next page at the
same address/format/modifiers, and any other input cancels paging (that input is discarded, not
run as a command).

### General

| Command | Description |
|---|---|
| `h`, `help`, `?` | Show the command list |
| `q`, `quit`, `exit` | Quit the debugger |
| `reset` | Reset the machine |
| `p` | Switch the current processor between CPU and PPU |

### Execution control

| Command | Description |
|---|---|
| `c`, `continue` | Continue; free run |
| `cXXXXXX`, `continue XXXXXX` | Continue; run and stop at address `XXXXXX` |
| `cfN`, `continue frames N` | Continue; run for exactly `N` frames (decimal; 50 frames = 1 second) |
| `s`, `step` | Step Into; execute one instruction |
| `n`, `next` | Step Over; execute the current instruction and stop right after it |

### Registers

| Command | Description |
|---|---|
| `r`, `regs` | Show all registers and PSW flags |
| `r ext`, `regs ext` | Show extended (I/O port) registers |
| `rN` | Show register `N` (0-7) |
| `rN=XXXXXX` | Set register `N` to `XXXXXX` |
| `rps` / `rps=XXXXXX` | Show / set the processor status word |
| `rpc` / `rpc=XXXXXX` | Show / set PC |
| `rsp` / `rsp=XXXXXX` | Show / set SP |

Register commands act on whichever processor is current (`CPU`/`PPU`, toggled with `p`).

### Disassembly and memory

| Command | Description |
|---|---|
| `d`, `disasm` | Disassemble from PC (paged); use `D` for short format (no raw opcode words) |
| `dXXXXXX`, `disasm XXXXXX` | Disassemble from address `XXXXXX` |
| `m`, `memory` | Examine memory at PC (paged) |
| `mXXXXXX`, `memory XXXXXX` | Examine memory at address `XXXXXX` |
| `ms ADDR=VALUE`, `memset ADDR=VALUE` | Set the word at `ADDR` to `VALUE` |
| `ms ADDR=VALUE bytes` | Same, but writes one byte instead of a word |
| `... bytes` | Modifier: byte granularity instead of words |
| `... hex` | Modifier: hexadecimal instead of octal |
| `... nochars` | Modifier: hide the trailing ASCII/character column |

Modifiers go after the address, in any order, e.g. `m100260 bytes hex` or `memory hex nochars`.
Values that changed since the last step/run are highlighted (in a terminal that supports color;
suppressed automatically otherwise).

### Breakpoints

| Command | Description |
|---|---|
| `b` | List all breakpoints |
| `bXXXXXX` | Set a breakpoint at `XXXXXX` |
| `bc` | Remove all breakpoints |
| `bcXXXXXX` | Remove the breakpoint at `XXXXXX` |

### Status and tracing

| Command | Description |
|---|---|
| `i`, `info` | Show uptime and floppy drive status |
| `i floppy`, `info floppy` | Show floppy engine state, per-drive attach status, and controller registers |
| `t`, `trace` | Toggle instruction tracing to `trace.log` on/off |
| `tXXXXXX`, `trace XXXXXX` | Set the trace flags explicitly (see `TRACE_xxx` in `emubase/Board.h`) |
| `tc`, `t clear`, `trace clear` | Clear `trace.log` |

### Keyboard

| Command | Description |
|---|---|
| `kd KEY`, `key down KEY` | Press and hold `KEY` |
| `ku KEY`, `key up KEY` | Release `KEY` |
| `k KEY`, `key KEY` | Click `KEY`: press, wait, release |
| `k MOD+KEY`, `key MOD+KEY` | Hold `MOD`, click `KEY`, release `MOD` |

`KEY`/`MOD` is a letter, digit, punctuation character, named key, or a raw octal scancode.
Named keys: `ENTER` `SPACE` `TAB` `BACKSPACE` `LEFT` `RIGHT` `UP` `DOWN`, the function keys
`K1`..`K5`, the editing/mode keys `POM` `UST` `ISP` `SBROS` `STOP` `AR2` `UPR` `ALF` `GRAF`
`FIKS` `SHIFT`, and the numeric keypad `NUM0`..`NUM9` `NUM+` `NUM-` `NUM,` `NUM.` `NUMENTER`.
Hold `AR2`/`UPR`/`ALF`/`GRAF`/`FIKS`/`SHIFT` as `MOD` in `key MOD+KEY`, e.g. `k SHIFT+A`.
Letters are named by the Latin glyph on the key. Scancodes match UKNCBTL's own
`emulator/KeyboardView.cpp`, so the same key works whether named or given as a raw octal code.

### Files, floppy images, and cartridges

| Command | Description |
|---|---|
| `memsave [FILE]` | Save a full memory dump (default `memdump.bin`) |
| `statesave FILE` / `stateload FILE` | Save / load full emulator state — memory, registers, ports |
| `diskN attach FILE`, `diskN a FILE` | Attach a floppy image to drive `N` (`1`-`4`) |
| `diskN detach`, `diskN d` | Detach the floppy image from drive `N` |
| `cartN attach FILE`, `cartN a FILE` | Attach a 24 KB ROM cartridge to slot `N` (`1`-`2`) |
| `cartN detach`, `cartN d` | Detach the ROM cartridge from slot `N` |
| `screen [FILE]` | Save a black-and-white screenshot as PNG (default filename: timestamp) |
| `screentext [FILE]` | OCR the screen to text; print to the console, or write it to `FILE` (UTF-8) |

`screentext` recognizes each 8×11 character cell against the machine's current and standard
fonts, translating the result through the UKNC KOI8-R character set to Unicode — so Latin,
Cyrillic, and the pseudographics all come out readable.

## Platform notes

- Console color output is automatically suppressed when stdout is not a terminal (redirected or
  piped), so scripted/batch use never sees raw escape codes.
- `TCHAR` is `char` on non-Windows builds; the Win32 `wmain`/wide-string code paths only compile
  under MSVC. Console output uses UTF-16 wide mode on Windows and a UTF-8 locale elsewhere.

## Status

This is a work in progress, developed interactively.
