// commands.cpp
//
// Console command interpreter. This is a port of the table-driven command
// dispatcher (ConsoleCommandStruct / ConsoleCommands[]) from the WinAPI GUI
// debugger at bkbtl/emulator/ConsoleView.cpp, adapted to a plain stdin/stdout
// console loop instead of an Edit control.
//
// Not ported from the original ConsoleView.cpp:
//   w / wXXXXXX / wc / wcXXXXXX (watches) -- dropped; there is no watch-list
//     backend in this project's Emulator.cpp/.h (Emulator_AddWatch & co.
//     don't exist here), so these were left out rather than faked.
//   t / tN / tc (trace log) -- excluded per request (PRODUCT-gated in the
//     original anyway).

#include "stdafx.h"
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <type_traits>
#include "ukncbtldebug.h"
#include "commands.h"
#include "Emulator.h"
#include "emubase/Emubase.h"
#include "util/BitmapFile.h"
#include "util/console.h"
#include "util/Symbols.h"


//////////////////////////////////////////////////////////////////////


const wchar_t* const MESSAGE_UNKNOWN_COMMAND  = L" Unknown command.";
const wchar_t* const MESSAGE_INVALID_REGNUM   = L" Invalid register number, 0..7 expected.";
const wchar_t* const MESSAGE_WRONG_VALUE      = L" Wrong value.";

// Modifier postfixes for "memory"/"m": any combination, any order, e.g.
// "memory bytes hex", "m100260 hex nochars", "m bytes".
const uint32_t MEMFLAG_BYTES    = 0x01;  // Byte granularity instead of word
const uint32_t MEMFLAG_HEX      = 0x02;  // Hexadecimal instead of octal
const uint32_t MEMFLAG_NOCHARS  = 0x04;  // Hide the trailing ASCII/character column

//////////////////////////////////////////////////////////////////////
// Continuation ("paging") for "memory"/"m" and "disasm"/"d"/"D".
//
// After printing a page, these commands can leave a "continuation" armed:
// the next address to show plus the modifiers/format that produced the
// current page. The main loop (bkbtldebug.cpp) checks for this before
// showing the normal prompt; if armed, it shows "-- more --" instead and
// reads a line. An empty line (just Enter) re-runs the same paging
// command at the saved address. Any other input is just "stop paging" --
// it answers the prompt, it is not a command line, so it's discarded and
// control returns to the normal prompt.

enum class ContinuationKind { None, Memory, Disasm };

struct ContinuationState
{
    ContinuationKind kind = ContinuationKind::None;
    uint16_t address = 0;
    uint32_t memoryFlags = 0;  // Used when kind == Memory
    bool disasmShort = false;   // Used when kind == Disasm (D vs d)
};

ContinuationState g_continuation;

// Print the "-- more --" prompt in the same color as the regular command
// prompt, and arm the given continuation so the next blank Enter resumes.
void ArmContinuation(const ContinuationState& state)
{
    g_continuation = state;
    Console_ColorPrompt();
    std::wcout << L"-- more (Enter to continue) --";
    Console_ColorReset();
}

bool m_okCurrentProc = false;  // Current processor: true - CPU, false - PPU

CProcessor* GetCurrentProcessor()
{
    if (m_okCurrentProc)
        return g_pBoard->GetCPU();
    else
        return g_pBoard->GetPPU();
}

// Forward declaration: RunUntilBreakpoint is defined further down (near the
// "continue" commands) but is also used by CmdStepOver defined before it.
void RunUntilBreakpoint(int maxFrames = 3000);

// Print register name, octal value and binary value -- one line.
// symbolSuffix: " <name+offset>" (from Symbols_FormatSuffix), or empty --
// only really meaningful for PC, but harmless to pass for any register.
void PrintRegisterLine(LPCTSTR strName, uint16_t value, const std::wstring& symbolSuffix = std::wstring())
{
    TCHAR bufOctal[7];
    PrintOctalValue(bufOctal, value);
    TCHAR bufBinary[17];
    PrintBinaryValue(bufBinary, value);

    std::wcout << L"  " << strName << L" " << bufOctal << symbolSuffix << L"  " << bufBinary << std::endl;
}

// Print one disassembled instruction line.
// okShort: omit the raw opcode-word column (mirrors the "D" vs "d" command).
void PrintDisassembleLine(uint16_t address, uint16_t value, LPCTSTR instr, LPCTSTR args, bool okShort)
{
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, address);
    std::wstring symbol = Symbols_FormatSuffix(address);

    if (okShort)
    {
        std::wcout << L" " << bufAddr << symbol << L" " << instr << L" " << args << std::endl;
    }
    else
    {
        TCHAR bufValue[7];
        PrintOctalValue(bufValue, value);
        std::wcout << L" " << bufAddr << symbol << L" " << bufValue << L" " << instr << L" " << args << std::endl;
    }
}

// Disassemble instructions starting at address.
// okOneInstr: stop after the first instruction (used by Step Into).
// Returns the number of words in the last instruction disassembled.
int PrintDisassemble(CProcessor* pProc, uint16_t address, bool okOneInstr, bool okShort, uint16_t* pNextAddress = nullptr)
{
    const CMemoryController* pMemCtl = pProc->GetMemoryController();
    bool okHaltMode = pProc->IsHaltMode();

    const int nWindowSize = 30;
    uint16_t memory[nWindowSize + 2];
    int addrtype;
    for (int i = 0; i < nWindowSize + 2; i++)
        memory[i] = pMemCtl->GetWordView((uint16_t)(address + i * 2), okHaltMode, true, &addrtype);

    int lastLength = 0;
    int length = 0;
    for (int index = 0; index < nWindowSize; index++)
    {
        uint16_t value = memory[index];

        if (length > 0)
        {
            // Continuation word(s) of the previous, longer instruction
        }
        else
        {
            if (okOneInstr && index > 0)
                break;

            TCHAR instr[8];
            TCHAR args[32];
            length = DisassembleInstruction(memory + index, address, instr, args);
            lastLength = length;
            if (index + length > nWindowSize)
                break;

            PrintDisassembleLine(address, value, instr, args, okShort);
        }

        length--;
        address += 2;
    }
    if (pNextAddress != nullptr)
        *pNextAddress = address;
    return lastLength;
}

// Print a memory dump: address, then either 8 words or 16 bytes (per
// MEMFLAG_BYTES), in octal or hex (per MEMFLAG_HEX), then optionally
// their ASCII/character representation (suppressed by MEMFLAG_NOCHARS).
// Always covers 16 bytes (one "line") per row regardless of granularity,
// so word and byte dumps of the same region line up the same way.
void PrintMemoryDumpGeneric(const CProcessor* pProc, uint16_t address, uint32_t flags, int lines = 8, uint16_t* pNextAddress = nullptr)
{
    bool okBytes = (flags & MEMFLAG_BYTES) != 0;
    bool okHex = (flags & MEMFLAG_HEX) != 0;
    bool okChars = (flags & MEMFLAG_NOCHARS) == 0;

    if (!okBytes)
        address &= ~1;  // Word dumps line up to an even address
    const CMemoryController* pMemCtl = pProc->GetMemoryControllerConst();
    bool okHaltMode = pProc->IsHaltMode();

    for (int line = 0; line < lines; line++)
    {
        uint16_t dump[8];
        uint16_t changed[8];
        int addrtype;
        for (int i = 0; i < 8; i++)
        {
            dump[i] = pMemCtl->GetWordView((uint16_t)(address + i * 2), okHaltMode, false, &addrtype);
            changed[i] = addrtype == ADDRTYPE_ROM
                ? 0
                : Emulator_GetChangeRamStatus(addrtype, address + i * 2);
        }

        TCHAR bufAddr[7];
        if (okHex)
            PrintHexValue(bufAddr, address);
        else
            PrintOctalValue(bufAddr, address);
        std::wcout << L"  " << bufAddr << L"  ";

        for (int i = 0; i < 8; i++)
        {
            uint16_t word = dump[i];
            if (changed[i] != 0) Console_ColorModified();
            if (okBytes)
            {
                TCHAR bufValue[7];
                if (okHex)
                {
                    PrintHexValue(bufValue, word & 0xFF);
                    std::wcout << (bufValue + 2) << L" ";
                    PrintHexValue(bufValue, word >> 8);
                    std::wcout << (bufValue + 2) << L" ";
                }
                else
                {
                    PrintOctalValue(bufValue, word & 0xFF);
                    std::wcout << (bufValue + 3) << L" ";
                    PrintOctalValue(bufValue, word >> 8);
                    std::wcout << (bufValue + 3) << L" ";
                }
            }
            else
            {
                TCHAR bufValue[7];
                if (okHex)
                    PrintHexValue(bufValue, word);
                else
                    PrintOctalValue(bufValue, word);
                std::wcout << bufValue << L" ";
            }
            Console_ColorReset();
        }

        if (okChars)
        {
            std::wcout << L" ";
            for (int i = 0; i < 8; i++)
            {
                uint16_t word = dump[i];
                uint8_t ch1 = (uint8_t)(word & 0xff);
                wchar_t wch1 = (ch1 < 32) ? L'\xB7' : (wchar_t)Translate_KOI8R(ch1);
                uint8_t ch2 = (uint8_t)(word >> 8);
                wchar_t wch2 = (ch2 < 32) ? L'\xB7' : (wchar_t)Translate_KOI8R(ch2);
                std::wcout << wch1 << wch2;
            }
        }
        std::wcout << std::endl;

        address += 16;
    }
    if (pNextAddress != nullptr)
        *pNextAddress = address;
}

// Convert a wstring command-line argument (e.g. a filename) to a TCHAR
// string. Under GCC, TCHAR is plain char, so this narrows via the current
// C locale; under real MSVC, TCHAR is wchar_t, so it's effectively a copy.
//
// Templated on a dummy parameter so only the branch matching TCHAR's
// actual type needs to type-check: plain "if constexpr" inside a
// non-template function still requires both branches to be well-formed
// even though only one runs, and basic_string<char> cannot convert to
// basic_string<wchar_t> (or vice versa) as a return statement regardless
// of which branch executes. (Checking #ifdef _UNICODE here, as an earlier
// version of this function did, is wrong: this project never defines
// _UNICODE anywhere, on any platform, so that branch was always dead and
// the function always silently took the narrow path -- harmless under
// GCC where TCHAR is already char, but it meant this function has never
// actually been exercised, or correct, under a genuine TCHAR=wchar_t
// build until this fix.)
template <typename T>
std::basic_string<T> WStringToTStringImpl(const std::wstring& ws)
{
    if constexpr (std::is_same_v<T, wchar_t>)
    {
        return ws;
    }
    else
    {
        return WStringToNarrowString(ws);
    }
}

std::basic_string<TCHAR> WStringToTString(const std::wstring& ws)
{
    return WStringToTStringImpl<TCHAR>(ws);
}

// Save full 64K memory dump to a file ("memdump.bin" by default).
// Ported from ConsoleView_SaveMemoryDump, using standard C++ file I/O
// instead of Win32 CreateFile/WriteFile.
bool SaveMemoryDump(const std::wstring& wfilename)
{
    CProcessor* pProc = GetCurrentProcessor();
    const CMemoryController* pMemCtl = pProc->GetMemoryControllerConst();

    std::wstring filename = wfilename.empty() ? L"memdump.bin" : wfilename;
    std::string narrowFilename = WStringToNarrowString(filename);

    std::vector<uint16_t> buf(32768);
    int addrtype;
    for (int i = 0; i < 32768; i++)
        buf[i] = pMemCtl->GetWordView((uint16_t)(i * 2), true, false, &addrtype);

    std::ofstream file(narrowFilename.c_str(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;

    file.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    return file.good();
}

// Render the current screen and save it as a PNG file.
// screenMode: 0 = black/white, 1 = color (see Emulator_PrepareScreenRGB32).
bool SaveScreenshot(const std::wstring& wfilename)
{
    int width = UKNC_SCREEN_WIDTH, height = UKNC_SCREEN_HEIGHT;
    //Emulator_GetScreenSize(screenMode, &width, &height);
    //if (width <= 0 || height <= 0)
    //    return false;

    const uint32_t* palette = Emulator_GetPalette();

    std::vector<uint32_t> bits((size_t)width * height);
    Emulator_PrepareScreenRGB32(bits.data(), palette);

    std::basic_string<TCHAR> filename = WStringToTString(wfilename);
    return PngFile_SaveScreenshot(bits.data(), palette, filename.c_str(), width, height);
}

//////////////////////////////////////////////////////////////////////
// "screentext" -- OCR of the current UKNC screen into text.
//
// Ported from UKNCBTL's ScreenView_ScreenToText. The screen is rendered
// to a grayscale RGB32 bitmap, the two on-machine fonts (current font from
// PPU RAM, standard font from PPU ROM) are read out, and each 8x11 cell is
// matched against both fonts to pick the best-fitting character.

// 8-level grayscale palette, indexed by the 3-bit plane value (only the
// first 8 entries of UKNCBTL's ScreenView_GrayColors are ever indexed).
static const uint32_t ScreenTextGrayColors[8] =
{
    0x000000, 0x242424, 0x484848, 0x6C6C6C, 0x909090, 0xB4B4B4, 0xD8D8D8, 0xFFFFFF,
};

// Match one 8x11 pixel cell (pBits, row stride 640) against both fonts and
// return the best-fitting character code (0x20-based, as UKNC lays out the
// 16*14 glyph table starting at space).
static uint8_t RecognizeCharacter(const uint8_t* fontcur, const uint8_t* fontstd, const uint32_t* pBits)
{
    int16_t bestmatch = -32767;
    uint8_t bestchar = 0;
    for (uint8_t charidx = 0; charidx < 16 * 14; charidx++)
    {
        int16_t matchcur = 0;
        int16_t matchstd = 0;
        const uint32_t* pb = pBits;
        for (int16_t y = 0; y < 11; y++)
        {
            uint8_t fontcurdata = fontcur[charidx * 11 + y];
            uint8_t fontstddata = fontstd[charidx * 11 + y];
            for (int x = 0; x < 8; x++)
            {
                uint32_t color = pb[x];
                int sum = (color & 0xff) + ((color >> 8) & 0xff) + ((color >> 16) & 0xff);
                uint8_t fontcurbit = (fontcurdata >> x) & 1;
                uint8_t fontstdbit = (fontstddata >> x) & 1;
                if (sum > 384)
                {
                    matchcur += fontcurbit;  matchstd += fontstdbit;
                }
                else
                {
                    matchcur -= fontcurbit;  matchstd -= fontstdbit;
                }
            }
            pb += 640;
        }
        if (matchcur > bestmatch) { bestmatch = matchcur; bestchar = charidx; }
        if (matchstd > bestmatch) { bestmatch = matchstd; bestchar = charidx; }
    }
    return 0x20 + bestchar;
}

// Recognize the whole screen into a text string (rows separated by CR/LF).
std::string ScreenToText()
{
    std::vector<uint32_t> bits((size_t)UKNC_SCREEN_WIDTH * UKNC_SCREEN_HEIGHT, 0);
    Emulator_PrepareScreenToText(bits.data(), ScreenTextGrayColors);

    CMemoryController* pPpuMemCtl = g_pBoard->GetPPUMemoryController();
    int addrtype = 0;

    // Current font: table of glyph addresses in PPU RAM
    uint8_t fontcur[11 * 16 * 14];
    uint16_t fontaddr = 014142 + 32 * 2;
    for (uint8_t charidx = 0; charidx < 16 * 14; charidx++)
    {
        uint16_t charaddr = pPpuMemCtl->GetWordView(fontaddr + charidx * 2, false, false, &addrtype);
        for (int16_t y = 0; y < 11; y++)
        {
            uint16_t fontdata = pPpuMemCtl->GetWordView((charaddr + y) & ~1, false, false, &addrtype);
            if (((charaddr + y) & 1) == 1) fontdata >>= 8;
            fontcur[charidx * 11 + y] = (uint8_t)(fontdata & 0xff);
        }
    }
    // Standard font: contiguous glyph rows in PPU ROM
    uint8_t fontstd[11 * 16 * 14];
    uint16_t charstdaddr = 0120170;
    for (uint16_t idx = 0; idx < 16 * 14 * 11; idx++)
    {
        uint16_t fontdata = pPpuMemCtl->GetWordView(charstdaddr & ~1, false, false, &addrtype);
        if ((charstdaddr & 1) == 1) fontdata >>= 8;
        fontstd[idx] = (uint8_t)(fontdata & 0xff);
        charstdaddr++;
    }

    std::string text;
    int y = 0;
    while (y <= UKNC_SCREEN_HEIGHT - 11)
    {
        const uint32_t* pCharBits = bits.data() + (size_t)y * 640;
        for (int x = 0; x < 640; x += 8)
            text.push_back((char)RecognizeCharacter(fontcur, fontstd, pCharBits + x));
        text.push_back('\n');

        y += 11;
        if (y == 11) y++;   // Extra line after upper indicator lines
        if (y == 276) y++;  // Extra line before lower indicator lines
    }
    return text;
}

//////////////////////////////////////////////////////////////////////
// Console command parameters -- filled in by the pattern matcher,
// consumed by the command callback. Mirrors ConsoleCommandParams.

struct ConsoleCommandParams
{
    std::wstring commandText;
    int paramReg1 = -1;
    uint16_t paramOct1 = 0;
    uint16_t paramOct2 = 0;
    std::wstring paramFilename;
    uint32_t paramFlags = 0;     // Modifier postfix flags, see MEMFLAG_xxx
    bool paramHasAddress = false; // Whether an explicit address was given (vs PC default)
    size_t paramPrefixLength = 0; // Length of the table prefix that matched commandText
};

//////////////////////////////////////////////////////////////////////
// Console command handlers
// Mirrors the ConsoleView_Cmd* functions.

void CmdShowHelp(const ConsoleCommandParams& /*params*/)
{
    std::wcout <<
        L"Console command list:\n"
        L"  h, help, ?     Show this help\n"
        L"  reset          Reset the machine\n"
        L"  p              Switch current processor CPU/PPU\n"
        L"  c, continue    Continue; free run\n"
        L"  cXXXXXX, continue XXXXXX  Continue; run and stop at address XXXXXX\n"
        L"  cfN, continue frames N  Continue; run for N frames, decimal (1 sec = 50 frames)\n"
        L"  s, step        Step Into; executes one instruction\n"
        L"  n, next        Step Over (Next); executes and stops after the current instruction\n"
        L"  r, regs        Show register values\n"
        L"  r ext, regs ext  Show extended (I/O port) registers\n"
        L"  i, info        Show machine status: uptime, floppy drives\n"
        L"  i floppy, info floppy  Show floppy controller registers and state\n"
        L"  rN             Show value of register N; N=0..7\n"
        L"  rN=XXXXXX      Set register N to value XXXXXX; N=0..7\n"
        L"  rps            Show PS (processor status word)\n"
        L"  rps=XXXXXX     Set PS to value XXXXXX\n"
        L"  rpc            Show PC (same as the PC shown by r/regs)\n"
        L"  rpc=XXXXXX     Set PC to value XXXXXX\n"
        L"  rsp            Show SP (same as the SP shown by r/regs)\n"
        L"  rsp=XXXXXX     Set SP to value XXXXXX\n"
        L"  d, disasm      Disassemble from PC; use D for short format; paged output\n"
        L"  dXXXXXX, disasm XXXXXX  Disassemble from address XXXXXX\n"
        L"  m, memory      Examine memory at current address; paged output\n"
        L"  mXXXXXX, memory XXXXXX  Examine memory at address XXXXXX\n"
        L"  ms ADDR=VALUE, memset ADDR=VALUE  Set memory at ADDR to VALUE (word)\n"
        L"  ms ADDR=VALUE bytes  Same, byte instead of word\n"
        L"  ... bytes      Modifier: byte granularity instead of words\n"
        L"  ... hex        Modifier: hexadecimal instead of octal\n"
        L"  ... nochars    Modifier: hide the ASCII/character column\n"
        L"                 Modifiers combine in any order, e.g. \"m100260 bytes hex\"\n"
        L"  b              List all breakpoints\n"
        L"  bXXXXXX        Set breakpoint at address XXXXXX\n"
        L"  b NAME         Set breakpoint at symbol NAME (see \"symbols load\")\n"
        L"  bc             Remove all breakpoints\n"
        L"  bcXXXXXX       Remove breakpoint at address XXXXXX\n"
        L"  t, trace       Toggle instruction tracing to trace.log on/off\n"
        L"  tXXXXXX, trace XXXXXX  Set trace flags XXXXXX (see TRACE_xxx constants)\n"
        L"  tc, t clear, trace clear  Clear trace.log\n"
        L"  memsave [FILE] Save memory dump as FILE; default memdump.bin\n"
        L"  statesave FILE Save full emulator state (memory, registers, ports) to FILE\n"
        L"  stateload FILE Load full emulator state from FILE\n"
        L"  symbols load FILE, sym load FILE  Load symbols from a GNU ld map file (-Wl,-Map=...)\n"
        L"  symbols, sym   List the currently loaded symbol table\n"
        L"  diskN attach FILE, diskN a FILE  Attach floppy image FILE to drive N; N=1..4\n"
        L"  diskN detach, diskN d  Detach floppy image from drive N; N=1..4\n"
        L"  cartN attach FILE, cartN a FILE  Attach 24K ROM cartridge FILE to slot N; N=1..2\n"
        L"  cartN detach, cartN d  Detach ROM cartridge from slot N; N=1..2\n"
        L"  screen [FILE]  Save black/white screenshot as FILE (PNG); default filename from timestamp\n"
        L"  screentext [FILE]  OCR the screen to text; print to console, or write to FILE\n"
        L"  kd KEY, key down KEY    Press and hold KEY\n"
        L"  ku KEY, key up KEY      Release KEY\n"
        L"  k KEY, key KEY          Click KEY: press, wait, release\n"
        L"  k MOD+KEY, key MOD+KEY  Hold MOD, click KEY, release MOD\n"
        L"                 KEY/MOD = letter, digit, punctuation, named key, or octal scancode\n"
        L"                 Named keys: ENTER SPACE TAB BACKSPACE LEFT RIGHT UP DOWN K1..K5\n"
        L"                 POM UST ISP SBROS STOP AR2 UPR ALF GRAF FIKS SHIFT; keypad NUM0..NUM9\n"
        L"                 NUM+ NUM- NUM, NUM. NUMENTER (hold AR2/UPR/ALF/GRAF/FIKS/SHIFT as MOD)\n"
        L"  q, quit, exit  Quit the debugger\n";
}

void CmdSwitchCpuPpu(const ConsoleCommandParams& /*params*/)
{
    m_okCurrentProc = !m_okCurrentProc;

    LPCTSTR procName = GetCurrentProcessor()->GetName();
    std::wcout << L"  Switched to " << procName << std::endl;
}

void CmdPrintAllRegisters(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    for (int r = 0; r < 8; r++)
    {
        TCHAR bufOctal[7];
        PrintOctalValue(bufOctal, pProc->GetReg(r));
        std::wcout << REGISTER_NAME[r] << L"=";
        Console_ColorModified(Emulator_IsRegisterChanged(r));
        std::wcout << bufOctal;
        Console_ColorReset();
        if (r == 7)  // R7 is PC
            std::wcout << Symbols_FormatSuffix(pProc->GetReg(r));
        std::wcout << L" ";
    }
    TCHAR bufPSW[7];
    PrintOctalValue(bufPSW, pProc->GetPSW());
    uint16_t pswprev = g_wEmulatorPrevCpuR[8];
    uint16_t psw = g_wEmulatorCpuR[8];
    std::wcout << L"PSW=";
    Console_ColorModified(pswprev != psw);
    std::wcout << bufPSW;
    Console_ColorReset();
    std::wcout << L" [N=";
    Console_ColorModified((pswprev & PSW_N) != (psw & PSW_N));
    std::wcout << pProc->GetN();
    Console_ColorReset();
    std::wcout << L" Z=";
    Console_ColorModified((pswprev & PSW_Z) != (psw & PSW_Z));
    std::wcout << pProc->GetZ();
    Console_ColorReset();
    std::wcout << L" V=";
    Console_ColorModified((pswprev & PSW_V) != (psw & PSW_V));
    std::wcout << pProc->GetV();
    Console_ColorReset();
    std::wcout << L" C=";
    Console_ColorModified((pswprev & PSW_C) != (psw & PSW_C));
    std::wcout << pProc->GetC();
    Console_ColorReset();
    std::wcout << L" T=";
    Console_ColorModified((pswprev & PSW_T) != (psw & PSW_T));
    std::wcout << ((pProc->GetPSW() & PSW_T) != 0 ? 1 : 0);
    Console_ColorReset();
    std::wcout << L"]" << std::endl;
}

// Print "Floppy engine: ON/off" and the per-drive attach/read-only list.
// Caller must have already checked BK_COPT_FDD.
// okMarkSelected: append "(selected)" to the currently selected drive's
// line (used by "info floppy"; "i"/"info" doesn't show this).
void PrintFloppyEngineAndDrives(bool okMarkSelected)
{
    std::wcout << L"Floppy engine: " << (Emulator_IsFloppyEngineOn() ? L"ON" : L"off") << std::endl;

    //int selectedDrive = -1;
    //if (okMarkSelected)
    //{
    //    uint16_t driveValue = g_pBoard->GetPortView(PORTVIEW_FDDDRIVE);
    //    if (driveValue != 0xFFFF)
    //        selectedDrive = driveValue;
    //}

    for (int slot = 0; slot < 4; slot++)
    {
        wchar_t digit = (wchar_t)(L'1' + slot);
        bool okAttached = Emulator_IsFloppyImageAttached(slot);
        std::wcout << L"  disk" << digit << L": ";
        if (okAttached)
        {
            std::wcout << L"attached"
                        << (Emulator_IsFloppyReadOnly(slot) ? L", read-only" : L", read-write");
        }
        else
        {
            std::wcout << L"not attached";
        }
        //if (slot == selectedDrive)
        //    std::wcout << L" (selected)";
        std::wcout << std::endl;
    }
}

void CmdPrintFloppyRegisters(const ConsoleCommandParams& /*params*/)
{
    PrintFloppyEngineAndDrives(true);

    //PrintPortRegisterLine(0177130, PORTVIEW_FDDSTATE, _T("floppy state"));
    //PrintPortRegisterLine(0177132, PORTVIEW_FDDDATA,  _T("floppy data"));

    // Track/side are internal controller state, not memory-mapped ports --
    // there's no real address for them -- but GetPortView() is still the
    // debugger access path used for everything else above, so they go
    // through it too. Printed without an address column to avoid implying
    // they live at some address.
    //TCHAR bufTrack[7];
    //PrintOctalValue(bufTrack, g_pBoard->GetPortView(PORTVIEW_FDDTRACK));
    //std::wcout << L"       " << bufTrack << L" track" << std::endl;

    //TCHAR bufSide[7];
    //PrintOctalValue(bufSide, g_pBoard->GetPortView(PORTVIEW_FDDSIDE));
    //std::wcout << L"       " << bufSide << L" side" << std::endl;
}

void CmdShowStatus(const ConsoleCommandParams& /*params*/)
{
    float uptime = Emulator_GetUptime();
    std::wcout << L"Uptime: " << std::fixed << std::setprecision(2) << uptime << L" sec" << std::endl;

    PrintFloppyEngineAndDrives(false);
}

void CmdPrintRegister(const ConsoleCommandParams& params)
{
    int r = params.paramReg1;
    LPCTSTR name = REGISTER_NAME[r];
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t value = pProc->GetReg(r);
    PrintRegisterLine(name, value);
}

void CmdSetRegisterValue(const ConsoleCommandParams& params)
{
    int r = params.paramReg1;
    uint16_t value = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    pProc->SetReg(r, value);

    TCHAR bufValue[7];
    PrintOctalValue(bufValue, value);
    std::wcout << REGISTER_NAME[r] << L" set to " << bufValue << std::endl;
}

void CmdPrintRegisterPSW(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t value = pProc->GetPSW();
    PrintRegisterLine(_T("PS"), value);
}

void CmdSetRegisterPSW(const ConsoleCommandParams& params)
{
    uint16_t value = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    pProc->SetPSW(value);

    TCHAR bufValue[7];
    PrintOctalValue(bufValue, value);
    std::wcout << L"PS set to " << bufValue << std::endl;
}

void CmdPrintRegisterSP(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t value = pProc->GetReg(6);
    PrintRegisterLine(_T("SP"), value);
}

void CmdSetRegisterSP(const ConsoleCommandParams& params)
{
    uint16_t value = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    pProc->SetReg(6, value);

    TCHAR bufValue[7];
    PrintOctalValue(bufValue, value);
    std::wcout << L"SP set to " << bufValue << std::endl;
}

void CmdPrintRegisterPC(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t value = pProc->GetReg(7);
    PrintRegisterLine(_T("PC"), value, Symbols_FormatSuffix(value));
}

void CmdSetRegisterPC(const ConsoleCommandParams& params)
{
    uint16_t value = params.paramOct1;
    CProcessor* pProc = GetCurrentProcessor();
    pProc->SetReg(7, value);

    TCHAR bufValue[7];
    PrintOctalValue(bufValue, value);
    std::wcout << L"PC set to " << bufValue << std::endl;
}

void CmdReset(const ConsoleCommandParams& /*params*/)
{
    Emulator_Reset();
    std::wcout << L"Reset." << std::endl;
}

void CmdStepInto(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    PrintDisassemble(pProc, pProc->GetPC(), true, false);
    g_pBoard->DebugTicks();
    Emulator_OnUpdate();  // Refresh change-tracking snapshot after this single instruction
}

void CmdStepOver(const ConsoleCommandParams& /*params*/)
{
    CProcessor* pProc = GetCurrentProcessor();
    const CMemoryController* pMemCtl = pProc->GetMemoryControllerConst();
    int instrLength = PrintDisassemble(pProc, pProc->GetPC(), true, false);

    int addrtype;
    uint16_t instr = pMemCtl->GetWordView(pProc->GetPC(), pProc->IsHaltMode(), true, &addrtype);

    // For JMP and BR use Step Into logic, not Step Over -- there's no
    // "next instruction" to break on, since control may not return here.
    if ((instr & ~(uint16_t)0077) == PI_JMP || (instr & ~(uint16_t)0377) == PI_BR)
    {
        g_pBoard->DebugTicks();
        Emulator_OnUpdate();  // Refresh change-tracking snapshot after this single instruction
        return;
    }

    uint16_t bpaddress = (uint16_t)(pProc->GetPC() + instrLength * 2);
    Emulator_SetTempCPUBreakpoint(bpaddress);
    RunUntilBreakpoint();
}

void CmdPrintDisassembleAtPC(const ConsoleCommandParams& params)
{
    bool okShort = (params.commandText[0] == L'D');
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t address = pProc->GetPC();
    uint16_t nextAddress;
    PrintDisassemble(pProc, address, false, okShort, &nextAddress);

    ContinuationState state;
    state.kind = ContinuationKind::Disasm;
    state.address = nextAddress;
    state.disasmShort = okShort;
    ArmContinuation(state);
}

void CmdPrintDisassembleAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    bool okShort = (params.commandText[0] == L'D');
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t nextAddress;
    PrintDisassemble(pProc, address, false, okShort, &nextAddress);

    ContinuationState state;
    state.kind = ContinuationKind::Disasm;
    state.address = nextAddress;
    state.disasmShort = okShort;
    ArmContinuation(state);
}

void CmdSaveMemoryDump(const ConsoleCommandParams& params)
{
    std::wstring filename = params.paramFilename.empty() ? L"memdump.bin" : params.paramFilename;
    if (SaveMemoryDump(filename))
        std::wcout << L"Saved memory dump " << filename << std::endl;
    else
        std::wcout << L"FAILED to save memory dump " << filename << std::endl;
}

void CmdStateSave(const ConsoleCommandParams& params)
{
    std::string filename = WStringToNarrowString(params.paramFilename);
    if (Emulator_SaveImage(filename))
        std::wcout << L"Saved state " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to save state " << params.paramFilename << std::endl;
}

void CmdStateLoad(const ConsoleCommandParams& params)
{
    std::string filename = WStringToNarrowString(params.paramFilename);
    if (Emulator_LoadImage(filename))
        std::wcout << L"Loaded state " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to load state " << params.paramFilename << std::endl;
}

// "symbols load FILE" / "sym load FILE" -- load a GNU ld map file (the same
// one this project's example Makefiles already produce via -Wl,-Map=...)
// so disasm/registers/breakpoints/"Stopped at" can show "<name+offset>"
// instead of a bare octal address.
void CmdLoadSymbols(const ConsoleCommandParams& params)
{
    size_t count = Symbols_LoadFromMapFile(params.paramFilename);
    if (count > 0)
        std::wcout << L"Loaded " << count << L" symbols from " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to load symbols from " << params.paramFilename << std::endl;
}

// "symbols" / "sym" -- list the currently loaded symbol table.
void CmdListSymbols(const ConsoleCommandParams& /*params*/)
{
    Symbols_PrintAll();
}

// "disk1 attach FILE" .. "disk4 attach FILE" -- the slot digit is the 5th
// character of commandText ("disk" is 4 chars), 1=slot 0 .. 4=slot 3.
void CmdAttachFloppyImage(const ConsoleCommandParams& params)
{
    wchar_t digit = params.commandText[4];
    int slot = digit - L'1';

    std::basic_string<TCHAR> tfilename = WStringToTString(params.paramFilename);
    bool result = Emulator_AttachFloppyImage(slot, tfilename.c_str());

    if (result)
        std::wcout << L"Attached disk" << digit << L": " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to attach disk" << digit << L": " << params.paramFilename << std::endl;
}

// "disk1 detach" .. "disk4 detach" -- same slot-digit convention as attach.
void CmdDetachFloppyImage(const ConsoleCommandParams& params)
{
    wchar_t digit = params.commandText[4];
    int slot = digit - L'1';

    Emulator_DetachFloppyImage(slot);
    std::wcout << L"Detached disk" << digit << std::endl;
}

// "cart1 attach FILE" / "cart2 attach FILE" -- the slot digit is the 5th
// character of commandText ("cart" is 4 chars); UKNC cartridge slots are 1..2.
void CmdAttachCartridge(const ConsoleCommandParams& params)
{
    wchar_t digit = params.commandText[4];
    int slot = digit - L'0';

    std::string filename = WStringToNarrowString(params.paramFilename);
    bool result = Emulator_LoadROMCartridge(slot, filename);

    if (result)
        std::wcout << L"Attached cart" << digit << L": " << params.paramFilename << std::endl;
    else
        std::wcout << L"Failed to attach the ROM cartridge image." << std::endl;
}

// "cart1 detach" / "cart2 detach" -- same slot-digit convention as attach.
void CmdDetachCartridge(const ConsoleCommandParams& params)
{
    wchar_t digit = params.commandText[4];
    int slot = digit - L'0';

    Emulator_DetachCartridge(slot);
    std::wcout << L"Detached cart" << digit << std::endl;
}

void CmdScreenshot(const ConsoleCommandParams& params)
{
    std::wstring filename = params.paramFilename;
    if (filename.empty())
    {
        // Generate default filename from current local time: YYYYMMDDHHMMSSmmm.png
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        std::tm tm = *std::localtime(&t);
        wchar_t buf[32];
        std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
                      L"%04d%02d%02d%02d%02d%02d%03d.png",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      (int)ms.count());
        filename = buf;
    }

    bool result = SaveScreenshot(filename);

    if (result)
        std::wcout << L"Saved screenshot " << filename << std::endl;
    else
        std::wcout << L"FAILED to save screenshot " << filename << std::endl;
}

// "screentext [FILE]" -- OCR the current screen; print to console, or write
// to FILE when a filename is given. Recognized bytes are UKNC KOI8-R codes,
// so each is translated to Unicode (ASCII passes through, Cyrillic and the
// pseudographics become their Unicode equivalents).
void CmdScreenText(const ConsoleCommandParams& params)
{
    std::string bytes = ScreenToText();

    std::wstring wtext;
    wtext.reserve(bytes.size());
    for (unsigned char b : bytes)
        wtext.push_back((wchar_t)Translate_KOI8R(b));

    if (params.paramFilename.empty())
    {
        std::wcout << wtext;
        std::wcout.flush();
        return;
    }

    // Write the file as UTF-8 (with BOM) so any editor renders the Cyrillic.
    // All translated code points are in the BMP (ASCII, Cyrillic U+04xx,
    // box-drawing U+25xx), so a 1-3 byte encoder covers everything.
    std::string utf8;
    utf8.reserve(wtext.size() * 2);
    for (wchar_t wc : wtext)
    {
        uint32_t cp = (uint32_t)wc;
        if (cp < 0x80)
            utf8.push_back((char)cp);
        else if (cp < 0x800)
        {
            utf8.push_back((char)(0xC0 | (cp >> 6)));
            utf8.push_back((char)(0x80 | (cp & 0x3F)));
        }
        else
        {
            utf8.push_back((char)(0xE0 | (cp >> 12)));
            utf8.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            utf8.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    std::basic_string<TCHAR> filename = WStringToTString(params.paramFilename);
    std::ofstream file(filename.c_str(), std::ios::binary | std::ios::trunc);
    if (file.is_open())
    {
        file.write("\xEF\xBB\xBF", 3);  // UTF-8 BOM
        file.write(utf8.data(), (std::streamsize)utf8.size());
    }
    if (file.good())
        std::wcout << L"Saved screen text " << params.paramFilename << std::endl;
    else
        std::wcout << L"FAILED to save screen text " << params.paramFilename << std::endl;
}

// "memory"/"m": optional address (default PC), then any combination of
// postfix modifiers ("bytes", "hex", "nochars") in any order, e.g.
// "m", "m100260", "memory 100260 bytes hex", "m hex nochars".
void CmdShowMemory(const ConsoleCommandParams& params)
{
    CProcessor* pProc = GetCurrentProcessor();
    uint16_t address = params.paramHasAddress ? params.paramOct1 : pProc->GetPC();
    uint16_t nextAddress;
    PrintMemoryDumpGeneric(pProc, address, params.paramFlags, 8, &nextAddress);

    ContinuationState state;
    state.kind = ContinuationKind::Memory;
    state.address = nextAddress;
    state.memoryFlags = params.paramFlags;
    ArmContinuation(state);
}

// Run until a breakpoint is hit, or until maxFrames frames have been run.
// Since this console has no message loop / timer driving frames the way the
// GUI build does, "continue" here just drives Emulator_SystemFrame() in a
// blocking loop until it reports a breakpoint.
//
// Safety valve: there is no way to interrupt this from another thread (no
// GUI Stop button, no Ctrl+C handler), so a free run with no breakpoint
// ever set -- or a breakpoint at an address the CPU never reaches -- would
// hang the console forever. maxFrames caps it; at 25 frames/sec the default
// 3000 is ~2 minutes of emulated time before giving up.
void RunUntilBreakpoint(int maxFrames)
{
    Emulator_Start();
    int frames = 0;
    bool hitBreakpoint = false;
    while (g_okEmulatorRunning)
    {
        if (!Emulator_SystemFrame())
        {
            hitBreakpoint = true;
            Emulator_Stop();
            break;
        }
        frames++;
        if (frames >= maxFrames)
        {
            Emulator_Stop();
            break;
        }
    }
    Emulator_OnUpdate();  // Refresh change-tracking snapshot now that we've stopped
    CProcessor* pProc = GetCurrentProcessor();
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, pProc->GetPC());
    std::wstring symbol = Symbols_FormatSuffix(pProc->GetPC());
    if (hitBreakpoint)
        std::wcout << L" Stopped at " << bufAddr << symbol << std::endl;
    else
        std::wcout << L" Stopped at " << bufAddr << symbol << L" (no breakpoint hit after "
                    << maxFrames << L" frames)" << std::endl;
}

void CmdRun(const ConsoleCommandParams& /*params*/)
{
    RunUntilBreakpoint();
}

void CmdRunFrames(const ConsoleCommandParams& params)
{
    RunUntilBreakpoint((int)params.paramOct1);
}

void CmdRunToAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    Emulator_SetTempCPUBreakpoint(address);
    RunUntilBreakpoint();
}

//////////////////////////////////////////////////////////////////////
// "mo" -- jump to Monitor
//
// The GUI build does this by injecting keystrokes into the running screen
// view (ScreenView_KeyEvent), relying on its always-on timer loop to drain
// the key queue and let the BK's keyboard ISR see them. There is no screen
// view here, but Emulator_KeyEvent / Emulator_ProcessKeyEvent are the same
// underlying queue, so we drive a handful of frames ourselves after each
// keypress/keyrelease to get the same effect.

// Run a few emulator frames -- just enough for a queued key event to be
// drained by Emulator_ProcessKeyEvent() and processed by the keyboard ISR.
// Not a debugging "run": breakpoints are intentionally ignored here, since
// typing a Monitor command shouldn't be derailed by a CPU breakpoint.
void PumpFrames(int count)
{
    for (int i = 0; i < count; i++)
        g_pBoard->SystemFrame();
}

// Press and release one BK keyboard scancode, pumping frames around each
// half so the queued event actually gets consumed.
void TypeKey(uint8_t scancode)
{
    Emulator_KeyEvent(scancode, true);
    PumpFrames(2);
    Emulator_KeyEvent(scancode, false);
    PumpFrames(2);
}

//////////////////////////////////////////////////////////////////////
// "key down KEY" / "kd KEY", "key up KEY" / "ku KEY", "key KEY" / "k KEY",
// "key MOD+KEY" / "k MOD+KEY" -- inject individual keyboard events.
//
// Scancodes are from UKNCBTL's emulator/KeyboardView.cpp (m_arrKeyboardKeys).
// They are hardware matrix scan-codes, not character codes. Letters are named
// by the Latin glyph printed on the key (the UKNC keyboard carries both a
// Cyrillic and a Latin layer; АЛФ/ГРАФ selects the layer). Punctuation is
// named by the unshifted character on that key. The keypad keys are prefixed
// "NUM". Function/control keys keep their transliterated legends
// (K1..K5, POM/УСТ/ИСП/СБРОС/СТОП, АР2/УПР/АЛФ/ГРАФ/ФИКС).

struct NamedKey
{
    const wchar_t* name;
    uint8_t scancode;
};

const NamedKey g_namedKeys[] =
{
    // Letters, named by the Latin glyph on the key (М/Т share the Cyrillic glyph)
    { L"A", 0072 }, { L"B", 0076 }, { L"C", 0050 }, { L"D", 0057 },
    { L"E", 0033 }, { L"F", 0047 }, { L"G", 0055 }, { L"H", 0156 },
    { L"I", 0073 }, { L"J", 0027 }, { L"K", 0052 }, { L"L", 0056 },
    { L"M", 0112 }, { L"N", 0054 }, { L"O", 0075 }, { L"P", 0053 },
    { L"Q", 0067 }, { L"R", 0074 }, { L"S", 0111 }, { L"T", 0114 },
    { L"U", 0051 }, { L"V", 0137 }, { L"W", 0071 }, { L"X", 0115 },
    { L"Y", 0070 }, { L"Z", 0157 },
    // Digits (main row)
    { L"0", 0176 }, { L"1", 0030 }, { L"2", 0031 }, { L"3", 0032 },
    { L"4", 0013 }, { L"5", 0034 }, { L"6", 0035 }, { L"7", 0016 },
    { L"8", 0017 }, { L"9", 0177 },
    // Punctuation, named by the unshifted character on that key
    { L";", 0007 }, { L"-", 0175 }, { L"/",  0173 }, { L":", 0174 },
    { L",", 0117 }, { L".", 0135 }, { L"\\", 0136 }, { L"[", 0036 },
    { L"]", 0037 }, { L"^", 0110 }, { L"@",  0077 },
    // Named special keys
    { L"ENTER",     0153 },
    { L"SPACE",     0113 },
    { L"TAB",       0026 },
    { L"BACKSPACE", 0132 },
    { L"LEFT",      0116 },
    { L"RIGHT",     0133 },
    { L"UP",        0154 },
    { L"DOWN",      0134 },
    // Function keys K1..K5
    { L"K1", 0010 }, { L"K2", 0011 }, { L"K3", 0012 }, { L"K4", 0014 },
    { L"K5", 0015 },
    // Editing / mode keys (transliterated legends)
    { L"POM",   0172 },  // ПОМ
    { L"UST",   0152 },  // УСТ
    { L"ISP",   0151 },  // ИСП
    { L"SBROS", 0171 },  // СБРОС (reset)
    { L"STOP",  0004 },  // СТОП
    { L"AR2",   0006 },  // АР2
    { L"UPR",   0046 },  // УПР
    { L"ALF",   0106 },  // АЛФ
    { L"GRAF",  0066 },  // ГРАФ
    { L"FIKS",  0107 },  // ФИКС
    { L"SHIFT", 0105 },  // both Shift keys share this scancode
    // Keypad
    { L"NUMPL",    0131 }, { L"NUMMI",    0025 }, { L"NUMCOMMA",    0005 },
    { L"NUM7",     0125 }, { L"NUM8",     0145 }, { L"NUM9",        0165 },
    { L"NUM4",     0130 }, { L"NUM5",     0150 }, { L"NUM6",        0170 },
    { L"NUM1",     0127 }, { L"NUM2",     0147 }, { L"NUM3",        0167 },
    { L"NUM0",     0126 }, { L"NUMDOT",   0146 }, { L"NUMENTER",    0166 },
};
const size_t g_namedKeysCount = sizeof(g_namedKeys) / sizeof(g_namedKeys[0]);

// How long a plain (non-modifier) key event is held visible to the
// emulator, in frames, before the matching press/release counterpart.
const int KEY_HOLD_FRAMES = 6;
// Same, but for a modifier held around another key in "key MOD+KEY".
const int KEY_MODIFIER_HOLD_FRAMES = 3;

// Look up a key by name (case-insensitive) or by raw octal scancode
// (digits only, e.g. "0102"). Returns true and fills *pScancode on success.
bool FindNamedKey(const std::wstring& name, uint8_t* pScancode)
{
    for (size_t i = 0; i < g_namedKeysCount; i++)
    {
        if (WStringEqualsIgnoreCase(name, g_namedKeys[i].name))
        {
            *pScancode = g_namedKeys[i].scancode;
            return true;
        }
    }

    bool okAllOctalDigits = !name.empty();
    for (wchar_t ch : name)
        if (ch < L'0' || ch > L'7') { okAllOctalDigits = false; break; }
    if (okAllOctalDigits)
    {
        uint16_t value = 0;
        for (wchar_t ch : name)
            value = (uint16_t)((value << 3) + (ch - L'0'));
        if (value > 0377)
            return false;
        *pScancode = (uint8_t)value;
        return true;
    }

    return false;
}

// Press (or release) one key and pump enough frames for the emulator to
// see it, the same way mo/TypeKey above does for a press+release pair.
void SetKeyState(uint8_t scancode, bool okPressed, int holdFrames)
{
    Emulator_KeyEvent(scancode, okPressed);
    PumpFrames(holdFrames);
}

// "key MOD+KEY": hold MOD, click KEY, release MOD, per the timing in the
// class comment above -- 1 frame around the modifier's own press/release,
// 3 frames around the target key's press/release (the final 3-frame wait
// after releasing MOD is deliberate: it's there so back-to-back "key ..."
// commands don't run together).
void ClickKeyWithModifier(uint8_t modScancode, uint8_t keyScancode)
{
    bool okAr2 = false; //(modScancode == BK_KEY_AR2);
    SetKeyState(modScancode, true, KEY_MODIFIER_HOLD_FRAMES);
    SetKeyState(keyScancode, true, KEY_HOLD_FRAMES);
    SetKeyState(keyScancode, false, KEY_MODIFIER_HOLD_FRAMES);
    SetKeyState(modScancode, false, KEY_HOLD_FRAMES);
}

// "key KEY" with no modifier: press, hold, release, hold.
void ClickKey(uint8_t scancode)
{
    SetKeyState(scancode, true, KEY_HOLD_FRAMES);
    SetKeyState(scancode, false, KEY_HOLD_FRAMES);
}

// Parse "KEY" or "MOD+KEY" out of params.commandText, given the prefix
// length the matched table row consumed (params.paramPrefixLength, set by
// MatchCommand -- this is the single source of truth for where the key
// spec starts, so CmdKeyDown/CmdKeyUp/CmdKeyClick don't each need their
// own logic to figure out whether "kd"/"ku"/"k" or "key down"/"key up"/
// "key" matched). paramPrefixLength covers only the literal table prefix
// (e.g. 2 for "kd", 8 for "key down"); the single space that ARGINFO_FILENAME
// requires right after it is not included, so it's skipped here. Reports
// an error and returns false if anything doesn't parse.
bool ParseKeySpec(const ConsoleCommandParams& params, size_t prefixLength,
                   uint8_t* pScancode, uint8_t* pModScancode, bool* pHasModifier)
{
    std::wstring spec = params.commandText.substr(prefixLength + 1);
    *pHasModifier = false;

    size_t plusPos = spec.find(L'+');
    if (plusPos != std::wstring::npos)
    {
        std::wstring modName = spec.substr(0, plusPos);
        std::wstring keyName = spec.substr(plusPos + 1);
        if (!FindNamedKey(modName, pModScancode))
        {
            std::wcout << L"Unknown modifier key: " << modName << std::endl;
            return false;
        }
        if (!FindNamedKey(keyName, pScancode))
        {
            std::wcout << L"Unknown key: " << keyName << std::endl;
            return false;
        }
        *pHasModifier = true;
        return true;
    }

    if (!FindNamedKey(spec, pScancode))
    {
        std::wcout << L"Unknown key: " << spec << std::endl;
        return false;
    }
    return true;
}

void CmdKeyDown(const ConsoleCommandParams& params)
{
    uint8_t scancode, modScancode;
    bool okHasModifier;
    if (!ParseKeySpec(params, params.paramPrefixLength, &scancode, &modScancode, &okHasModifier))
        return;

    if (okHasModifier)
        SetKeyState(modScancode, true, KEY_MODIFIER_HOLD_FRAMES);
    SetKeyState(scancode, true, KEY_HOLD_FRAMES);
}

void CmdKeyUp(const ConsoleCommandParams& params)
{
    uint8_t scancode, modScancode;
    bool okHasModifier;
    if (!ParseKeySpec(params, params.paramPrefixLength, &scancode, &modScancode, &okHasModifier))
        return;

    SetKeyState(scancode, false, KEY_MODIFIER_HOLD_FRAMES);
    if (okHasModifier)
        SetKeyState(modScancode, false, KEY_HOLD_FRAMES);
}

void CmdKeyClick(const ConsoleCommandParams& params)
{
    uint8_t scancode, modScancode;
    bool okHasModifier;
    if (!ParseKeySpec(params, params.paramPrefixLength, &scancode, &modScancode, &okHasModifier))
        return;

    if (okHasModifier)
        ClickKeyWithModifier(modScancode, scancode);
    else
        ClickKey(scancode);
}

// Set the board's trace mask and report the new state.
// Mirrors ConsoleView_TraceLog: turning tracing off also closes the log
// file handle so trace.log is flushed and available immediately.
void TraceLog(uint32_t value)
{
    g_pBoard->SetTrace(value);
    if (value != TRACE_NONE)
    {
        TCHAR bufFlags[7];
        PrintOctalValue(bufFlags, (uint16_t)g_pBoard->GetTrace());
        std::wcout << L" Trace ON, trace flags " << bufFlags << std::endl;
    }
    else
    {
        std::wcout << L" Trace OFF." << std::endl;
        DebugLogCloseFile();
    }
}

void CmdTraceLogWithMask(const ConsoleCommandParams& params)
{
    TraceLog(params.paramOct1);
}

void CmdTraceLogOnOff(const ConsoleCommandParams& /*params*/)
{
    uint32_t dwTrace = (g_pBoard->GetTrace() == TRACE_NONE ? TRACE_ALL : TRACE_NONE);
    TraceLog(dwTrace);
}

void CmdClearTraceLog(const ConsoleCommandParams& /*params*/)
{
    DebugLogClear();
    std::wcout << L" Trace log cleared." << std::endl;
}

void CmdPrintAllBreakpoints(const ConsoleCommandParams& /*params*/)
{
    const uint16_t* pbps = Emulator_GetCPUBreakpointList();
    if (pbps == nullptr || *pbps == 0177777)
    {
        std::wcout << L" No breakpoints." << std::endl;
    }
    else
    {
        while (*pbps != 0177777)
        {
            TCHAR bufAddr[7];
            PrintOctalValue(bufAddr, *pbps);
            std::wcout << L"  " << bufAddr << Symbols_FormatSuffix(*pbps) << std::endl;
            pbps++;
        }
    }
}

void CmdSetBreakpointAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    bool result = Emulator_AddCPUBreakpoint(address);
    if (!result)
    {
        std::wcout << L" Failed to add breakpoint." << std::endl;
        return;
    }
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, address);
    std::wcout << L"Breakpoint set at " << bufAddr << Symbols_FormatSuffix(address) << std::endl;
}

// "b NAME" -- set a breakpoint at a symbol loaded via "symbols load"/"sym
// load", as an alternative to the raw-address "bXXXXXX" form.
void CmdSetBreakpointByName(const ConsoleCommandParams& params)
{
    uint16_t address;
    if (!Symbols_FindByName(params.paramFilename, &address))
    {
        std::wcout << L" Unknown symbol: " << params.paramFilename << std::endl;
        return;
    }
    bool result = Emulator_AddCPUBreakpoint(address);
    if (!result)
    {
        std::wcout << L" Failed to add breakpoint." << std::endl;
        return;
    }
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, address);
    std::wcout << L"Breakpoint set at " << bufAddr << L" <" << params.paramFilename << L">" << std::endl;
}

void CmdRemoveBreakpointAtAddress(const ConsoleCommandParams& params)
{
    uint16_t address = params.paramOct1;
    bool result = Emulator_RemoveCPUBreakpoint(address);
    if (!result)
    {
        std::wcout << L" Failed to remove breakpoint." << std::endl;
        return;
    }
    TCHAR bufAddr[7];
    PrintOctalValue(bufAddr, address);
    std::wcout << L"Breakpoint removed at " << bufAddr << std::endl;
}

void CmdRemoveAllBreakpoints(const ConsoleCommandParams& /*params*/)
{
    Emulator_RemoveAllBreakpoints(m_okCurrentProc);
    std::wcout << L"All breakpoints removed." << std::endl;
}

//////////////////////////////////////////////////////////////////////
// Command table
//
// IMPORTANT: as with the original, list more specific forms (with more
// parameters) before less specific ones, since matching stops at the
// first pattern that fits -- e.g. "r%d" must come before "r", and
// "bc%ho" must come before "bc" which must come before "b".

enum ConsoleCommandArgInfo
{
    ARGINFO_NONE,       // No parameters
    ARGINFO_REG,        // Register number 0..7
    ARGINFO_OCT,        // Octal value
    ARGINFO_DEC,        // Decimal value
    ARGINFO_REG_OCT,    // Register number, octal value
    ARGINFO_FILENAME,       // A space then a filename (rest of the line, verbatim)
    ARGINFO_OPT_FILENAME,   // Optional: either bare command, or space + filename
    ARGINFO_OCT_MODIFIERS,  // Optional address, then any combination of postfix modifiers
    ARGINFO_OCT_EQ_OCT,     // "ADDR=VALUE" or "ADDR VALUE", then optional " bytes"
};

typedef void (*CONSOLE_COMMAND_CALLBACK)(const ConsoleCommandParams& params);

struct ConsoleCommandStruct
{
    const wchar_t* prefix;          // Fixed command prefix to match, e.g. L"r"
    ConsoleCommandArgInfo arginfo;  // What follows the prefix, if anything
    CONSOLE_COMMAND_CALLBACK callback;
};

const ConsoleCommandStruct ConsoleCommands[] =
{
    { L"?",     ARGINFO_NONE,    CmdShowHelp },
    { L"help",  ARGINFO_NONE,    CmdShowHelp },
    { L"h",     ARGINFO_NONE,    CmdShowHelp },

    { L"p",     ARGINFO_NONE,    CmdSwitchCpuPpu },
    { L"r",     ARGINFO_REG_OCT, CmdSetRegisterValue },          // rN=XXXXXX
    { L"r",     ARGINFO_REG,     CmdPrintRegister },              // rN
    { L"rps=",  ARGINFO_OCT,     CmdSetRegisterPSW },             // rps=XXXXXX
    { L"rps ",  ARGINFO_OCT,     CmdSetRegisterPSW },             // rps XXXXXX
    { L"rps",   ARGINFO_NONE,    CmdPrintRegisterPSW },           // rps
    { L"rpc=",  ARGINFO_OCT,     CmdSetRegisterPC },              // rpc=XXXXXX
    { L"rpc ",  ARGINFO_OCT,     CmdSetRegisterPC },              // rpc XXXXXX
    { L"rpc",   ARGINFO_NONE,    CmdPrintRegisterPC },            // rpc
    { L"rsp=",  ARGINFO_OCT,     CmdSetRegisterSP },              // rsp=XXXXXX
    { L"rsp ",  ARGINFO_OCT,     CmdSetRegisterSP },              // rsp XXXXXX
    { L"rsp",   ARGINFO_NONE,    CmdPrintRegisterSP },            // rsp
    { L"info floppy", ARGINFO_NONE, CmdPrintFloppyRegisters },     // info floppy
    { L"i floppy", ARGINFO_NONE,  CmdPrintFloppyRegisters },       // i floppy
    { L"info",     ARGINFO_NONE,  CmdShowStatus },                 // info
    { L"i",        ARGINFO_NONE,  CmdShowStatus },                 // i
    { L"regs",  ARGINFO_NONE,    CmdPrintAllRegisters },          // regs
    { L"r",     ARGINFO_NONE,    CmdPrintAllRegisters },          // r

    { L"next",    ARGINFO_NONE,    CmdStepOver },
    { L"n",       ARGINFO_NONE,    CmdStepOver },
    { L"step",    ARGINFO_NONE,    CmdStepInto },
    { L"s",       ARGINFO_NONE,    CmdStepInto },

    { L"reset", ARGINFO_NONE,    CmdReset },

    { L"disasm ", ARGINFO_OCT,   CmdPrintDisassembleAtAddress }, // disasm XXXXXX
    { L"disasm",  ARGINFO_NONE,  CmdPrintDisassembleAtPC },      // disasm
    { L"d",     ARGINFO_OCT,     CmdPrintDisassembleAtAddress },  // dXXXXXX
    { L"D",     ARGINFO_OCT,     CmdPrintDisassembleAtAddress },  // DXXXXXX
    { L"d",     ARGINFO_NONE,    CmdPrintDisassembleAtPC },       // d
    { L"D",     ARGINFO_NONE,    CmdPrintDisassembleAtPC },       // D

    { L"memsave", ARGINFO_OPT_FILENAME, CmdSaveMemoryDump },        // memsave [FILE]
    { L"statesave", ARGINFO_FILENAME,   CmdStateSave },              // statesave FILENAME
    { L"stateload", ARGINFO_FILENAME,   CmdStateLoad },              // stateload FILENAME

    { L"symbols load", ARGINFO_FILENAME, CmdLoadSymbols },           // symbols load FILENAME
    { L"sym load",      ARGINFO_FILENAME, CmdLoadSymbols },          // sym load FILENAME
    { L"symbols",       ARGINFO_NONE,     CmdListSymbols },          // symbols
    { L"sym",           ARGINFO_NONE,     CmdListSymbols },          // sym

    { L"disk1 attach", ARGINFO_FILENAME, CmdAttachFloppyImage },     // disk1 attach FILENAME
    { L"disk2 attach", ARGINFO_FILENAME, CmdAttachFloppyImage },     // disk2 attach FILENAME
    { L"disk3 attach", ARGINFO_FILENAME, CmdAttachFloppyImage },     // disk3 attach FILENAME
    { L"disk4 attach", ARGINFO_FILENAME, CmdAttachFloppyImage },     // disk4 attach FILENAME
    { L"disk1 a", ARGINFO_FILENAME,      CmdAttachFloppyImage },     // disk1 a FILENAME
    { L"disk2 a", ARGINFO_FILENAME,      CmdAttachFloppyImage },     // disk2 a FILENAME
    { L"disk3 a", ARGINFO_FILENAME,      CmdAttachFloppyImage },     // disk3 a FILENAME
    { L"disk4 a", ARGINFO_FILENAME,      CmdAttachFloppyImage },     // disk4 a FILENAME
    { L"disk1 detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // disk1 detach
    { L"disk2 detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // disk2 detach
    { L"disk3 detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // disk3 detach
    { L"disk4 detach", ARGINFO_NONE,     CmdDetachFloppyImage },     // disk4 detach
    { L"disk1 d", ARGINFO_NONE,          CmdDetachFloppyImage },     // disk1 d
    { L"disk2 d", ARGINFO_NONE,          CmdDetachFloppyImage },     // disk2 d
    { L"disk3 d", ARGINFO_NONE,          CmdDetachFloppyImage },     // disk3 d
    { L"disk4 d", ARGINFO_NONE,          CmdDetachFloppyImage },     // disk4 d
    { L"cart1 attach", ARGINFO_FILENAME, CmdAttachCartridge },       // cart1 attach FILENAME
    { L"cart2 attach", ARGINFO_FILENAME, CmdAttachCartridge },       // cart2 attach FILENAME
    { L"cart1 a", ARGINFO_FILENAME,      CmdAttachCartridge },       // cart1 a FILENAME
    { L"cart2 a", ARGINFO_FILENAME,      CmdAttachCartridge },       // cart2 a FILENAME
    { L"cart1 detach", ARGINFO_NONE,     CmdDetachCartridge },       // cart1 detach
    { L"cart2 detach", ARGINFO_NONE,     CmdDetachCartridge },       // cart2 detach
    { L"cart1 d", ARGINFO_NONE,          CmdDetachCartridge },       // cart1 d
    { L"cart2 d", ARGINFO_NONE,          CmdDetachCartridge },       // cart2 d

    { L"screentext", ARGINFO_OPT_FILENAME, CmdScreenText },
    { L"screen",  ARGINFO_OPT_FILENAME, CmdScreenshot },

    { L"key down", ARGINFO_FILENAME, CmdKeyDown },   // key down KEY, key down MOD+KEY
    { L"kd",       ARGINFO_FILENAME, CmdKeyDown },   // kd KEY, kd MOD+KEY
    { L"key up",   ARGINFO_FILENAME, CmdKeyUp },     // key up KEY, key up MOD+KEY
    { L"ku",       ARGINFO_FILENAME, CmdKeyUp },     // ku KEY, ku MOD+KEY
    { L"key",      ARGINFO_FILENAME, CmdKeyClick },  // key KEY, key MOD+KEY
    { L"k",        ARGINFO_FILENAME, CmdKeyClick },  // k KEY, k MOD+KEY

    { L"tc",    ARGINFO_NONE,    CmdClearTraceLog },              // tc
    { L"trace clear", ARGINFO_NONE, CmdClearTraceLog },           // trace clear
    { L"t clear", ARGINFO_NONE,  CmdClearTraceLog },               // t clear
    { L"trace ", ARGINFO_OCT,    CmdTraceLogWithMask },           // trace XXXXXX
    { L"t",     ARGINFO_OCT,     CmdTraceLogWithMask },           // tXXXXXX
    { L"trace", ARGINFO_NONE,    CmdTraceLogOnOff },              // trace
    { L"t",     ARGINFO_NONE,    CmdTraceLogOnOff },              // t
    { L"memory", ARGINFO_OCT_MODIFIERS, CmdShowMemory },      // memory [XXXXXX] [bytes] [hex] [nochars]
    { L"m",      ARGINFO_OCT_MODIFIERS, CmdShowMemory },      // m[XXXXXX] [bytes] [hex] [nochars]
    //{ L"memset", ARGINFO_OCT_EQ_OCT,    CmdSetMemory },       // memset ADDR=VALUE [bytes]
    //{ L"ms",     ARGINFO_OCT_EQ_OCT,    CmdSetMemory },       // ms ADDR=VALUE [bytes]

    { L"continue frames ", ARGINFO_DEC, CmdRunFrames },             // continue frames N (decimal)
    { L"cf",    ARGINFO_DEC,    CmdRunFrames },                     // cfN (decimal)
    { L"continue ", ARGINFO_OCT, CmdRunToAddress },                 // continue XXXXXX
    { L"continue",  ARGINFO_NONE, CmdRun },                         // continue
    { L"c",     ARGINFO_OCT,     CmdRunToAddress },                 // cXXXXXX
    { L"c",     ARGINFO_NONE,    CmdRun },                          // c

    { L"bc",    ARGINFO_OCT,     CmdRemoveBreakpointAtAddress },  // bcXXXXXX
    { L"bc",    ARGINFO_NONE,    CmdRemoveAllBreakpoints },       // bc
    { L"b",     ARGINFO_OCT,     CmdSetBreakpointAtAddress },     // bXXXXXX
    { L"b",     ARGINFO_FILENAME, CmdSetBreakpointByName },       // b NAME (symbol, from "symbols load")
    { L"b",     ARGINFO_NONE,    CmdPrintAllBreakpoints },        // b
};

const size_t ConsoleCommandsCount = sizeof(ConsoleCommands) / sizeof(ConsoleCommands[0]);

// Try to match `command` against one table entry.
// On success, fills in `params` (paramReg1 / paramOct1) as needed and returns true.
bool MatchCommand(const std::wstring& command, const ConsoleCommandStruct& cmd, ConsoleCommandParams& params)
{
    const std::wstring prefix = cmd.prefix;
    if (command.compare(0, prefix.size(), prefix) != 0)
        return false;
    std::wstring rest = command.substr(prefix.size());
    params.paramPrefixLength = prefix.size();

    switch (cmd.arginfo)
    {
    case ARGINFO_NONE:
        return rest.empty();

    case ARGINFO_REG:
        {
            if (rest.empty() || rest.size() > 1 || !iswdigit(rest[0]))
                return false;
            params.paramReg1 = rest[0] - L'0';
            return true;  // range-checked by caller (0..7 expected)
        }

    case ARGINFO_OCT:
        {
            if (rest.empty())
                return false;
            for (wchar_t ch : rest)
                if (ch < L'0' || ch > L'7') return false;
            uint16_t value = 0;
            for (wchar_t ch : rest)
                value = (uint16_t)((value << 3) + (ch - L'0'));
            params.paramOct1 = value;
            return true;
        }

    case ARGINFO_DEC:
        {
            if (rest.empty())
                return false;
            for (wchar_t ch : rest)
                if (ch < L'0' || ch > L'9') return false;
            uint32_t value = 0;
            for (wchar_t ch : rest)
            {
                value = value * 10 + (uint32_t)(ch - L'0');
                if (value > 0xffff) value = 0xffff;  // clamp, paramOct1 is 16-bit
            }
            params.paramOct1 = (uint16_t)value;
            return true;
        }

    case ARGINFO_REG_OCT:
        {
            // Accept "N=XXXXXX" or "N XXXXXX"
            if (rest.empty() || !iswdigit(rest[0]))
                return false;
            params.paramReg1 = rest[0] - L'0';
            std::wstring tail = rest.substr(1);
            if (tail.empty() || (tail[0] != L'=' && tail[0] != L' '))
                return false;
            tail = tail.substr(1);
            if (tail.empty())
                return false;
            for (wchar_t ch : tail)
                if (ch < L'0' || ch > L'7') return false;
            uint16_t value = 0;
            for (wchar_t ch : tail)
                value = (uint16_t)((value << 3) + (ch - L'0'));
            params.paramOct1 = value;
            return true;
        }

    case ARGINFO_OCT_EQ_OCT:
        {
            // "ADDR=VALUE" or "ADDR VALUE", optionally with one leading
            // space before ADDR too (so both "ms1000=123" and "ms 1000=123"
            // work), then optional " bytes" at the end.
            size_t startPos = 0;
            if (!rest.empty() && rest[0] == L' ')
                startPos = 1;
            if (startPos >= rest.size() || !iswdigit(rest[startPos]))
                return false;

            size_t pos = startPos;
            while (pos < rest.size() && rest[pos] >= L'0' && rest[pos] <= L'7')
                pos++;
            std::wstring addrToken = rest.substr(startPos, pos - startPos);
            if (addrToken.empty())
                return false;

            if (pos >= rest.size() || (rest[pos] != L'=' && rest[pos] != L' '))
                return false;
            pos++;  // skip the '=' or ' '

            size_t valueStart = pos;
            while (pos < rest.size() && rest[pos] >= L'0' && rest[pos] <= L'7')
                pos++;
            std::wstring valueToken = rest.substr(valueStart, pos - valueStart);
            if (valueToken.empty())
                return false;

            // Optional trailing " bytes"
            if (pos < rest.size())
            {
                std::wstring trailing = rest.substr(pos);
                if (trailing == L" bytes")
                    params.paramFlags |= MEMFLAG_BYTES;
                else
                    return false;  // anything else trailing is not recognized
            }

            uint16_t addrValue = 0;
            for (wchar_t ch : addrToken)
                addrValue = (uint16_t)((addrValue << 3) + (ch - L'0'));
            uint16_t dataValue = 0;
            for (wchar_t ch : valueToken)
                dataValue = (uint16_t)((dataValue << 3) + (ch - L'0'));

            params.paramOct1 = addrValue;
            params.paramOct2 = dataValue;
            return true;
        }

    case ARGINFO_FILENAME:
        {
            // Expect a single space then a non-empty filename, e.g. "screen shot.png"
            if (rest.empty() || rest[0] != L' ')
                return false;
            std::wstring filename = rest.substr(1);
            if (filename.empty())
                return false;
            params.paramFilename = filename;
            return true;
        }

    case ARGINFO_OPT_FILENAME:
        {
            // Bare command (no filename) or "command FILENAME"
            if (rest.empty())
                return true;  // paramFilename stays empty; caller generates default name
            if (rest[0] != L' ')
                return false;
            params.paramFilename = rest.substr(1);
            return !params.paramFilename.empty();
        }

    case ARGINFO_OCT_MODIFIERS:
        {
            // Optional address -- either glued directly to the prefix
            // ("m100260") or separated by one space ("m 100260") -- then
            // zero or more space-separated modifier words in any
            // order/combination ("bytes", "hex", "nochars"), e.g.:
            //   "m", "m100260", "m hex", "m100260 bytes hex nochars",
            //   "m 100260 bytes hex nochars"
            size_t pos = 0;

            // Peek at the first token, whether glued (no leading space) or
            // separated by one space, and check if it's a genuine octal
            // address before committing to consuming it -- "x bytes" must
            // NOT mistake the leading space for "address then modifiers"
            // when there's no address at all.
            size_t addrStart = (pos < rest.size() && rest[pos] == L' ') ? pos + 1 : pos;
            size_t addrEnd = rest.find(L' ', addrStart);
            std::wstring addrToken = (addrEnd == std::wstring::npos) ? rest.substr(addrStart) : rest.substr(addrStart, addrEnd - addrStart);

            bool okAddrToken = !addrToken.empty();
            for (wchar_t ch : addrToken)
                if (ch < L'0' || ch > L'7') { okAddrToken = false; break; }

            if (okAddrToken)
            {
                uint16_t value = 0;
                for (wchar_t ch : addrToken)
                    value = (uint16_t)((value << 3) + (ch - L'0'));
                params.paramOct1 = value;
                params.paramHasAddress = true;
                pos = addrStart + addrToken.size();
            }
            // else: no valid address present: leave pos at 0, so the
            // modifier loop below sees any leading space and token itself.

            // Remaining modifier words, space-separated, any order.
            while (pos < rest.size())
            {
                if (rest[pos] != L' ')
                    return false;
                pos++;  // skip the space
                size_t wordEnd = rest.find(L' ', pos);
                std::wstring word = (wordEnd == std::wstring::npos) ? rest.substr(pos) : rest.substr(pos, wordEnd - pos);
                if (word.empty())
                    return false;  // double space or trailing space -- reject rather than silently ignore

                if (word == L"bytes")
                    params.paramFlags |= MEMFLAG_BYTES;
                else if (word == L"hex")
                    params.paramFlags |= MEMFLAG_HEX;
                else if (word == L"nochars")
                    params.paramFlags |= MEMFLAG_NOCHARS;
                else
                    return false;  // Unknown modifier word

                pos = (wordEnd == std::wstring::npos) ? rest.size() : wordEnd;
            }
            return true;
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////

void PrintConsolePrompt()
{
    CProcessor* pProc = GetCurrentProcessor();
    TCHAR bufAddr[7];
    Console_ColorPrompt();
    PrintOctalValue(bufAddr, pProc->GetPC());
    std::wcout << pProc->GetName() << ":" << bufAddr << L"> ";
    Console_ColorReset();
}

bool HasPendingContinuation()
{
    return g_continuation.kind != ContinuationKind::None;
}

void ClearPendingContinuation()
{
    g_continuation.kind = ContinuationKind::None;
}

// Print the next page for the armed continuation (memory or disasm),
// picking up exactly where the previous page left off, and re-arm for
// the page after that. Does nothing if no continuation is armed.
void RunPendingContinuation()
{
    if (g_continuation.kind == ContinuationKind::None)
        return;

    ContinuationState state = g_continuation;  // Local copy: handlers below overwrite g_continuation
    CProcessor* pProc = GetCurrentProcessor();

    if (state.kind == ContinuationKind::Memory)
    {
        uint16_t nextAddress;
        PrintMemoryDumpGeneric(pProc, state.address, state.memoryFlags, 8, &nextAddress);
        state.address = nextAddress;
        ArmContinuation(state);
    }
    else if (state.kind == ContinuationKind::Disasm)
    {
        uint16_t nextAddress;
        PrintDisassemble(pProc, state.address, false, state.disasmShort, &nextAddress);
        state.address = nextAddress;
        ArmContinuation(state);
    }
}

bool DoConsoleCommand(const std::wstring& command)
{
    if (command.empty())
        return true;  // Nothing to do, keep looping

    if (command == L"q" || command == L"quit" || command == L"exit")
        return false;

    ConsoleCommandParams params;
    params.commandText = command;

    bool parsedOkay = false, parseError = false;
    for (size_t i = 0; i < ConsoleCommandsCount; i++)
    {
        const ConsoleCommandStruct& cmd = ConsoleCommands[i];
        ConsoleCommandParams trialParams;
        trialParams.commandText = command;

        if (!MatchCommand(command, cmd, trialParams))
            continue;

        if ((cmd.arginfo == ARGINFO_REG || cmd.arginfo == ARGINFO_REG_OCT) &&
            (trialParams.paramReg1 < 0 || trialParams.paramReg1 > 7))
        {
            std::wcout << MESSAGE_INVALID_REGNUM << std::endl;
            parseError = true;
            break;
        }

        params = trialParams;
        cmd.callback(params);
        parsedOkay = true;
        break;
    }

    if (!parsedOkay && !parseError)
        std::wcout << MESSAGE_UNKNOWN_COMMAND << std::endl;

    return true;
}

//////////////////////////////////////////////////////////////////////
