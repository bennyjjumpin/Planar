#pragma once

#include <wx/colour.h>

// Planar's small palette. Kept in one place so the UI stays consistent.
namespace theme {
inline const wxColour kBackground(0xFF, 0xFF, 0xFF);
inline const wxColour kText(0x11, 0x18, 0x27);      // near-black
inline const wxColour kMuted(0x6B, 0x72, 0x80);     // secondary text
inline const wxColour kDivider(0xE5, 0xE7, 0xEB);
inline const wxColour kAccent(0x4F, 0x46, 0xE5);    // indigo-600, logo front plane
inline const wxColour kAccentSoft(0xC7, 0xD2, 0xFE); // indigo-200, logo back plane
inline const wxColour kError(0xB9, 0x1C, 0x1C);
} // namespace theme
