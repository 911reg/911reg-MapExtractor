#pragma once

// Palette, DPI scale, antialiased shapes and vector icons for the window.
// Included inside ui.cpp's anonymous namespace. GDI+ is included at the top of ui.cpp.

namespace theme {
constexpr COLORREF base=RGB(8,10,15),header=RGB(10,12,18),surface=RGB(14,17,24),raised=RGB(22,26,35),
    hover=RGB(31,36,48),pressed=RGB(11,13,19),inset=RGB(10,12,18),border=RGB(37,43,56),line=RGB(26,30,40),
    accent=RGB(255,122,26),accentHot=RGB(255,150,70),accentDeep=RGB(238,90,14),accentTint=RGB(46,30,20),
    text=RGB(238,241,246),body=RGB(206,211,221),muted=RGB(140,148,163),faint=RGB(88,96,112),
    pink=RGB(236,72,153),violet=RGB(168,85,247),cyan=RGB(34,211,238),green=RGB(52,211,120),
    amber=RGB(251,191,36),red=RGB(248,113,113),blue=RGB(96,165,250);
constexpr COLORREF none=CLR_INVALID;
}

float uiScale=1;
int dp(int value) { return int(std::lround(float(value)*uiScale)); }
float dpf(float value) { return value*uiScale; }

COLORREF mix(COLORREF from,COLORREF to,float amount) {
    const auto channel=[&](int a,int b) { return int(std::lround(float(a)+(float(b)-float(a))*std::clamp(amount,0.0f,1.0f))); };
    return RGB(channel(GetRValue(from),GetRValue(to)),channel(GetGValue(from),GetGValue(to)),channel(GetBValue(from),GetBValue(to)));
}
Gdiplus::Color gp(COLORREF color,BYTE alpha=255) { return Gdiplus::Color(alpha,GetRValue(color),GetGValue(color),GetBValue(color)); }

struct Canvas {
    Gdiplus::Graphics g;
    explicit Canvas(HDC dc) : g(dc) { g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); }
};

void roundPath(Gdiplus::GraphicsPath& path,Gdiplus::RectF r,float radius) {
    radius=std::min({radius,r.Width/2,r.Height/2});
    if (radius<=0.5f) { path.AddRectangle(r); return; }
    const float d=radius*2;
    path.AddArc(r.X,r.Y,d,d,180,90); path.AddArc(r.GetRight()-d,r.Y,d,d,270,90);
    path.AddArc(r.GetRight()-d,r.GetBottom()-d,d,d,0,90); path.AddArc(r.X,r.GetBottom()-d,d,d,90,90); path.CloseFigure();
}
Gdiplus::RectF pixelRect(RECT r) { return {float(r.left)+0.5f,float(r.top)+0.5f,float(r.right-r.left)-1,float(r.bottom-r.top)-1}; }

// Rounded rectangle with an optional fill and an optional 1px outline. Radius is in device pixels.
void roundRect(HDC dc,RECT r,float radius,COLORREF fill,COLORREF stroke=theme::none,float width=1,BYTE alpha=255) {
    if (r.right<=r.left || r.bottom<=r.top) return;
    Canvas canvas(dc); Gdiplus::GraphicsPath path; roundPath(path,pixelRect(r),radius);
    if (fill!=theme::none) { Gdiplus::SolidBrush brush(gp(fill,alpha)); canvas.g.FillPath(&brush,&path); }
    if (stroke!=theme::none) { Gdiplus::Pen pen(gp(stroke,alpha),width); canvas.g.DrawPath(&pen,&path); }
}
void roundGradient(HDC dc,RECT r,float radius,COLORREF top,COLORREF bottom,COLORREF stroke=theme::none,bool horizontal=false) {
    if (r.right<=r.left || r.bottom<=r.top) return;
    Canvas canvas(dc); Gdiplus::GraphicsPath path; const auto bounds=pixelRect(r); roundPath(path,bounds,radius);
    Gdiplus::RectF area{bounds.X-1,bounds.Y-1,bounds.Width+2,bounds.Height+2};
    Gdiplus::LinearGradientBrush brush(area,gp(top),gp(bottom),horizontal ? Gdiplus::LinearGradientModeHorizontal : Gdiplus::LinearGradientModeVertical);
    canvas.g.FillPath(&brush,&path);
    if (stroke!=theme::none) { Gdiplus::Pen pen(gp(stroke),1); canvas.g.DrawPath(&pen,&path); }
}
// Soft glow drawn inward from the edge, so it stays inside the control that paints it.
void innerGlow(HDC dc,RECT r,float radius,COLORREF color,int spread) {
    for (int i=spread;i>=1;--i) {
        RECT ring=r; InflateRect(&ring,-i,-i);
        roundRect(dc,ring,std::max(1.0f,radius-float(i)),theme::none,color,1,BYTE(70/(i+1)));
    }
}
void fillCircle(HDC dc,float x,float y,float radius,COLORREF color,BYTE alpha=255) {
    Canvas canvas(dc); Gdiplus::SolidBrush brush(gp(color,alpha)); canvas.g.FillEllipse(&brush,x-radius,y-radius,radius*2,radius*2);
}

enum class Icon { globe,door,water,layers,nodes,folder,folderOpen,trash,save,copy,search,check,checkAll,close,minimize,cpu,clock,play,pause,map,spark,chevron,terminal,sliders,donut,pulse,grid };

// Icons are drawn on a 24-unit grid and scaled to the box, with round caps and joins.
void icon(HDC dc,Icon kind,RECT box,COLORREF color,float stroke=1.7f) {
    Canvas canvas(dc);
    const float size=float(std::min(box.right-box.left,box.bottom-box.top)),scale=size/24;
    canvas.g.TranslateTransform(float(box.left+box.right)/2-12*scale,float(box.top+box.bottom)/2-12*scale);
    canvas.g.ScaleTransform(scale,scale);
    Gdiplus::Pen pen(gp(color),stroke); pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(gp(color));
    auto& g=canvas.g;
    const auto lines=[&](std::initializer_list<Gdiplus::PointF> points,bool closed=false) {
        std::vector<Gdiplus::PointF> p(points); if (closed) g.DrawPolygon(&pen,p.data(),int(p.size())); else g.DrawLines(&pen,p.data(),int(p.size()));
    };
    const auto box24=[&](float l,float t,float r,float b,float radius) { Gdiplus::GraphicsPath path; roundPath(path,{l,t,r-l,b-t},radius); g.DrawPath(&pen,&path); };
    const auto circle=[&](float x,float y,float radius) { g.DrawEllipse(&pen,x-radius,y-radius,radius*2,radius*2); };
    switch (kind) {
    case Icon::globe: circle(12,12,9); g.DrawEllipse(&pen,8.0f,3.0f,8.0f,18.0f); lines({{3,12},{21,12}}); break;
    case Icon::door: box24(6,3,18,21,1.5f); lines({{3,21},{21,21}}); g.FillEllipse(&brush,13.6f,10.6f,2.8f,2.8f); break;
    case Icon::water:
        for (const float y:{7.0f,12.0f,17.0f}) { g.DrawBezier(&pen,3.0f,y,6.0f,y-3,9.0f,y-3,12.0f,y); g.DrawBezier(&pen,12.0f,y,15.0f,y+3,18.0f,y+3,21.0f,y); }
        break;
    case Icon::layers: lines({{12,3},{21,8},{12,13},{3,8}},true); lines({{3,12.5f},{12,17.5f},{21,12.5f}}); lines({{3,17},{12,22},{21,17}}); break;
    case Icon::nodes: circle(6,6,2.6f); circle(18,6,2.6f); circle(12,18.5f,2.6f); lines({{8.6f,6},{15.4f,6}}); lines({{7.3f,8.3f},{10.7f,16.2f}}); lines({{16.7f,8.3f},{13.3f,16.2f}}); break;
    case Icon::folder: lines({{3,6.5f},{3,19},{21,19},{21,9},{12,9},{10,6.5f}},true); break;
    case Icon::folderOpen: lines({{3,19},{3,6.5f},{10,6.5f},{12,9},{19,9},{19,11}}); lines({{3,19},{6,11},{22,11},{19,19}},true); break;
    case Icon::trash: lines({{4,7},{20,7}}); lines({{9,7},{9,4},{15,4},{15,7}}); lines({{6,7},{7,20},{17,20},{18,7}}); lines({{10,11},{10,16}}); lines({{14,11},{14,16}}); break;
    case Icon::save: box24(4,4,20,20,2); lines({{8,4},{8,9},{16,9},{16,4}}); lines({{8,20},{8,14},{16,14},{16,20}}); break;
    case Icon::copy: box24(8,8,20,20,2); lines({{16,8},{16,4},{4,4},{4,16},{8,16}}); break;
    case Icon::search: circle(10.5f,10.5f,6.5f); lines({{15.5f,15.5f},{20.5f,20.5f}}); break;
    case Icon::check: lines({{5,12.5f},{10,17},{19,7}}); break;
    case Icon::checkAll: lines({{2.5f,12.5f},{7,17},{15,7}}); lines({{11,16},{12,17},{21.5f,7}}); break;
    case Icon::close: lines({{6,6},{18,18}}); lines({{18,6},{6,18}}); break;
    case Icon::minimize: lines({{6,12},{18,12}}); break;
    case Icon::cpu:
        box24(7,7,17,17,1.5f); box24(10,10,14,14,0.5f);
        for (const float v:{10.0f,14.0f}) { lines({{v,3},{v,7}}); lines({{v,17},{v,21}}); lines({{3,v},{7,v}}); lines({{17,v},{21,v}}); }
        break;
    case Icon::clock: circle(12,12,9); lines({{12,7},{12,12},{15.5f,14}}); break;
    case Icon::play: { Gdiplus::PointF p[]{{8,5},{19,12},{8,19}}; g.FillPolygon(&brush,p,3); break; }
    case Icon::pause: g.FillRectangle(&brush,6.5f,5.0f,4.0f,14.0f); g.FillRectangle(&brush,13.5f,5.0f,4.0f,14.0f); break;
    case Icon::map: lines({{3,6},{9,4},{15,6},{21,4},{21,18},{15,20},{9,18},{3,20}},true); lines({{9,4},{9,18}}); lines({{15,6},{15,20}}); break;
    case Icon::spark: lines({{12,3},{14,10},{21,12},{14,14},{12,21},{10,14},{3,12},{10,10}},true); break;
    case Icon::chevron: lines({{7,10},{12,15},{17,10}}); break;
    case Icon::terminal: box24(3,5,21,19,2.5f); lines({{7,10},{10,12.5f},{7,15}}); lines({{12.5f,15},{16.5f,15}}); break;
    case Icon::sliders:
        lines({{4,7},{7,7}}); lines({{11,7},{20,7}}); circle(9,7,2); lines({{4,12},{13,12}}); lines({{17,12},{20,12}}); circle(15,12,2);
        lines({{4,17},{6,17}}); lines({{10,17},{20,17}}); circle(8,17,2); break;
    case Icon::donut: circle(12,12,8.5f); circle(12,12,3); break;
    case Icon::pulse: lines({{3,12},{8,12},{10,7},{14,17},{16,12},{21,12}}); break;
    case Icon::grid: box24(4,4,11,11,1.5f); box24(13,4,20,11,1.5f); box24(4,13,11,20,1.5f); box24(13,13,20,20,1.5f); break;
    }
}
