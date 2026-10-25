#include "Logo.h"

#include <memory>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include "Theme.h"

namespace {

struct Pt {
    double x, y;
};

// 64x64 design grid, as in make_icon.py.
constexpr Pt kBack[] = {{32, 26}, {58, 40}, {32, 54}, {6, 40}};
constexpr Pt kFront[] = {{32, 10}, {58, 24}, {32, 38}, {6, 24}};

void FillPolygon(wxGraphicsContext& gc, const Pt (&pts)[4], double x, double y, double scale, const wxColour& colour) {
    wxGraphicsPath path = gc.CreatePath();
    path.MoveToPoint(x + pts[0].x * scale, y + pts[0].y * scale);
    for (int i = 1; i < 4; ++i)
        path.AddLineToPoint(x + pts[i].x * scale, y + pts[i].y * scale);
    path.CloseSubpath();
    gc.SetBrush(gc.CreateBrush(wxBrush(colour)));
    gc.SetPen(*wxTRANSPARENT_PEN);
    gc.FillPath(path);
}

} // namespace

void DrawPlanarLogo(wxGraphicsContext& gc, double x, double y, double size) {
    const double scale = size / 64.0;
    gc.SetAntialiasMode(wxANTIALIAS_DEFAULT);
    FillPolygon(gc, kBack, x, y, scale, theme::kAccentSoft);
    FillPolygon(gc, kFront, x, y, scale, theme::kAccent);
}

LogoCanvas::LogoCanvas(wxWindow* parent, int sizeDip)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    const wxSize size = FromDIP(wxSize(sizeDip, sizeDip));
    SetMinSize(size);
    SetSize(size);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            const wxSize s = GetClientSize();
            DrawPlanarLogo(*gc, 0, 0, std::min(s.x, s.y));
        }
    });
}
