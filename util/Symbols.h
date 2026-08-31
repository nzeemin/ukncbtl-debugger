// util/Symbols.h
//
// Address -> symbol name lookup, loaded from a GNU ld "-Map" file -- the
// same linker map this project's own example Makefiles already produce via
// -Wl,-Map=out.map, no toolchain/object-format change required. The flat
// RT-11 SAV image has no room for a symbol table (it's a raw memory image,
// header + bitmap, nothing else), but the *linker* already emits one as
// ordinary text in its map file, so that's where this reads from.

#pragma once

// Load symbols from a linker map file, replacing any previously loaded
// table. Returns the number of symbols loaded, or 0 on failure (file not
// found, or no symbol-looking lines in it).
size_t Symbols_LoadFromMapFile(const std::wstring& filename);

// True if any symbols are currently loaded.
bool Symbols_IsLoaded();

// Look up the symbol at or immediately below `address`. Returns true and
// fills `name`/`offset` (address - symbol's address; 0 for an exact match)
// if one exists. Returns false if no symbols are loaded, or `address` is
// below the lowest loaded symbol.
bool Symbols_Find(uint16_t address, std::wstring* name, uint16_t* offset);

// Look up a symbol by exact name (case-sensitive, matching the linker's own
// spelling). Returns true and fills `address` on success.
bool Symbols_FindByName(const std::wstring& name, uint16_t* address);

// "name" or "name+NNNNNN" (octal offset) for `address` -- a convenience
// wrapper around Symbols_Find. Empty string if no symbol covers it.
std::wstring Symbols_Format(uint16_t address);

// " <name>" / " <name+NNNNNN>", already bracketed and space-prefixed for
// appending directly after a printed address -- empty string (not " <>")
// if there's no match, so callers can append it unconditionally.
std::wstring Symbols_FormatSuffix(uint16_t address);

// Print the whole loaded symbol table, address-sorted (the "symbols" command).
void Symbols_PrintAll();
