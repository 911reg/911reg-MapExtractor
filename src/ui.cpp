#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include "localization.h"
#include "history.h"
#include <memory>
#include <commctrl.h>
#include <uxtheme.h>
#include <vector>
#include <algorithm>
#include <array>
#include "map_selection.h"
#include "output_tasks.h"
#include "ui_preferences.h"
#include "settings_file.h"

// Charcoal window. The three tiles are Doors (120), Water (121) and Geodata (123).
// There is no Map check tile and no Advanced switch. taskSelector stays hidden with
// a scan entry at index 2 so --verify-ui can still select that command-line task.
// The server dropdown shows L2jMabios; the saved id remains "mobius".

namespace {
HWND input, output, status, startButton, openButton, inputButton, outputButton;
HWND clearOutputButton;
HWND subtitle, clientLabel, outputLabel, taskLabel, capability, taskSelector, languageSelector;
HWND mapInput;
HWND logView, progressBar, appTitle, activityLabel;
HWND taskTiles[4]{};
size_t displayedLog = 0;
std::ofstream jobLog;
HANDLE process = nullptr;
HANDLE scanPipe = nullptr;
HFONT font, titleFont;
HFONT logFont;
HBRUSH background, field, panel;
std::wstring manifest;
std::filesystem::path runFolder;
std::string scanLog;
bool verifying = false;
int verificationResult = 1;
std::wstring verificationInput, verificationOutput;
unsigned taskMode = 0;
bool singleMap = false;
bool selectedMapMode = true;
std::vector<std::filesystem::path> selectedMaps;
std::filesystem::path selectedMapsRoot;
HWND tips;
std::map<HWND,std::string> tipKeys;
std::wstring tooltipText;
std::filesystem::path previewPath;
std::unique_ptr<extractor::ClientHistory> clientHistory;
std::wstring text(HWND control);
bool sourceReady() {
    return selectedMapMode ? !selectedMaps.empty() : !(singleMap ? text(mapInput) : text(input)).empty();
}

void rounded(HDC dc,RECT r,COLORREF color,COLORREF border,int radius=12) {
    auto brush=CreateSolidBrush(color); auto pen=CreatePen(PS_SOLID,1,border);
    auto oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
    RoundRect(dc,r.left,r.top,r.right,r.bottom,radius,radius);
    SelectObject(dc,oldBrush); SelectObject(dc,oldPen); DeleteObject(brush); DeleteObject(pen);
}
LRESULT CALLBACK tooltipProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if (message==WM_ERASEBKGND || message==WM_NCPAINT) return 1;
    if (message==WM_WINDOWPOSCHANGED) {
        const auto result=DefSubclassProc(window,message,wParam,lParam);
        const auto position=reinterpret_cast<WINDOWPOS*>(lParam);
        if (!(position->flags&SWP_NOSIZE)) {
            RECT bounds{}; GetWindowRect(window,&bounds);
            auto region=CreateRoundRectRgn(0,0,bounds.right-bounds.left+1,bounds.bottom-bounds.top+1,12,12);
            if (!SetWindowRgn(window,region,TRUE)) DeleteObject(region);
        }
        return result;
    }
    if (message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{}; auto dc=message==WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wParam);
        RECT bounds{}; GetClientRect(window,&bounds);
        rounded(dc,bounds,RGB(38,38,38),RGB(82,66,54),12);
        RECT accent{bounds.left+1,bounds.top+10,bounds.left+3,bounds.bottom-10};
        auto brush=CreateSolidBrush(RGB(255,132,40)); FillRect(dc,&accent,brush); DeleteObject(brush);
        InflateRect(&bounds,-14,-10); auto previous=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(235,235,235));
        DrawTextW(dc,tooltipText.c_str(),-1,&bounds,DT_WORDBREAK|DT_NOPREFIX); SelectObject(dc,previous);
        if (message==WM_PAINT) EndPaint(window,&paint); return 0;
    }
    return DefSubclassProc(window,message,wParam,lParam);
}
void keyboardFocus(const DRAWITEMSTRUCT& item) {
    if (!(item.itemState&ODS_FOCUS) || (item.itemState&ODS_NOFOCUSRECT) || (SendMessageW(GetParent(item.hwndItem),WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) return;
    auto bounds=item.rcItem; InflateRect(&bounds,-5,-5);
    auto pen=CreatePen(PS_SOLID,1,item.CtlID==103 || item.CtlID==IDOK ? RGB(60,40,20) : RGB(255,170,80));
    auto previousPen=SelectObject(item.hDC,pen); auto previousBrush=SelectObject(item.hDC,GetStockObject(HOLLOW_BRUSH));
    RoundRect(item.hDC,bounds.left,bounds.top,bounds.right,bounds.bottom,6,6);
    SelectObject(item.hDC,previousBrush); SelectObject(item.hDC,previousPen); DeleteObject(pen);
}
void focusInput(HWND window,const MSG& message) {
    if (message.message==WM_LBUTTONDOWN || message.message==WM_RBUTTONDOWN) SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
    else if (message.message==WM_KEYDOWN && (message.wParam==VK_TAB || message.wParam==VK_UP || message.wParam==VK_DOWN || message.wParam==VK_LEFT || message.wParam==VK_RIGHT)) SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0);
}
void glyph(HDC dc,int kind,int x,int y,COLORREF color) {
    auto pen=CreatePen(PS_SOLID,2,color); auto previous=SelectObject(dc,pen); auto brush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
    if (kind==0) {
        Rectangle(dc,x+3,y+1,x+21,y+25); MoveToEx(dc,x+8,y+24,nullptr); LineTo(dc,x+8,y+5); LineTo(dc,x+20,y+2);
        Ellipse(dc,x+15,y+12,x+18,y+15);
    } else if (kind==1) {
        for (int i=0;i<3;++i) { POINT p[]{{x,y+5+i*8},{x+6,y+2+i*8},{x+12,y+5+i*8},{x+18,y+2+i*8},{x+24,y+5+i*8}}; Polyline(dc,p,5); }
    } else if (kind==2) {
        POINT p[]{{x,y+6},{x+12,y},{x+24,y+6},{x+12,y+12},{x,y+6}}; Polyline(dc,p,5);
        for (int i=1;i<3;++i) { POINT q[]{{x,y+6+i*7},{x+12,y+12+i*7},{x+24,y+6+i*7}}; Polyline(dc,q,3); }
    } else if (kind==3) {
        Rectangle(dc,x+1,y+1,x+20,y+21); MoveToEx(dc,x+7,y+11,nullptr); LineTo(dc,x+11,y+15); LineTo(dc,x+17,y+7);
        MoveToEx(dc,x+20,y+20,nullptr); LineTo(dc,x+26,y+26);
    } else {
        POINT p[]{{x,y+21},{x,y+3},{x+9,y+3},{x+12,y+7},{x+25,y+7},{x+25,y+21},{x,y+21}}; Polyline(dc,p,7);
    }
    SelectObject(dc,brush); SelectObject(dc,previous); DeleteObject(pen);
}
struct Choice { std::vector<std::wstring> items; int selected=-1; HWND edit=nullptr; };
thread_local HWND openChoice=nullptr;
LRESULT CALLBACK choiceMenuFilter(int code,WPARAM wParam,LPARAM lParam) {
    if (code==MSGF_MENU && openChoice) {
        const auto& message=*reinterpret_cast<MSG*>(lParam);
        if (message.message==WM_LBUTTONDOWN || message.message==WM_NCLBUTTONDOWN) {
            RECT bounds{}; GetWindowRect(openChoice,&bounds);
            if (PtInRect(&bounds,message.pt)) { EndMenu(); return 1; }
        }
    }
    return CallNextHookEx(nullptr,code,wParam,lParam);
}
struct Progress { int low=0,high=100,position=0; };
LRESULT CALLBACK progressProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto data=reinterpret_cast<Progress*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_CREATE) { SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(new Progress)); return 0; }
    if (message==PBM_SETRANGE32) { data->low=int(wParam); data->high=int(lParam); InvalidateRect(window,nullptr,TRUE); return 0; }
    if (message==PBM_SETPOS) { data->position=int(wParam); InvalidateRect(window,nullptr,TRUE); return 0; }
    if (message==PBM_SETBARCOLOR || message==PBM_SETBKCOLOR || message==WM_ERASEBKGND) return 1;
    if (message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{}; const auto dc=message==WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wParam);
        RECT r{}; GetClientRect(window,&r); FillRect(dc,&r,panel); rounded(dc,r,RGB(61,61,61),RGB(61,61,61),4);
        if (data->high>data->low && data->position>data->low) {
            r.right=std::clamp((r.right*(data->position-data->low))/(data->high-data->low),0L,r.right);
            rounded(dc,r,RGB(255,132,40),RGB(255,132,40),4);
        }
        if (message==WM_PAINT) EndPaint(window,&paint); return 0;
    }
    if (message==WM_NCDESTROY) { delete data; SetWindowLongPtrW(window,GWLP_USERDATA,0); }
    return DefWindowProcW(window,message,wParam,lParam);
}
LRESULT CALLBACK choiceProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto data=reinterpret_cast<Choice*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    auto notify=[&](int code) { SendMessageW(GetParent(window),WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(window),code),reinterpret_cast<LPARAM>(window)); };
    auto select=[&](int index) {
        if (index<0 || size_t(index)>=data->items.size()) return false;
        data->selected=index; SetWindowTextW(window,data->items[index].c_str()); InvalidateRect(window,nullptr,TRUE); return true;
    };
    switch (message) {
    case WM_CREATE: {
        data=new Choice; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(data));
        if (reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams) {
            RECT r{}; GetClientRect(window,&r);
            data->edit=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,12,9,r.right-48,22,window,reinterpret_cast<HMENU>(1),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(data->edit,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        }
        return 0;
    }
    case CB_ADDSTRING: data->items.emplace_back(reinterpret_cast<const wchar_t*>(lParam)); return data->items.size()-1;
    case CB_RESETCONTENT: data->items.clear(); data->selected=-1; SetWindowTextW(window,L""); return 0;
    case CB_SETCURSEL: if (wParam==WPARAM(-1)) { data->selected=-1; return CB_ERR; } return select(int(wParam)) ? LRESULT(wParam) : CB_ERR;
    case CB_GETCURSEL: return data->selected;
    case CB_GETCOUNT: return data->items.size();
    case CB_GETLBTEXTLEN: return wParam<data->items.size() ? LRESULT(data->items[wParam].size()) : CB_ERR;
    case CB_GETLBTEXT:
        if (wParam>=data->items.size()) return CB_ERR;
        wcscpy_s(reinterpret_cast<wchar_t*>(lParam),data->items[wParam].size()+1,data->items[wParam].c_str()); return data->items[wParam].size();
    case CB_LIMITTEXT: if (data->edit) SendMessageW(data->edit,EM_SETLIMITTEXT,wParam,0); return 0;
    case CB_SETDROPPEDWIDTH: return 0;
    case WM_SETTEXT: {
        const auto result=DefWindowProcW(window,message,wParam,lParam);
        if (data && data->edit) SetWindowTextW(data->edit,reinterpret_cast<const wchar_t*>(lParam));
        InvalidateRect(window,nullptr,TRUE); return result;
    }
    case WM_GETTEXT: if (data && data->edit) return SendMessageW(data->edit,message,wParam,lParam); break;
    case WM_GETTEXTLENGTH: if (data && data->edit) return GetWindowTextLengthW(data->edit); break;
    case WM_COMMAND: if (HIWORD(wParam)==EN_CHANGE) notify(CBN_EDITCHANGE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | ((wParam==VK_RETURN || wParam==VK_SPACE) ? DLGC_WANTALLKEYS : 0);
    case WM_SETFOCUS: if (data->edit) SetFocus(data->edit); InvalidateRect(window,nullptr,TRUE); return 0;
    case WM_KILLFOCUS: InvalidateRect(window,nullptr,TRUE); return 0;
    case WM_ENABLE: if (data->edit) EnableWindow(data->edit,BOOL(wParam)); InvalidateRect(window,nullptr,TRUE); return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: SetTextColor(reinterpret_cast<HDC>(wParam),IsWindowEnabled(window) ? RGB(228,228,228) : RGB(130,130,130)); SetBkColor(reinterpret_cast<HDC>(wParam),RGB(40,40,40)); return reinterpret_cast<LRESULT>(field);
    case WM_MEASUREITEM: {
        auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(lParam); RECT r{}; GetClientRect(window,&r); item->itemWidth=std::max(180L,r.right); item->itemHeight=34; return TRUE;
    }
    case WM_DRAWITEM: {
        auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        rounded(item.hDC,item.rcItem,(item.itemState&ODS_SELECTED) ? RGB(68,48,34) : RGB(40,40,40),RGB(40,40,40),0);
        SelectObject(item.hDC,font); SetBkMode(item.hDC,TRANSPARENT); SetTextColor(item.hDC,RGB(235,235,235));
        auto r=item.rcItem; r.left+=12; r.right-=12;
        DrawTextW(item.hDC,reinterpret_cast<const std::wstring*>(item.itemData)->c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS); return TRUE;
    }
    case WM_KEYDOWN:
        if (data->items.empty()) return 0;
        if (wParam==VK_UP || wParam==VK_DOWN) { if (select(std::clamp(data->selected+(wParam==VK_UP ? -1 : 1),0,int(data->items.size())-1))) notify(CBN_SELCHANGE); return 0; }
        if (wParam!=VK_SPACE && wParam!=VK_RETURN && wParam!=VK_F4) break;
        [[fallthrough]];
    case WM_LBUTTONDOWN: {
        if (data->items.empty()) return 0;
        SetFocus(window); HMENU menu=CreatePopupMenu();
        for (size_t i=0;i<data->items.size();++i) {
            MENUITEMINFOW item{sizeof(item)}; item.fMask=MIIM_ID|MIIM_FTYPE|MIIM_DATA; item.fType=MFT_OWNERDRAW; item.wID=unsigned(i+1); item.dwItemData=reinterpret_cast<ULONG_PTR>(&data->items[i]); InsertMenuItemW(menu,unsigned(i),TRUE,&item);
        }
        RECT r{}; GetWindowRect(window,&r);
        openChoice=window;
        const auto hook=SetWindowsHookExW(WH_MSGFILTER,choiceMenuFilter,nullptr,GetCurrentThreadId());
        const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,r.left,r.bottom+4,0,window,nullptr);
        if (hook) UnhookWindowsHookEx(hook);
        openChoice=nullptr;
        DestroyMenu(menu); if (command && select(int(command)-1)) notify(CBN_SELCHANGE); return 0;
    }
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc=message==WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wParam); RECT r{}; GetClientRect(window,&r);
        rounded(dc,r,RGB(40,40,40),(GetFocus()==window || (GetDlgCtrlID(window)==116 && IsWindowEnabled(window))) ? RGB(255,132,40) : RGB(64,64,64),8);
        SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,IsWindowEnabled(window) ? RGB(228,228,228) : RGB(110,110,110));
        auto label=r; label.left+=12; label.right-=32;
        if (!data->edit) DrawTextW(dc,text(window).c_str(),-1,&label,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        auto pen=CreatePen(PS_SOLID,1,RGB(170,170,170)); auto old=SelectObject(dc,pen);
        POINT arrow[]{{r.right-21,r.bottom/2-2},{r.right-16,r.bottom/2+3},{r.right-11,r.bottom/2-2}}; Polyline(dc,arrow,3);
        SelectObject(dc,old); DeleteObject(pen); if (message==WM_PAINT) EndPaint(window,&paint); return 0;
    }
    case WM_NCDESTROY: delete data; SetWindowLongPtrW(window,GWLP_USERDATA,0); break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

void refreshClients(const std::wstring& selected) {
    SendMessageW(input,CB_RESETCONTENT,0,0);
    for (const auto& client : clientHistory->load()) SendMessageW(input,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(client.c_str()));
    SetWindowTextW(input,selected.c_str());
    EnableWindow(startButton,!process && sourceReady() && !text(output).empty());
}
std::filesystem::path clientMaps() {
    auto root=std::filesystem::path(text(input));
    if (std::filesystem::is_directory(root/L"Maps")) root/=L"Maps";
    else if (extractor::folded(root.filename().wstring())==L"system" && std::filesystem::is_directory(root.parent_path()/L"Maps")) root=root.parent_path()/L"Maps";
    return root;
}
HWND preferClassicButton,preferNormalButton;
HWND mapList,mapFilter,mapFilterLabel,mapLabel,mapCount,selectAllButton,clearMapsButton,serverSelector,combineButton;
std::vector<std::filesystem::path> availableMaps;
HWND serverLabel;
bool updatingMaps=false,combineResults=true,debugResults=false,verificationSimple=false,lockMapSelection=false;
std::string selectedServer="l2jserver",geoFormat="both";
std::vector<std::string> visibleServers;
std::filesystem::path preferencesPath,selectionPath;
void savePreferences() {
    if (verifying) return;
    if (!extractor::UiPreferences{selectedServer,combineResults,debugResults,geoFormat}.save(preferencesPath)) SetWindowTextW(status,i18n::tr("settings_error").c_str());
}
void populateServers() {
    visibleServers.clear(); SendMessageW(serverSelector,CB_RESETCONTENT,0,0);
    if (taskMode==3) {
        const std::array<std::pair<const char*,const char*>,3> choices{{{"both","geo_both"},{"l2j","geo_l2j"},{"l2off","geo_conv"}}};
        int selected=0;
        for (int i=0;i<3;++i) {
            const auto label=i18n::tr(choices[size_t(i)].second);
            if (geoFormat==choices[size_t(i)].first) selected=i;
            SendMessageW(serverSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(serverSelector,CB_SETCURSEL,selected,0);
        tipKeys[serverSelector]="tip_geo_format";
        return;
    }
    tipKeys[serverSelector]="tip_server";
    for (size_t i=0;i<extractor::serverProfiles.size();++i) {
        const auto& profile=extractor::serverProfiles[i]; if (profile=="all" && selectedServer!="all") continue;
        const auto label=i==5 ? i18n::tr("server_all") : std::array<std::wstring,5>{L"L2JServer",L"aCis",L"L2Scripts",L"L2jMabios",L"L2OFF"}[i];
        visibleServers.push_back(profile); SendMessageW(serverSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(serverSelector,CB_SETCURSEL,std::find(visibleServers.begin(),visibleServers.end(),selectedServer)-visibleServers.begin(),0);
}
// Tiles 0, 1 and 3. Slot 2 is the hidden scan task and has no button.
void placeTasks() {
    const unsigned shown[]{0,1,3};
    for (unsigned position=0;position<3;++position) SetWindowPos(taskTiles[shown[position]],nullptr,44+int(position)*204,410,192,76,SWP_NOZORDER|SWP_NOACTIVATE);
    populateServers();
}
void updateMapCount() {
    SetWindowTextW(mapCount,i18n::format("maps_count",selectedMaps.size(),availableMaps.size()).c_str());
    EnableWindow(startButton,!process && sourceReady() && !text(output).empty());
}
void populateMapList(bool reload) {
    if (!mapList || updatingMaps) return;
    updatingMaps=true;
    try {
        if (reload) {
            const auto root=std::filesystem::weakly_canonical(clientMaps());
            std::error_code sameRoot;
            const bool newClient=selectedMapsRoot.empty() || !std::filesystem::equivalent(root,selectedMapsRoot,sameRoot) || sameRoot;
            if (newClient) {
                availableMaps.clear(); selectedMaps.clear(); selectedMapsRoot=root;
                if (!text(input).empty() && std::filesystem::is_directory(root)) {
                    availableMaps=extractor::selectMaps(root,false);
                    selectedMaps=availableMaps;
                }
            }
        }
        ListView_DeleteAllItems(mapList);
        const auto filter=extractor::folded(text(mapFilter));
        for (size_t i=0;i<availableMaps.size();++i) {
            auto label=std::filesystem::relative(availableMaps[i],selectedMapsRoot).wstring();
            if (!filter.empty() && extractor::folded(label).find(filter)==std::wstring::npos) continue;
            LVITEMW item{}; item.mask=LVIF_TEXT|LVIF_PARAM; item.iItem=ListView_GetItemCount(mapList); item.pszText=label.data(); item.lParam=LPARAM(i);
            const auto row=ListView_InsertItem(mapList,&item);
            ListView_SetCheckState(mapList,row,std::find(selectedMaps.begin(),selectedMaps.end(),availableMaps[i])!=selectedMaps.end());
        }
    } catch (const std::exception&) { availableMaps.clear(); selectedMaps.clear(); ListView_DeleteAllItems(mapList); }
    updatingMaps=false; updateMapCount();
}
void addTip(HWND owner,HWND target,const char* key) {
    tipKeys[target]=key;
    TOOLINFOW info{TTTOOLINFOW_V2_SIZE}; info.uFlags=TTF_IDISHWND|TTF_SUBCLASS; info.hwnd=owner; info.uId=reinterpret_cast<UINT_PTR>(target); info.lpszText=LPSTR_TEXTCALLBACKW;
    SendMessageW(tips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
}
bool confirmClearOutput(HWND owner,const std::filesystem::path& root) {
    const auto target=extractor::taskOutput(root,taskMode);
    if (!std::filesystem::exists(std::filesystem::symlink_status(target))) return true;
    const auto source=singleMap ? text(mapInput) : text(input);
    if (!source.empty()) {
        const auto relative=std::filesystem::weakly_canonical(source).lexically_relative(target);
        if (!relative.empty() && *relative.begin()!=L"..") throw std::runtime_error("Task output contains the selected client or map");
    }
    if (verifying) return false;
    auto message=i18n::tr(taskMode==2 ? "clear_confirm_file" : "clear_confirm_folder");
    for (const auto& [token,value]:std::array<std::pair<std::wstring,std::wstring>,2>{{{L"{name}",target.filename().wstring()},{L"{path}",target.wstring()}}}) {
        const auto at=message.find(token); if (at!=std::wstring::npos) message.replace(at,token.size(),value);
    }
    if (MessageBoxW(owner,message.c_str(),i18n::tr("clear_confirm_title").c_str(),MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return false;
    extractor::clearTaskOutput(root,taskMode); return true;
}
void rememberClient(const std::filesystem::path& folder) {
    if (verifying || !previewPath.empty()) return;
    try {
        if (clientHistory->remember(folder)) {
            const auto recent = clientHistory->load();
            refreshClients(recent.empty() ? folder.wstring() : recent.front());
        } else SetWindowTextW(status,i18n::tr("history_error").c_str());
    } catch (const std::exception&) { SetWindowTextW(status,i18n::tr("history_error").c_str()); }
}

// Render this application's own window into a test artifact, without capturing the desktop.
void renderPreview(HWND window) {
    RECT rectangle{}; GetWindowRect(window,&rectangle);
    const int width = rectangle.right-rectangle.left, height = rectangle.bottom-rectangle.top;
    HDC screen = GetDC(window), memory = CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!bitmap) throw std::runtime_error("Cannot create UI preview");
    HGDIOBJ previous = SelectObject(memory,bitmap);
    SendMessageW(window,WM_PRINT,reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND|PRF_NONCLIENT);
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header)+sizeof(info.bmiHeader);
    const DWORD bytes = DWORD(width*height*4); header.bfSize = header.bfOffBits+bytes;
    std::ofstream file(previewPath,std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header),sizeof(header));
    file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
    file.write(reinterpret_cast<const char*>(pixels),bytes); file.close();
    SelectObject(memory,previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window,screen);
    if (!file) throw std::runtime_error("UI preview failed");
}

void setText(HWND target, const std::string& key) { SetWindowTextW(target,i18n::tr(key).c_str()); }
void updateLanguage() {
    setText(subtitle,"subtitle"); setText(clientLabel,"client_label"); setText(outputLabel,"output_label");
    setText(inputButton,"choose"); setText(outputButton,"change"); setText(taskLabel,"task_label");
    setText(capability,"variants_hint"); setText(startButton,"start"); setText(openButton,"open_results");
    setText(clearOutputButton,"clear_results");
    setText(activityLabel,"activity");
    const std::array<std::string,4> tileKeys{"tile_doors","tile_water","", "tile_geo"};
    for (unsigned i=0;i<4;++i) if (taskTiles[i]) { setText(taskTiles[i],tileKeys[i]); InvalidateRect(taskTiles[i],nullptr,TRUE); }
    SendMessageW(taskSelector,CB_RESETCONTENT,0,0);
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("task_doors").c_str()));
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("task_water").c_str()));
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("task_scan").c_str()));
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("task_geodata").c_str()));
    SendMessageW(taskSelector,CB_SETCURSEL,taskMode,0);
    if (mapLabel) {
        setText(preferClassicButton,"maps_prefer_classic"); setText(preferNormalButton,"maps_prefer_normal"); setText(mapLabel,"maps_title"); setText(selectAllButton,"maps_all"); setText(clearMapsButton,"maps_none"); setText(combineButton,"combine_xml");
        setText(mapFilterLabel,"maps_search");
        SendMessageW(mapFilter,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(i18n::tr("maps_search").c_str()));
        setText(serverLabel,taskMode==3 ? "geo_format" : "server_type");
        populateServers();
        updateMapCount();
    }
    setText(status,"ready");
}

std::wstring wide(const std::string& value) {
    const auto length = MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),nullptr,0);
    std::wstring result(length,L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),result.data(),length);
    return result;
}
std::pair<std::wstring,std::wstring> completionText(DWORD code) {
    size_t exported=0,skipped=0,maps=0,extra=0,duplicates=0;
    std::string kind; bool found=false,duplicateBlock=false;
    std::wstring reasons,duplicateText;
    std::istringstream lines(scanLog); std::string line;
    while (std::getline(lines,line)) {
        if (line.starts_with("RESULT ")) {
            std::istringstream values(line.substr(7)); values>>kind>>exported>>skipped>>maps>>extra>>duplicates; found=bool(values); duplicateBlock=false;
        } else if (line.starts_with("SKIPPED: ")) reasons+=wide(line.substr(9))+L"\r\n\r\n";
        else if (line.starts_with("ERROR: ")) reasons+=wide(line.substr(7))+L"\r\n\r\n";
        else if (line.starts_with("Duplicate door IDs mean")) { duplicateBlock=true; duplicateText+=wide(line)+L"\r\n"; }
        else if (duplicateBlock) duplicateText+=wide(line)+L"\r\n";
    }
    const auto key=kind=="doors" ? "result_doors" : kind=="water" ? "result_water" : kind=="geodata" ? "result_geodata" : "result_scan";
    const auto title=found ? i18n::format(key,exported,skipped+maps+extra+(kind=="doors" ? duplicates : 0)) : i18n::tr(code ? "failed" : "done");
    auto details=title+L"\r\n\r\n";
    if (!reasons.empty()) details+=i18n::tr("skipped_reasons")+L"\r\n\r\n"+reasons;
    if (!duplicateText.empty()) details+=duplicateText;
    if (reasons.empty() && duplicateText.empty() && code==0) details+=i18n::tr("result_ready");
    return {title,details};
}

void readProgress() {
    DWORD available = 0;
    char buffer[4096];
    while (scanPipe && PeekNamedPipe(scanPipe,nullptr,0,nullptr,&available,nullptr) && available) {
        DWORD count = 0;
        if (!ReadFile(scanPipe,buffer,sizeof(buffer),&count,nullptr) || !count) break;
        scanLog.append(buffer,count);
    }
    for (;;) {
        const auto end = scanLog.find('\n',displayedLog); if (end == std::string::npos) break;
        const auto line = scanLog.substr(displayedLog,end-displayedLog); displayedLog = end+1;
        SYSTEMTIME now{}; GetLocalTime(&now); wchar_t prefix[32]{};
        swprintf_s(prefix,L"[%02u:%02u:%02u] ",now.wHour,now.wMinute,now.wSecond);
        const auto shown = std::wstring(prefix)+wide(line)+L"\r\n";
        if (GetWindowTextLengthW(logView)>200000) { SendMessageW(logView,EM_SETSEL,0,50000); SendMessageW(logView,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L"")); }
        SendMessageW(logView,EM_SETSEL,WPARAM(-1),LPARAM(-1));
        SendMessageW(logView,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(shown.c_str())); SendMessageW(logView,EM_SCROLLCARET,0,0);
        if (jobLog.is_open()) { jobLog << line << '\n'; jobLog.flush(); }
        if (line.starts_with("MAP ") || line.starts_with("STAGE ") || line.starts_with("Geodata complete:")) SetWindowTextW(status,wide(line).c_str());
        if (taskMode==3 && line.starts_with("STAGE ")) {
            size_t block=0,blocks=0;
            int stage=line.starts_with("STAGE spatial") ? 10 : line.starts_with("STAGE rasterize") ? 10 : line.starts_with("STAGE movement") ? 35 : line.starts_with("STAGE diagonal") ? 70 : 90;
            const auto at=line.find("| blocks ");
            if (at!=std::string::npos && sscanf_s(line.c_str()+at,"| blocks %zu/%zu",&block,&blocks)==2 && blocks)
                stage+=int((line.starts_with("STAGE rasterize") ? 25 : line.starts_with("STAGE movement") ? 35 : 20)*std::min(block,blocks)/blocks);
            SendMessageW(progressBar,PBM_SETRANGE32,0,100);
            SendMessageW(progressBar,PBM_SETPOS,stage,0);
        }
        size_t done=0,total=0;
        if (sscanf_s(line.c_str(),"Checked %zu of %zu maps",&done,&total)==2) {
            SendMessageW(progressBar,PBM_SETRANGE32,0,total); SendMessageW(progressBar,PBM_SETPOS,done,0);
        }
    }
    // Keep technical details in the saved report; show the latest complete update here.
    const auto end = scanLog.find_last_of('\n');
    if (end != std::string::npos && end) {
        const auto begin = scanLog.find_last_of('\n',end-1);
        const auto line = scanLog.substr(begin == std::string::npos ? 0 : begin+1,
            end-(begin == std::string::npos ? 0 : begin+1));
        size_t first = 0, second = 0;
        if (sscanf_s(line.c_str(),"Checked %zu of %zu maps",&first,&second) == 2)
            SetWindowTextW(status,i18n::format("progress",first,second).c_str());
        else if (sscanf_s(line.c_str(),"Doors exported: %zu. Need attention: %zu.",&first,&second) == 2)
            SetWindowTextW(status,i18n::format("doors_finished",first,second).c_str());
        else if (sscanf_s(line.c_str(),"Water zones exported: %zu. Need attention: %zu.",&first,&second) == 2)
            SetWindowTextW(status,i18n::format("water_finished",first,second).c_str());
        else if (sscanf_s(line.c_str(),"Finished: %zu maps checked. %zu need attention.",&first,&second) == 2)
            SetWindowTextW(status,i18n::format("finished",first,second).c_str());
    }
}

void chooseFolder(HWND owner, HWND target, const wchar_t* title) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0; dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
    dialog->SetTitle(title);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {
                SetWindowTextW(target,path); CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

std::wstring text(HWND control) {
    std::wstring value(GetWindowTextLengthW(control)+1, L'\0');
    const int length = GetWindowTextW(control, value.data(), int(value.size()));
    value.resize(length);
    return value;
}

// Windows command-line quoting preserves spaces, quotes and trailing backslashes.
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes*2+1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes*2, L'\\');
    return result + L'"';
}

HWND control(HWND parent, const wchar_t* type, const wchar_t* caption,
    DWORD style, int x, int y, int width, int height, int id = 0) {
    const bool choice=wcscmp(type,L"COMBOBOX")==0;
    HWND window = CreateWindowW(choice ? L"911regChoice" : type, caption, choice ? WS_CHILD|WS_VISIBLE|WS_TABSTOP : WS_CHILD|WS_VISIBLE|style,
        x,y,width,choice ? 38 : height,parent,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),choice && (style&3)==CBS_DROPDOWN ? reinterpret_cast<void*>(1) : nullptr);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return window;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc=message==WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wParam);
        for (const RECT card: {RECT{24,106,664,222},RECT{24,234,664,350},RECT{24,362,664,602},RECT{688,106,1008,666},RECT{1032,106,1352,666}})
            rounded(dc,card,RGB(36,36,36),RGB(48,48,48),16);
        rounded(dc,{44,286,532,324},RGB(40,40,40),RGB(64,64,64),8);

        auto pen=CreatePen(PS_SOLID,1,RGB(49,49,49)); auto old=SelectObject(dc,pen);
        MoveToEx(dc,24,88,nullptr); LineTo(dc,1352,88); SelectObject(dc,old); DeleteObject(pen);
        if (message==WM_PAINT) EndPaint(window,&paint); return 0;
    }
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        auto logo = control(window,L"STATIC",L"",SS_ICON,24,22,40,40);
        SendMessageW(logo,STM_SETICON,reinterpret_cast<WPARAM>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),IMAGE_ICON,40,40,0)),0);
        appTitle = control(window,L"STATIC",L"MapExtractor",0,80,20,520,30);
        SendMessageW(appTitle,WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
        languageSelector = control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,1194,24,158,240,105);
        for (const auto& language : i18n::languages()) SendMessageW(languageSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(language.name.c_str()));
        SendMessageW(languageSelector,CB_SETCURSEL,i18n::selected(),0);
        subtitle = control(window,L"STATIC",L"",0,80,52,720,22);
        clientLabel = control(window,L"STATIC",L"",0,44,122,570,22);
        input = control(window,L"COMBOBOX",L"",CBS_DROPDOWN|CBS_AUTOHSCROLL|WS_TABSTOP|WS_VSCROLL,44,158,488,320,109);
        SendMessageW(input,CB_LIMITTEXT,32767,0); SendMessageW(input,CB_SETDROPPEDWIDTH,640,0);
        inputButton = control(window,L"BUTTON",L"Choose",WS_TABSTOP|BS_OWNERDRAW,544,158,100,38,101);
        outputLabel = control(window,L"STATIC",L"",0,44,250,570,22);
        output = control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,56,296,464,22);
        outputButton = control(window,L"BUTTON",L"Change",WS_TABSTOP|BS_OWNERDRAW,544,286,100,38,102);
        taskLabel = control(window,L"STATIC",L"",0,44,378,570,22);
        taskSelector = control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,145,294,440,200,106);
        ShowWindow(taskSelector,SW_HIDE); // index 2 is scan, kept for --verify-ui
        taskTiles[0]=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,44,410,192,76,120);
        taskTiles[1]=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,248,410,192,76,121);
        taskTiles[3]=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,452,410,192,76,123);
        mapInput=control(window,L"EDIT",L"",0,0,0,0,0); ShowWindow(mapInput,SW_HIDE);
        serverLabel=control(window,L"STATIC",L"",0,400,22,184,22);
        serverSelector=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,588,16,340,200,116);
        combineButton=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,44,502,600,38,117);
        mapLabel=control(window,L"STATIC",L"",0,1052,126,280,24);
        mapFilterLabel=control(window,L"STATIC",L"",0,1052,160,280,20);
        mapFilter=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,1052,184,280,26,114);
        mapList=control(window,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_NOCOLUMNHEADER,1052,222,280,286,113);
        SetWindowTheme(mapList,L"DarkMode_Explorer",nullptr);
        ListView_SetExtendedListViewStyle(mapList,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(mapList,RGB(36,36,36)); ListView_SetTextBkColor(mapList,RGB(36,36,36)); ListView_SetTextColor(mapList,RGB(235,235,235));
        LVCOLUMNW column{}; column.mask=LVCF_WIDTH; column.cx=250; ListView_InsertColumn(mapList,0,&column);
        selectAllButton=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,1052,518,134,36,118);
        clearMapsButton=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,1198,518,134,36,119);
        preferNormalButton=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,1052,562,134,36,128);
        preferClassicButton=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,1198,562,134,36,129);
        mapCount=control(window,L"STATIC",L"",0,1052,628,280,22);
        capability = control(window,L"STATIC",L"",0,184,558,460,22);
        startButton = control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,24,620,208,46,103);
        openButton = control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,244,620,216,46,104);
        clearOutputButton=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,472,620,192,46,125);
        EnableWindow(openButton,FALSE);
        progressBar = control(window,L"911regProgress",L"",0,708,248,280,5);
        SetWindowTheme(progressBar,L"",L""); SendMessageW(progressBar,PBM_SETBARCOLOR,0,RGB(255,132,40));
        SendMessageW(progressBar,PBM_SETBKCOLOR,0,RGB(48,48,48));
        status = control(window,L"STATIC",L"",0,708,170,280,64);
        activityLabel = control(window,L"STATIC",L"",0,708,126,280,22);
        logView = control(window,L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY,708,276,280,368,112);
        SendMessageW(logView,WM_SETFONT,reinterpret_cast<WPARAM>(logFont),TRUE); SendMessageW(logView,EM_SETLIMITTEXT,262144,0);
        wchar_t executable[32768]{};
        if (GetModuleFileNameW(nullptr,executable,32768)) {
            const auto folder = std::filesystem::path(executable).parent_path() / L"Output";
            SetWindowTextW(output,folder.c_str());
        }
        EnableWindow(startButton,FALSE);
        updateLanguage();
        tips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(tips,CCM_SETUNICODEFORMAT,TRUE,0);
        SetWindowTheme(tips,L"",L"");
        SendMessageW(tips,TTM_SETTIPBKCOLOR,RGB(38,38,38),0);
        SendMessageW(tips,TTM_SETTIPTEXTCOLOR,RGB(235,235,235),0);
        SetWindowLongPtrW(tips,GWL_STYLE,GetWindowLongPtrW(tips,GWL_STYLE)&~WS_BORDER);
        SetWindowSubclass(tips,tooltipProc,1,0);
        SendMessageW(tips,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        SendMessageW(tips,TTM_SETMAXTIPWIDTH,0,310);
        SendMessageW(tips,TTM_SETDELAYTIME,TTDT_INITIAL,450);
        SendMessageW(tips,TTM_SETDELAYTIME,TTDT_AUTOPOP,16000);
        RECT tipMargin{14,10,14,10}; SendMessageW(tips,TTM_SETMARGIN,0,reinterpret_cast<LPARAM>(&tipMargin));
        for (const auto& entry:std::vector<std::pair<HWND,const char*>>{{input,"tip_client"},{inputButton,"tip_client"},{output,"tip_output"},{outputButton,"tip_output"},{mapList,"tip_list"},{mapFilter,"tip_search"},{serverSelector,"tip_server"},{preferClassicButton,"tip_prefer_classic"},{preferNormalButton,"tip_prefer_normal"},{combineButton,"tip_combine"},{languageSelector,"tip_language"},{startButton,"tip_start"},{openButton,"tip_results"},{clearOutputButton,"tip_clear"},{logView,"tip_log"},{progressBar,"tip_log"},{taskTiles[0],"tip_doors"},{taskTiles[1],"tip_water"},{taskTiles[3],"tip_geodata"}}) addTip(window,entry.first,entry.second);
        if (const auto choice=reinterpret_cast<Choice*>(GetWindowLongPtrW(input,GWLP_USERDATA)); choice && choice->edit) addTip(window,choice->edit,"tip_client");
        const auto recent = clientHistory->load();
        refreshClients(recent.empty() ? L"" : recent.front());

        ShowWindow(capability,SW_SHOW);

        populateMapList(true); placeTasks();
        return 0;
    }
    case WM_NOTIFYFORMAT: return NFR_UNICODE;
    case WM_NOTIFY: {
        const auto header=reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom==mapList && header->code==LVN_ITEMCHANGING && process) {
            const auto change=reinterpret_cast<NMLISTVIEW*>(lParam);
            if ((change->uOldState^change->uNewState)&LVIS_STATEIMAGEMASK) return TRUE;
        }
        if (header->hwndFrom==mapList && header->code==LVN_ITEMCHANGED && !updatingMaps && !process && !lockMapSelection) {
            const auto change=reinterpret_cast<NMLISTVIEW*>(lParam);
            if (change->iItem>=0 && ((change->uOldState^change->uNewState)&LVIS_STATEIMAGEMASK)) {
                const int image=int(ListView_GetItemState(mapList,change->iItem,LVIS_STATEIMAGEMASK)>>12);
                if (image<1) return 0;
                LVITEMW item{}; item.mask=LVIF_PARAM; item.iItem=change->iItem;
                if (ListView_GetItem(mapList,&item) && size_t(item.lParam)<availableMaps.size()) {
                    const auto& path=availableMaps[size_t(item.lParam)];
                    std::erase(selectedMaps,path);
                    if (image==2) selectedMaps.push_back(path);
                    std::sort(selectedMaps.begin(),selectedMaps.end()); updateMapCount();
                }
            }
            return 0;
        }
        if (header->hwndFrom==tips && header->code==TTN_GETDISPINFOW) {
            auto info=reinterpret_cast<NMTTDISPINFOW*>(lParam);
            const auto found=tipKeys.find(reinterpret_cast<HWND>(header->idFrom));
            tooltipText=found==tipKeys.end() ? L"" : i18n::tr(found->second); info->lpszText=tooltipText.data(); return 0;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        FillRect(item.hDC,&item.rcItem,item.CtlID==103 || item.CtlID==104 ? background : panel);
        rounded(item.hDC,item.rcItem,disabled ? RGB(40,40,40) : pressed ? RGB(68,48,34)
            : item.CtlID==103 ? RGB(255,132,40) : RGB(44,44,44),item.CtlID==103 ? RGB(255,132,40) : RGB(58,58,58),10);
        SetBkMode(item.hDC,TRANSPARENT);
        SetTextColor(item.hDC,disabled ? RGB(130,130,130) : item.CtlID == 103 ? RGB(24,24,24) : RGB(238,238,238));
        SelectObject(item.hDC,font);
        auto caption = text(item.hwndItem); RECT rectangle = item.rcItem;
        if (item.CtlID>=120 && item.CtlID<=123) {
            const unsigned index=item.CtlID-120; const bool selected=taskMode==index;
            rounded(item.hDC,rectangle,selected ? RGB(57,43,33) : RGB(40,40,40),selected ? RGB(255,132,40) : RGB(58,58,58),10);
            glyph(item.hDC,index==2 ? 3 : index==3 ? 2 : int(index),rectangle.left+14,rectangle.top+12,disabled ? RGB(100,100,100) : selected ? RGB(255,155,70) : RGB(160,160,160));
            rectangle.left+=14; rectangle.top+=48;
            SetTextColor(item.hDC,disabled ? RGB(100,100,100) : RGB(235,235,235));
            DrawTextW(item.hDC,caption.c_str(),-1,&rectangle,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS);
            keyboardFocus(item);
            return TRUE;
        }
        if (item.CtlID == 117) {
            const bool selected=combineResults;
            FillRect(item.hDC,&rectangle,panel);
            const int centerY=(rectangle.top+rectangle.bottom)/2;
            RECT track{rectangle.left,centerY-10,rectangle.left+38,centerY+10};
            rounded(item.hDC,track,selected && !disabled ? RGB(255,132,40) : RGB(68,68,68),RGB(68,68,68),20);
            const int knob=selected ? track.right-18 : track.left+2;
            RECT thumb{knob,track.top+2,knob+16,track.bottom-2}; rounded(item.hDC,thumb,RGB(232,232,232),RGB(232,232,232),16);
            SetTextColor(item.hDC,disabled ? RGB(100,100,100) : RGB(200,200,200)); rectangle.left=track.right+12;
            DrawTextW(item.hDC,caption.c_str(),-1,&rectangle,DT_LEFT|DT_VCENTER|DT_SINGLELINE); keyboardFocus(item); return TRUE;
        }
        if (item.CtlID == 101 || item.CtlID == 102 || item.CtlID == 104 || item.CtlID == 108) {
            const int x=rectangle.left+9,y=(rectangle.top+rectangle.bottom)/2;
            HPEN pen=CreatePen(PS_SOLID,1,disabled ? RGB(130,130,130) : RGB(255,170,80));
            auto oldPen=SelectObject(item.hDC,pen); auto oldBrush=SelectObject(item.hDC,GetStockObject(HOLLOW_BRUSH));
            POINT shape[]{{x,y+7},{x,y-7},{x+7,y-7},{x+10,y-4},{x+18,y-4},{x+18,y+7},{x,y+7}};
            Polyline(item.hDC,shape,7);
            SelectObject(item.hDC,oldPen); SelectObject(item.hDC,oldBrush); DeleteObject(pen); rectangle.left += 25;
        }
        DrawTextW(item.hDC,caption.c_str(),-1,&rectangle,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        keyboardFocus(item);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const HWND target=reinterpret_cast<HWND>(lParam);
        const bool header=target==appTitle || target==subtitle || target==serverLabel;
        const bool edit=(message==WM_CTLCOLOREDIT || target==output || target==mapInput || target==mapFilter) && target!=logView;
        SetTextColor(dc,target==serverLabel ? RGB(255,155,70) : target==appTitle || edit ? RGB(235,235,235) : target==status ? RGB(220,220,220) : RGB(160,160,160));
        SetBkColor(dc,edit ? RGB(40,40,40) : header ? RGB(32,32,32) : RGB(36,36,36));
        return reinterpret_cast<LRESULT>(edit ? field : header ? background : panel);
    }
    case WM_COMMAND:
        if (LOWORD(wParam)==125 && !process) {
            try {
                if (confirmClearOutput(window,std::filesystem::path(text(output)))) setText(status,"clear_ready");
            } catch (const std::exception&) { setText(status,"folder_error"); }
            return 0;
        }
        if (LOWORD(wParam)>=120 && LOWORD(wParam)<=123 && !process) {
            SendMessageW(taskSelector,CB_SETCURSEL,LOWORD(wParam)-120,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(106,CBN_SELCHANGE),reinterpret_cast<LPARAM>(taskSelector)); return 0;
        }
        if (LOWORD(wParam)==117 && !process) { combineResults=!combineResults; savePreferences(); InvalidateRect(combineButton,nullptr,TRUE); return 0; }
        if (LOWORD(wParam)==116 && HIWORD(wParam)==CBN_SELCHANGE) {
            const auto index=SendMessageW(serverSelector,CB_GETCURSEL,0,0);
            if (taskMode==3) {
                geoFormat=index==1 ? "l2j" : index==2 ? "l2off" : "both";
                savePreferences(); return 0;
            }
            if (index>=0 && size_t(index)<visibleServers.size()) selectedServer=visibleServers[size_t(index)];
            savePreferences(); EnableWindow(combineButton,taskMode<2 && selectedServer!="l2off"); return 0;
        }
        if ((LOWORD(wParam)==128 || LOWORD(wParam)==129) && !process) { extractor::preferMapVariant(selectedMaps,LOWORD(wParam)==129); populateMapList(false); return 0; }
        if ((LOWORD(wParam)==118 || LOWORD(wParam)==119) && !process) { selectedMaps=LOWORD(wParam)==118 ? availableMaps : std::vector<std::filesystem::path>{}; populateMapList(false); return 0; }
        if (LOWORD(wParam)==114 && HIWORD(wParam)==EN_CHANGE) { populateMapList(false); return 0; }
        if (HIWORD(wParam) == EN_CHANGE || (LOWORD(wParam) == 109 && HIWORD(wParam) == CBN_EDITCHANGE)) {
            if (LOWORD(wParam)==109 && !process && !lockMapSelection) populateMapList(true);
            EnableWindow(startButton,!process && sourceReady() && !text(output).empty());
            return 0;
        }
        if (LOWORD(wParam) == 109 && HIWORD(wParam) == CBN_SELCHANGE) {
            const auto index = SendMessageW(input,CB_GETCURSEL,0,0);
            if (index != CB_ERR && !lockMapSelection) {
                const auto length = SendMessageW(input,CB_GETLBTEXTLEN,index,0);
                std::wstring value(size_t(length)+1,L'\0');
                SendMessageW(input,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(value.data()));
                SetWindowTextW(input,value.c_str()); populateMapList(true);
                EnableWindow(startButton,!process && !text(output).empty() && sourceReady());
            }
            return 0;
        }
        if (LOWORD(wParam) == 105 && HIWORD(wParam) == CBN_SELCHANGE && !process) {
            i18n::select(size_t(SendMessageW(languageSelector,CB_GETCURSEL,0,0))); updateLanguage(); return 0;
        }
        if (LOWORD(wParam) == 106 && HIWORD(wParam) == CBN_SELCHANGE && !process) {
            const auto selected = SendMessageW(taskSelector,CB_GETCURSEL,0,0);
            if (selected >= 0 && selected <= 3) taskMode = unsigned(selected);
            for (auto tile:taskTiles) InvalidateRect(tile,nullptr,TRUE);
            EnableWindow(serverSelector,TRUE); EnableWindow(combineButton,taskMode<2 && selectedServer!="l2off");
            if (serverLabel) setText(serverLabel,taskMode==3 ? "geo_format" : "server_type");
            populateServers();
            ShowWindow(capability,SW_SHOW); InvalidateRect(window,nullptr,TRUE);
            setText(capability,"variants_hint");
            EnableWindow(startButton,sourceReady() && !text(output).empty());
            return 0;
        }
        if (LOWORD(wParam) == 101) {
            chooseFolder(window,input,i18n::tr("choose_client_title").c_str());
            if (!text(input).empty()) rememberClient(text(input));
        } else if (LOWORD(wParam) == 102) {
            chooseFolder(window,output,i18n::tr("choose_output_title").c_str());
        } else if (LOWORD(wParam) == 104 && !runFolder.empty()) {
            ShellExecuteW(window,L"open",runFolder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        } else if (LOWORD(wParam) == 103 && !process) {
            if (selectedMapMode && selectedMaps.empty()) { setText(status,"no_maps"); return 0; }
            const auto chosenMaps=selectedMaps;
            lockMapSelection=true;
            auto source = singleMap ? text(mapInput) : text(input);
            try {
                auto folder = std::filesystem::path(source);
                const auto map = singleMap ? folder : std::filesystem::path{};
                if (singleMap) folder = folder.parent_path();
                else if (std::filesystem::is_directory(folder / L"Maps")) folder /= L"Maps";
                else if (folder.filename() == L"System" && std::filesystem::is_directory(folder.parent_path() / L"Maps"))
                    folder = folder.parent_path() / L"Maps";
                bool hasMap = false;
                if (singleMap) hasMap = std::filesystem::is_regular_file(map) && _wcsicmp(map.extension().c_str(),L".unr") == 0;
                else if (std::filesystem::is_directory(folder)) {
                    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                        if (entry.is_regular_file() && _wcsicmp(entry.path().extension().c_str(),L".unr") == 0) { hasMap = true; break; }
                    }
                }
                if (!hasMap) {
                    lockMapSelection=false; setText(status,"no_maps"); return 0;
                }
                source = singleMap ? map.wstring() : folder.wstring();
                rememberClient(folder);
                const auto destination=std::filesystem::weakly_canonical(std::filesystem::absolute(text(output)));
                if (!confirmClearOutput(window,destination)) { lockMapSelection=false; return 0; }
                const auto candidate=destination;
                std::filesystem::create_directories(candidate);
                if (selectedMapMode) {
                    selectionPath=candidate/L"SelectedMaps.txt";
                    if (!debugResults) {
                        wchar_t temporary[32768]{},filename[32768]{};
                        if (!GetTempPathW(32768,temporary) || !GetTempFileNameW(temporary,L"mxe",0,filename)) throw std::runtime_error("Cannot prepare selected maps");
                        selectionPath=filename;
                    }
                    std::ofstream selection(selectionPath,std::ios::binary);
                    for (const auto& selected : chosenMaps) selection << extractor::output::utf8(selected) << "\n";
                    selection.close(); if (!selection) throw std::runtime_error("Cannot save selected maps");
                }
                runFolder = candidate; manifest = extractor::taskOutput(runFolder,taskMode).wstring();
            } catch (const std::exception&) {
                lockMapSelection=false; setText(status,"folder_error"); return 0;
            }
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr,executable,32768);
            auto command = quote(executable)+(taskMode == 0 ? L" doors " : taskMode == 1 ? L" water " : taskMode==3 ? L" geodata " : L" scan ")+quote(source)+L" --output "+quote(manifest);
            command+=L" --all-variants";
            if (combineResults && taskMode<2 && selectedServer!="l2off") command+=L" --combine-xml";
            command+=L" --server "+quote(std::wstring(selectedServer.begin(),selectedServer.end()));
            if (!debugResults) command+=L" --simple-output";
            if (taskMode==3) command+=L" --geo "+std::wstring(geoFormat.begin(),geoFormat.end());
            if (selectedMapMode) command+=L" --map-list "+quote(selectionPath.wstring());
            STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
            HANDLE writer = nullptr;
            if (!CreatePipe(&scanPipe,&writer,&security,0)) {
                lockMapSelection=false; setText(status,"progress_error"); return 0;
            }
            SetHandleInformation(scanPipe,HANDLE_FLAG_INHERIT,0);
            HANDLE nullInput = CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = writer; startup.hStdError = writer; startup.hStdInput = nullInput;
            // Extraction runs in a separate CPU process so the UI remains responsive.
            const BOOL launched = CreateProcessW(executable,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child);
            CloseHandle(writer); if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
            if (!launched) {
                CloseHandle(scanPipe); scanPipe = nullptr;
                if (!debugResults && !selectionPath.empty()) { std::error_code error; std::filesystem::remove(selectionPath,error); }
                lockMapSelection=false; setText(status,"launch_error"); return 0;
            }
            scanLog.clear(); displayedLog=0; SetWindowTextW(logView,L"");
            if (debugResults) jobLog.open(runFolder/L"911reg.log",std::ios::binary); SendMessageW(progressBar,PBM_SETRANGE32,0,100); SendMessageW(progressBar,PBM_SETPOS,0,0);
            CloseHandle(child.hThread); process = child.hProcess; lockMapSelection=false;
            EnableWindow(startButton,FALSE);
            EnableWindow(input,FALSE); EnableWindow(output,FALSE);
            EnableWindow(inputButton,FALSE); EnableWindow(outputButton,FALSE); EnableWindow(openButton,FALSE); EnableWindow(clearOutputButton,FALSE);
            EnableWindow(taskSelector,FALSE); EnableWindow(languageSelector,FALSE);
            for (auto tile:taskTiles) EnableWindow(tile,FALSE);
            EnableWindow(mapFilter,FALSE); EnableWindow(serverSelector,FALSE); EnableWindow(combineButton,FALSE); EnableWindow(preferNormalButton,FALSE); EnableWindow(preferClassicButton,FALSE); EnableWindow(selectAllButton,FALSE); EnableWindow(clearMapsButton,FALSE);

            setText(status,"finding");
            SetTimer(window,1,250,nullptr);
        }
        return 0;
    case WM_TIMER:
        readProgress();
        if (process && WaitForSingleObject(process,0) == WAIT_OBJECT_0) {
            DWORD code = 1; GetExitCodeProcess(process,&code);
            CloseHandle(process); process = nullptr; KillTimer(window,1);
            readProgress(); CloseHandle(scanPipe); scanPipe = nullptr;
            jobLog.close(); SendMessageW(progressBar,PBM_SETRANGE32,0,100); SendMessageW(progressBar,PBM_SETPOS,code==0 ? 100 : 0,0);
            EnableWindow(startButton,TRUE);
            EnableWindow(input,TRUE); EnableWindow(output,TRUE);
            EnableWindow(inputButton,TRUE); EnableWindow(outputButton,TRUE); EnableWindow(openButton,TRUE); EnableWindow(clearOutputButton,TRUE);
            EnableWindow(taskSelector,TRUE); EnableWindow(languageSelector,TRUE);
            for (auto tile:taskTiles) EnableWindow(tile,TRUE);
            EnableWindow(mapList,TRUE); EnableWindow(mapFilter,TRUE); EnableWindow(serverSelector,TRUE); EnableWindow(combineButton,taskMode<2 && selectedServer!="l2off"); EnableWindow(preferNormalButton,TRUE); EnableWindow(preferClassicButton,TRUE); EnableWindow(selectAllButton,TRUE); EnableWindow(clearMapsButton,TRUE);
            if (debugResults) {
                std::ofstream summary(runFolder/"Summary.txt",std::ios::binary); summary<<scanLog;
            } else if (!selectionPath.empty()) { std::error_code error; std::filesystem::remove(selectionPath,error); }
            const auto summary=completionText(code);
            SetWindowTextW(status,summary.first.c_str()); SetWindowTextW(logView,summary.second.c_str());
            if (!verifying && code!=1 && std::filesystem::exists(manifest) && MessageBoxW(window,i18n::tr("open_output_prompt").c_str(),L"MapExtractor",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES) ShellExecuteW(window,L"open",runFolder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            if (verifying) {
                if (verificationSimple) {
                    previewPath=runFolder.parent_path()/L"completion-preview.bmp"; renderPreview(window); previewPath.clear();
                    if (code==2 && text(logView).find(i18n::tr("skipped_reasons"))==std::wstring::npos) throw std::runtime_error("Skipped reasons are not visible in Activity");
                }
                verificationResult = (code == 0 || (verificationSimple && code==2)) && std::filesystem::exists(manifest) ? 0 : 1;
                PostMessageW(window,WM_CLOSE,0,0);
            }
        }
        return 0;
    case WM_CLOSE:
        if (process) { setText(status,"running"); return 0; }
        DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
}

int showUi() {
    i18n::load(); extractor::allVariants=true;
    wchar_t settingsExecutable[32768]{}; GetModuleFileNameW(nullptr,settingsExecutable,32768);
    preferencesPath=extractor::settingsFile(std::filesystem::path(settingsExecutable).parent_path());
    const auto preferences=extractor::UiPreferences::load(preferencesPath);
    selectedServer=preferences.server; combineResults=preferences.combine; debugResults=false; geoFormat=preferences.geodata;
    // The window never writes debug files. The verify path still does, unless the simple-output check is running.
    if (verifying && previewPath.empty() && !verificationSimple) { selectedServer="all"; combineResults=false; debugResults=true; }
    if (verificationSimple) { selectedServer="l2jserver"; combineResults=true; debugResults=false; }
    clientHistory = std::make_unique<extractor::ClientHistory>(preferencesPath);
    SetProcessDPIAware();
    const auto initialized = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS|ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    background = CreateSolidBrush(RGB(32,32,32)); field = CreateSolidBrush(RGB(40,40,40)); panel=CreateSolidBrush(RGB(36,36,36));
    font = CreateFontW(-15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    titleFont = CreateFontW(-24,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    logFont = CreateFontW(-12,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
    WNDCLASSW choice{}; choice.lpfnWndProc=choiceProc; choice.hInstance=GetModuleHandleW(nullptr); choice.lpszClassName=L"911regChoice"; choice.hCursor=LoadCursorW(nullptr,IDC_HAND); RegisterClassW(&choice);
    WNDCLASSW progress{}; progress.lpfnWndProc=progressProc; progress.hInstance=GetModuleHandleW(nullptr); progress.lpszClassName=L"911regProgress"; RegisterClassW(&progress);
    WNDCLASSW cls{}; cls.lpfnWndProc = windowProc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"911regMapExtractor"; cls.hbrBackground = background;
    cls.hIcon = LoadIconW(cls.hInstance,MAKEINTRESOURCEW(101));
    cls.hCursor = LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&cls);
    HWND window = CreateWindowW(cls.lpszClassName,L"911reg MapExtractor",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,1392,730,nullptr,nullptr,cls.hInstance,nullptr);
    if (!window) return 1;
    SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
    if (!previewPath.empty()) {
        SetWindowPos(window,HWND_BOTTOM,-32000,-32000,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
        ShowWindow(window,SW_SHOWNOACTIVATE); UpdateWindow(window);
        renderPreview(window); DestroyWindow(window);
    } else if (verifying) {
        if (size_t(SendMessageW(tips,TTM_GETTOOLCOUNT,0,0))!=tipKeys.size()) throw std::runtime_error("Tooltip registration failed");
        const auto recent = clientHistory->load();
        if (size_t(SendMessageW(input,CB_GETCOUNT,0,0)) != recent.size()) throw std::runtime_error("Recent-client dropdown count mismatch");
        if (!recent.empty()) {
            const auto index = recent.size()-1;
            SendMessageW(input,CB_SETCURSEL,index,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(109,CBN_SELCHANGE),reinterpret_cast<LPARAM>(input));
            if (text(input) != recent[index]) throw std::runtime_error("Recent-client selection failed");
        }
        SetWindowTextW(input,verificationInput.c_str());
        SetWindowTextW(mapInput,verificationInput.c_str());
        SetWindowTextW(output,verificationOutput.c_str());
        if (SendMessageW(languageSelector,CB_GETCOUNT,0,0)!=LRESULT(i18n::languages().size())) throw std::runtime_error("Language selector items missing");
        SendMessageW(window,WM_COMMAND,120+taskMode,0);
        SendMessageW(window,WM_COMMAND,103,0);
        if (!process) DestroyWindow(window);
    } else ShowWindow(window,SW_SHOW);
    MSG message{};
    while (GetMessageW(&message,nullptr,0,0) > 0) {
        focusInput(window,message);
        if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    DeleteObject(font); DeleteObject(titleFont); DeleteObject(logFont); DeleteObject(background); DeleteObject(field); DeleteObject(panel);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return verifying ? verificationResult : int(message.wParam);
}

int verifyCompletion() {
    i18n::load();
    scanLog="SKIPPED: 20_22.unr / Mover8: Missing door ID\nRESULT doors 303 2 0 0 0\n";
    const auto skipped=completionText(2);
    if (skipped.first!=i18n::format("result_doors",303,2) || skipped.second.find(L"Mover8: Missing door ID")==std::wstring::npos) throw std::runtime_error("Skipped-item reason is not visible");
    scanLog="Duplicate door IDs mean that multiple doors share an ID.\nDuplicate door IDs:\n19_26_Classic: 1 duplicate door ID. ID: 19260003.\nRESULT doors 303 0 0 0 1\n";
    const auto duplicate=completionText(0);
    if (duplicate.first!=i18n::format("result_doors",303,1) || duplicate.second.find(L"19260003")==std::wstring::npos) throw std::runtime_error("Duplicate warning was lost");
    scanLog="ERROR: Output directory is not writable\n";
    if (completionText(1).second.find(L"not writable")==std::wstring::npos) throw std::runtime_error("Failure reason was lost");
    return 0;
}

int verifyUi(const wchar_t* source, const wchar_t* destination, bool doors, bool water, bool geodata, bool simple) {
    verifying = true; verificationSimple=simple;
    taskMode = geodata ? 3 : water ? 1 : doors ? 0 : 2;
    selectedMapMode=false;
    singleMap = std::filesystem::is_regular_file(source);
    verificationInput = source; verificationOutput = destination;
    return showUi();
}

int previewUi(const wchar_t* destination) {
    previewPath = destination; verifying = true; verificationResult = 0;
    return showUi();
}
