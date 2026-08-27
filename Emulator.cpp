// Emulator.cpp

#include "stdafx.h"
#include "ukncbtldebug.h"
#include "Emulator.h"
#include "emubase/Emubase.h"


//////////////////////////////////////////////////////////////////////


CMotherboard* g_pBoard = nullptr;

static bool g_okEmulatorInitialized = false;
bool g_okEmulatorRunning = false;

int m_wEmulatorCPUBpsCount = 0;
int m_wEmulatorPPUBpsCount = 0;
uint16_t m_EmulatorCPUBps[MAX_BREAKPOINTCOUNT + 1];
uint16_t m_EmulatorPPUBps[MAX_BREAKPOINTCOUNT + 1];
uint16_t m_wEmulatorTempCPUBreakpoint = 0177777;
uint16_t m_wEmulatorTempPPUBreakpoint = 0177777;

static bool m_okEmulatorSound = false;
static bool m_okEmulatorSoundAY = false;

static int m_nTickCount = 0;
static uint32_t m_dwEmulatorUptime = 0;  // UKNC uptime, seconds, from turn on or reset, increments every 25 frames
static long m_nUptimeFrameCount = 0;

uint8_t* g_pEmulatorRam[3];  // RAM values - for change tracking
uint8_t* g_pEmulatorChangedRam[3];  // RAM change flags
uint16_t g_wEmulatorCpuR[9];      // Current CPU register values
uint16_t g_wEmulatorPrevCpuR[9];  // Previous CPU register values
uint16_t g_wEmulatorPpuR[9];      // Current PPU register values
uint16_t g_wEmulatorPrevPpuR[9];  // Previous PPU register values

static const int KEYEVENT_QUEUE_SIZE = 32;
static uint16_t m_EmulatorKeyQueue[KEYEVENT_QUEUE_SIZE];
static int m_EmulatorKeyQueueTop = 0;
static int m_EmulatorKeyQueueBottom = 0;
static int m_EmulatorKeyQueueCount = 0;

void CALLBACK Emulator_FeedDAC(unsigned short l, unsigned short r);


//////////////////////////////////////////////////////////////////////


const uint32_t ScreenView_StandardGRBColors[16 * 8] =
{
    0x000000, 0x000080, 0x800000, 0x800080, 0x008000, 0x008080, 0x808000, 0x808080,
    0x000000, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF,
    0x000000, 0x000060, 0x800000, 0x800060, 0x008000, 0x008060, 0x808000, 0x808060,
    0x000000, 0x0000DF, 0xFF0000, 0xFF00DF, 0x00FF00, 0x00FFDF, 0xFFFF00, 0xFFFFDF,
    0x000000, 0x000080, 0x600000, 0x600080, 0x008000, 0x008080, 0x608000, 0x608080,
    0x000000, 0x0000FF, 0xDF0000, 0xDF00FF, 0x00FF00, 0x00FFFF, 0xDFFF00, 0xDFFFFF,
    0x000000, 0x000060, 0x600000, 0x600060, 0x008000, 0x008060, 0x608000, 0x608060,
    0x000000, 0x0000DF, 0xDF0000, 0xDF00DF, 0x00FF00, 0x00FFDF, 0xDFFF00, 0xDFFFDF,
    0x000000, 0x000080, 0x800000, 0x800080, 0x006000, 0x006080, 0x806000, 0x806080,
    0x000000, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00DF00, 0x00DFFF, 0xFFDF00, 0xFFDFFF,
    0x000000, 0x000060, 0x800000, 0x800060, 0x006000, 0x006060, 0x806000, 0x806060,
    0x000000, 0x0000DF, 0xFF0000, 0xFF00DF, 0x00DF00, 0x00DFDF, 0xFFDF00, 0xFFDFDF,
    0x000000, 0x000080, 0x600000, 0x600080, 0x006000, 0x006080, 0x606000, 0x606080,
    0x000000, 0x0000FF, 0xDF0000, 0xDF00FF, 0x00DF00, 0x00DFFF, 0xDFDF00, 0xDFDFFF,
    0x000000, 0x000060, 0x600000, 0x600060, 0x006000, 0x006060, 0x606000, 0x606060,
    0x000000, 0x0000DF, 0xDF0000, 0xDF00DF, 0x00DF00, 0x00DFDF, 0xDFDF00, 0xDFDFDF,
};


//////////////////////////////////////////////////////////////////////

bool Emulator_Init()
{
    ASSERT(g_pBoard == nullptr);

    ::memset(g_pEmulatorRam, 0, sizeof(g_pEmulatorRam));
    ::memset(g_pEmulatorChangedRam, 0, sizeof(g_pEmulatorChangedRam));
    CProcessor::Init();

    m_wEmulatorCPUBpsCount = m_wEmulatorPPUBpsCount = 0;
    for (int i = 0; i <= MAX_BREAKPOINTCOUNT; i++)
    {
        m_EmulatorCPUBps[i] = 0177777;
        m_EmulatorPPUBps[i] = 0177777;
        //uint16_t address = Settings_GetDebugBreakpoint(i, true);
        //m_EmulatorCPUBps[i] = address;
        //if (address != 0177777) m_wEmulatorCPUBpsCount = i + 1;
        //address = Settings_GetDebugBreakpoint(i, false);
        //m_EmulatorPPUBps[i] = address;
        //if (address != 0177777) m_wEmulatorPPUBpsCount = i + 1;
    }

    g_pBoard = new CMotherboard();

    uint8_t buffer[32768];

    // Load ROM file
    memset(buffer, 0, 32768);
    FILE* fpRomFile = ::fopen("uknc_rom.bin", "rb");
    if (fpRomFile == nullptr)
    {
        AlertWarning(_T("Could not open ROM file uknc_rom.bin"));
        return false;
    }
    size_t bytesToRead = 32256;
    size_t dwBytesRead = ::fread(buffer, 1, bytesToRead, fpRomFile);
    if (dwBytesRead != bytesToRead)
    {
        ::fclose(fpRomFile);
        AlertWarning(_T("Could not read ROM file data uknc_rom.bin"));
        return false;
    }

    g_pBoard->LoadROM(buffer);

    //g_pBoard->SetNetStation((uint16_t)Settings_GetNetStation());

    g_pBoard->SetSoundGenCallback(nullptr);

    g_pBoard->Reset();

    m_nUptimeFrameCount = 0;
    m_dwEmulatorUptime = 0;

    // Allocate memory for old RAM values
    for (int i = 0; i < 3; i++)
    {
        g_pEmulatorRam[i] = (uint8_t*) ::calloc(65536, 1);
        g_pEmulatorChangedRam[i] = (uint8_t*) ::calloc(65536, 1);
    }

    g_okEmulatorInitialized = true;
    return true;
}

void Emulator_Done()
{
    ASSERT(g_pBoard != nullptr);

    //// Save breakpoints
    //for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    //    Settings_SetDebugBreakpoint(i, true, i < m_wEmulatorCPUBpsCount ? m_EmulatorCPUBps[i] : 0177777);
    //for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    //    Settings_SetDebugBreakpoint(i, false, i < m_wEmulatorPPUBpsCount ? m_EmulatorPPUBps[i] : 0177777);

    CProcessor::Done();

    g_pBoard->SetSoundGenCallback(nullptr);

    delete g_pBoard;
    g_pBoard = nullptr;

    // Free memory used for old RAM values
    for (int i = 0; i < 3; i++)
    {
        ::free(g_pEmulatorRam[i]);
        ::free(g_pEmulatorChangedRam[i]);
    }

    g_okEmulatorInitialized = false;
}

void Emulator_Start()
{
    g_okEmulatorRunning = true;

    m_nTickCount = 0;

    // For proper breakpoint processing
    if (m_wEmulatorCPUBpsCount != 0 || m_wEmulatorPPUBpsCount)
    {
        g_pBoard->GetCPU()->ClearInternalTick();
        g_pBoard->GetPPU()->ClearInternalTick();
    }
}
void Emulator_Stop()
{
    g_okEmulatorRunning = false;

    Emulator_SetTempCPUBreakpoint(0177777);
    Emulator_SetTempPPUBreakpoint(0177777);
}

void Emulator_Reset()
{
    ASSERT(g_pBoard != nullptr);

    g_pBoard->Reset();

    m_nUptimeFrameCount = 0;
    m_dwEmulatorUptime = 0;
}

bool Emulator_AddCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorCPUBpsCount == MAX_BREAKPOINTCOUNT - 1 || address == 0177777)
        return false;
    for (int i = 0; i < m_wEmulatorCPUBpsCount; i++)  // Check if the BP exists
    {
        if (m_EmulatorCPUBps[i] == address)
            return false;  // Already in the list
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)  // Put in the first empty cell
    {
        if (m_EmulatorCPUBps[i] > address)  // found the place
        {
            memcpy(m_EmulatorCPUBps + i + 1, m_EmulatorCPUBps + i, sizeof(uint16_t) * (m_wEmulatorCPUBpsCount - i));
            m_EmulatorCPUBps[i] = address;
            break;
        }
        if (m_EmulatorCPUBps[i] == 0177777)
        {
            m_EmulatorCPUBps[i] = address;
            break;
        }
    }
    m_wEmulatorCPUBpsCount++;
    return true;
}
bool Emulator_AddPPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorPPUBpsCount == MAX_BREAKPOINTCOUNT - 1 || address == 0177777)
        return false;
    for (int i = 0; i < m_wEmulatorPPUBpsCount; i++)  // Check if the BP exists
    {
        if (m_EmulatorPPUBps[i] == address)
            return false;  // Already in the list
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)  // Put in the first empty cell
    {
        if (m_EmulatorPPUBps[i] > address)  // found the place
        {
            memcpy(m_EmulatorPPUBps + i + 1, m_EmulatorPPUBps + i, sizeof(uint16_t) * (m_wEmulatorPPUBpsCount - i));
            m_EmulatorPPUBps[i] = address;
            break;
        }
        if (m_EmulatorPPUBps[i] == 0177777)
        {
            m_EmulatorPPUBps[i] = address;
            break;
        }
    }
    m_wEmulatorPPUBpsCount++;
    return true;
}
bool Emulator_RemoveCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorCPUBpsCount == 0 || address == 0177777)
        return false;
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorCPUBps[i] == address)
        {
            m_EmulatorCPUBps[i] = 0177777;
            m_wEmulatorCPUBpsCount--;
            if (m_wEmulatorCPUBpsCount > i)  // fill the hole
            {
                memcpy(m_EmulatorCPUBps + i, m_EmulatorCPUBps + i + 1, sizeof(uint16_t) * (m_wEmulatorCPUBpsCount - i));
                m_EmulatorCPUBps[m_wEmulatorCPUBpsCount] = 0177777;
            }
            return true;
        }
    }
    return false;
}
bool Emulator_RemovePPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorPPUBpsCount == 0 || address == 0177777)
        return false;
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorPPUBps[i] == address)
        {
            m_EmulatorPPUBps[i] = 0177777;
            m_wEmulatorPPUBpsCount--;
            if (m_wEmulatorPPUBpsCount > i)  // fill the hole
            {
                memcpy(m_EmulatorPPUBps + i, m_EmulatorPPUBps + i + 1, sizeof(uint16_t) * (m_wEmulatorPPUBpsCount - i));
                m_EmulatorPPUBps[m_wEmulatorPPUBpsCount] = 0177777;
            }
            return true;
        }
    }
    return false;
}
void Emulator_SetTempCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorTempCPUBreakpoint != 0177777)
        Emulator_RemoveCPUBreakpoint(m_wEmulatorTempCPUBreakpoint);
    if (address == 0177777)
    {
        m_wEmulatorTempCPUBreakpoint = 0177777;
        return;
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorCPUBps[i] == address)
            return;  // We have regular breakpoint with the same address
    }
    m_wEmulatorTempCPUBreakpoint = address;
    m_EmulatorCPUBps[m_wEmulatorCPUBpsCount] = address;
    m_wEmulatorCPUBpsCount++;
}
void Emulator_SetTempPPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorTempPPUBreakpoint != 0177777)
        Emulator_RemovePPUBreakpoint(m_wEmulatorTempPPUBreakpoint);
    if (address == 0177777)
    {
        m_wEmulatorTempPPUBreakpoint = 0177777;
        return;
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorPPUBps[i] == address)
            return;  // We have regular breakpoint with the same address
    }
    m_wEmulatorTempPPUBreakpoint = address;
    m_EmulatorPPUBps[m_wEmulatorPPUBpsCount] = address;
    m_wEmulatorPPUBpsCount++;
}
const uint16_t* Emulator_GetCPUBreakpointList() { return m_EmulatorCPUBps; }
const uint16_t* Emulator_GetPPUBreakpointList() { return m_EmulatorPPUBps; }
bool Emulator_IsBreakpoint()
{
    uint16_t address = g_pBoard->GetCPU()->GetPC();
    if (m_wEmulatorCPUBpsCount > 0)
    {
        for (int i = 0; i < m_wEmulatorCPUBpsCount; i++)
        {
            if (address == m_EmulatorCPUBps[i])
                return true;
        }
    }
    address = g_pBoard->GetPPU()->GetPC();
    if (m_wEmulatorPPUBpsCount > 0)
    {
        for (int i = 0; i < m_wEmulatorPPUBpsCount; i++)
        {
            if (address == m_EmulatorPPUBps[i])
                return true;
        }
    }
    return false;
}
bool Emulator_IsBreakpoint(bool okCpuPpu, uint16_t address)
{
    int bpsCount = okCpuPpu ? m_wEmulatorCPUBpsCount : m_wEmulatorPPUBpsCount;
    uint16_t* pbps = okCpuPpu ? m_EmulatorCPUBps : m_EmulatorPPUBps;
    if (bpsCount == 0)
        return false;
    for (int i = 0; i < bpsCount; i++)
    {
        if (address == pbps[i])
            return true;
    }
    return false;
}
void Emulator_RemoveAllBreakpoints(bool okCpuPpu)
{
    uint16_t* pbps = okCpuPpu ? m_EmulatorCPUBps : m_EmulatorPPUBps;
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
        pbps[i] = 0177777;
    if (okCpuPpu)
        m_wEmulatorCPUBpsCount = 0;
    else
        m_wEmulatorPPUBpsCount = 0;
}

bool Emulator_SystemFrame()
{
    Emulator_ProcessKeyEvent();

    g_pBoard->SetCPUBreakpoints(m_wEmulatorCPUBpsCount > 0 ? m_EmulatorCPUBps : nullptr);
    g_pBoard->SetPPUBreakpoints(m_wEmulatorPPUBpsCount > 0 ? m_EmulatorPPUBps : nullptr);

    if (!g_pBoard->SystemFrame())  // Breakpoint hit
    {
        Emulator_SetTempCPUBreakpoint(0177777);
        Emulator_SetTempPPUBreakpoint(0177777);
        return false;
    }

    // Calculate emulator uptime (25 frames per second)
    m_nUptimeFrameCount++;
    if (m_nUptimeFrameCount >= 25)
    {
        m_dwEmulatorUptime++;
        m_nUptimeFrameCount = 0;

        //Global_showUptime(m_dwEmulatorUptime);
    }

    return true;
}

float Emulator_GetUptime()
{
    return (float)m_dwEmulatorUptime + float(m_nUptimeFrameCount) / 50.0f;
}

// Update cached values after Run or Step
void Emulator_OnUpdate()
{
    // Update stored register values
    for (int r = 0; r < 9; r++)
        g_wEmulatorPrevCpuR[r] = g_wEmulatorCpuR[r];
    for (int r = 0; r < 8; r++)
        g_wEmulatorCpuR[r] = g_pBoard->GetCPU()->GetReg(r);
    g_wEmulatorCpuR[8] = g_pBoard->GetCPU()->GetPSW();
    for (int r = 0; r < 9; r++)
        g_wEmulatorPrevPpuR[r] = g_wEmulatorPpuR[r];
    for (int r = 0; r < 8; r++)
        g_wEmulatorPpuR[r] = g_pBoard->GetPPU()->GetReg(r);
    g_wEmulatorPpuR[8] = g_pBoard->GetPPU()->GetPSW();

    // Update memory change flags
    for (int plane = 0; plane < 3; plane++)
    {
        uint8_t* pOld = g_pEmulatorRam[plane];
        uint8_t* pChanged = g_pEmulatorChangedRam[plane];
        uint16_t addr = 0;
        do
        {
            uint8_t newvalue = g_pBoard->GetRAMByte(plane, addr);
            uint8_t oldvalue = *pOld;
            *pChanged = (newvalue != oldvalue) ? 255 : 0;
            *pOld = newvalue;
            addr++;
            pOld++;  pChanged++;
        }
        while (addr < 65535);
    }
}

bool Emulator_IsRegisterChanged(int r)
{
    return g_wEmulatorPrevCpuR[r] != g_wEmulatorCpuR[r];
}

// Get RAM change flag for RAM word
//   addrtype - address mode - see ADDRTYPE_XXX constants
uint16_t Emulator_GetChangeRamStatus(int addrtype, uint16_t address)
{
    switch (addrtype)
    {
    case ADDRTYPE_RAM0:
    case ADDRTYPE_RAM1:
    case ADDRTYPE_RAM2:
        return *((uint16_t*)(g_pEmulatorChangedRam[addrtype] + address));
    case ADDRTYPE_RAM12:
        if (address < 0170000)
            return MAKEWORD(
                    *(g_pEmulatorChangedRam[1] + address / 2),
                    *(g_pEmulatorChangedRam[2] + address / 2));
        else
            return 0;
    default:
        return 0;
    }
}

bool Emulator_LoadROMCartridge(int slot, std::string& sFilePath)
{
    // Open file
    FILE* fpFile = ::fopen(sFilePath.c_str(), "rb");
    if (fpFile == nullptr)
        return false;

    // Allocate memory
    uint8_t* pImage = (uint8_t*) ::calloc(24 * 1024, 1);
    if (pImage == nullptr)
    {
        ::fclose(fpFile);
        return false;
    }
    size_t dwBytesRead = ::fread(pImage, 1, 24 * 1024, fpFile);
    if (dwBytesRead != 24 * 1024)
    {
        ::free(pImage);
        ::fclose(fpFile);
        return false;
    }

    g_pBoard->LoadROMCartridge(slot, pImage);

    // Free memory, close file
    ::free(pImage);
    ::fclose(fpFile);

    //TODO: Save the file name for a future SaveImage() call

    return true;
}

void Emulator_DetachCartridge(int slot)
{
    g_pBoard->UnloadROMCartridge(slot);
}

const uint32_t* Emulator_GetPalette()
{
    return ScreenView_StandardGRBColors;
}

void Emulator_PrepareScreenRGB32(void* pImageBits, const uint32_t* colors)
{
    if (pImageBits == nullptr) return;
    if (!g_okEmulatorInitialized) return;

    // Tag parsing loop
    uint8_t cursorYRGB = 0;
    bool okCursorType = false;
    uint8_t cursorPos = 128;
    bool cursorOn = false;
    uint8_t cursorAddress = 0;  // Address of graphical cursor
    uint16_t address = 0000270;  // Tag sequence start address
    bool okTagSize = false;  // Tag size: true - 4-word, false - 2-word (first tag is always 2-word)
    bool okTagType = false;  // Type of 4-word tag: true - set palette, false - set params
    int scale = 1;           // Horizontal scale: 1, 2, 4, or 8
    uint32_t palette = 0;       // Palette
    int32_t palettecurrent[8]; // Current palette; update each time we change the "palette" variable
    for (int i = 0; i < 8; i++)
        palettecurrent[i] = 0xFF0000000;
    uint8_t pbpgpr = 0;         // 3-bit Y-value modifier
    for (int yy = 0; yy < 307; yy++)
    {
        if (okTagSize)  // 4-word tag
        {
            uint16_t tag1 = g_pBoard->GetRAMWord(0, address);
            address += 2;
            uint16_t tag2 = g_pBoard->GetRAMWord(0, address);
            address += 2;

            if (okTagType)  // 4-word palette tag
            {
                palette = ((uint32_t)tag1) | ((uint32_t)tag2 << 16);
            }
            else  // 4-word params tag
            {
                scale = (tag2 >> 4) & 3;  // Bits 4-5 - new scale value
                pbpgpr = (uint8_t)((7 - (tag2 & 7)) << 4);  // Y-value modifier
                cursorYRGB = (uint8_t)(tag1 & 15);  // Cursor color
                okCursorType = ((tag1 & 16) != 0);  // true - graphical cursor, false - symbolic cursor
                //ASSERT(okCursorType==0);  //DEBUG
                cursorPos = (uint8_t)(((tag1 >> 8) >> scale) & 0x7f);  // Cursor position in the line
                cursorAddress = (uint8_t)((tag1 >> 5) & 7);
                scale = 1 << scale;
            }
            for (uint8_t c = 0; c < 8; c++)  // Update palettecurrent
            {
                uint8_t valueYRGB = (uint8_t) (palette >> (c << 2)) & 15;
                palettecurrent[c] = colors[pbpgpr | valueYRGB];
                //if (pbpgpr != 0) DebugLogFormat("pbpgpr %02x\r\n", pbpgpr | valueYRGB);
            }
        }

        uint16_t addressBits = g_pBoard->GetRAMWord(0, address);  // The word before the last word - is address of bits from all three memory planes
        address += 2;

        // Calculate size, type and address of the next tag
        uint16_t tagB = g_pBoard->GetRAMWord(0, address);  // Last word of the tag - is address and type of the next tag
        okTagSize = (tagB & 2) != 0;  // Bit 1 shows size of the next tag
        if (okTagSize)
        {
            address = tagB & ~7;
            okTagType = (tagB & 4) != 0;  // Bit 2 shows type of the next tag
        }
        else
            address = tagB & ~3;
        if ((tagB & 1) != 0)
            cursorOn = !cursorOn;

        // Draw bits into the bitmap, from line 20 to line 307
        if (yy < 19 /*|| yy > 306*/)
            continue;

        // Loop thru bits from addressBits, planes 0,1,2
        // For each pixel:
        //   Get bit from planes 0,1,2 and make value
        //   Map value to palette; result is 4-bit value YRGB
        //   Translate value to 24-bit RGB
        //   Put value to m_bits; repeat using scale value

        int xr = 640;
        int y = yy - 19;
        uint32_t* pBits = (static_cast<uint32_t*>(pImageBits)) + y * 640;
        int pos = 0;
        for (;;)
        {
            // Get bit from planes 0,1,2
            uint8_t src0 = g_pBoard->GetRAMByte(0, addressBits);
            uint8_t src1 = g_pBoard->GetRAMByte(1, addressBits);
            uint8_t src2 = g_pBoard->GetRAMByte(2, addressBits);
            // Loop through the bits of the byte
            int bit = 0;
            for (;;)
            {
                uint32_t valueRGB;
                if (cursorOn && (pos == cursorPos) && (!okCursorType || (okCursorType && bit == cursorAddress)))
                    valueRGB = colors[cursorYRGB];  // 4-bit to 32-bit color
                else
                {
                    // Make 3-bit value from the bits
                    uint8_t value012 = (src0 & 1) | ((src1 & 1) << 1) | ((src2 & 1) << 2);
                    valueRGB = palettecurrent[value012];  // 3-bit to 32-bit color
                }

                // Put value to m_bits; repeat using scale value
                //WAS: for (int s = 0; s < scale; s++) *pBits++ = valueRGB;
                switch (scale)
                {
                case 8:
                    *pBits++ = valueRGB;
                    *pBits++ = valueRGB;
                    *pBits++ = valueRGB;
                    *pBits++ = valueRGB;
                    /* FALLTHRU */
                case 4:
                    *pBits++ = valueRGB;
                    *pBits++ = valueRGB;
                    /* FALLTHRU */
                case 2:
                    *pBits++ = valueRGB;
                    /* FALLTHRU */
                case 1:
                    *pBits++ = valueRGB;
                    /* FALLTHRU */
                default:
                    break;
                }

                xr -= scale;

                if (bit == 7)
                    break;
                bit++;

                // Shift to the next bit
                src0 >>= 1;
                src1 >>= 1;
                src2 >>= 1;
            }
            if (xr <= 0)
                break;  // End of line
            addressBits++;  // Go to the next byte
            pos++;
        }
    }
}

void Emulator_PrepareScreenToText(void* pImageBits, const uint32_t* colors)
{
    if (pImageBits == nullptr) return;
    if (!g_okEmulatorInitialized) return;

    // Tag parsing loop
    uint16_t address = 0000270;  // Tag sequence start address
    bool okTagSize = false;  // Tag size: TRUE - 4-word, false - 2-word (first tag is always 2-word)
    bool okTagType = false;  // Type of 4-word tag: TRUE - set palette, false - set params
    int scale = 1;           // Horizontal scale: 1, 2, 4, or 8
    for (int yy = 0; yy < 307; yy++)
    {
        if (okTagSize)  // 4-word tag
        {
            //WORD tag1 = g_pBoard->GetRAMWord(0, address);
            address += 2;
            uint16_t tag2 = g_pBoard->GetRAMWord(0, address);
            address += 2;

            if (okTagType)  // 4-word palette tag
            {
                //palette = MAKELONG(tag1, tag2);
            }
            else  // 4-word params tag
            {
                scale = (tag2 >> 4) & 3;  // Bits 4-5 - new scale value
                scale = 1 << scale;
            }
        }

        uint16_t addressBits = g_pBoard->GetRAMWord(0, address);  // The word before the last word - is address of bits from all three memory planes
        address += 2;

        // Calculate size, type and address of the next tag
        uint16_t tagB = g_pBoard->GetRAMWord(0, address);  // Last word of the tag - is address and type of the next tag
        okTagSize = (tagB & 2) != 0;  // Bit 1 shows size of the next tag
        if (okTagSize)
        {
            address = tagB & ~7;
            okTagType = (tagB & 4) != 0;  // Bit 2 shows type of the next tag
        }
        else
            address = tagB & ~3;

        // Draw bits into the bitmap, from line 20 to line 307
        if (yy < 19 /*|| yy > 306*/)
            continue;

        // Loop thru bits from addressBits, planes 0,1,2
        int xr = 640;
        int y = yy - 19;
        uint32_t* pBits = (static_cast<uint32_t*>(pImageBits)) + y * 640;
        int pos = 0;
        for (;;)
        {
            // Get bit from planes 0,1,2
            uint8_t src0 = g_pBoard->GetRAMByte(0, addressBits);
            uint8_t src1 = g_pBoard->GetRAMByte(1, addressBits);
            uint8_t src2 = g_pBoard->GetRAMByte(2, addressBits);
            // Loop through the bits of the byte
            int bit = 0;
            for (;;)
            {
                // Make 3-bit value from the bits
                uint8_t value012 = (src0 & 1) | ((src1 & 1) << 1) | ((src2 & 1) << 2);
                uint32_t valueRGB = colors[value012];  // 3-bit to 32-bit color

                // Put value to m_bits; (do not repeat using scale value)
                *pBits++ = valueRGB;
                xr -= scale;

                if (bit == 7)
                    break;
                bit++;

                // Shift to the next bit
                src0 >>= 1;
                src1 >>= 1;
                src2 >>= 1;
            }
            if (xr <= 0)
                break;  // End of line
            addressBits++;  // Go to the next byte
            pos++;
        }
    }
}

void Emulator_KeyEvent(uint8_t keyscan, bool pressed)
{
    if (m_EmulatorKeyQueueCount == KEYEVENT_QUEUE_SIZE) return;  // Full queue

    uint16_t keyevent = MAKEWORD(keyscan, pressed ? 128 : 0);

    m_EmulatorKeyQueue[m_EmulatorKeyQueueTop] = keyevent;
    m_EmulatorKeyQueueTop++;
    if (m_EmulatorKeyQueueTop >= KEYEVENT_QUEUE_SIZE)
        m_EmulatorKeyQueueTop = 0;
    m_EmulatorKeyQueueCount++;
}

uint16_t Emulator_GetKeyEventFromQueue()
{
    if (m_EmulatorKeyQueueCount == 0) return 0;  // Empty queue

    uint16_t keyevent = m_EmulatorKeyQueue[m_EmulatorKeyQueueBottom];
    m_EmulatorKeyQueueBottom++;
    if (m_EmulatorKeyQueueBottom >= KEYEVENT_QUEUE_SIZE)
        m_EmulatorKeyQueueBottom = 0;
    m_EmulatorKeyQueueCount--;

    return keyevent;
}

void Emulator_ProcessKeyEvent()
{
    // Process next event in the keyboard queue
    uint16_t keyevent = Emulator_GetKeyEventFromQueue();
    if (keyevent != 0)
    {
        bool pressed = ((keyevent & 0x8000) != 0);
        uint8_t ukncscan = (uint8_t)(keyevent & 0xff);
        g_pBoard->KeyboardEvent(ukncscan, pressed);
    }
}

void CALLBACK Emulator_FeedDAC(unsigned short l, unsigned short r)
{
    //
}

void Emulator_SetSound(bool enable)
{
    m_okEmulatorSound = enable;
    if (g_pBoard != nullptr)
    {
        if (enable)
            g_pBoard->SetSoundGenCallback(Emulator_FeedDAC);
        else
            g_pBoard->SetSoundGenCallback(nullptr);
    }
}

void Emulator_SetSoundAY(bool enable)
{
    m_okEmulatorSoundAY = enable;
    if (g_pBoard != nullptr)
    {
        g_pBoard->SetSoundAY(m_okEmulatorSoundAY);
    }
}


//////////////////////////////////////////////////////////////////////

bool Emulator_AttachFloppyImage(int slot, LPCTSTR sFilePath)
{
    return g_pBoard->AttachFloppyImage(slot, sFilePath);
}

void Emulator_DetachFloppyImage(int slot)
{
    g_pBoard->DetachFloppyImage(slot);
}

bool Emulator_IsFloppyImageAttached(int slot)
{
    return g_pBoard->IsFloppyImageAttached(slot);
}

bool Emulator_IsFloppyReadOnly(int slot)
{
    return g_pBoard->IsFloppyReadOnly(slot);
}

bool Emulator_IsFloppyEngineOn()
{
    return g_pBoard->IsFloppyEngineOn();
}



//////////////////////////////////////////////////////////////////////
//
// Emulator image format - see CMotherboard::SaveToImage()
// Image header format (32 bytes):
//   4 bytes        UKNC_IMAGE_HEADER1
//   4 bytes        UKNC_IMAGE_HEADER2
//   4 bytes        UKNC_IMAGE_VERSION
//   4 bytes        UKNC_IMAGE_SIZE
//   4 bytes        UKNC uptime
//   12 bytes       Not used
//TODO: 256 bytes * 2 - Cartridge 1..2 path
//TODO: 256 bytes * 4 - Floppy 1..4 path
//TODO: 256 bytes * 2 - Hard 1..2 path

bool Emulator_SaveImage(const std::string& sFilePath)
{
    std::ofstream file(sFilePath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!file.is_open())
    {
        AlertWarning(_T("Failed to save image file."));
        return false;
    }

    // Allocate memory
    uint8_t* pImage = (uint8_t*) ::calloc(UKNCIMAGE_SIZE, 1);
    if (pImage == nullptr)
    {
        file.close();
        return false;
    }
    // Prepare header
    uint32_t* pHeader = (uint32_t*) pImage;
    *pHeader++ = UKNCIMAGE_HEADER1;
    *pHeader++ = UKNCIMAGE_HEADER2;
    *pHeader++ = UKNCIMAGE_VERSION;
    *pHeader++ = UKNCIMAGE_SIZE;
    // Store emulator state to the image
    g_pBoard->SaveToImage(pImage);
    *(uint32_t*)(pImage + 16) = m_dwEmulatorUptime;

    // Save image to the file
    file.write(reinterpret_cast<const char*>(pImage), UKNCIMAGE_SIZE);
    if (!file)
    {
        AlertWarning(_T("Failed to save image file data."));
        return false;
    }

    // Free memory, close file
    ::free(pImage);
    file.close();

    return true;
}

bool Emulator_LoadImage(const std::string& sFilePath)
{
    Emulator_Stop();

    std::ifstream file(sFilePath, std::ios::in | std::ios::binary);
    if (!file.is_open())
    {
        AlertWarning(_T("Failed to load image file."));
        return false;
    }

    // Read header
    uint32_t bufHeader[UKNCIMAGE_HEADER_SIZE / sizeof(uint32_t)];
    file.read((char*)bufHeader, UKNCIMAGE_HEADER_SIZE);
    if (!file)
    {
        file.close();
        return false;
    }

    //TODO: Check version and size

    // Allocate memory
    uint8_t* pImage = (uint8_t*) ::malloc(UKNCIMAGE_SIZE);
    if (pImage == nullptr)
    {
        file.close();
        return false;
    }

    // Read image
    file.seekg(0);
    file.read((char*)pImage, UKNCIMAGE_SIZE);
    if (!file)
    {
        ::free(pImage);
        file.close();
        AlertWarning(_T("Failed to load image file data."));
        return false;
    }
    else
    {
        // Restore emulator state from the image
        g_pBoard->Reset();
        g_pBoard->LoadFromImage(pImage);

        m_dwEmulatorUptime = *(uint32_t*)(pImage + 16);
    }

    // Free memory, close file
    ::free(pImage);
    file.close();

    return true;
}


//////////////////////////////////////////////////////////////////////
