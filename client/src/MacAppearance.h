#pragma once

// Planar's palette (Theme.h) is light-only and paints its own white
// backgrounds. In macOS Dark Mode the native controls would switch to light
// text and dark fills and vanish against it, so the app opts out of Dark Mode,
// matching how it looks on Windows.
void UseLightAppearance();
