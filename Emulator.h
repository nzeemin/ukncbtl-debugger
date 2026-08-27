// Emulator.h

#pragma once

#include "ukncbtldebug.h"
#include "emubase/Board.h"

//////////////////////////////////////////////////////////////////////

const int MAX_BREAKPOINTCOUNT = 16;

extern CMotherboard* g_pBoard;

extern bool g_okEmulatorRunning;

extern uint8_t* g_pEmulatorRam[3];  // RAM values - for change tracking
extern uint8_t* g_pEmulatorChangedRam[3];  // RAM change flags
extern uint16_t g_wEmulatorCpuR[9];      // Current CPU register values
extern uint16_t g_wEmulatorPrevCpuR[9];  // Previous CPU register values
extern uint16_t g_wEmulatorPpuR[9];      // Current PPU register values
extern uint16_t g_wEmulatorPrevPpuR[9];  // Previous PPU register values


//////////////////////////////////////////////////////////////////////

bool Emulator_Init();
void Emulator_Done();

bool Emulator_AddCPUBreakpoint(uint16_t address);
bool Emulator_AddPPUBreakpoint(uint16_t address);
bool Emulator_RemoveCPUBreakpoint(uint16_t address);
bool Emulator_RemovePPUBreakpoint(uint16_t address);
void Emulator_SetTempCPUBreakpoint(uint16_t address);
void Emulator_SetTempPPUBreakpoint(uint16_t address);
const uint16_t* Emulator_GetCPUBreakpointList();
const uint16_t* Emulator_GetPPUBreakpointList();
bool Emulator_IsBreakpoint();
bool Emulator_IsBreakpoint(bool okCpuPpu, uint16_t address);
void Emulator_RemoveAllBreakpoints(bool okCpuPpu);

void Emulator_SetSound(bool enable);
void Emulator_SetSoundAY(bool enable);
void Emulator_Start();
void Emulator_Stop();
void Emulator_Reset();
bool Emulator_SystemFrame();
float Emulator_GetUptime();  // UKNC uptime, in seconds

//void Emulator_GetScreenSize(int scrmode, int* pwid, int* phei);
void Emulator_PrepareScreenRGB32(void* pBits, const uint32_t* colors);
void Emulator_PrepareScreenToText(void* pBits, const uint32_t* colors);
const uint32_t* Emulator_GetPalette();

void Emulator_KeyEvent(uint8_t keyPressed, bool pressed);
uint16_t Emulator_GetKeyEventFromQueue();
void Emulator_ProcessKeyEvent();

// Update cached values after Run or Step
void Emulator_OnUpdate();
bool Emulator_IsRegisterChanged(int r);
uint16_t Emulator_GetChangeRamStatus(int addrtype, uint16_t address);

bool Emulator_LoadROMCartridge(int slot, std::string& sFilePath);
void Emulator_DetachCartridge(int slot);
bool Emulator_AttachFloppyImage(int slot, LPCTSTR sFilePath);
void Emulator_DetachFloppyImage(int slot);
bool Emulator_IsFloppyImageAttached(int slot);
bool Emulator_IsFloppyReadOnly(int slot);
bool Emulator_IsFloppyEngineOn();

bool Emulator_SaveImage(const std::string &sFilePath);
bool Emulator_LoadImage(const std::string &sFilePath);


//////////////////////////////////////////////////////////////////////
