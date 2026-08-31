// util/Symbols.cpp

#include "stdafx.h"
#include <algorithm>
#include <cwctype>
#include <sstream>
#include "Symbols.h"

//////////////////////////////////////////////////////////////////////

namespace {

struct Symbol
{
    uint16_t address;
    std::wstring name;
};

std::vector<Symbol> g_symbols;  // Kept sorted by address ascending

// Parse one line of a GNU ld map file. A pure symbol-definition line looks
// like (leading whitespace, then "0x" + hex address, then whitespace, then
// the symbol name and nothing else):
//
//                 0x0000040e                main
//
// Section-content lines have extra fields after the name (a size, and/or a
// source file/archive-member path) and don't match, e.g.:
//
//  .text          0x000002f0      0x4f8 spigot.o
//
// nor do "LOAD file.o" lines, blank lines, or the few one-off assignment
// lines like ". = 0x200" (no valid identifier follows the address there).
bool ParseSymbolLine(const std::wstring& line, uint16_t* address, std::wstring* name)
{
    size_t pos = line.find_first_not_of(L" \t");
    if (pos == std::wstring::npos || line.compare(pos, 2, L"0x") != 0)
        return false;

    size_t hexStart = pos + 2;
    size_t hexEnd = hexStart;
    while (hexEnd < line.size() && iswxdigit(line[hexEnd]))
        hexEnd++;
    if (hexEnd == hexStart)
        return false;

    size_t nameStart = line.find_first_not_of(L" \t", hexEnd);
    if (nameStart == std::wstring::npos)
        return false;
    size_t nameEnd = line.find_first_of(L" \t", nameStart);
    std::wstring token = (nameEnd == std::wstring::npos)
        ? line.substr(nameStart)
        : line.substr(nameStart, nameEnd - nameStart);
    if (token.empty())
        return false;

    // A pure symbol-definition line has nothing else after the name.
    if (nameEnd != std::wstring::npos && line.find_first_not_of(L" \t", nameEnd) != std::wstring::npos)
        return false;

    // Must look like a C identifier/assembler symbol, not e.g. a bare "="
    // (from ". = 0x200"-style assignment lines).
    if (!iswalpha(token[0]) && token[0] != L'_' && token[0] != L'.' && token[0] != L'$')
        return false;
    for (wchar_t ch : token)
        if (!iswalnum(ch) && ch != L'_' && ch != L'.' && ch != L'$')
            return false;

    uint32_t value = 0;
    for (size_t i = hexStart; i < hexEnd; i++)
    {
        wchar_t ch = line[i];
        value <<= 4;
        if (ch >= L'0' && ch <= L'9') value |= (uint32_t)(ch - L'0');
        else if (ch >= L'a' && ch <= L'f') value |= (uint32_t)(ch - L'a' + 10);
        else if (ch >= L'A' && ch <= L'F') value |= (uint32_t)(ch - L'A' + 10);
    }
    if (value > 0xffff)
        return false;  // Out of PDP-11 address range -- not a real code/data symbol

    *address = (uint16_t)value;
    *name = token;
    return true;
}

}  // namespace

//////////////////////////////////////////////////////////////////////

size_t Symbols_LoadFromMapFile(const std::wstring& filename)
{
    std::string narrowName = WStringToNarrowString(filename);
    std::wifstream file(narrowName);
    if (!file.is_open())
        return 0;

    std::vector<Symbol> loaded;
    std::wstring line;
    while (std::getline(file, line))
    {
        uint16_t address;
        std::wstring name;
        if (ParseSymbolLine(line, &address, &name))
            loaded.push_back(Symbol{ address, name });
    }

    if (loaded.empty())
        return 0;

    std::sort(loaded.begin(), loaded.end(),
        [](const Symbol& a, const Symbol& b) { return a.address < b.address; });

    g_symbols = std::move(loaded);
    return g_symbols.size();
}

bool Symbols_IsLoaded()
{
    return !g_symbols.empty();
}

bool Symbols_Find(uint16_t address, std::wstring* name, uint16_t* offset)
{
    if (g_symbols.empty())
        return false;

    // Largest address <= given address: upper_bound (first entry strictly
    // greater), then step back one.
    auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), address,
        [](uint16_t addr, const Symbol& s) { return addr < s.address; });
    if (it == g_symbols.begin())
        return false;  // address is below the lowest known symbol
    --it;

    *name = it->name;
    *offset = (uint16_t)(address - it->address);
    return true;
}

bool Symbols_FindByName(const std::wstring& name, uint16_t* address)
{
    for (const Symbol& s : g_symbols)
    {
        if (s.name == name)
        {
            *address = s.address;
            return true;
        }
    }
    return false;
}

std::wstring Symbols_Format(uint16_t address)
{
    std::wstring name;
    uint16_t offset;
    if (!Symbols_Find(address, &name, &offset))
        return std::wstring();
    if (offset == 0)
        return name;

    TCHAR bufOffset[7];
    PrintOctalValue(bufOffset, offset);
    std::wostringstream oss;
    oss << name << L"+" << bufOffset;
    return oss.str();
}

std::wstring Symbols_FormatSuffix(uint16_t address)
{
    std::wstring formatted = Symbols_Format(address);
    if (formatted.empty())
        return std::wstring();

    std::wostringstream oss;
    oss << L" <" << formatted << L">";
    return oss.str();
}

void Symbols_PrintAll()
{
    if (g_symbols.empty())
    {
        std::wcout << L" No symbols loaded." << std::endl;
        return;
    }
    for (const Symbol& s : g_symbols)
    {
        TCHAR bufAddr[7];
        PrintOctalValue(bufAddr, s.address);
        std::wcout << L"  " << bufAddr << L"  " << s.name << std::endl;
    }
}
