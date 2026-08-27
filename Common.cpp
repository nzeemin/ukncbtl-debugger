/*  This file is part of UKNCBTL.
    UKNCBTL is free software: you can redistribute it and/or modify it under the terms
of the GNU Lesser General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
    UKNCBTL is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU Lesser General Public License for more details.
    You should have received a copy of the GNU Lesser General Public License along with
UKNCBTL. If not, see <http://www.gnu.org/licenses/>. */

// Common.cpp

#include "stdafx.h"
#include <cstdarg>
#include <cstdlib>
#include <type_traits>
#include "ukncbtldebug.h"


//////////////////////////////////////////////////////////////////////


bool AssertFailedLine(const char * lpszFileName, int nLine)
{
    DebugPrintFormat(_T("ASSERT in %s at line %n"), lpszFileName, nLine);

    return false;
}

void AlertInfo(LPCTSTR message)
{
    std::wcout << _T("[INFO] ") << message << std::endl;
}

void AlertWarning(LPCTSTR message)
{
    std::wcout << _T("[WARN] ") << message << std::endl;
}

bool AlertOkCancel(LPCTSTR message)
{
    std::wcout << _T("[ASK] ") << message << std::endl;
    //TODO
    return false;
}


//////////////////////////////////////////////////////////////////////
// DebugPrint and DebugLog

#if !defined(PRODUCT)

void DebugPrint(LPCTSTR message)
{
    std::wcout << message << std::endl;
}

void DebugPrintFormat(LPCTSTR format, ...)
{
    TCHAR buffer[512];

    va_list ptr;
    va_start(ptr, format);
    _vsntprintf_s(buffer, 512, 512 - 1, format, ptr);
    va_end(ptr);

    DebugPrint(buffer);
}

const char* TRACELOG_FILE_NAME = "trace.log";
const char* TRACELOG_NEWLINE = "\r\n";

FILE* Common_LogFile = nullptr;

void DebugLog(LPCTSTR message)
{
    if (Common_LogFile == nullptr)
    {
        Common_LogFile = ::fopen(TRACELOG_FILE_NAME, "a+b");
        //TODO: Check if Common_LogFile == nullptr
    }

    ::fseek(Common_LogFile, 0, SEEK_END);

    size_t dwLength = _tcslen(message) * sizeof(TCHAR);
    ::fwrite(message, 1, dwLength, Common_LogFile);
}

void DebugLogFormat(LPCTSTR pszFormat, ...)
{
    TCHAR buffer[512];

    va_list ptr;
    va_start(ptr, pszFormat);
    _vsntprintf_s(buffer, 512, 512 - 1, pszFormat, ptr);
    va_end(ptr);

    DebugLog(buffer);
}

void DebugLogCloseFile()
{
    if (Common_LogFile != nullptr)
    {
        ::fclose(Common_LogFile);
        Common_LogFile = nullptr;
    }
}

void DebugLogClear()
{
    // Close the handle first if open, since it's positioned at EOF from
    // appending and reopening in "w" mode from here wouldn't affect an
    // already-open append handle held elsewhere.
    DebugLogCloseFile();
    FILE* f = ::fopen(TRACELOG_FILE_NAME, "wb");
    if (f != nullptr)
        ::fclose(f);
}


#endif // !defined(PRODUCT)


//////////////////////////////////////////////////////////////////////


// Processor register names
const TCHAR* REGISTER_NAME[] = { _T("R0"), _T("R1"), _T("R2"), _T("R3"), _T("R4"), _T("R5"), _T("SP"), _T("PC") };


// Print octal 16-bit value to buffer
// buffer size at least 7 characters
void PrintOctalValue(TCHAR* buffer, uint16_t value)
{
    for (int p = 0; p < 6; p++)
    {
        int digit = value & 7;
        buffer[5 - p] = _T('0') + (TCHAR)digit;
        value = (value >> 3);
    }
    buffer[6] = 0;
}
// Print hex 16-bit value to buffer
// buffer size at least 5 characters
void PrintHexValue(TCHAR* buffer, uint16_t value)
{
    for (int p = 0; p < 4; p++)
    {
        int digit = value & 15;
        buffer[3 - p] = (digit < 10) ? _T('0') + (TCHAR)digit : _T('A') + (TCHAR)(digit - 10);
        value = (value >> 4);
    }
    buffer[4] = 0;
}
// Print binary 16-bit value to buffer
// buffer size at least 17 characters
void PrintBinaryValue(TCHAR* buffer, uint16_t value)
{
    for (int b = 0; b < 16; b++)
    {
        int bit = (value >> b) & 1;
        buffer[15 - b] = bit ? _T('1') : _T('0');
    }
    buffer[16] = 0;
}

// Parse 16-bit octal value from text
bool ParseOctalValue(const char* text, uint16_t* pValue)
{
    uint16_t value = 0;
    char* pChar = (char*) text;
    for (int p = 0; ; p++)
    {
        if (p > 6) return false;
        char ch = *pChar;  pChar++;
        if (ch == 0) break;
        if (ch < '0' || ch > '7') return false;
        value = (value << 3);
        int digit = ch - '0';
        value += digit;
    }
    *pValue = value;
    return true;
}

// Parse 16-bit hex value from text
bool ParseHexValue(const char* text, uint16_t* pValue)
{
    uint16_t value = 0;
    char* pChar = (char*) text;
    for (int p = 0; ; p++)
    {
        if (p > 4) return false;
        char ch = *pChar;  pChar++;
        if (ch == 0) break;
        if (ch >= '0' && ch <= '9')
        {
            value = (value << 4);
            int digit = ch - '0';
            value += digit;
        }
        else if (ch >= 'a' && ch <= 'f')
        {
            value = (value << 4);
            int digit = ch - 'a' + 10;
            value += digit;
        }
        else if (ch >= 'A' && ch <= 'F')
        {
            value = (value << 4);
            int digit = ch - 'A' + 10;
            value += digit;
        }
        else
            return false;
    }
    *pValue = value;
    return true;
}


// UKNC KOI8-R (Russian) to Unicode conversion table
const uint16_t KOI8R_CODES[] =
{
    0x2191, 0x2193, 0x2190, 0x2192, 0x25B2, 0x25BC, 0x25C4, 0x25BA, 0x2555, 0x255C, 0x255B, 0x2559, 0x2552, 0x2553, 0x2558, 0x2556,
    0x00D7, 0x00F7, 0x2265, 0x2264, 0x21B2, 0x00B1, 0x2248, 0x2320, 0x2321, 0x00B0, 0x2260, 0x2591, 0x2591, 0x2592, 0x2593, 0x2588,
    0x2562, 0x2534, 0x255F, 0x2510, 0x2551, 0x251C, 0x2514, 0x2560, 0x2554, 0x256A, 0x250C, 0x252C, 0x2569, 0x2568, 0x2524, 0x256C,
    0x253C, 0x255E, 0x256B, 0x2565, 0x2567, 0x2500, 0x255A, 0x2502, 0x2564, 0x2518, 0x2550, 0x2563, 0x255D, 0x2566, 0x2561, 0x2557,
    0x044e, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433, 0x0445, 0x0438, 0x0439, 0x043a, 0x043b, 0x043c, 0x043d, 0x043e,
    0x043f, 0x044f, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432, 0x044c, 0x044b, 0x0437, 0x0448, 0x044d, 0x0449, 0x0447, 0x044a,
    0x042e, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413, 0x0425, 0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e,
    0x041f, 0x042f, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412, 0x042c, 0x042b, 0x0417, 0x0428, 0x042d, 0x0429, 0x0427, 0x042a
};
// Translate one KOI8-R character to Unicode character
uint16_t Translate_KOI8R(uint8_t ch)
{
    if (ch < 128) return (char)ch;
    return KOI8R_CODES[ch - 128];
}


std::string WStringToNarrowString(const std::wstring& ws)
{
    std::vector<char> buf(ws.size() * MB_CUR_MAX + 1);
    std::wcstombs(buf.data(), ws.c_str(), buf.size());
    return std::string(buf.data());
}

bool WStringEqualsIgnoreCase(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (towlower(a[i]) != towlower(b[i]))
            return false;
    }
    return true;
}

// Template helper so only the branch matching TCHAR's actual type needs to
// type-check: plain "if constexpr" inside a non-template function still
// requires both branches to be well-formed even though only one runs, which
// breaks here since basic_string<char> cannot convert to basic_string<wchar_t>
// (or vice versa) as a return statement regardless of which branch executes.
template <typename T>
std::basic_string<T> NarrowStringToTStringImpl(const std::string& s)
{
    if constexpr (std::is_same_v<T, char>)
    {
        return s;
    }
    else
    {
        std::vector<T> buf(s.size() + 1);
        std::mbstowcs(reinterpret_cast<wchar_t*>(buf.data()), s.c_str(), buf.size());
        return std::basic_string<T>(buf.data());
    }
}

std::basic_string<TCHAR> NarrowStringToTString(const std::string& s)
{
    return NarrowStringToTStringImpl<TCHAR>(s);
}


//////////////////////////////////////////////////////////////////////
