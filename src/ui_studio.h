#pragma once

HWND minimizeButton,closeButton,statsCanvas,spinnerCanvas,labCanvas;
SIZE fixedWindowSize{1420,900};
HWND configurationHeading,clearLogButton,saveLogButton,copyLogButton,activityCanvas,brandIcon,versionTag;
HFONT headingFont,asciiFont,smallAsciiFont,labelFont,smallFont,buttonFont,percentFont,monoSmallFont;
std::wstring uiFace=L"Segoe UI",displayFace=L"Segoe UI",monoFace=L"Consolas";
HIMAGELIST mapChecks=nullptr,mapRows=nullptr;
RECT configurationCard{},mapsCard{},labCard{},consoleCard{},activityCard{},outputFrame{},searchFrame{},terminalFrame{},columnHeader{};
RECT configurationTitle{},mapsTitle{},consoleTitle{};
int hotRow=-1;
bool capabilityFits=true;
ULONGLONG startedAt=0,finishedAt=0;
size_t completedMaps=0,totalMaps=0;
// CPU threads that fit in free memory; the extractor runs below normal priority so the desktop stays responsive.
unsigned workerCount=extractor::defaultWorkers();
std::wstring currentMap;
DWORD lastExit=0;
bool hasRun=false,animateDonut=true;
float donutAngle=0.6f;
int activityStage=0;
std::filesystem::path studioProof;

bool fontInstalled(const wchar_t* face) {
    LOGFONTW query{}; query.lfCharSet=DEFAULT_CHARSET; wcscpy_s(query.lfFaceName,face);
    bool found=false; const auto dc=GetDC(nullptr);
    EnumFontFamiliesExW(dc,&query,[](const LOGFONTW*,const TEXTMETRICW*,DWORD,LPARAM found) -> int { *reinterpret_cast<bool*>(found)=true; return 0; },reinterpret_cast<LPARAM>(&found),0);
    ReleaseDC(nullptr,dc); return found;
}
std::wstring upper(std::wstring value) { if (!value.empty()) CharUpperBuffW(value.data(),DWORD(value.size())); return value; }

void studioText(HDC dc,std::wstring value,RECT r,COLORREF color,HFONT face,UINT flags=DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS) {
    const auto old=SelectObject(dc,face);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,value.c_str(),-1,&r,flags|DT_NOPREFIX);SelectObject(dc,old);
}
int textWidth(HDC dc,const std::wstring& value,HFONT face) {
    const auto old=SelectObject(dc,face); SIZE size{}; GetTextExtentPoint32W(dc,value.c_str(),int(value.size()),&size); SelectObject(dc,old); return size.cx;
}
void hline(HDC dc,int left,int right,int y,COLORREF color) {
    const auto pen=CreatePen(PS_SOLID,1,color); const auto old=SelectObject(dc,pen);
    MoveToEx(dc,left,y,nullptr); LineTo(dc,right,y); SelectObject(dc,old); DeleteObject(pen);
}

// Paint into a memory bitmap and copy it once, so animated panels never flicker.
struct Buffered {
    HDC target,dc; HBITMAP bitmap; HGDIOBJ previous; RECT bounds;
    Buffered(HDC destination,RECT area) : target(destination),dc(CreateCompatibleDC(destination)),bounds(area) {
        bitmap=CreateCompatibleBitmap(destination,std::max(1L,area.right),std::max(1L,area.bottom)); previous=SelectObject(dc,bitmap);
    }
    ~Buffered() { BitBlt(target,0,0,bounds.right,bounds.bottom,dc,0,0,SRCCOPY); SelectObject(dc,previous); DeleteObject(bitmap); DeleteDC(dc); }
};

void consoleAppend(const std::wstring& value,COLORREF color) {
    if(GetWindowTextLengthW(logView)>220000) {SendMessageW(logView,EM_SETSEL,0,50000);SendMessageW(logView,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L""));}
    CHARRANGE end{MAXLONG,MAXLONG};SendMessageW(logView,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&end));
    CHARFORMAT2W style{};style.cbSize=sizeof(style);style.dwMask=CFM_COLOR;style.crTextColor=color;
    SendMessageW(logView,EM_SETCHARFORMAT,SCF_SELECTION,reinterpret_cast<LPARAM>(&style));
    auto normalized=value;
    for(size_t at=0;(at=normalized.find(L"\r\n",at))!=std::wstring::npos;)normalized.replace(at,2,L"\r");
    std::replace(normalized.begin(),normalized.end(),L'\n',L'\r');
    SendMessageW(logView,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(normalized.c_str()));
    SendMessageW(logView,EM_SCROLLCARET,0,0);SendMessageW(logView,WM_VSCROLL,SB_BOTTOM,0);
}
// One console row: dim timestamp, a fixed-width severity tag, then the message.
void consoleLine(const std::wstring& tag,COLORREF tagColor,const std::wstring& message,COLORREF color) {
    SYSTEMTIME now{}; GetLocalTime(&now); wchar_t stamp[24]{};
    swprintf_s(stamp,L"[%02u:%02u:%02u] ",now.wHour,now.wMinute,now.wSecond);
    consoleAppend(stamp,theme::faint);
    consoleAppend(L"["+tag+std::wstring(tag.size()<5 ? 5-tag.size() : 0,L' ')+L"]  ",tagColor);
    consoleAppend(message+L"\r\n",color);
}

std::wstring elapsedText(ULONGLONG milliseconds) {
    const auto seconds=milliseconds/1000;wchar_t value[40]{};
    swprintf_s(value,L"%02llu:%02llu:%02llu",seconds/3600,(seconds/60)%60,seconds%60);return value;
}

// Section title: accent icon, "N. TITLE" in caps, and a short accent underline under the words.
void sectionTitle(HDC dc,RECT row,int number,const std::wstring& title,Icon glyph) {
    const int size=dp(18),cy=(row.top+row.bottom)/2;
    icon(dc,glyph,{row.left,cy-size/2,row.left+size,cy+size/2},theme::accentHot,1.9f);
    const auto caption=std::to_wstring(number)+L".  "+upper(title);
    const int left=row.left+size+dp(12),width=std::min(textWidth(dc,caption,headingFont),int(row.right)-left);
    studioText(dc,caption,{left,row.top,row.right,row.bottom},theme::text,headingFont);
    roundRect(dc,{left,cy+dp(11),left+width,cy+dp(13)},1,theme::accent,theme::none,1,170);
}
void pill(HDC dc,RECT r,COLORREF color,const std::wstring& label,HFONT face,bool dot) {
    roundRect(dc,r,float(r.bottom-r.top)/2,mix(color,theme::surface,0.84f),mix(color,theme::surface,0.5f));
    RECT caption=r; caption.left+=dp(10); caption.right-=dp(10);
    if (dot) { fillCircle(dc,float(caption.left)+dpf(3),float(r.top+r.bottom)/2,dpf(3.2f),color); caption.left+=dp(12); }
    studioText(dc,label,caption,mix(color,theme::text,0.35f),face,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|(dot ? 0u : DT_CENTER));
}
// Pause and resume suspend the extractor process for this session only; nothing is saved.
bool paused=false;
ULONGLONG pausedAt=0,pausedTotal=0;
ULONGLONG runElapsed() {
    if(!startedAt)return 0;
    const auto now=process?GetTickCount64():finishedAt;
    return now-startedAt-pausedTotal-(paused?now-pausedAt:0);
}
bool suspendProcess(HANDLE target,bool suspend) {
    using Call=LONG(NTAPI*)(HANDLE);
    const auto call=reinterpret_cast<Call>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),suspend?"NtSuspendProcess":"NtResumeProcess")));
    return call && call(target)>=0;
}
COLORREF phaseColor() { return paused ? theme::amber : process ? theme::accent : !hasRun ? theme::blue : lastExit ? theme::red : theme::green; }
const char* phaseKey() { return paused ? "studio_paused" : process ? "studio_running" : !hasRun ? "studio_idle" : lastExit ? "studio_stage_failed" : "studio_stage_done"; }

LRESULT CALLBACK statsProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};auto target=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(wParam);RECT r{};GetClientRect(window,&r);
        {
            Buffered buffer(target,r); const auto dc=buffer.dc;
            FillRect(dc,&r,panel); roundRect(dc,r,dpf(10),theme::inset,theme::line);
            auto progress=reinterpret_cast<Progress*>(GetWindowLongPtrW(progressBar,GWLP_USERDATA));
            const int percent=progress && progress->high>progress->low?std::clamp((progress->position-progress->low)*100/(progress->high-progress->low),0,100):0;
            std::wstring heading=upper(i18n::tr("studio_progress"));
            if(process && !paused){constexpr wchar_t frames[]=L"⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏";heading+=L"  ";heading+=frames[(GetTickCount64()/80)%10];}
            studioText(dc,heading,{dp(14),dp(10),r.right-dp(80),dp(36)},process?theme::accentHot:theme::muted,smallFont);
            studioText(dc,std::to_wstring(percent)+L"%",{r.right-dp(100),dp(6),r.right-dp(14),dp(38)},process?theme::accentHot:theme::text,percentFont,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
            const auto elapsed=runElapsed();
            const std::array<std::tuple<Icon,std::wstring,std::wstring,COLORREF>,4> facts{{
                {Icon::grid,i18n::tr("studio_processed"),std::to_wstring(completedMaps)+L" / "+std::to_wstring(hasRun?totalMaps:selectedMaps.size()),theme::text},
                {Icon::clock,i18n::tr("studio_elapsed"),startedAt?elapsedText(elapsed):L"—",theme::text},
                {Icon::pulse,i18n::tr("studio_eta"),process && completedMaps && totalMaps>completedMaps?elapsedText(elapsed*(totalMaps-completedMaps)/completedMaps):L"—",theme::text},
                {Icon::map,i18n::tr("studio_map"),currentMap.empty()?L"—":currentMap,currentMap.empty()?theme::text:theme::accentHot}}};
            const int top=dp(64),row=std::max(dp(26),(int(r.bottom)-top-dp(6))/4),twoLine=row>=dp(44);
            for(size_t i=0;i<facts.size();++i) {
                const int y=top+int(i)*row; if(i)hline(dc,dp(14),r.right-dp(14),y,theme::line);
                const auto& [glyph,label,value,color]=facts[i];
                const int lineTop=twoLine?y+(row-dp(38))/2:y,lineHeight=twoLine?dp(18):row;
                icon(dc,glyph,{dp(14),lineTop+(lineHeight-dp(14))/2,dp(28),lineTop+(lineHeight+dp(14))/2},theme::muted,1.8f);
                studioText(dc,label,{dp(36),lineTop,r.right-dp(14),lineTop+lineHeight},theme::muted,smallFont);
                if(twoLine)studioText(dc,value,{dp(36),lineTop+dp(18),r.right-dp(14),lineTop+dp(38)},color,logFont);
                else studioText(dc,value,{r.right/2,y,r.right-dp(14),y+row},color,logFont,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
            }
        }
        if(message==WM_PAINT)EndPaint(window,&paint);return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

COLORREF donutColor(float position) {
    constexpr std::array<COLORREF,8> stops{theme::accent,theme::pink,theme::violet,theme::blue,theme::cyan,theme::green,theme::amber,theme::accent};
    position-=std::floor(position); const float scaled=position*7; const int index=std::min(6,int(scaled));
    return mix(stops[size_t(index)],stops[size_t(index)+1],scaled-float(index));
}
// Classic rotating torus. Cell width and height come from the real font metrics,
// so the donut keeps its proportions in any monospace face.
void paintDonut(HDC dc,RECT area) {
    const auto face=area.bottom-area.top>dp(200)?asciiFont:smallAsciiFont;
    const auto oldFont=SelectObject(dc,face); TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics); SetBkMode(dc,TRANSPARENT);
    const int cellWidth=std::max<int>(1,metrics.tmAveCharWidth),cellHeight=std::max<int>(1,metrics.tmHeight);
    const int cols=std::clamp(int(area.right-area.left)/cellWidth,8,140),rows=std::clamp(int(area.bottom-area.top)/cellHeight,6,70);
    const float width=float(cols*cellWidth),height=float(rows*cellHeight),k1=0.64f*std::min(width,height),k2=5;
    std::vector<float> depth(size_t(cols*rows),0),light(size_t(cols*rows),0),hue(size_t(cols*rows),0);
    // Mostly face-on so the hole stays visible: a slow wobble on one axis, a full spin on the other.
    const float a=1.2f+0.42f*std::sin(donutAngle*0.7f),b=donutAngle,ca=std::cos(a),sa=std::sin(a),cb=std::cos(b),sb=std::sin(b);
    for(float theta=0;theta<6.28319f;theta+=0.055f) {
        const float ct=std::cos(theta),st=std::sin(theta),cx=2+ct,cy=st;
        for(float phi=0;phi<6.28319f;phi+=0.016f) {
            const float cp=std::cos(phi),sp=std::sin(phi);
            const float x=cx*(cb*cp+sa*sb*sp)-cy*ca*sb,y=cx*(sb*cp-sa*cb*sp)+cy*ca*cb,inverse=1/(k2+ca*cx*sp+cy*sa);
            const int col=int((width/2+k1*inverse*x)/float(cellWidth)),row=int((height/2-k1*inverse*y)/float(cellHeight));
            if(col<0 || row<0 || col>=cols || row>=rows)continue;
            const auto index=size_t(row)*size_t(cols)+size_t(col); if(inverse<=depth[index])continue;
            const float luminance=cp*ct*sb-ca*ct*sp-sa*st+cb*(ca*st-ct*sa*sp);
            depth[index]=inverse; light[index]=std::clamp((luminance+0.55f)/1.95f,0.0f,1.0f); hue[index]=phi/6.28319f;
        }
    }
    const int ox=area.left+(int(area.right-area.left)-int(width))/2,oy=area.top+(int(area.bottom-area.top)-int(height))/2;
    constexpr wchar_t shades[]=L".,-~:;=!*#$@";
    for(int row=0;row<rows;++row)for(int col=0;col<cols;++col) {
        const auto index=size_t(row)*size_t(cols)+size_t(col); if(depth[index]<=0)continue;
        const float luminance=light[index]; const wchar_t ch=shades[std::min(11,int(luminance*11.99f))];
        SetTextColor(dc,mix(theme::surface,donutColor(hue[index]*0.55f+donutAngle*0.035f),0.42f+0.58f*luminance));
        TextOutW(dc,ox+col*cellWidth,oy+row*cellHeight,&ch,1);
    }
    SelectObject(dc,oldFont);
}

// ASCII plasma strip with drifting rainbow colors, for the spare space in the configuration card.
LRESULT CALLBACK spinnerProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};const auto target=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(wParam);
        RECT r{};GetClientRect(window,&r);
        {
            Buffered buffer(target,r); const auto dc=buffer.dc;
            FillRect(dc,&r,panel); roundRect(dc,r,dpf(10),theme::inset,theme::line);
            const auto oldFont=SelectObject(dc,smallAsciiFont); TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics); SetBkMode(dc,TRANSPARENT);
            const int cellWidth=std::max<int>(1,metrics.tmAveCharWidth),cellHeight=std::max<int>(1,metrics.tmHeight);
            const int cols=std::max(4,int(r.right*42/100-dp(16))/cellWidth),rows=std::max(2,int(r.bottom-dp(10))/cellHeight);
            const int ox=dp(10),oy=(int(r.bottom)-rows*cellHeight)/2;
            const float t=donutAngle*1.6f;
            constexpr wchar_t shades[]=L" .:-=+*#%@";
            for(int row=0;row<rows;++row)for(int col=0;col<cols;++col) {
                const float x=float(col)*0.22f,y=float(row)*0.9f;
                const float value=(std::sin(x+t)+std::sin(y*0.8f-t*1.3f)+std::sin((x+y)*0.55f+t*0.7f)+std::sin(std::sqrt(x*x*0.3f+y*y+1)-t*1.1f))/4;
                const float level=std::clamp(value*0.5f+0.5f,0.0f,1.0f); if(level<0.18f)continue;
                const wchar_t ch=shades[std::min(9,int(level*9.99f))];
                SetTextColor(dc,mix(theme::inset,donutColor(float(col)/float(cols)*0.6f+value*0.15f+donutAngle*0.035f),0.25f+0.75f*level));
                TextOutW(dc,ox+col*cellWidth,oy+row*cellHeight,&ch,1);
            }            SelectObject(dc,oldFont);
            const int textLeft=ox+cols*cellWidth+dp(14);
            const std::wstring tagline=L"/ extract • parse • enjoy /";
            const float pulse=0.5f+0.5f*std::sin(donutAngle*1.6f);
            studioText(dc,tagline,{textLeft,0,r.right-dp(12),r.bottom/2+dp(2)},mix(theme::muted,donutColor(donutAngle*0.035f),0.35f+0.3f*pulse),monoSmallFont,DT_SINGLELINE|DT_BOTTOM|DT_END_ELLIPSIS);
            constexpr wchar_t frames[]=L"◐◓◑◒";
            const std::wstring idle=std::wstring(1,frames[size_t(donutAngle*2)%4])+L"  "+upper(i18n::tr(phaseKey()));
            studioText(dc,idle,{textLeft,r.bottom/2+dp(4),r.right-dp(12),r.bottom},phaseColor(),smallFont,DT_SINGLELINE|DT_TOP|DT_END_ELLIPSIS);
        }
        if(message==WM_PAINT)EndPaint(window,&paint);return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

// Spinning ASCII cube with rainbow faces; cell metrics keep it square in any monospace face.
void paintCube(HDC dc,RECT area,HFONT typeface) {
    const auto oldFont=SelectObject(dc,typeface); TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics); SetBkMode(dc,TRANSPARENT);
    const int cellWidth=std::max<int>(1,metrics.tmAveCharWidth),cellHeight=std::max<int>(1,metrics.tmHeight);
    const int cols=std::max(4,int(area.right-area.left)/cellWidth),rows=std::max(3,int(area.bottom-area.top)/cellHeight);
    std::vector<float> depth(size_t(cols*rows),-1e9f),shade(size_t(cols*rows),0); std::vector<int> faceOf(size_t(cols*rows),-1);
    const float t=donutAngle*0.9f,ax=0.55f+t*0.61f,ay=t,cx=std::cos(ax),sx=std::sin(ax),cy=std::cos(ay),sy=std::sin(ay);
    const float scale=std::min(float(cols*cellWidth),float(rows*cellHeight))*0.3f;
    for(int face=0;face<6;++face)for(float u=-1;u<=1.001f;u+=0.035f)for(float v=-1;v<=1.001f;v+=0.035f) {
        float p[3]{u,v,1},n[3]{0,0,1}; const int axis=face/2; const float sign=face%2?-1.0f:1.0f;
        const auto orient=[&](float* q){const float a=q[0],b=q[1],c=q[2]*sign;if(axis==0){q[0]=a;q[1]=b;q[2]=c;}else if(axis==1){q[0]=c;q[1]=a;q[2]=b;}else{q[0]=b;q[1]=c;q[2]=a;}};
        orient(p); orient(n);
        const auto rotate=[&](float* q){const float y=q[1]*cx-q[2]*sx,z=q[1]*sx+q[2]*cx,x=q[0]*cy+z*sy;q[2]=-q[0]*sy+z*cy;q[0]=x;q[1]=y;};
        rotate(p); rotate(n); if(n[2]>0.02f)continue;
        const float perspective=4.5f/(4.5f+p[2]);
        const int col=int(float(cols)/2+p[0]*scale*perspective/float(cellWidth)),row=int(float(rows)/2-p[1]*scale*perspective/float(cellHeight));
        if(col<0 || row<0 || col>=cols || row>=rows)continue;
        const auto index=size_t(row)*size_t(cols)+size_t(col); if(-p[2]<=depth[index])continue;
        const bool edge=std::abs(u)>0.93f || std::abs(v)>0.93f;
        const float lit=std::max(0.0f,-0.5f*n[0]+0.62f*n[1]-0.6f*n[2]);
        depth[index]=-p[2]; faceOf[index]=face; shade[index]=edge?1.0f:0.1f+0.72f*lit;
    }
    const int ox=area.left+(int(area.right-area.left)-cols*cellWidth)/2,oy=area.top+(int(area.bottom-area.top)-rows*cellHeight)/2;
    constexpr wchar_t shades[]=L".:-=+*#%@";
    for(int row=0;row<rows;++row)for(int col=0;col<cols;++col) {
        const auto index=size_t(row)*size_t(cols)+size_t(col); if(faceOf[index]<0)continue;
        const wchar_t ch=shades[std::min(8,int(shade[index]*8.99f))];
        SetTextColor(dc,mix(theme::surface,donutColor(float(faceOf[index])/6+donutAngle*0.035f),0.35f+0.65f*shade[index]));
        TextOutW(dc,ox+col*cellWidth,oy+row*cellHeight,&ch,1);
    }
    SelectObject(dc,oldFont);
}
// Falling glyph columns. Deterministic hashes of column and time, no randomness to seed.
void paintRain(HDC dc,RECT area) {
    const auto oldFont=SelectObject(dc,monoSmallFont); TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics); SetBkMode(dc,TRANSPARENT);
    const int cellWidth=std::max<int>(1,metrics.tmAveCharWidth),cellHeight=std::max<int>(1,metrics.tmHeight);
    const int cols=std::max(1,int(area.right-area.left)/cellWidth),rows=std::max(1,int(area.bottom-area.top)/cellHeight);
    const auto hash=[](unsigned value){value^=value>>16;value*=0x7feb352dU;value^=value>>15;value*=0x846ca68bU;value^=value>>16;return value;};
    constexpr wchar_t glyphs[]=L"0101<>/\\|=+*#$%&@{}[]~^:;";
    constexpr size_t glyphCount=sizeof(glyphs)/sizeof(wchar_t)-1;
    const float time=donutAngle*9;
    const int ox=area.left+(int(area.right-area.left)-cols*cellWidth)/2;
    for(int col=0;col<cols;++col) {
        const unsigned seed=hash(unsigned(col)*2654435761U+17); const float speed=0.6f+float(seed%100)/90.0f,trail=float(4+seed%9);
        const float head=std::fmod(time*speed+float(seed%997),float(rows)+trail+6);
        for(int row=0;row<rows;++row) {
            const float behind=head-float(row); if(behind<0 || behind>trail)continue;
            const float fade=1-behind/trail;
            const wchar_t ch=glyphs[hash(seed+unsigned(row)*31+unsigned(time*0.5f+float(row)))%glyphCount];
            const COLORREF tint=mix(theme::green,donutColor(float(col)/float(cols)*0.3f+donutAngle*0.035f),0.35f);
            SetTextColor(dc,behind<1?mix(tint,RGB(255,255,255),0.7f):mix(theme::surface,tint,0.15f+0.85f*fade*fade));
            TextOutW(dc,ox+col*cellWidth,area.top+row*cellHeight,&ch,1);
        }
    }
    SelectObject(dc,oldFont);
}

LRESULT CALLBACK labProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};const auto target=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(wParam);
        RECT r{};GetClientRect(window,&r);
        {
            Buffered buffer(target,r); const auto dc=buffer.dc;
            FillRect(dc,&r,panel);
            icon(dc,Icon::spark,{dp(14),dp(12),dp(32),dp(30)},theme::accentHot,1.9f);
            studioText(dc,L"ASCII LAB",{dp(42),dp(8),r.right-dp(120),dp(36)},theme::text,headingFont);
            const std::wstring mood=L"UNHINGED"; const int moodWidth=textWidth(dc,mood,smallFont)+dp(34);
            pill(dc,{r.right-dp(12)-moodWidth,dp(10),r.right-dp(12),dp(34)},donutColor(donutAngle*0.035f+0.4f),mood,smallFont,true);
            const int split=dp(44)+(int(r.bottom)-dp(44))*58/100;
            paintCube(dc,{dp(10),dp(42),r.right-dp(10),split},asciiFont);
            hline(dc,dp(14),r.right-dp(14),split+dp(4),theme::line);
            RECT rain{dp(12),split+dp(10),r.right-dp(12),r.bottom-dp(30)};
            roundRect(dc,{rain.left-dp(4),rain.top-dp(2),rain.right+dp(4),rain.bottom+dp(2)},dpf(8),theme::inset,theme::line);
            paintRain(dc,{rain.left+dp(4),rain.top+dp(4),rain.right-dp(4),rain.bottom-dp(4)});
            studioText(dc,L"no maps were harmed in this panel",{dp(10),r.bottom-dp(26),r.right-dp(10),r.bottom-dp(6)},theme::faint,monoSmallFont,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        }
        if(message==WM_PAINT)EndPaint(window,&paint);return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

LRESULT CALLBACK activityProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};const auto target=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(wParam);
        RECT r{};GetClientRect(window,&r);
        {
            Buffered buffer(target,r); const auto dc=buffer.dc;
            FillRect(dc,&r,panel);
            icon(dc,Icon::donut,{dp(14),dp(12),dp(34),dp(32)},theme::accentHot,1.8f);
            studioText(dc,L"ASCII DONUT",{dp(42),dp(8),r.right-dp(130),dp(36)},theme::text,headingFont);
            const auto phase=upper(i18n::tr(phaseKey()));
            const int phaseWidth=textWidth(dc,phase,smallFont)+dp(34);
            pill(dc,{r.right-dp(12)-phaseWidth,dp(10),r.right-dp(12),dp(34)},phaseColor(),phase,smallFont,true);
            const bool stagesFit=r.right>=dp(300);
            const int donutRight=stagesFit?int(r.right*62/100):int(r.right)-dp(10);
            const RECT donut{dp(8),dp(44),donutRight,r.bottom-dp(50)};
            paintDonut(dc,donut);
            if(stagesFit) {
                const std::array<const char*,5> stages{"studio_scanning","studio_parsing","studio_extracting","studio_writing","studio_stage_done"};
                const int rowHeight=std::clamp(int(donut.bottom-donut.top)/5,dp(22),dp(34)),top=donut.top+(int(donut.bottom-donut.top)-rowHeight*5)/2;
                const int x=donutRight+dp(18);
                const auto pen=CreatePen(PS_SOLID,1,theme::line); const auto oldPen=SelectObject(dc,pen);
                MoveToEx(dc,x,top+rowHeight/2,nullptr); LineTo(dc,x,top+rowHeight*4+rowHeight/2); SelectObject(dc,oldPen); DeleteObject(pen);
                for(int i=0;i<5;++i) {
                    const float cy=float(top+i*rowHeight+rowHeight/2);
                    const bool finished=hasRun && !process && !lastExit;
                    const bool complete=hasRun && (i<activityStage || finished);
                    const bool active=hasRun && process && i==activityStage;
                    const bool failed=hasRun && !process && lastExit && i==activityStage;
                    COLORREF label=theme::faint;
                    if(failed){fillCircle(dc,float(x),cy,dpf(8),theme::red,45);fillCircle(dc,float(x),cy,dpf(4),theme::red);label=theme::red;}
                    else if(active){fillCircle(dc,float(x),cy,dpf(9),theme::accent,40);fillCircle(dc,float(x),cy,dpf(4.5f),theme::accent);label=theme::text;}
                    else if(complete){fillCircle(dc,float(x),cy,dpf(7),theme::surface);fillCircle(dc,float(x),cy,dpf(7),theme::green,50);icon(dc,Icon::check,{x-dp(5),int(cy)-dp(5),x+dp(5),int(cy)+dp(5)},theme::green,2.6f);label=theme::body;}
                    else {fillCircle(dc,float(x),cy,dpf(4.5f),theme::surface);roundRect(dc,{x-dp(5),int(cy)-dp(5),x+dp(5),int(cy)+dp(5)},dpf(5),theme::surface,theme::faint);}
                    studioText(dc,i18n::tr(stages[size_t(i)]),{x+dp(16),int(cy)-dp(11),r.right-dp(8),int(cy)+dp(11)},label,font);
                }
            }
            studioText(dc,i18n::tr("studio_donut_caption"),{dp(10),r.bottom-dp(48),r.right-dp(10),r.bottom-dp(28)},mix(theme::pink,theme::text,0.35f),monoSmallFont,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            const std::wstring face=process?L"( •̀ᴗ•́ )":hasRun?L"( ᵔ◡ᵔ )":L"( •ᴗ• )";
            studioText(dc,face,{dp(10),r.bottom-dp(28),r.right-dp(10),r.bottom-dp(8)},theme::accentHot,font,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        if(message==WM_PAINT)EndPaint(window,&paint);return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

LRESULT CALLBACK hoverProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_MOUSEMOVE) {TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,window,0};TrackMouseEvent(&event);InvalidateRect(window,nullptr,FALSE);}
    if(message==WM_MOUSELEAVE)InvalidateRect(window,nullptr,FALSE);
    return DefSubclassProc(window,message,wParam,lParam);
}
void invalidateRow(HWND list,int row) {
    if(row<0)return; RECT bounds{}; if(!ListView_GetItemRect(list,row,&bounds,LVIR_BOUNDS))return;
    RECT client{}; GetClientRect(list,&client); bounds.left=0; bounds.right=client.right; InvalidateRect(list,&bounds,FALSE);
}
LRESULT CALLBACK mapListProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_MOUSEMOVE || message==WM_MOUSELEAVE) {
        int hit=-1;
        if(message==WM_MOUSEMOVE) {
            TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,window,0}; TrackMouseEvent(&event);
            LVHITTESTINFO info{}; info.pt={short(LOWORD(lParam)),short(HIWORD(lParam))}; hit=ListView_HitTest(window,&info);
        }
        if(hit!=hotRow){const int previous=hotRow;hotRow=hit;invalidateRow(window,previous);invalidateRow(window,hit);}
    }
    return DefSubclassProc(window,message,wParam,lParam);
}

// Map rows are painted here: checkbox, name, variant badge, size and a separator.
LRESULT drawMapRow(NMLVCUSTOMDRAW& draw) {
    auto& item=draw.nmcd; const HDC dc=item.hdc;
    if(item.dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW|CDRF_NOTIFYPOSTPAINT;
    if(item.dwDrawStage==CDDS_POSTPAINT) {
        // Cover the column separators the list theme draws below the last row.
        if(const int count=ListView_GetItemCount(mapList);count>0) {
            RECT client{},last{}; GetClientRect(mapList,&client);
            if(ListView_GetItemRect(mapList,count-1,&last,LVIR_BOUNDS) && last.bottom<client.bottom){client.top=last.bottom;FillRect(dc,&client,panel);}
        }
        if(ListView_GetItemCount(mapList)==0) {
            RECT client{}; GetClientRect(mapList,&client); FillRect(dc,&client,panel); const int cx=client.right/2,cy=client.bottom/2;
            icon(dc,Icon::map,{cx-dp(18),cy-dp(40),cx+dp(18),cy-dp(4)},theme::faint,1.5f);
            studioText(dc,i18n::tr(availableMaps.empty()?"studio_maps_empty":"studio_maps_no_match"),{dp(12),cy+dp(4),client.right-dp(12),cy+dp(28)},theme::muted,font,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        }
        return CDRF_DODEFAULT;
    }
    if(item.dwDrawStage!=CDDS_ITEMPREPAINT)return CDRF_DODEFAULT;
    const int row=int(item.dwItemSpec); RECT bounds{}; ListView_GetItemRect(mapList,row,&bounds,LVIR_BOUNDS);
    RECT client{}; GetClientRect(mapList,&client); bounds.left=client.left; bounds.right=client.right;
    const bool checked=ListView_GetCheckState(mapList,row)!=FALSE,selected=(ListView_GetItemState(mapList,row,LVIS_SELECTED)&LVIS_SELECTED)!=0,hot=row==hotRow;
    FillRect(dc,&bounds,panel);
    RECT band=bounds; band.left+=dp(2); band.right-=dp(2); band.top+=1;
    if(selected) {
        roundRect(dc,band,dpf(6),theme::accentTint,mix(theme::accent,theme::surface,0.5f));
        roundRect(dc,{band.left+dp(2),band.top+dp(6),band.left+dp(5),band.bottom-dp(6)},dpf(1.5f),theme::accent);
    } else if(hot) roundRect(dc,band,dpf(6),theme::raised);
    else hline(dc,bounds.left+dp(8),bounds.right-dp(8),bounds.bottom-1,theme::line);
    const int box=dp(16),boxLeft=bounds.left+dp(12),cy=(bounds.top+bounds.bottom)/2;
    const RECT check{boxLeft,cy-box/2,boxLeft+box,cy-box/2+box};
    if(checked){roundGradient(dc,check,dpf(4),theme::accentHot,theme::accentDeep);icon(dc,Icon::check,{check.left+dp(2),check.top+dp(2),check.right-dp(2),check.bottom-dp(2)},theme::base,3);}
    else roundRect(dc,check,dpf(4),theme::inset,hot||selected?theme::muted:theme::border);
    wchar_t value[260]{};
    const int nameRight=bounds.left+ListView_GetColumnWidth(mapList,0);
    ListView_GetItemText(mapList,row,0,value,260);
    studioText(dc,value,{check.right+dp(10),bounds.top,nameRight-dp(8),bounds.bottom},checked?theme::text:theme::muted,font);
    ListView_GetItemText(mapList,row,1,value,260);
    const bool classic=std::wstring(value)==L"Classic"; const COLORREF tint=classic?theme::violet:theme::blue;
    const int variantLeft=nameRight,badgeWidth=std::min(textWidth(dc,value,smallFont)+dp(26),ListView_GetColumnWidth(mapList,1)-dp(8));
    pill(dc,{variantLeft,cy-dp(10),variantLeft+badgeWidth,cy+dp(10)},tint,value,smallFont,false);
    ListView_GetItemText(mapList,row,2,value,260);
    const int sizeLeft=variantLeft+ListView_GetColumnWidth(mapList,1);
    const int sizeRight=sizeLeft+ListView_GetColumnWidth(mapList,2)-dp(30);
    studioText(dc,value,{sizeLeft,bounds.top,sizeRight,bounds.bottom},theme::muted,monoSmallFont,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    // Status dot: green when the map file is readable, red when its size could not be read.
    const bool readable=std::wstring(value)!=L"—"; const float dotX=float(sizeRight+dp(16));
    fillCircle(dc,dotX,float(cy),dpf(6),readable?theme::green:theme::red,40); fillCircle(dc,dotX,float(cy),dpf(3),readable?theme::green:theme::red);
    return CDRF_SKIPDEFAULT;
}

void paintBackdrop(HDC dc,RECT r) {
    Canvas canvas(dc); auto& g=canvas.g;
    const Gdiplus::RectF area{float(r.left),float(r.top),float(r.right-r.left),float(r.bottom-r.top)};
    g.SetClip(area);
    Gdiplus::LinearGradientBrush fill(Gdiplus::RectF{area.X,area.Y-1,area.Width,area.Height+2},gp(RGB(11,13,20)),gp(theme::base),Gdiplus::LinearGradientModeVertical);
    g.FillRectangle(&fill,area);
    const auto glow=[&](Gdiplus::RectF ellipse,COLORREF color,BYTE alpha) {
        Gdiplus::GraphicsPath path; path.AddEllipse(ellipse); Gdiplus::PathGradientBrush brush(&path);
        brush.SetCenterColor(gp(color,alpha)); Gdiplus::Color edge[]{gp(color,0)}; int count=1; brush.SetSurroundColors(edge,&count); g.FillPath(&brush,&path);
    };
    glow({area.X-area.Width*0.25f,area.Y-area.Height*0.55f,area.Width*0.75f,area.Height*1.1f},theme::accent,14);
    glow({area.X+area.Width*0.55f,area.Y+area.Height*0.35f,area.Width*0.7f,area.Height*1.1f},theme::violet,16);
}
void paintCard(HDC dc,RECT r) {
    const float radius=dpf(12);
    for(int i=3;i>=1;--i){RECT shadow=r;OffsetRect(&shadow,0,i);InflateRect(&shadow,i,i);roundRect(dc,shadow,radius+float(i),RGB(0,0,0),theme::none,1,BYTE(16+6*(3-i)));}
    roundRect(dc,r,radius,theme::surface,theme::border);
    Canvas canvas(dc); Gdiplus::LinearGradientBrush shine(Gdiplus::RectF{float(r.left),0,float(r.right-r.left),1},gp(RGB(80,90,110),0),gp(RGB(80,90,110),140),Gdiplus::LinearGradientModeHorizontal);
    shine.SetBlendBellShape(0.5f); Gdiplus::Pen pen(&shine,1); canvas.g.DrawLine(&pen,float(r.left)+radius,float(r.top)+0.5f,float(r.right)-radius,float(r.top)+0.5f);
}
void fieldFrame(HDC dc,RECT r,bool focused) { roundRect(dc,r,dpf(8),theme::inset,focused?theme::accent:theme::border); }

void paintStatusBar(HDC dc,RECT bar) {
    FillRect(dc,&bar,headerBrush); hline(dc,bar.left,bar.right,bar.top,theme::line);
    const int top=bar.top+dp(7),bottom=bar.bottom-dp(7); int x=bar.left+dp(16);
    const auto segment=[&](int width,Icon glyph,COLORREF tint,const std::wstring& value,bool mono) {
        RECT r{x,top,x+width,bottom}; roundRect(dc,r,dpf(6),theme::surface,theme::line);
        icon(dc,glyph,{r.left+dp(8),r.top+(r.bottom-r.top-dp(14))/2,r.left+dp(22),r.top+(r.bottom-r.top+dp(14))/2},tint,1.8f);
        studioText(dc,value,{r.left+dp(28),r.top,r.right-dp(8),r.bottom},theme::body,mono?monoSmallFont:smallFont,DT_SINGLELINE|DT_VCENTER|DT_PATH_ELLIPSIS);
        x=r.right+dp(8);
    };
    const auto phase=upper(i18n::tr(phaseKey()));
    RECT state{x,top,x+textWidth(dc,phase,smallFont)+dp(34),bottom}; pill(dc,state,phaseColor(),phase,smallFont,true); x=state.right+dp(8);
    SYSTEMTIME time{};GetLocalTime(&time);wchar_t clock[16]{};swprintf_s(clock,L"%02u:%02u:%02u",time.wHour,time.wMinute,time.wSecond);
    const auto workers=std::to_wstring(workerCount)+L" × "+i18n::tr("studio_workers");
    const int fixed=dp(104)+textWidth(dc,workers,smallFont)+dp(44)+dp(16);
    const int pathWidth=std::max(dp(120),(int(bar.right)-x-fixed-dp(16))/2-dp(8));
    segment(pathWidth,Icon::folder,theme::accentHot,text(input).empty()?i18n::tr("client_label")+L": —":text(input),true);
    segment(pathWidth,Icon::folderOpen,theme::cyan,text(output),true);
    segment(textWidth(dc,workers,smallFont)+dp(40),Icon::cpu,theme::violet,workers,false);
    x=bar.right-dp(16)-dp(104); segment(dp(104),Icon::clock,theme::green,clock,true);
}

// Paint everything that is not a child control: backdrop, cards, titles, field frames and the status bar.
void paintWindow(HWND window,HDC dc) {
    RECT bounds{}; GetClientRect(window,&bounds);
    const int headerHeight=dp(68),footerHeight=dp(36);
    RECT head{0,0,bounds.right,headerHeight}; FillRect(dc,&head,headerBrush); hline(dc,0,bounds.right,headerHeight-1,theme::line);
    {
        Canvas canvas(dc); Gdiplus::LinearGradientBrush glow(Gdiplus::RectF{0,0,float(bounds.right),1},gp(theme::accent,0),gp(theme::accent,200),Gdiplus::LinearGradientModeHorizontal);
        glow.SetBlendBellShape(0.5f,0.9f); Gdiplus::Pen pen(&glow,1.5f); canvas.g.DrawLine(&pen,0.0f,float(headerHeight)-1,float(bounds.right),float(headerHeight)-1);
    }
    paintBackdrop(dc,{0,headerHeight,bounds.right,bounds.bottom-footerHeight});
    RECT title{}; GetWindowRect(appTitle,&title); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&title),2);
    const int versionLeft=title.left+textWidth(dc,text(appTitle),titleFont)+dp(10);
    pill(dc,{versionLeft,title.top+dp(4),versionLeft+textWidth(dc,text(versionTag),smallFont)+dp(26),title.top+dp(24)},theme::accent,text(versionTag),smallFont,false);
    for(const RECT card:{configurationCard,mapsCard,labCard,consoleCard,activityCard})paintCard(dc,card);
    for(const auto& [label,glyph]:std::array<std::pair<HWND,Icon>,3>{{{clientLabel,Icon::folder},{outputLabel,Icon::folderOpen},{taskLabel,Icon::grid}}}) {
        RECT box{}; GetWindowRect(label,&box); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&box),2);
        const int cy=(box.top+box.bottom)/2; icon(dc,glyph,{box.left-dp(22),cy-dp(7),box.left-dp(8),cy+dp(7)},theme::accentHot,1.9f);
    }
    sectionTitle(dc,configurationTitle,1,text(configurationHeading),Icon::sliders);
    sectionTitle(dc,mapsTitle,2,text(mapLabel),Icon::map);
    sectionTitle(dc,consoleTitle,3,text(activityLabel),Icon::terminal);
    const auto count=text(mapCount); const int countWidth=textWidth(dc,count,smallFont)+dp(22);
    pill(dc,{mapsTitle.right-countWidth,mapsTitle.top+dp(2),mapsTitle.right,mapsTitle.bottom-dp(2)},selectedMaps.empty()?theme::muted:theme::accent,count,smallFont,false);
    fieldFrame(dc,outputFrame,GetFocus()==output); fieldFrame(dc,searchFrame,GetFocus()==mapFilter);
    icon(dc,Icon::folder,{outputFrame.left+dp(11),(outputFrame.top+outputFrame.bottom)/2-dp(8),outputFrame.left+dp(27),(outputFrame.top+outputFrame.bottom)/2+dp(8)},theme::accentHot,1.8f);
    icon(dc,Icon::search,{searchFrame.left+dp(12),searchFrame.top+(searchFrame.bottom-searchFrame.top-dp(16))/2,searchFrame.left+dp(28),searchFrame.top+(searchFrame.bottom-searchFrame.top+dp(16))/2},theme::muted,1.8f);
    if(mapList) {
        const int nameLeft=columnHeader.left+dp(38),variantLeft=columnHeader.left+ListView_GetColumnWidth(mapList,0),sizeLeft=variantLeft+ListView_GetColumnWidth(mapList,1);
        studioText(dc,upper(i18n::tr("studio_col_map")),{nameLeft,columnHeader.top,variantLeft,columnHeader.bottom},theme::faint,smallFont);
        studioText(dc,upper(i18n::tr("studio_col_variant")),{variantLeft,columnHeader.top,sizeLeft,columnHeader.bottom},theme::faint,smallFont);
        studioText(dc,upper(i18n::tr("studio_col_size")),{sizeLeft,columnHeader.top,sizeLeft+ListView_GetColumnWidth(mapList,2)-dp(30),columnHeader.bottom},theme::faint,smallFont,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
        hline(dc,columnHeader.left,columnHeader.right,columnHeader.bottom,theme::line);
    }
    roundRect(dc,terminalFrame,dpf(10),theme::inset,theme::line);
    RECT statusBox{}; GetWindowRect(status,&statusBox); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&statusBox),2);
    icon(dc,Icon::terminal,{statusBox.left-dp(24),statusBox.top+(statusBox.bottom-statusBox.top-dp(16))/2,statusBox.left-dp(8),statusBox.top+(statusBox.bottom-statusBox.top+dp(16))/2},process?theme::accentHot:theme::muted,1.7f);
    paintStatusBar(dc,{0,bounds.bottom-footerHeight,bounds.right,bounds.bottom});
}

void studioLayout(HWND window) {
    if(!activityCanvas)return;
    RECT r{};GetClientRect(window,&r);const int w=r.right,h=r.bottom,gap=dp(14),margin=dp(16),headerHeight=dp(68),footerHeight=dp(36);
    const int bottomHeight=std::clamp(h*34/100,dp(214),dp(320)),cardsBottom=h-footerHeight-dp(12),bottomTop=cardsBottom-bottomHeight;
    const int top=headerHeight+dp(14),split=(w-gap*2-margin*2)*35/100;
    const int mapsWidth=(w-gap*2-margin*2)*39/100;
    configurationCard={margin,top,margin+split,bottomTop-gap};mapsCard={margin+split+gap,top,margin+split+gap+mapsWidth,bottomTop-gap};labCard={mapsCard.right+gap,top,w-margin,bottomTop-gap};
    const int consoleWidth=(w-margin*2-gap)*71/100;
    consoleCard={margin,bottomTop,margin+consoleWidth,cardsBottom};activityCard={consoleCard.right+gap,bottomTop,w-margin,cardsBottom};
    const auto place=[](HWND child,int x,int y,int width,int height){MoveWindow(child,x,y,std::max(1,width),std::max(1,height),TRUE);};
    const int iconSize=dp(36);place(brandIcon,dp(18),(headerHeight-iconSize)/2,iconSize,iconSize);
    { const auto dc=GetDC(appTitle); place(appTitle,dp(66),dp(12),textWidth(dc,text(appTitle),titleFont)+dp(4),dp(28)); ReleaseDC(appTitle,dc); }place(subtitle,dp(67),dp(40),dp(380),dp(18));
    place(closeButton,w-dp(50),dp(18),dp(32),dp(32));place(minimizeButton,w-dp(88),dp(18),dp(32),dp(32));
    const int languageLeft=w-dp(104)-dp(168),serverLeft=languageLeft-dp(14)-dp(250);
    place(languageSelector,languageLeft,dp(24),dp(168),dp(34));place(serverLabel,serverLeft+dp(2),dp(5),dp(250),dp(17));place(serverSelector,serverLeft,dp(24),dp(250),dp(34));
    place(versionTag,0,0,1,1);

    // Configuration card: a vertical flow whose gaps and control heights shrink together on short screens.
    struct Slot { int low,high; };
    constexpr std::array<Slot,22> slots{{{8,14},{22,26},{6,12},{17,18},{3,4},{28,36},{6,10},{17,18},{3,4},{28,36},{6,10},{17,18},{3,4},{36,46},{8,12},{24,26},{0,20},{6,10},{36,44},{6,8},{28,32},{10,14}}};
    int low=0,high=0; for(const auto slot:slots){low+=dp(slot.low);high+=dp(slot.high);}
    const float room=std::clamp(float(configurationCard.bottom-configurationCard.top-low)/float(std::max(1,high-low)),0.0f,1.0f);
    std::array<int,slots.size()> at{},size{}; int cursor=configurationCard.top;
    for(size_t i=0;i<slots.size();++i){size[i]=dp(slots[i].low)+int(float(dp(slots[i].high)-dp(slots[i].low))*room);at[i]=cursor;cursor+=size[i];}
    const int x=configurationCard.left+dp(20),cw=configurationCard.right-configurationCard.left-dp(40),browse=dp(40);
    configurationTitle={x,at[1],x+cw,at[1]+size[1]};place(configurationHeading,x,at[1],cw,size[1]);
    place(clientLabel,x+dp(22),at[3],cw-dp(22),size[3]);place(input,x,at[5],cw-browse-dp(8),size[5]);place(inputButton,x+cw-browse,at[5],browse,size[5]);
    place(outputLabel,x+dp(22),at[7],cw-dp(22),size[7]);outputFrame={x,at[9],x+cw-browse-dp(8),at[9]+size[9]};
    place(output,x+dp(36),at[9]+(size[9]-dp(20))/2,cw-browse-dp(56),dp(20));place(outputButton,x+cw-browse,at[9],browse,size[9]);
    place(taskLabel,x+dp(22),at[11],cw-dp(22),size[11]);const unsigned shown[]{0,1,3,4};const int tileWidth=(cw-dp(8)*3)/4;
    for(int i=0;i<4;++i)place(taskTiles[shown[i]],x+i*(tileWidth+dp(8)),at[13],tileWidth,size[13]);
    place(combineButton,x,at[15],cw,size[15]);
    capabilityFits=size[16]>=dp(16);place(capability,x,at[16]+size[16]-dp(18),cw,dp(18));ShowWindow(capability,capabilityFits?SW_SHOW:SW_HIDE);
    place(startButton,x,at[18],cw,size[18]);
    place(openButton,x,at[20],(cw-dp(10))/2,size[20]);place(clearOutputButton,x+(cw+dp(10))/2,at[20],(cw-dp(10))/2,size[20]);
    // Spare height under the buttons goes to the spinning cube instead of stretching the controls.
    const int spareTop=at[20]+size[20]+dp(12),spare=configurationCard.bottom-dp(12)-spareTop;
    if(spinnerCanvas){place(spinnerCanvas,x,spareTop,cw,spare);ShowWindow(spinnerCanvas,spare>=dp(44)?SW_SHOWNA:SW_HIDE);}

    const int mx=mapsCard.left+dp(20),mw=mapsCard.right-mapsCard.left-dp(40);
    mapsTitle={mx,mapsCard.top+dp(14),mx+mw,mapsCard.top+dp(40)};place(mapLabel,mx,mapsTitle.top,dp(180),dp(26));place(mapCount,mx+dp(180),mapsTitle.top,dp(10),dp(10));
    ShowWindow(mapFilterLabel,SW_HIDE);searchFrame={mx,mapsCard.top+dp(52),mx+mw,mapsCard.top+dp(88)};
    place(mapFilter,mx+dp(38),searchFrame.top+(dp(36)-dp(20))/2,mw-dp(50),dp(20));
    columnHeader={mx,mapsCard.top+dp(98),mx+mw,mapsCard.top+dp(118)};
    const int listTop=columnHeader.bottom+dp(4);
    place(mapList,mx,listTop,mw,mapsCard.bottom-dp(62)-listTop);
    const int scroll=GetSystemMetrics(SM_CXVSCROLL)+2;
    ListView_SetColumnWidth(mapList,0,std::max(dp(120),mw-dp(206)-scroll));ListView_SetColumnWidth(mapList,1,dp(90));ListView_SetColumnWidth(mapList,2,dp(116));
    if(labCanvas)place(labCanvas,labCard.left+dp(6),labCard.top+dp(6),labCard.right-labCard.left-dp(12),labCard.bottom-labCard.top-dp(12));
    const HWND filters[]{selectAllButton,clearMapsButton,preferNormalButton,preferClassicButton};const int filterWidth=(mw-dp(8)*3)/4;
    for(int i=0;i<4;++i)place(filters[i],mx+i*(filterWidth+dp(8)),mapsCard.bottom-dp(50),filterWidth,dp(34));

    const int statsWidth=dp(224),statsLeft=consoleCard.right-dp(16)-statsWidth,terminalRight=statsLeft-dp(14);
    place(statsCanvas,statsLeft,consoleCard.top+dp(14),statsWidth,consoleCard.bottom-consoleCard.top-dp(28));place(progressBar,dp(14),dp(44),statsWidth-dp(28),dp(8));
    const int toolTop=consoleCard.top+dp(12),toolHeight=dp(30);int toolRight=terminalRight;
    for(const auto& [button,width]:std::array<std::pair<HWND,int>,3>{{{clearLogButton,dp(84)},{saveLogButton,dp(104)},{copyLogButton,dp(84)}}}){toolRight-=width;place(button,toolRight,toolTop,width,toolHeight);toolRight-=dp(8);}
    consoleTitle={consoleCard.left+dp(20),consoleCard.top+dp(14),toolRight,consoleCard.top+dp(40)};place(activityLabel,consoleTitle.left,consoleTitle.top,dp(10),dp(10));
    terminalFrame={consoleCard.left+dp(16),consoleCard.top+dp(52),terminalRight,consoleCard.bottom-dp(44)};
    place(logView,terminalFrame.left+dp(12),terminalFrame.top+dp(8),terminalFrame.right-terminalFrame.left-dp(16),terminalFrame.bottom-terminalFrame.top-dp(14));
    place(status,consoleCard.left+dp(44),consoleCard.bottom-dp(36),terminalRight-consoleCard.left-dp(44),dp(24));
    place(activityCanvas,activityCard.left+dp(6),activityCard.top+dp(6),activityCard.right-activityCard.left-dp(12),activityCard.bottom-activityCard.top-dp(12));
    InvalidateRect(window,nullptr,TRUE);
}

// Every owner-drawn button: window buttons, task tiles, the toggle, Start and the secondary buttons.
void drawControl(const DRAWITEMSTRUCT& item) {
    const HDC dc=item.hDC; const UINT id=item.CtlID; RECT r=item.rcItem;
    const bool disabled=(item.itemState&ODS_DISABLED)!=0,pressed=(item.itemState&ODS_SELECTED)!=0;
    POINT mouse{};GetCursorPos(&mouse);ScreenToClient(item.hwndItem,&mouse);const bool hovered=!disabled && PtInRect(&r,mouse)!=FALSE;
    const auto caption=text(item.hwndItem);
    const auto content=[&](Icon glyph,bool hasIcon,COLORREF tint,COLORREF color,HFONT face,RECT area) {
        const int glyphSize=dp(16),space=dp(8);
        const bool bare=caption.empty() || caption==L"…";
        if(hasIcon && !bare && textWidth(dc,caption,face)+glyphSize+space+dp(16)>int(area.right-area.left))hasIcon=false;
        const int width=(hasIcon?glyphSize:0)+(bare?0:(hasIcon?space:0)+std::min(textWidth(dc,caption,face),int(area.right-area.left)-glyphSize-space-dp(12)));
        int x=(area.left+area.right-width)/2; const int cy=(area.top+area.bottom)/2;
        if(hasIcon){icon(dc,glyph,{x,cy-glyphSize/2,x+glyphSize,cy+glyphSize/2},tint,1.8f);x+=glyphSize+space;}
        if(!bare)studioText(dc,caption,{x,area.top,area.right-dp(6),area.bottom},color,face);
    };
    if(id==133 || id==134) {
        FillRect(dc,&r,headerBrush);
        if(hovered)roundRect(dc,r,dpf(8),id==134?RGB(140,38,38):theme::raised);
        const int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;
        icon(dc,id==133?Icon::minimize:Icon::close,{cx-dp(8),cy-dp(8),cx+dp(8),cy+dp(8)},hovered?theme::text:theme::accentHot,2);
        keyboardFocus(item);return;
    }
    FillRect(dc,&r,panel);
    if(id>=120 && id<=124) {
        const unsigned index=id-120; const bool selected=taskMode==index;
        if(selected && !disabled) {
            roundGradient(dc,r,dpf(8),pressed?theme::accentDeep:hovered?mix(theme::accentHot,RGB(255,255,255),0.1f):theme::accentHot,theme::accentDeep,mix(theme::accentHot,RGB(255,255,255),0.2f));
            roundRect(dc,{r.left+1,r.top+1,r.right-1,r.bottom-1},dpf(7),theme::none,RGB(255,255,255),1,40);
        } else if(selected) roundRect(dc,r,dpf(8),theme::accentTint,mix(theme::accent,theme::surface,0.5f));
        else roundRect(dc,r,dpf(8),disabled?theme::surface:pressed?theme::pressed:hovered?theme::hover:theme::raised,hovered?mix(theme::border,theme::muted,0.4f):theme::border);
        const Icon glyph=index==0?Icon::door:index==1?Icon::water:index==3?Icon::layers:Icon::nodes;
        const COLORREF ink=selected && !disabled?theme::base:disabled?theme::faint:theme::text;
        content(glyph,true,selected && !disabled?theme::base:disabled?theme::faint:theme::body,ink,selected?buttonFont:font,r);
        keyboardFocus(item);return;
    }
    if(id==117) {
        const bool on=taskMode==3?pathnodesEnabled:combineResults;const int cy=(r.top+r.bottom)/2;
        const RECT track{r.left,cy-dp(10),r.left+dp(38),cy+dp(10)};
        if(on && !disabled)roundGradient(dc,track,dpf(10),theme::accentHot,theme::accentDeep,theme::none,true);
        else roundRect(dc,track,dpf(10),theme::inset,hovered?theme::muted:theme::border);
        const float knob=on?float(track.right)-dpf(10):float(track.left)+dpf(10);
        fillCircle(dc,knob,float(cy)+1,dpf(7.5f),RGB(0,0,0),70);fillCircle(dc,knob,float(cy),dpf(7),on && !disabled?RGB(255,255,255):theme::muted);
        studioText(dc,caption,{track.right+dp(12),r.top,r.right,r.bottom},disabled?theme::faint:theme::body,font);
        keyboardFocus(item);return;
    }
    if(id==103) {
        const bool running=process && !paused;
        if(disabled)roundRect(dc,r,dpf(10),theme::raised,theme::line);
        else if(running) {
            roundRect(dc,r,dpf(10),pressed?theme::pressed:hovered?theme::hover:theme::raised,theme::accent);
            innerGlow(dc,r,dpf(10),theme::accent,dp(3));
        } else {
            roundGradient(dc,r,dpf(10),pressed?theme::accentDeep:hovered?mix(theme::accentHot,RGB(255,255,255),0.12f):theme::accentHot,pressed?mix(theme::accentDeep,RGB(0,0,0),0.15f):theme::accentDeep,mix(theme::accentHot,RGB(255,255,255),0.25f),true);
            roundRect(dc,{r.left+1,r.top+1,r.right-1,r.bottom-1},dpf(9),theme::none,RGB(255,255,255),1,45);
        }
        const COLORREF ink=disabled?theme::faint:running?theme::text:theme::base;
        const std::wstring shortcut=L"Ctrl + Enter"; const int chipWidth=textWidth(dc,shortcut,smallFont)+dp(18);
        RECT area=r; const bool chip=!process && r.right-r.left>dp(300);
        if(chip) {
            const int cy=(r.top+r.bottom)/2; RECT box{r.right-dp(12)-chipWidth,cy-dp(11),r.right-dp(12),cy+dp(11)};
            roundRect(dc,box,dpf(6),disabled?theme::surface:RGB(0,0,0),disabled?theme::line:theme::none,1,disabled?255:BYTE(40));
            studioText(dc,shortcut,box,disabled?theme::faint:theme::base,smallFont,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
        }
        const int glyphSize=dp(14),cy=(r.top+r.bottom)/2,left=r.left+dp(22);
        icon(dc,running?Icon::pause:Icon::play,{left,cy-glyphSize/2,left+glyphSize,cy+glyphSize/2},running?theme::accentHot:ink);
        area.left=left+glyphSize+dp(12); if(chip)area.right-=chipWidth+dp(20);
        studioText(dc,caption,area,ink,buttonFont);
        keyboardFocus(item);return;
    }
    const bool ghost=id==130 || id==131 || id==136;
    const COLORREF fill=disabled?theme::surface:pressed?theme::pressed:hovered?theme::hover:ghost?theme::surface:theme::raised;
    roundRect(dc,r,dpf(8),fill,disabled?theme::line:hovered?mix(theme::border,theme::muted,0.45f):ghost?theme::line:theme::border);
    Icon glyph=Icon::folder;bool hasIcon=true;
    switch(id){case 101:case 102:glyph=Icon::folder;break;case 104:glyph=Icon::folderOpen;break;case 125:case 130:glyph=Icon::trash;break;case 131:glyph=Icon::save;break;
        case 136:glyph=Icon::copy;break;case 118:glyph=Icon::checkAll;break;case 119:glyph=Icon::close;break;case 128:glyph=Icon::map;break;case 129:glyph=Icon::spark;break;default:hasIcon=false;}
    if(id==101 || id==102) {
        const int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;const COLORREF dots=disabled?theme::faint:hovered?theme::accentHot:theme::body;
        for(int i=-1;i<=1;++i)fillCircle(dc,float(cx+i*dp(6)),float(cy),dpf(1.7f),dots);
    } else content(glyph,hasIcon,disabled?theme::faint:hovered?theme::accentHot:theme::muted,disabled?theme::faint:theme::text,font,r);
    keyboardFocus(item);
}

void installMapChecks() {
    // The state images only give the list its checkbox hit area; drawMapRow paints the visible box.
    const int box=dp(18);
    mapChecks=ImageList_Create(box,box,ILC_COLOR32|ILC_MASK,2,0);
    const auto screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);const auto bitmap=CreateCompatibleBitmap(screen,box*2,box);const auto old=SelectObject(dc,bitmap);
    RECT bounds{0,0,box*2,box};FillRect(dc,&bounds,panel);
    SelectObject(dc,old);ImageList_Add(mapChecks,bitmap,nullptr);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);ListView_SetImageList(mapList,mapChecks,LVSIL_STATE);
    mapRows=ImageList_Create(1,dp(30),ILC_COLOR32,1,0);ListView_SetImageList(mapList,mapRows,LVSIL_SMALL);
}

void saveConsole(HWND owner) {
    std::vector<wchar_t> path(32768);wcscpy_s(path.data(),path.size(),L"MapExtractor.log");OPENFILENAMEW dialog{sizeof(dialog)};dialog.hwndOwner=owner;dialog.lpstrFile=path.data();dialog.nMaxFile=32768;
    dialog.lpstrFilter=L"Log (*.log)\0*.log\0Text (*.txt)\0*.txt\0";dialog.lpstrDefExt=L"log";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
    if(!GetSaveFileNameW(&dialog))return;
    const auto contents=text(logView);const int length=WideCharToMultiByte(CP_UTF8,0,contents.data(),int(contents.size()),nullptr,0,nullptr,nullptr);
    std::string encoded(size_t(length),'\0');WideCharToMultiByte(CP_UTF8,0,contents.data(),int(contents.size()),encoded.data(),length,nullptr,nullptr);
    std::ofstream file(std::filesystem::path(path.data()),std::ios::binary);file<<encoded;
    if(!file)MessageBoxW(owner,i18n::tr("folder_error").c_str(),L"MapExtractor",MB_OK|MB_ICONERROR);
}
void copyConsole(HWND owner) {
    auto contents=text(logView);
    for(size_t at=0;(at=contents.find(L'\r',at))!=std::wstring::npos;at+=2)if(at+1>=contents.size() || contents[at+1]!=L'\n')contents.insert(at+1,1,L'\n');
    if(!OpenClipboard(owner))return;
    EmptyClipboard();
    if(const auto memory=GlobalAlloc(GMEM_MOVEABLE,(contents.size()+1)*sizeof(wchar_t))) {
        if(const auto target=static_cast<wchar_t*>(GlobalLock(memory))){wcscpy_s(target,contents.size()+1,contents.c_str());GlobalUnlock(memory);}
        if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);
    }
    CloseClipboard();
}

void verifyStudioWindow(HWND window) {
    const auto need=[](bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);};
    wchar_t className[80]{};GetClassNameW(logView,className,80);need(std::wstring(className)==L"RICHEDIT50W","Console is not RichEdit");
    SetWindowPos(window,nullptr,-32000,-32000,1420,940,SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(window,SW_SHOWNOACTIVATE);
    need((GetWindowLongPtrW(window,GWL_STYLE)&(WS_CAPTION|WS_THICKFRAME|WS_MAXIMIZEBOX))==0,"Native resizable frame remains");
    RECT before{};GetWindowRect(window,&before);
    for(int i=0;i<32;++i){SendMessageW(window,WM_SYSCOMMAND,SC_MAXIMIZE,0);SendMessageW(window,WM_NCLBUTTONDBLCLK,HTCAPTION,0);}
    RECT after{};GetWindowRect(window,&after);need(EqualRect(&before,&after) && !IsZoomed(window),"Maximize command changed fixed window");
    const auto originalSelection=selectedMaps;
    if(ListView_GetItemCount(mapList)>0) {
        ListView_SetCheckState(mapList,0,FALSE);need(selectedMaps.size()+1==originalSelection.size(),"Map checkbox did not update selection");
        ListView_SetCheckState(mapList,0,TRUE);need(selectedMaps==originalSelection,"Map checkbox restoration failed");
    }
    consoleAppend(L"Studio self-check\r\nSecond line\r\n",theme::accent);need(text(logView).find(L"Studio self-check")!=std::wstring::npos,"Console append failed");need(SendMessageW(logView,EM_GETLINECOUNT,0,0)>=3,"Console lost paragraph breaks");
    for(int i=0;i<50;++i)consoleAppend(L"Console scrolling check\r",theme::muted);
    need(SendMessageW(logView,EM_GETFIRSTVISIBLELINE,0,0)>0,"Console failed to follow appended lines");
    SendMessageW(window,WM_COMMAND,130,0);need(text(logView).empty(),"Console clear failed");
    consoleLine(L"INFO",theme::blue,i18n::tr("studio_console_ready"),theme::body);
    consoleLine(L"SCAN",theme::accent,L"MAP 22_22.unr | demo",theme::body);
    consoleLine(L"OK",theme::green,L"RESULT doors 128 0 0 0 0",RGB(167,243,208));
    need(text(logView).find(L"[OK   ]  RESULT")!=std::wstring::npos,"Console tag alignment changed");
    for(const auto& [name,size]:std::array<std::pair<const wchar_t*,SIZE>,4>{{{L"default",{1420,940}},{L"narrow",{1100,800}},{L"wide",{1660,1000}},{L"short",{1366,704}}}}) {
        SetWindowPos(window,nullptr,-32000,-32000,size.cx,size.cy,SWP_NOZORDER|SWP_NOACTIVATE);studioLayout(window);UpdateWindow(window);
        RECT client{};GetClientRect(window,&client);
        for(const auto child:{input,output,startButton,openButton,clearOutputButton,mapList,logView,activityCanvas,serverSelector,languageSelector,clearLogButton,saveLogButton,copyLogButton,statsCanvas}) {
            RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
            need(bounds.left>=0 && bounds.top>=0 && bounds.right<=client.right && bounds.bottom<=client.bottom,"Control escaped resized window");
        }
        RECT start{},open{};GetWindowRect(startButton,&start);GetWindowRect(openButton,&open);
        need(open.top>=start.bottom,"Configuration controls overlap");
        previewPath=studioProof/(std::wstring(name)+L".bmp");renderPreview(window);
    }
    SetWindowPos(window,nullptr,-32000,-32000,1420,940,SWP_NOZORDER|SWP_NOACTIVATE);studioLayout(window);
    for(int i=0;i<16;++i){donutAngle=0.6f+float(i)*0.12f;previewPath=studioProof/(L"spin-"+std::to_wstring(i)+L".bmp");renderPreview(window);}
    LOGFONTW face{};GetObjectW(headingFont,sizeof(face),&face);need(std::wstring(face.lfFaceName)==displayFace,"Heading font changed unexpectedly");
    previewPath.clear();verificationResult=0;DestroyWindow(window);
}
