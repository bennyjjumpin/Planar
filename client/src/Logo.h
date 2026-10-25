#pragma once

#include <wx/window.h>

class wxGraphicsContext;

// Draws Planar's logo (two stacked flat planes) into a size x size square
// at (x, y). Same geometry as client/assets/make_icon.py.
void DrawPlanarLogo(wxGraphicsContext& gc, double x, double y, double size);

// A small window that just shows the logo, for headers and the start page.
class LogoCanvas : public wxWindow {
public:
    LogoCanvas(wxWindow* parent, int sizeDip);
};
