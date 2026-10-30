#pragma once

struct DetailsPanel {
    HWND window=nullptr,search=nullptr,list=nullptr,body=nullptr,copy=nullptr,copyAll=nullptr,close=nullptr,x=nullptr,empty=nullptr;
    size_t category=0,revision=SIZE_MAX;
    std::vector<size_t> rows;
    RECT searchFrame{},listFrame{},bodyFrame{};
    std::string selectedKey;
    ULONGLONG copiedAt=0;
};
DetailsPanel detailsPanel;
int counterHot=-1,counterFocus=-1,counterPressed=-1;
std::array<RECT,3> counterBoxes{};

COLORREF detailsColor() { return std::get<2>(counterFacts()[detailsPanel.category]); }
size_t detailsTotal() { return std::get<1>(counterFacts()[detailsPanel.category]); }
std::wstring diagnosticSubject(const RunDiagnostic& row) { return row.subject.empty()?i18n::tr("details_extraction"):wide(row.subject); }
std::wstring diagnosticReason(const RunDiagnostic& row) {
    return row.reason.empty()?i18n::tr(detailsPanel.category==0?"details_no_error_reason":detailsPanel.category==1?"details_no_warning_reason":"details_no_skip_reason"):wide(row.reason);
}
std::wstring searchFold(std::wstring value) { if(!value.empty())CharLowerBuffW(value.data(),DWORD(value.size()));return value; }
const RunDiagnostic* selectedDiagnostic() {
    const auto selected=SendMessageW(detailsPanel.list,LB_GETCURSEL,0,0);
    if(selected==LB_ERR || size_t(selected)>=detailsPanel.rows.size())return nullptr;
    const auto index=detailsPanel.rows[size_t(selected)];
    const auto& rows=runCounters.details[detailsPanel.category];
    return index<rows.size()?&rows[index]:nullptr;
}
void updateDetailsSelection() {
    const auto row=selectedDiagnostic();
    const auto reason=row?diagnosticReason(*row):i18n::tr("details_select");
    if(text(detailsPanel.body)!=reason)SetWindowTextW(detailsPanel.body,reason.c_str());
    detailsPanel.selectedKey=row?row->key:std::string{};
    EnableWindow(detailsPanel.copy,row!=nullptr);
}
void layoutDetails() {
    if(!detailsPanel.window)return;
    RECT r{};GetClientRect(detailsPanel.window,&r);
    const auto place=[](HWND child,RECT box){MoveWindow(child,box.left,box.top,box.right-box.left,box.bottom-box.top,TRUE);};
    detailsPanel.searchFrame={dp(18),dp(84),r.right-dp(18),dp(124)};
    detailsPanel.listFrame={dp(18),dp(138),r.right-dp(18),r.bottom-dp(210)};
    detailsPanel.bodyFrame={dp(18),r.bottom-dp(168),r.right-dp(18),r.bottom-dp(68)};
    auto box=detailsPanel.searchFrame;box.left+=dp(38);box.right-=dp(12);box.top+=dp(10);box.bottom-=dp(9);place(detailsPanel.search,box);
    box=detailsPanel.listFrame;InflateRect(&box,-dp(5),-dp(5));place(detailsPanel.list,box);
    place(detailsPanel.empty,{box.left+dp(28),box.top+dp(12),box.right-dp(28),box.bottom-dp(12)});
    box=detailsPanel.bodyFrame;InflateRect(&box,-dp(12),-dp(10));place(detailsPanel.body,box);
    place(detailsPanel.x,{r.right-dp(50),dp(18),r.right-dp(18),dp(50)});
    const auto dc=GetDC(detailsPanel.window);int right=r.right-dp(18);
    for(const auto button:{detailsPanel.close,detailsPanel.copyAll,detailsPanel.copy}) {
        const int width=std::max(dp(92),textWidth(dc,text(button),smallFont)+dp(46));
        place(button,{right-width,r.bottom-dp(50),right,r.bottom-dp(18)});right-=width+dp(8);
    }
    ReleaseDC(detailsPanel.window,dc);
}
void refreshDetails(bool force=false) {
    if(!detailsPanel.window || (!force && detailsPanel.revision==runCounters.revision))return;
    detailsPanel.revision=runCounters.revision;
    const auto selected=selectedDiagnostic();const auto key=selected?selected->key:detailsPanel.selectedKey;
    const auto top=SendMessageW(detailsPanel.list,LB_GETTOPINDEX,0,0);
    const auto query=searchFold(text(detailsPanel.search));
    detailsPanel.rows.clear();
    const auto& rows=runCounters.details[detailsPanel.category];
    for(size_t i=0;i<rows.size();++i)if(query.empty() || searchFold(diagnosticSubject(rows[i])+L"\n"+diagnosticReason(rows[i])).find(query)!=std::wstring::npos)detailsPanel.rows.push_back(i);
    SendMessageW(detailsPanel.list,WM_SETREDRAW,FALSE,0);SendMessageW(detailsPanel.list,LB_RESETCONTENT,0,0);
    size_t selection=0;
    for(size_t i=0;i<detailsPanel.rows.size();++i) {
        const auto& row=rows[detailsPanel.rows[i]];
        const auto label=diagnosticSubject(row);SendMessageW(detailsPanel.list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        if(row.key==key)selection=i;
    }
    if(!detailsPanel.rows.empty())SendMessageW(detailsPanel.list,LB_SETCURSEL,selection,0);
    SendMessageW(detailsPanel.list,LB_SETTOPINDEX,std::max<LRESULT>(0,top),0);
    SendMessageW(detailsPanel.list,WM_SETREDRAW,TRUE,0);InvalidateRect(detailsPanel.list,nullptr,FALSE);
    EnableWindow(detailsPanel.copyAll,!detailsPanel.rows.empty());
    ShowWindow(detailsPanel.empty,detailsPanel.rows.empty()?SW_SHOW:SW_HIDE);
    SetWindowTextW(detailsPanel.empty,i18n::tr(query.empty()?"details_empty":"details_no_match").c_str());
    SetWindowTextW(detailsPanel.copy,i18n::tr("details_copy_selected").c_str());
    SetWindowTextW(detailsPanel.copyAll,i18n::tr("details_copy_listed").c_str());
    SetWindowTextW(detailsPanel.close,i18n::tr("studio_close").c_str());
    SetWindowTextW(detailsPanel.window,i18n::tr(std::get<0>(counterFacts()[detailsPanel.category])).c_str());
    SendMessageW(detailsPanel.search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(i18n::tr("details_search").c_str()));
    updateDetailsSelection();layoutDetails();InvalidateRect(detailsPanel.window,nullptr,FALSE);
}
std::wstring detailsCopyText(bool all) {
    std::wstring value;
    const auto append=[&](const RunDiagnostic& row) {
        if(!value.empty())value+=L"\r\n\r\n";
        value+=diagnosticSubject(row)+L"\r\n"+diagnosticReason(row);
        if(row.occurrences>1)value+=L"\r\n"+i18n::format("details_occurrences",row.occurrences,0);
    };
    if(all)for(const auto index:detailsPanel.rows)append(runCounters.details[detailsPanel.category][index]);
    else if(const auto row=selectedDiagnostic())append(*row);
    for(size_t at=0;(at=value.find(L'\n',at))!=std::wstring::npos;++at)if(!at || value[at-1]!=L'\r'){value.insert(at,1,L'\r');++at;}
    return value;
}
void copyDetails(bool all) {
    const auto value=detailsCopyText(all);if(value.empty())return;
    bool copied=false;
    if(OpenClipboard(detailsPanel.window)) {
        const auto memory=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
        if(memory) {
            if(const auto destination=static_cast<wchar_t*>(GlobalLock(memory))) {
                wcscpy_s(destination,value.size()+1,value.c_str());GlobalUnlock(memory);
                if(EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory))copied=true;
            }
            if(!copied)GlobalFree(memory);
        }
        CloseClipboard();
    }
    if(copied){detailsPanel.copiedAt=GetTickCount64();InvalidateRect(detailsPanel.window,nullptr,FALSE);}
    else MessageBoxW(detailsPanel.window,i18n::tr("details_copy_failed").c_str(),L"MapExtractor",MB_OK|MB_ICONWARNING);
}
LRESULT CALLBACK detailsButtonProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_MOUSEMOVE && !GetPropW(window,L"hover")) {SetPropW(window,L"hover",HANDLE(1));TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,window,0};TrackMouseEvent(&event);InvalidateRect(window,nullptr,FALSE);}
    if(message==WM_MOUSELEAVE){RemovePropW(window,L"hover");InvalidateRect(window,nullptr,FALSE);}
    if(message==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IsWindowEnabled(window)?IDC_HAND:IDC_ARROW));return TRUE;}
    return DefSubclassProc(window,message,wParam,lParam);
}
LRESULT CALLBACK detailsProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    switch(message) {
    case WM_CREATE: {
        detailsPanel.window=window;
        const auto child=[&](const wchar_t* type,int id,DWORD style) {
            const auto handle=CreateWindowExW(0,type,L"",WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(handle,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);return handle;
        };
        detailsPanel.search=child(L"EDIT",900,WS_TABSTOP|ES_AUTOHSCROLL);
        detailsPanel.list=child(L"LISTBOX",901,WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT);
        SendMessageW(detailsPanel.list,LB_SETITEMHEIGHT,0,dp(86));
        detailsPanel.body=child(MSFTEDIT_CLASS,902,WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL);
        SendMessageW(detailsPanel.body,EM_SETBKGNDCOLOR,0,theme::inset);SendMessageW(detailsPanel.body,EM_SETLIMITTEXT,16777216,0);
        CHARFORMAT2W format{};format.cbSize=sizeof(format);format.dwMask=CFM_COLOR;format.crTextColor=theme::body;
        SendMessageW(detailsPanel.body,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&format));
        detailsPanel.copy=child(L"BUTTON",903,WS_TABSTOP|BS_OWNERDRAW);detailsPanel.copyAll=child(L"BUTTON",904,WS_TABSTOP|BS_OWNERDRAW);
        detailsPanel.close=child(L"BUTTON",905,WS_TABSTOP|BS_OWNERDRAW);detailsPanel.x=child(L"BUTTON",906,WS_TABSTOP|BS_OWNERDRAW);
        detailsPanel.empty=child(L"STATIC",907,SS_CENTER|SS_CENTERIMAGE);
        for(const auto button:{detailsPanel.copy,detailsPanel.copyAll,detailsPanel.close,detailsPanel.x})SetWindowSubclass(button,detailsButtonProc,1,0);
        for(const auto handle:{detailsPanel.search,detailsPanel.list,detailsPanel.body})SetWindowTheme(handle,L"DarkMode_Explorer",nullptr);
        BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));const DWORD corners=1;DwmSetWindowAttribute(window,33,&corners,sizeof(corners));
        SetTimer(window,1,250,nullptr);refreshDetails(true);return 0;
    }
    case WM_ERASEBKGND:return 1;
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT paint{};const auto target=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(wParam);RECT r{};GetClientRect(window,&r);
        {
            Buffered buffer(target,r);const auto dc=buffer.dc;const auto tint=detailsColor();FillRect(dc,&r,panel);
            roundRect(dc,r,0,theme::none,mix(tint,theme::border,0.7f));
            roundGradient(dc,{1,1,r.right-1,dp(3)},0,tint,mix(tint,theme::header,0.9f),theme::none,true);
            icon(dc,detailsPanel.category==2?Icon::map:detailsPanel.category==1?Icon::spark:Icon::pulse,{dp(20),dp(20),dp(42),dp(42)},tint,2);
            const auto title=upper(i18n::tr(std::get<0>(counterFacts()[detailsPanel.category])));
            studioText(dc,title,{dp(54),dp(12),r.right-dp(154),dp(36)},theme::text,titleFont);
            studioText(dc,i18n::tr("details_subtitle"),{dp(54),dp(38),r.right-dp(64),dp(58)},theme::muted,smallFont);
            const auto count=std::to_wstring(detailsTotal());const int width=textWidth(dc,count,logFont)+dp(38);
            pill(dc,{r.right-dp(64)-width,dp(20),r.right-dp(64),dp(46)},tint,count,logFont,true);
            hline(dc,dp(18),r.right-dp(18),dp(68),theme::line);
            roundRect(dc,detailsPanel.searchFrame,dpf(7),theme::inset,GetFocus()==detailsPanel.search?tint:theme::border);
            icon(dc,Icon::search,{dp(30),dp(96),dp(46),dp(112)},theme::muted);
            roundRect(dc,detailsPanel.listFrame,dpf(7),theme::inset,theme::line);
            studioText(dc,i18n::tr("details_reason"),{dp(20),r.bottom-dp(200),r.right-dp(20),r.bottom-dp(174)},theme::muted,smallFont);
            roundRect(dc,detailsPanel.bodyFrame,dpf(7),theme::inset,theme::line);
            RECT copy{};GetWindowRect(detailsPanel.copy,&copy);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&copy),2);
            const bool copied=detailsPanel.copiedAt && GetTickCount64()-detailsPanel.copiedAt<1600;
            studioText(dc,copied?i18n::tr("details_copied"):i18n::format("details_showing",detailsPanel.rows.size(),runCounters.details[detailsPanel.category].size()),{dp(20),r.bottom-dp(50),copy.left-dp(12),r.bottom-dp(18)},copied?theme::green:theme::muted,smallFont);
        }
        if(message==WM_PAINT)EndPaint(window,&paint);return 0;
    }
    case WM_MEASUREITEM:reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight=dp(86);return TRUE;
    case WM_DRAWITEM: {
        auto item=*reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        Buffered buffer(item.hDC,item.rcItem);item.hDC=buffer.dc;const auto dc=item.hDC;RECT r=item.rcItem;const auto tint=detailsColor();
        FillRect(dc,&r,item.CtlID==901?field:panel);
        if(item.CtlID==901) {
            if(item.itemID==UINT(-1) || item.itemID>=detailsPanel.rows.size())return TRUE;
            const auto& row=runCounters.details[detailsPanel.category][detailsPanel.rows[item.itemID]];
            const bool selected=(item.itemState&ODS_SELECTED)!=0;
            InflateRect(&r,-dp(3),-dp(3));roundRect(dc,r,dpf(6),selected?mix(tint,theme::inset,0.92f):theme::surface,selected?mix(tint,theme::inset,0.5f):theme::line);
            if(selected)roundRect(dc,{r.left+dp(2),r.top+dp(10),r.left+dp(4),r.bottom-dp(10)},1,tint);
            studioText(dc,diagnosticSubject(row),{r.left+dp(14),r.top+dp(6),r.right-dp(62),r.top+dp(28)},selected?tint:theme::text,labelFont);
            if(row.occurrences>1)studioText(dc,L"×"+std::to_wstring(row.occurrences),{r.right-dp(60),r.top+dp(6),r.right-dp(12),r.top+dp(28)},tint,monoSmallFont,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
            studioText(dc,diagnosticReason(row),{r.left+dp(14),r.top+dp(32),r.right-dp(14),r.bottom-dp(8)},theme::muted,font,DT_WORDBREAK|DT_END_ELLIPSIS);
        } else {
            const bool disabled=(item.itemState&ODS_DISABLED)!=0,hot=GetPropW(item.hwndItem,L"hover")!=nullptr,pressed=(item.itemState&ODS_SELECTED)!=0;
            roundRect(dc,r,dpf(6),pressed?theme::pressed:hot?mix(tint,theme::surface,0.92f):theme::raised,hot?tint:theme::border);
            if(item.CtlID==906)icon(dc,Icon::close,{r.left+dp(8),r.top+dp(8),r.right-dp(8),r.bottom-dp(8)},hot?tint:theme::muted,2);
            else {
                const bool copy=item.CtlID==903 || item.CtlID==904;
                if(copy)icon(dc,Icon::copy,{r.left+dp(10),r.top+(r.bottom-r.top-dp(14))/2,r.left+dp(24),r.top+(r.bottom-r.top+dp(14))/2},disabled?theme::faint:tint);
                studioText(dc,text(item.hwndItem),{r.left+dp(copy?30:10),r.top,r.right-dp(10),r.bottom},disabled?theme::faint:theme::text,smallFont,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
            }
            if(item.itemState&ODS_FOCUS){InflateRect(&r,-dp(3),-dp(3));roundRect(dc,r,dpf(4),theme::none,tint);}
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:SetTextColor(reinterpret_cast<HDC>(wParam),theme::body);SetBkColor(reinterpret_cast<HDC>(wParam),theme::inset);return reinterpret_cast<LRESULT>(field);
    case WM_COMMAND:
        switch(LOWORD(wParam)) {
        case 900:if(HIWORD(wParam)==EN_CHANGE)refreshDetails(true);return 0;
        case 901:if(HIWORD(wParam)==LBN_SELCHANGE)updateDetailsSelection();return 0;
        case 903:copyDetails(false);return 0;
        case 904:copyDetails(true);return 0;
        case IDCANCEL:case 905:case 906:DestroyWindow(window);return 0;
        }
        break;
    case WM_TIMER:
        refreshDetails();
        if(detailsPanel.copiedAt && GetTickCount64()-detailsPanel.copiedAt>=1600){detailsPanel.copiedAt=0;InvalidateRect(window,nullptr,FALSE);}return 0;
    case WM_NCHITTEST: {
        POINT point{short(LOWORD(lParam)),short(HIWORD(lParam))};ScreenToClient(window,&point);RECT r{};GetClientRect(window,&r);
        return point.y<dp(68) && point.x<r.right-dp(54)?HTCAPTION:HTCLIENT;
    }
    case WM_SYSCOMMAND:if((wParam&0xfff0)==SC_MAXIMIZE || (wParam&0xfff0)==SC_SIZE)return 0;break;
    case WM_NCLBUTTONDBLCLK:return 0;
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:KillTimer(window,1);detailsPanel.window=nullptr;return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
void openDetails(HWND owner,size_t category) {
    if(category>=3 || !std::get<1>(counterFacts()[category]))return;
    if(detailsPanel.window) {
        SendMessageW(detailsPanel.list,LB_SETCURSEL,WPARAM(-1),0);
        detailsPanel.category=category;detailsPanel.selectedKey.clear();SetWindowTextW(detailsPanel.search,L"");refreshDetails(true);
        ShowWindow(detailsPanel.window,SW_SHOWNORMAL);SetForegroundWindow(detailsPanel.window);SetFocus(detailsPanel.search);return;
    }
    detailsPanel={};detailsPanel.category=category;
    RECT parent{},work{};GetWindowRect(owner,&parent);SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    const int width=std::min<LONG>(dp(780),work.right-work.left-dp(24)),height=std::min<LONG>(dp(580),work.bottom-work.top-dp(24));
    int x=std::clamp(int((parent.left+parent.right-width)/2),int(work.left)+dp(12),int(work.right)-width-dp(12));
    int y=std::clamp(int((parent.top+parent.bottom-height)/2),int(work.top)+dp(12),int(work.bottom)-height-dp(12));
    if(verifying){x=-32000;y=-32000;}
    CreateWindowExW(WS_EX_TOOLWINDOW,L"911regDiagnostics",L"",WS_POPUP|WS_CLIPCHILDREN,x,y,width,height,owner,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(detailsPanel.window){ShowWindow(detailsPanel.window,SW_SHOW);SetFocus(detailsPanel.search);}
}

void verifyDetailsWindow(HWND owner) {
    const auto need=[](bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);};
    auto saved=runCounters;runCounters={};
    openDetails(owner,1);need(!detailsPanel.window,"Zero-count badge opened a panel");
    runCounters.consume("ERROR: 22_22.unr: Output directory is not writable",3);
    const std::string warning="WARNING: 16_15.unr / StaticMeshActor119: Missing client package: Instance_Giran_Devil_island_S. This actor contributes no collision.";
    runCounters.consume(warning,3);runCounters.consume(warning,3);
    runCounters.consume("WARNING: 13_23.unr: Ignored 1066 floor candidates outside the packed geodata height range [-16384, 16384). Solid collision and representable floors were retained; heights were not clamped or wrapped.",3);
    for(int i=0;i<12;++i)runCounters.consume("WARNING: 19_26_Classic.unr / Mover"+std::to_string(i)+": Duplicate server door ID; upper placement retained.",3);
    runCounters.consume("SKIPPED: 18_13.unr / StaticMeshActor1: Invalid collision dimensions",3);
    runCounters.consume("SKIPPED: 18_13.unr / StaticMeshActor2: Invalid collision transform",3);
    runCounters.consume("SKIPPED: 18_13.unr: Client collision data could not be read completely (2 issues)",3);
    InvalidateRect(countersCanvas,nullptr,FALSE);UpdateWindow(countersCanvas);
    CHARRANGE mark{4,12};SendMessageW(logView,EM_EXSETSEL,0,reinterpret_cast<LPARAM>(&mark));
    POINT middle{0,60};SendMessageW(logView,EM_SETSCROLLPOS,0,reinterpret_cast<LPARAM>(&middle));
    const auto console=text(logView);CHARRANGE selection{};SendMessageW(logView,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&selection));
    POINT scroll{};SendMessageW(logView,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&scroll));
    const auto box=counterBoxes[1];const auto point=MAKELPARAM((box.left+box.right)/2,(box.top+box.bottom)/2);
    SendMessageW(countersCanvas,WM_LBUTTONDOWN,MK_LBUTTON,point);SendMessageW(countersCanvas,WM_LBUTTONUP,0,point);
    need(detailsPanel.window && detailsPanel.category==1,"Warning badge click failed");
    need(detailsPanel.rows.size()==14,"Warning detail rows were not deduplicated");
    previewPath=studioProof/L"details-warnings.bmp";renderPreview(detailsPanel.window);
    SetWindowTextW(detailsPanel.search,L"instance_giran");
    need(detailsPanel.rows.size()==1,"Reason search failed");
    need(selectedDiagnostic() && selectedDiagnostic()->occurrences==2,"Repeated warning count lost");
    need(detailsCopyText(true).find(L"Instance_Giran_Devil_island_S")!=std::wstring::npos,"Filtered copy omitted full reason");
    need(detailsCopyText(false).find(i18n::format("details_occurrences",2,0))!=std::wstring::npos,"Selected copy omitted occurrence count");
    runCounters.consume(warning,3);SendMessageW(detailsPanel.window,WM_TIMER,1,0);
    need(text(detailsPanel.search)==L"instance_giran" && selectedDiagnostic()->occurrences==3,"Live refresh lost filter or occurrence count");
    previewPath=studioProof/L"details-filtered.bmp";renderPreview(detailsPanel.window);
    SetWindowTextW(detailsPanel.search,L"there_is_no_such_record");
    need(detailsPanel.rows.empty() && !IsWindowEnabled(detailsPanel.copy) && !IsWindowEnabled(detailsPanel.copyAll),"Empty filter leaves copy enabled");
    previewPath=studioProof/L"details-empty.bmp";renderPreview(detailsPanel.window);
    openDetails(owner,2);
    need(detailsPanel.rows.size()==1 && text(detailsPanel.body).find(L"StaticMeshActor1")!=std::wstring::npos && text(detailsPanel.body).find(L"StaticMeshActor2")!=std::wstring::npos,"Skipped map did not retain grouped actor reasons");
    previewPath=studioProof/L"details-skipped.bmp";renderPreview(detailsPanel.window);
    openDetails(owner,0);previewPath=studioProof/L"details-errors.bmp";renderPreview(detailsPanel.window);
    const auto language=i18n::selected();
    for(size_t i=0;i<i18n::languages().size();++i) {
        i18n::select(i);refreshDetails(true);
        RECT window{};GetClientRect(detailsPanel.window,&window);
        for(const auto child:{detailsPanel.search,detailsPanel.list,detailsPanel.body,detailsPanel.copy,detailsPanel.copyAll,detailsPanel.close,detailsPanel.x}) {
            RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,detailsPanel.window,reinterpret_cast<POINT*>(&bounds),2);
            need(bounds.left>=0 && bounds.top>=0 && bounds.right<=window.right && bounds.bottom<=window.bottom,"Translated detail controls escaped the panel");
        }
        previewPath=studioProof/(L"details-language-"+std::to_wstring(i)+L".bmp");renderPreview(detailsPanel.window);
    }
    i18n::select(language);refreshDetails(true);
    SendMessageW(detailsPanel.window,WM_COMMAND,IDCANCEL,0);need(!detailsPanel.window,"Details close failed");
    counterFocus=0;SetFocus(countersCanvas);SendMessageW(countersCanvas,WM_KEYDOWN,VK_RETURN,0);
    need(detailsPanel.window && detailsPanel.category==0,"Keyboard badge activation failed");
    DestroyWindow(detailsPanel.window);
    CHARRANGE after{};SendMessageW(logView,EM_EXGETSEL,0,reinterpret_cast<LPARAM>(&after));
    POINT afterScroll{};SendMessageW(logView,EM_GETSCROLLPOS,0,reinterpret_cast<LPARAM>(&afterScroll));
    need(text(logView)==console && after.cpMin==selection.cpMin && after.cpMax==selection.cpMax && scroll.x==afterScroll.x && scroll.y==afterScroll.y,"Details disturbed console text, selection or scroll");
    SendMessageW(owner,WM_COMMAND,130,0);openDetails(owner,1);
    need(detailsPanel.rows.size()==14,"Console clear erased diagnostic details");
    runCounters={};refreshDetails(true);need(detailsPanel.rows.empty(),"New run did not clear detail panel");
    DestroyWindow(detailsPanel.window);
    runCounters=std::move(saved);counterHot=-1;counterFocus=-1;counterPressed=-1;
}
