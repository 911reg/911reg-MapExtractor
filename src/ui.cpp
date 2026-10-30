#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <cstdio>
#include "localization.h"
#include "history.h"
#include <memory>
#include <commctrl.h>
#include <uxtheme.h>
#include <vector>
#include <algorithm>
#include <array>
#include <cmath>
#include <thread>
#include <richedit.h>
#include <tuple>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "map_selection.h"
#include "output_tasks.h"
#include "ui_preferences.h"
#include "settings_file.h"
#include "cpu_workers.h"

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
HWND taskTiles[5]{};
size_t displayedLog = 0;
std::ofstream jobLog;
HANDLE process = nullptr;
HANDLE scanPipe = nullptr;
HFONT font, titleFont;
HFONT logFont;
HBRUSH background, field, panel, headerBrush, raisedBrush;
std::wstring manifest;
std::filesystem::path runFolder;
std::string scanLog;
bool verifying = false;
bool runningProofCaptured=false;
bool recoveredRun=false;
HANDLE extractionJob=nullptr;
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

#include "ui_theme.h"
void rounded(HDC dc,RECT r,COLORREF color,COLORREF border,int radius=12) { roundRect(dc,r,dpf(float(radius)),color,border); }
LRESULT CALLBACK tooltipProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR) {
    if (message==WM_ERASEBKGND || message==WM_NCPAINT) return 1;
    if (message==WM_WINDOWPOSCHANGED) {
        const auto result=DefSubclassProc(window,message,wParam,lParam);
        const auto position=reinterpret_cast<WINDOWPOS*>(lParam);
        if (!(position->flags&SWP_NOSIZE)) {
            SetWindowRgn(window,nullptr,TRUE);
        }
        return result;
    }
    if (message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{}; auto dc=message==WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wParam);
        RECT bounds{}; GetClientRect(window,&bounds); FillRect(dc,&bounds,panel);
        roundRect(dc,bounds,dpf(6),theme::raised,theme::border);
        roundRect(dc,{bounds.left+dp(4),bounds.top+dp(9),bounds.left+dp(7),bounds.bottom-dp(9)},dpf(1.5f),theme::accent);
        InflateRect(&bounds,-14,-10); auto previous=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,theme::text);
        DrawTextW(dc,tooltipText.c_str(),-1,&bounds,DT_WORDBREAK|DT_NOPREFIX); SelectObject(dc,previous);
        if (message==WM_PAINT) EndPaint(window,&paint); return 0;
    }
    return DefSubclassProc(window,message,wParam,lParam);
}
void keyboardFocus(const DRAWITEMSTRUCT& item) {
    if (!(item.itemState&ODS_FOCUS) || (item.itemState&ODS_NOFOCUSRECT) || (SendMessageW(GetParent(item.hwndItem),WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)) return;
    auto bounds=item.rcItem; InflateRect(&bounds,-dp(3),-dp(3));
    roundRect(item.hDC,bounds,dpf(7),theme::none,item.CtlID==103 ? theme::base : theme::accentHot,1.5f);
}
void focusInput(HWND window,const MSG& message) {
    if (message.message==WM_LBUTTONDOWN || message.message==WM_RBUTTONDOWN) SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
    else if (message.message==WM_KEYDOWN && (message.wParam==VK_TAB || message.wParam==VK_UP || message.wParam==VK_DOWN || message.wParam==VK_LEFT || message.wParam==VK_RIGHT)) SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0);
}
struct Choice { std::vector<std::wstring> items; int selected=-1; HWND edit=nullptr; bool hovered=false; };
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
    if (message==PBM_SETRANGE32) { if(data->low!=int(wParam) || data->high!=int(lParam)) { data->low=int(wParam); data->high=int(lParam); InvalidateRect(window,nullptr,FALSE); } return 0; }
    if (message==PBM_SETPOS) { if(data->position!=int(wParam)) { data->position=int(wParam); InvalidateRect(window,nullptr,FALSE); } return 0; }
    if (message==PBM_SETBARCOLOR || message==PBM_SETBKCOLOR || message==WM_ERASEBKGND) return 1;
    if (message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{}; const auto dc=message==WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wParam);
        RECT r{}; GetClientRect(window,&r); FillRect(dc,&r,field); roundRect(dc,r,float(r.bottom)/2,theme::line);
        if (data->high>data->low && data->position>data->low) {
            r.right=std::max(r.bottom,std::clamp((r.right*(data->position-data->low))/(data->high-data->low),0L,r.right));
            roundGradient(dc,r,float(r.bottom)/2,theme::accentDeep,theme::accentHot,theme::none,true);
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
            data->edit=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,dp(36),9,r.right-dp(72),22,window,reinterpret_cast<HMENU>(1),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(data->edit,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        }
        return 0;
    }
    case WM_SIZE: if(data && data->edit){RECT r{};GetClientRect(window,&r);MoveWindow(data->edit,dp(36),(r.bottom-dp(20))/2,r.right-dp(72),dp(20),TRUE);}return 0;
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
    case WM_COMMAND:
        if (HIWORD(wParam)==EN_CHANGE) notify(CBN_EDITCHANGE);
        if (HIWORD(wParam)==EN_SETFOCUS || HIWORD(wParam)==EN_KILLFOCUS) InvalidateRect(window,nullptr,FALSE);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | ((wParam==VK_RETURN || wParam==VK_SPACE) ? DLGC_WANTALLKEYS : 0);
    case WM_SETFOCUS: if (data->edit) SetFocus(data->edit); InvalidateRect(window,nullptr,TRUE); return 0;
    case WM_KILLFOCUS: InvalidateRect(window,nullptr,TRUE); return 0;
    case WM_ENABLE: if (data->edit) EnableWindow(data->edit,BOOL(wParam)); InvalidateRect(window,nullptr,TRUE); return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: SetTextColor(reinterpret_cast<HDC>(wParam),IsWindowEnabled(window) ? theme::text : theme::faint); SetBkColor(reinterpret_cast<HDC>(wParam),theme::inset); return reinterpret_cast<LRESULT>(field);
    case WM_MOUSEMOVE: if (!data->hovered) { data->hovered=true; TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,window,0}; TrackMouseEvent(&event); InvalidateRect(window,nullptr,FALSE); } return 0;
    case WM_MOUSELEAVE: data->hovered=false; InvalidateRect(window,nullptr,FALSE); return 0;
    case WM_MEASUREITEM: {
        auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(lParam); RECT r{}; GetClientRect(window,&r); item->itemWidth=std::max(LONG(dp(180)),r.right-dp(12)); item->itemHeight=dp(32); return TRUE;
    }
    case WM_DRAWITEM: {
        auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        const auto& label=*reinterpret_cast<const std::wstring*>(item.itemData);
        const bool current=data->selected>=0 && size_t(data->selected)<data->items.size() && &data->items[size_t(data->selected)]==&label;
        FillRect(item.hDC,&item.rcItem,raisedBrush);
        RECT band=item.rcItem; InflateRect(&band,-dp(4),-dp(2));
        if (item.itemState&ODS_SELECTED) roundRect(item.hDC,band,dpf(6),theme::accentTint,mix(theme::accent,theme::raised,0.5f));
        SelectObject(item.hDC,font); SetBkMode(item.hDC,TRANSPARENT); SetTextColor(item.hDC,current ? theme::accentHot : theme::text);
        auto r=item.rcItem; r.left+=dp(14); r.right-=dp(30);
        DrawTextW(item.hDC,label.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        if (current) icon(item.hDC,Icon::check,{item.rcItem.right-dp(26),(item.rcItem.top+item.rcItem.bottom)/2-dp(7),item.rcItem.right-dp(12),(item.rcItem.top+item.rcItem.bottom)/2+dp(7)},theme::accentHot,2.2f);
        return TRUE;
    }
    case WM_KEYDOWN:
        if (data->items.empty()) return 0;
        if (wParam==VK_UP || wParam==VK_DOWN) { if (select(std::clamp(data->selected+(wParam==VK_UP ? -1 : 1),0,int(data->items.size())-1))) notify(CBN_SELCHANGE); return 0; }
        if (wParam!=VK_SPACE && wParam!=VK_RETURN && wParam!=VK_F4) break;
        [[fallthrough]];
    case WM_LBUTTONDOWN: {
        if (data->items.empty()) return 0;
        SetFocus(window); HMENU menu=CreatePopupMenu();
        MENUINFO look{sizeof(look)}; look.fMask=MIM_BACKGROUND; look.hbrBack=raisedBrush; SetMenuInfo(menu,&look);
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
        const int id=GetDlgCtrlID(window); const bool enabled=IsWindowEnabled(window)!=FALSE;
        const bool focused=GetFocus()==window || (data->edit && GetFocus()==data->edit);
        FillRect(dc,&r,id==105 || id==116 ? headerBrush : panel);
        const COLORREF edge=focused ? theme::accent : id==116 && enabled ? mix(theme::accent,theme::border,0.45f) : data->hovered && enabled ? theme::muted : theme::border;
        roundRect(dc,r,dpf(8),!enabled ? theme::surface : data->hovered && !data->edit ? theme::raised : theme::inset,edge);
        SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,enabled ? theme::text : theme::faint);
        auto label=r; label.left+=dp(12); label.right-=dp(32);
        // Server shows a live dot, language a globe, as small leading markers.
        if (id==116) { fillCircle(dc,float(label.left)+dpf(4),float(r.bottom)/2,dpf(4),enabled ? theme::green : theme::faint); label.left+=dp(16); }
        if (id==105) { icon(dc,Icon::globe,{label.left,(r.bottom-dp(16))/2,label.left+dp(16),(r.bottom+dp(16))/2},enabled ? theme::muted : theme::faint,1.7f); label.left+=dp(24); }
        if (data->edit) icon(dc,Icon::folder,{r.left+dp(11),(r.bottom-dp(16))/2,r.left+dp(27),(r.bottom+dp(16))/2},enabled ? theme::accentHot : theme::faint,1.8f);
        else DrawTextW(dc,text(window).c_str(),-1,&label,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        icon(dc,Icon::chevron,{r.right-dp(26),(r.bottom-dp(14))/2,r.right-dp(12),(r.bottom+dp(14))/2},enabled ? (focused ? theme::accentHot : theme::muted) : theme::faint,2);
        if (message==WM_PAINT) EndPaint(window,&paint); return 0;
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
bool pathnodesEnabled=true;
bool updatingMaps=false,combineResults=true,debugResults=false,verificationSimple=false,lockMapSelection=false;
std::string selectedServer="l2jserver",geoFormat="l2j";
std::vector<std::string> visibleServers;
std::filesystem::path preferencesPath,selectionPath;
void renderPreview(HWND window);
void updateLanguage();
std::wstring wide(const std::string& value);
#include "ui_studio.h"
void savePreferences() {
    if (verifying) return;
    if (!extractor::UiPreferences{selectedServer,combineResults,debugResults,geoFormat,pathnodesEnabled}.save(preferencesPath)) SetWindowTextW(status,i18n::tr("settings_error").c_str());
}
void populateServers() {
    visibleServers.clear(); SendMessageW(serverSelector,CB_RESETCONTENT,0,0);
    if (taskMode==4) {
        SendMessageW(serverSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Legacy PN (.pn)"));
        SendMessageW(serverSelector,CB_SETCURSEL,0,0); EnableWindow(serverSelector,FALSE); return;
    }
    if (taskMode>=3) {
        const std::array<std::pair<const char*,const char*>,3> choices{{{"l2j","geo_l2j"},{"both","geo_both"},{"l2off","geo_conv"}}};
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
    const unsigned shown[]{0,1,3,4};
    for (unsigned position=0;position<4;++position) SetWindowPos(taskTiles[shown[position]],nullptr,44+int(position)*152,410,144,76,SWP_NOZORDER|SWP_NOACTIVATE);
    populateServers();
    studioLayout(GetParent(startButton));
}
void updateMapCount() {
    SetWindowTextW(mapCount,i18n::format("maps_count",selectedMaps.size(),availableMaps.size()).c_str());
    EnableWindow(startButton,!process && sourceReady() && !text(output).empty());
    if(activityCanvas) InvalidateRect(activityCanvas,nullptr,FALSE);
    if(const auto parent=GetParent(mapCount)) InvalidateRect(parent,&mapsTitle,FALSE);
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
            auto variant=extractor::classicMap(availableMaps[i])?std::wstring(L"Classic"):i18n::tr("studio_normal");
            ListView_SetItemText(mapList,row,1,variant.data());
            std::error_code sizeError;const auto bytes=std::filesystem::file_size(availableMaps[i],sizeError);wchar_t sizeLabel[40]{};
            if(!sizeError)swprintf_s(sizeLabel,L"%.1f MB",double(bytes)/(1024*1024));else wcscpy_s(sizeLabel,L"—");
            ListView_SetItemText(mapList,row,2,sizeLabel);
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
    SetWindowTextW(minimizeButton,i18n::tr("studio_minimize").c_str());SetWindowTextW(closeButton,i18n::tr("studio_close").c_str());
    setText(configurationHeading,"studio_configuration");setText(clearLogButton,"studio_clear_log");setText(saveLogButton,"studio_save_log");setText(copyLogButton,"studio_copy_log");
    setText(subtitle,"subtitle"); setText(clientLabel,"client_label"); setText(outputLabel,"output_label");
    SetWindowTextW(inputButton,L"…"); SetWindowTextW(outputButton,L"…"); setText(taskLabel,"task_label");
    setText(capability,taskMode==4 ? "pn_hint" : "variants_hint"); setText(startButton,"start"); setText(openButton,"open_results");
    setText(clearOutputButton,"clear_results");
    setText(activityLabel,"studio_console");
    const std::array<std::string,5> tileKeys{"tile_doors","tile_water","", "tile_geo","tile_pathnodes"};
    for (unsigned i=0;i<5;++i) if (taskTiles[i]) { setText(taskTiles[i],tileKeys[i]); InvalidateRect(taskTiles[i],nullptr,TRUE); }
    SendMessageW(taskSelector,CB_RESETCONTENT,0,0);
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("task_doors").c_str()));
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("task_water").c_str()));
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("task_scan").c_str()));
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("task_geodata").c_str()));
    SendMessageW(taskSelector,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(i18n::tr("tile_pathnodes").c_str()));
    SendMessageW(taskSelector,CB_SETCURSEL,taskMode,0);
    if (mapLabel) {
        setText(preferClassicButton,"maps_prefer_classic"); setText(preferNormalButton,"maps_prefer_normal"); setText(mapLabel,"maps_title"); setText(selectAllButton,"maps_all"); setText(clearMapsButton,"maps_none"); setText(combineButton,taskMode==3 ? "include_pathnodes" : "combine_xml");
        ShowWindow(combineButton,taskMode==4 ? SW_HIDE : SW_SHOW); tipKeys[combineButton]=taskMode==3 ? "tip_pathnodes" : "tip_combine";
        setText(mapFilterLabel,"maps_search");
        SendMessageW(mapFilter,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(i18n::tr("maps_search").c_str()));
        setText(serverLabel,taskMode==4 ? "node_format" : taskMode==3 ? "geo_format" : "server_type");
        populateServers();
        updateMapCount();
    }
    setText(status,"ready");
    refreshDetails(true);
    if (const auto parent=GetParent(status)) InvalidateRect(parent,nullptr,TRUE);
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
    std::wstring reasons,duplicateText,warnings;
    std::set<std::string> seenWarnings;
    std::istringstream lines(scanLog); std::string line;
    while (std::getline(lines,line)) {
        if (line.starts_with("RESULT ")) {
            std::istringstream values(line.substr(7)); values>>kind>>exported>>skipped>>maps>>extra>>duplicates; found=bool(values); duplicateBlock=false;
        } else if (line.starts_with("SKIPPED: ")) reasons+=wide(line.substr(9))+L"\r\n\r\n";
        else if (line.starts_with("ERROR: ")) reasons+=wide(line.substr(7))+L"\r\n\r\n";
        else if (line.starts_with("WARNING: ") && seenWarnings.insert(line).second) warnings+=wide(line.substr(9))+L"\r\n\r\n";
        else if (line.starts_with("Duplicate door IDs mean")) { duplicateBlock=true; duplicateText+=wide(line)+L"\r\n"; }
        else if (duplicateBlock) duplicateText+=wide(line)+L"\r\n";
    }
    const auto key=kind=="doors" ? "result_doors" : kind=="water" ? "result_water" : kind=="geodata" ? "result_geodata" : kind=="pathnodes" ? "result_pathnodes" : "result_scan";
    const auto title=found ? i18n::format(key,exported,skipped+maps+extra) : i18n::tr(code ? "failed" : "done");
    auto details=title+L"\r\n\r\n";
    if (!reasons.empty()) details+=i18n::tr("skipped_reasons")+L"\r\n\r\n"+reasons;
    if (!warnings.empty()) details+=i18n::tr("result_warnings")+L"\r\n\r\n"+warnings;
    if (!duplicateText.empty()) details+=duplicateText;
    if (reasons.empty() && warnings.empty() && duplicateText.empty() && code==0) details+=i18n::tr("result_ready");
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
        auto line = scanLog.substr(displayedLog,end-displayedLog); displayedLog = end+1;
        if(!line.empty() && line.back()=='\r')line.pop_back();
        struct Severity { const wchar_t* tag; COLORREF tint,text; };
        const auto severity=line.starts_with("SKIPPED:") ? Severity{L"SKIP",theme::violet,RGB(221,190,255)}
            : line.starts_with("WARNING:") ? Severity{L"WARN",theme::amber,RGB(253,224,150)}
            : line.starts_with("ERROR") ? Severity{L"FAIL",theme::red,RGB(254,190,190)}
            : line.starts_with("MAP ") ? Severity{L"SCAN",theme::accent,theme::text}
            : line.starts_with("RESULT ") ? Severity{L"OK",theme::green,RGB(167,243,208)}
            : line.starts_with("STAGE ") ? Severity{L"STEP",theme::violet,theme::body}
            : line.starts_with("Checked ") ? Severity{L"PROG",theme::cyan,theme::body}
            : Severity{L"INFO",theme::blue,theme::body};
        consoleLine(severity.tag,severity.tint,wide(line),severity.text);
        if(runCounters.consume(line,taskMode))InvalidateRect(countersCanvas,nullptr,FALSE);
        if(line.starts_with("MAP "))activityStage=1;
        if(line.starts_with("STAGE "))activityStage=line.find("serialize")!=std::string::npos || line.find("pathnodes")!=std::string::npos?3:2;
        if(line.starts_with("RESULT "))activityStage=4;
        if(line.starts_with("MAP ") && line.find("neighbouring")==std::string::npos) {
            const auto endMap=line.find(" | ");currentMap=wide(line.substr(4,endMap==std::string::npos?std::string::npos:endMap-4));
        }
        if (jobLog.is_open()) { jobLog << line << '\n'; jobLog.flush(); }
        if (line.starts_with("MAP ") || line.starts_with("STAGE ") || line.starts_with("Geodata complete:")) SetWindowTextW(status,wide(line).c_str());
        if (taskMode>=3 && line.starts_with("STAGE ")) {
            size_t block=0,blocks=0;
            int stage=line.starts_with("STAGE spatial") ? 10 : line.starts_with("STAGE rasterize") ? 10 : line.starts_with("STAGE movement") ? 35 : line.starts_with("STAGE diagonal") ? 70 : 90;
            const auto at=line.find("| blocks ");
            if (at!=std::string::npos && sscanf_s(line.c_str()+at,"| blocks %zu/%zu",&block,&blocks)==2 && blocks)
                stage+=int((line.starts_with("STAGE rasterize") ? 25 : line.starts_with("STAGE movement") ? 35 : 20)*std::min(block,blocks)/blocks);
            // Geodata stages advance within a map; the bar covers all selected maps.
            const size_t maps=std::max<size_t>(1,totalMaps);
            SendMessageW(progressBar,PBM_SETRANGE32,0,LPARAM(maps*100));
            SendMessageW(progressBar,PBM_SETPOS,WPARAM(std::min(completedMaps,maps)*100+size_t(stage)),0);
        }
        size_t done=0,total=0;
        if (sscanf_s(line.c_str(),"Checked %zu of %zu maps",&done,&total)==2) {
            completedMaps=done;totalMaps=total;
            if (taskMode>=3) { SendMessageW(progressBar,PBM_SETRANGE32,0,LPARAM(std::max<size_t>(1,total)*100)); SendMessageW(progressBar,PBM_SETPOS,WPARAM(done*100),0); }
            else { SendMessageW(progressBar,PBM_SETRANGE32,0,total); SendMessageW(progressBar,PBM_SETPOS,done,0); }
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
    if(wcscmp(type,L"BUTTON")==0)SetWindowSubclass(window,hoverProc,2,0);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return window;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PRINTCLIENT:
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc=message==WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wParam);
        RECT bounds{};GetClientRect(window,&bounds);
        { Buffered buffer(dc,bounds); paintWindow(window,buffer.dc); }
        if (message==WM_PAINT) EndPaint(window,&paint); return 0;
    }
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        const DWORD corners=1;DwmSetWindowAttribute(window,33,&corners,sizeof(corners));
        brandIcon = control(window,L"STATIC",L"",SS_ICON,24,22,40,40);
        SendMessageW(brandIcon,STM_SETICON,reinterpret_cast<WPARAM>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),IMAGE_ICON,dp(36),dp(36),0)),0);
        appTitle = control(window,L"STATIC",L"911reg MapExtractor",0,80,20,520,30);
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
        taskTiles[4]=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,500,410,144,76,124);
        mapInput=control(window,L"EDIT",L"",0,0,0,0,0); ShowWindow(mapInput,SW_HIDE);
        serverLabel=control(window,L"STATIC",L"",0,400,22,184,22);
        serverSelector=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,588,16,340,200,116);
        combineButton=control(window,L"BUTTON",L"",WS_TABSTOP|BS_OWNERDRAW,44,502,600,38,117);
        mapLabel=control(window,L"STATIC",L"",0,1052,126,280,24);
        mapFilterLabel=control(window,L"STATIC",L"",0,1052,160,280,20);
        mapFilter=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,1052,184,280,26,114);
        mapList=control(window,WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SHAREIMAGELISTS|LVS_NOCOLUMNHEADER,1052,222,280,286,113);
        SetWindowTheme(mapList,L"DarkMode_Explorer",nullptr);
        ListView_SetExtendedListViewStyle(mapList,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(mapList,theme::surface); ListView_SetTextBkColor(mapList,theme::surface); ListView_SetTextColor(mapList,theme::text);
        SetWindowSubclass(mapList,mapListProc,3,0);
        LVCOLUMNW column{};column.mask=LVCF_WIDTH;column.cx=450;ListView_InsertColumn(mapList,0,&column);column.cx=95;ListView_InsertColumn(mapList,1,&column);column.cx=85;ListView_InsertColumn(mapList,2,&column);
        SetWindowTheme(mapList,L"DarkMode_Explorer",nullptr);
        installMapChecks();
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
        SendMessageW(progressBar,PBM_SETBKCOLOR,0,RGB(37,48,65));
        status = control(window,L"STATIC",L"",SS_ENDELLIPSIS,708,170,280,64);
        activityLabel = control(window,L"STATIC",L"",0,708,126,280,22);
        logView = control(window,MSFTEDIT_CLASS,L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,708,276,280,368,112);
        SendMessageW(logView,WM_SETFONT,reinterpret_cast<WPARAM>(logFont),TRUE); SendMessageW(logView,EM_SETLIMITTEXT,262144,0);
        SendMessageW(logView,EM_SETBKGNDCOLOR,0,theme::inset);SetWindowTheme(logView,L"DarkMode_Explorer",nullptr);
        { CHARFORMAT2W style{}; style.cbSize=sizeof(style); style.dwMask=CFM_COLOR; style.crTextColor=theme::body; SendMessageW(logView,EM_SETCHARFORMAT,SCF_ALL,reinterpret_cast<LPARAM>(&style)); }
        configurationHeading=control(window,L"STATIC",L"",0,0,0,1,1);
        versionTag=control(window,L"STATIC",L"v0.9.0",0,0,0,1,1);
        // Section titles, the map count and the version are painted by paintWindow; the statics keep their text.
        for (const auto hidden:{configurationHeading,mapLabel,activityLabel,mapCount,versionTag}) ShowWindow(hidden,SW_HIDE);
        for (const auto label:{clientLabel,outputLabel,taskLabel,serverLabel}) SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(labelFont),TRUE);
        for (const auto label:{subtitle,capability}) SendMessageW(label,WM_SETFONT,reinterpret_cast<WPARAM>(smallFont),TRUE);
        clearLogButton=control(window,L"BUTTON",L"",BS_OWNERDRAW|WS_TABSTOP,0,0,1,1,130);
        saveLogButton=control(window,L"BUTTON",L"",BS_OWNERDRAW|WS_TABSTOP,0,0,1,1,131);
        copyLogButton=control(window,L"BUTTON",L"",BS_OWNERDRAW|WS_TABSTOP,0,0,1,1,136);
        minimizeButton=control(window,L"BUTTON",L"—",BS_OWNERDRAW|WS_TABSTOP,0,0,1,1,133);
        closeButton=control(window,L"BUTTON",L"×",BS_OWNERDRAW|WS_TABSTOP,0,0,1,1,134);
        statsCanvas=control(window,L"911regStats",L"",WS_CLIPCHILDREN,0,0,1,1,135);
        countersCanvas=control(window,L"911regCounters",L"",WS_TABSTOP,0,0,1,1,137);
        SetParent(progressBar,statsCanvas);
        activityCanvas=control(window,L"911regActivity",L"",0,0,0,1,1,132);
        spinnerCanvas=control(window,L"911regSpinner",L"",0,0,0,1,1,137);
        labCanvas=control(window,L"911regLab",L"",0,0,0,1,1,138);
        BOOL animation=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animation,0);animateDonut=animation!=FALSE;
        SetTimer(window,2,30,nullptr);
        std::vector<wchar_t> executable(32768);
        if (GetModuleFileNameW(nullptr,executable.data(),32768)) {
            const auto folder = std::filesystem::path(executable.data()).parent_path() / L"Output";
            SetWindowTextW(output,folder.c_str());
        }
        EnableWindow(startButton,FALSE);
        updateLanguage();
        tips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
        SendMessageW(tips,CCM_SETUNICODEFORMAT,TRUE,0);
        SetWindowTheme(tips,L"",L"");
        SendMessageW(tips,TTM_SETTIPBKCOLOR,RGB(38,38,38),0);
        SendMessageW(tips,TTM_SETTIPTEXTCOLOR,RGB(226,235,247),0);
        SetWindowLongPtrW(tips,GWL_STYLE,GetWindowLongPtrW(tips,GWL_STYLE)&~WS_BORDER);
        SetWindowSubclass(tips,tooltipProc,1,0);
        SendMessageW(tips,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        SendMessageW(tips,TTM_SETMAXTIPWIDTH,0,310);
        SendMessageW(tips,TTM_SETDELAYTIME,TTDT_INITIAL,450);
        SendMessageW(tips,TTM_SETDELAYTIME,TTDT_AUTOPOP,16000);
        RECT tipMargin{14,10,14,10}; SendMessageW(tips,TTM_SETMARGIN,0,reinterpret_cast<LPARAM>(&tipMargin));
        for (const auto& entry:std::vector<std::pair<HWND,const char*>>{{minimizeButton,"studio_minimize"},{closeButton,"studio_close"},{input,"tip_client"},{inputButton,"tip_client"},{output,"tip_output"},{outputButton,"tip_output"},{mapList,"tip_list"},{mapFilter,"tip_search"},{serverSelector,"tip_server"},{preferClassicButton,"tip_prefer_classic"},{preferNormalButton,"tip_prefer_normal"},{combineButton,"tip_combine"},{languageSelector,"tip_language"},{startButton,"tip_start"},{openButton,"tip_results"},{clearOutputButton,"tip_clear"},{logView,"tip_log"},{progressBar,"tip_log"},{taskTiles[0],"tip_doors"},{taskTiles[1],"tip_water"},{taskTiles[3],"tip_geodata"},{taskTiles[4],"tip_pathnodes"}}) addTip(window,entry.first,entry.second);
        if (const auto choice=reinterpret_cast<Choice*>(GetWindowLongPtrW(input,GWLP_USERDATA)); choice && choice->edit) addTip(window,choice->edit,"tip_client");
        addTip(window,countersCanvas,"studio_counters_tip");
        addTip(window,statsCanvas,"studio_rate_tip");
        const auto recent = clientHistory->load();
        refreshClients(recent.empty() ? L"" : recent.front());

        populateMapList(true); placeTasks();
        consoleLine(L"INFO",theme::blue,i18n::tr("studio_console_ready"),theme::body);
        if (!verifying) for (unsigned mode=0;mode<=4;++mode) {
            try { if (extractor::cleanupIncomplete(text(output),mode)) consoleLine(L"CLEAN",theme::cyan,i18n::tr("studio_cleaned")+L" "+extractor::taskOutput(text(output),mode).wstring(),theme::body); }
            catch (const std::exception&) {}
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: if(wParam!=SIZE_MINIMIZED)studioLayout(window);return 0;
    case WM_SYSCOMMAND: if((wParam&0xfff0)==SC_MAXIMIZE || (wParam&0xfff0)==SC_SIZE)return 0;break;
    case WM_NCLBUTTONDBLCLK: if(wParam==HTCAPTION)return 0;break;
    case WM_NCHITTEST: {
        POINT point{short(LOWORD(lParam)),short(HIWORD(lParam))};ScreenToClient(window,&point);
        if(point.y>=0 && point.y<dp(68))return HTCAPTION;return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto limits=reinterpret_cast<MINMAXINFO*>(lParam);limits->ptMinTrackSize={fixedWindowSize.cx,fixedWindowSize.cy};limits->ptMaxTrackSize=limits->ptMinTrackSize;return 0;
    }
    case WM_NOTIFYFORMAT: return NFR_UNICODE;
    case WM_NOTIFY: {
        const auto header=reinterpret_cast<NMHDR*>(lParam);
        if (header->hwndFrom==mapList && header->code==NM_CUSTOMDRAW) return drawMapRow(*reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
        // A click anywhere on a row toggles it; the checkbox itself is toggled by the list.
        if (header->hwndFrom==mapList && header->code==NM_CLICK && !process) {
            LVHITTESTINFO hit{}; hit.pt=reinterpret_cast<NMITEMACTIVATE*>(lParam)->ptAction;
            if (ListView_SubItemHitTest(mapList,&hit)>=0 && !(hit.flags&LVHT_ONITEMSTATEICON) && !(GetKeyState(VK_SHIFT)&0x8000) && !(GetKeyState(VK_CONTROL)&0x8000))
                ListView_SetCheckState(mapList,hit.iItem,!ListView_GetCheckState(mapList,hit.iItem));
            return 0;
        }
        if (header->hwndFrom==mapList && header->code==LVN_ITEMCHANGED && (reinterpret_cast<NMLISTVIEW*>(lParam)->uChanged&LVIF_STATE)) {
            const auto change=reinterpret_cast<NMLISTVIEW*>(lParam); if (change->iItem>=0) invalidateRow(mapList,change->iItem);
            InvalidateRect(window,&mapsTitle,FALSE);
        }
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
        auto item=*reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        { Buffered buffer(item.hDC,item.rcItem); item.hDC=buffer.dc; drawControl(item); }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const HWND target=reinterpret_cast<HWND>(lParam);
        const bool header=target==appTitle || target==subtitle || target==serverLabel || target==versionTag;
        const bool edit=(message==WM_CTLCOLOREDIT || target==output || target==mapInput || target==mapFilter) && target!=logView;
        SetTextColor(dc,target==appTitle || edit ? theme::text : target==status ? theme::body : target==subtitle || target==serverLabel ? theme::muted : target==capability ? theme::faint : theme::muted);
        SetBkColor(dc,edit ? theme::inset : header ? theme::header : theme::surface);
        return reinterpret_cast<LRESULT>(edit ? field : header ? headerBrush : panel);
    }
    case WM_COMMAND:
        if(LOWORD(wParam)==133){ShowWindow(window,SW_MINIMIZE);return 0;}
        if(LOWORD(wParam)==134){SendMessageW(window,WM_CLOSE,0,0);return 0;}
        if(LOWORD(wParam)==130){SetWindowTextW(logView,L"");return 0;}
        if(LOWORD(wParam)==131){saveConsole(window);return 0;}
        if(LOWORD(wParam)==136){copyConsole(window);setText(status,"studio_copied");return 0;}
        if ((HIWORD(wParam)==EN_SETFOCUS || HIWORD(wParam)==EN_KILLFOCUS) && (reinterpret_cast<HWND>(lParam)==output || reinterpret_cast<HWND>(lParam)==mapFilter)) {
            for (const auto& frame:{outputFrame,searchFrame}) { RECT ring=frame; InflateRect(&ring,1,1); InvalidateRect(window,&ring,FALSE); }
            return 0;
        }
        if (LOWORD(wParam)==125 && !process) {
            try {
                if (confirmClearOutput(window,std::filesystem::path(text(output)))) setText(status,"clear_ready");
            } catch (const std::exception&) { setText(status,"folder_error"); }
            return 0;
        }
        if (LOWORD(wParam)>=120 && LOWORD(wParam)<=124 && !process) {
            SendMessageW(taskSelector,CB_SETCURSEL,LOWORD(wParam)-120,0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(106,CBN_SELCHANGE),reinterpret_cast<LPARAM>(taskSelector)); return 0;
        }
        if (LOWORD(wParam)==117 && !process) { if (taskMode==3) pathnodesEnabled=!pathnodesEnabled; else combineResults=!combineResults; savePreferences(); InvalidateRect(combineButton,nullptr,TRUE); return 0; }
        if (LOWORD(wParam)==116 && HIWORD(wParam)==CBN_SELCHANGE) {
            const auto index=SendMessageW(serverSelector,CB_GETCURSEL,0,0);
            if (taskMode>=3) {
                geoFormat=index==1 ? "both" : index==2 ? "l2off" : "l2j";
                savePreferences(); return 0;
            }
            if (index>=0 && size_t(index)<visibleServers.size()) selectedServer=visibleServers[size_t(index)];
            savePreferences(); EnableWindow(combineButton,(taskMode<2 && selectedServer!="l2off") || taskMode==3); return 0;
        }
        if ((LOWORD(wParam)==128 || LOWORD(wParam)==129) && !process) { extractor::preferMapVariant(selectedMaps,LOWORD(wParam)==129); populateMapList(false); return 0; }
        if ((LOWORD(wParam)==118 || LOWORD(wParam)==119) && !process) { selectedMaps=LOWORD(wParam)==118 ? availableMaps : std::vector<std::filesystem::path>{}; populateMapList(false); return 0; }
        if (LOWORD(wParam)==114 && HIWORD(wParam)==EN_CHANGE) { populateMapList(false); return 0; }
        if (HIWORD(wParam) == EN_CHANGE || (LOWORD(wParam) == 109 && HIWORD(wParam) == CBN_EDITCHANGE)) {
            if (LOWORD(wParam)==109 && !process && !lockMapSelection) populateMapList(true);
            EnableWindow(startButton,!process && sourceReady() && !text(output).empty());
            RECT client{}; GetClientRect(window,&client); client.top=client.bottom-dp(36); InvalidateRect(window,&client,FALSE);
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
            if (selected >= 0 && selected <= 4) taskMode = unsigned(selected);
            for (auto tile:taskTiles) InvalidateRect(tile,nullptr,TRUE);
            EnableWindow(serverSelector,taskMode!=4); EnableWindow(combineButton,(taskMode<2 && selectedServer!="l2off") || taskMode==3);
            setText(combineButton,taskMode==3 ? "include_pathnodes" : "combine_xml");
            ShowWindow(combineButton,taskMode==4 ? SW_HIDE : SW_SHOW); tipKeys[combineButton]=taskMode==3 ? "tip_pathnodes" : "tip_combine";
            if (serverLabel) setText(serverLabel,taskMode==4 ? "node_format" : taskMode==3 ? "geo_format" : "server_type");
            populateServers(); EnableWindow(serverSelector,taskMode!=4);
            ShowWindow(capability,capabilityFits ? SW_SHOW : SW_HIDE); InvalidateRect(window,&configurationCard,FALSE);
            setText(capability,taskMode==4 ? "pn_hint" : "variants_hint");
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
        } else if (LOWORD(wParam) == 103 && process) {
            if (!suspendProcess(process,!paused)) { setText(status,"studio_pause_error"); return 0; }
            paused=!paused;
            if (paused) pausedAt=GetTickCount64(); else pausedTotal+=GetTickCount64()-pausedAt;
            setText(startButton,paused ? "studio_resume" : "studio_pause"); setText(status,paused ? "studio_paused_log" : "studio_resumed_log");
            consoleLine(paused ? L"PAUSE" : L"RUN",paused ? theme::amber : theme::accent,i18n::tr(paused ? "studio_paused_log" : "studio_resumed_log"),theme::body);
            InvalidateRect(startButton,nullptr,FALSE); InvalidateRect(statsCanvas,nullptr,FALSE); InvalidateRect(window,nullptr,FALSE);
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
                // Output left by a run that was closed or killed mid-way is removed without asking.
                recoveredRun=extractor::cleanupIncomplete(destination,taskMode);
                if (!confirmClearOutput(window,destination)) { lockMapSelection=false; return 0; }
                const auto candidate=destination;
                std::filesystem::create_directories(candidate);
                if (selectedMapMode) {
                    selectionPath=candidate/L"SelectedMaps.txt";
                    if (!debugResults) {
                        std::vector<wchar_t> temporary(32768),filename(32768);
                        if (!GetTempPathW(32768,temporary.data()) || !GetTempFileNameW(temporary.data(),L"mxe",0,filename.data())) throw std::runtime_error("Cannot prepare selected maps");
                        selectionPath=filename.data();
                    }
                    std::ofstream selection(selectionPath,std::ios::binary);
                    for (const auto& selected : chosenMaps) selection << extractor::output::utf8(selected) << "\n";
                    selection.close(); if (!selection) throw std::runtime_error("Cannot save selected maps");
                }
                runFolder = candidate; manifest = extractor::taskOutput(runFolder,taskMode).wstring();
                extractor::markIncomplete(runFolder,taskMode,selectedMapMode && !debugResults ? selectionPath : std::filesystem::path{});
            } catch (const std::exception&) {
                lockMapSelection=false; setText(status,"folder_error"); return 0;
            }
            std::vector<wchar_t> executable(32768);
            GetModuleFileNameW(nullptr,executable.data(),32768);
            auto command = quote(executable.data())+(taskMode == 0 ? L" doors " : taskMode == 1 ? L" water " : taskMode==3 ? L" geodata " : taskMode==4 ? L" pathnodes " : L" scan ")+quote(source)+L" --output "+quote(manifest);
            command+=L" --all-variants --threads "+std::to_wstring(workerCount);
            if (combineResults && taskMode<2 && selectedServer!="l2off") command+=L" --combine-xml";
            command+=L" --server "+quote(std::wstring(selectedServer.begin(),selectedServer.end()));
            if (!debugResults) command+=L" --simple-output";
            if (taskMode==3) { command+=L" --geo "+std::wstring(geoFormat.begin(),geoFormat.end()); if (!pathnodesEnabled) command+=L" --no-pathnodes"; }
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
            const BOOL launched = CreateProcessW(executable.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS|CREATE_SUSPENDED,nullptr,nullptr,&startup,&child);
            CloseHandle(writer); if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
            if (!launched) {
                CloseHandle(scanPipe); scanPipe = nullptr;
                if (!debugResults && !selectionPath.empty()) { std::error_code error; std::filesystem::remove(selectionPath,error); }
                extractor::markComplete(runFolder,taskMode);
                lockMapSelection=false; setText(status,"launch_error"); return 0;
            }
            // The job ends the extractor together with this window, even when the window is killed.
            if (!extractionJob) {
                extractionJob=CreateJobObjectW(nullptr,nullptr);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (extractionJob) SetInformationJobObject(extractionJob,JobObjectExtendedLimitInformation,&limits,sizeof(limits));
            }
            if (extractionJob) AssignProcessToJobObject(extractionJob,child.hProcess);
            ResumeThread(child.hThread);
            scanLog.clear(); displayedLog=0; SetWindowTextW(logView,L"");
            if (recoveredRun) consoleLine(L"CLEAN",theme::cyan,i18n::tr("studio_cleaned")+L" "+manifest,theme::body);
            if (debugResults) jobLog.open(runFolder/L"911reg.log",std::ios::binary); SendMessageW(progressBar,PBM_SETRANGE32,0,100); SendMessageW(progressBar,PBM_SETPOS,0,0);
            CloseHandle(child.hThread); process = child.hProcess; lockMapSelection=false;
            runCounters={};refreshDetails(true);InvalidateRect(countersCanvas,nullptr,FALSE);
            activityStage=0;startedAt=GetTickCount64();finishedAt=0;hasRun=true;paused=false;pausedAt=0;pausedTotal=0;completedMaps=0;totalMaps=selectedMapMode?selectedMaps.size():singleMap?1:availableMaps.size();currentMap.clear();
            setText(startButton,"studio_pause"); EnableWindow(startButton,TRUE); InvalidateRect(startButton,nullptr,FALSE);
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
        if(wParam==2){
            static ULONGLONG lastFrame=0,lastSecond=0;
            const auto now=GetTickCount64();
            if(IsIconic(window) || !IsWindowVisible(window)) { lastFrame=now; return 0; }
            const auto elapsed=lastFrame ? std::min<ULONGLONG>(now-lastFrame,100) : 0;
            lastFrame=now;
            if(animateDonut) {
                donutAngle+=float(elapsed)*0.000875f;
                InvalidateRect(activityCanvas,nullptr,FALSE);
                if(IsWindowVisible(spinnerCanvas))InvalidateRect(spinnerCanvas,nullptr,FALSE);
                if(IsWindowVisible(labCanvas))InvalidateRect(labCanvas,nullptr,FALSE);
            }
            if(now/1000!=lastSecond){lastSecond=now/1000;InvalidateRect(statsCanvas,nullptr,FALSE);RECT r{};GetClientRect(window,&r);r.top=r.bottom-dp(36);InvalidateRect(window,&r,FALSE);}
            return 0;
        }
        readProgress();
        if(verifying && verificationSimple && process && runCounters.warnings && !detailsPanel.window) {
            openDetails(window,1);previewPath=runFolder.parent_path()/L"live-warning-details.bmp";renderPreview(detailsPanel.window);previewPath.clear();
        }
        if(verifying && verificationSimple && process && !runningProofCaptured && !scanLog.empty()) {
            runningProofCaptured=true;previewPath=runFolder.parent_path()/L"running-preview.bmp";renderPreview(window);previewPath.clear();
            // Pause and resume the live run once, and keep a picture of the paused window.
            if(WaitForSingleObject(process,0)==WAIT_TIMEOUT) {
                SendMessageW(window,WM_COMMAND,103,0); if(!paused) throw std::runtime_error("Pause did not take effect");
                previewPath=runFolder.parent_path()/L"paused-preview.bmp";renderPreview(window);previewPath.clear();
                SendMessageW(window,WM_COMMAND,103,0); if(paused) throw std::runtime_error("Resume did not take effect");
            }
        }
        if (process && WaitForSingleObject(process,0) == WAIT_OBJECT_0) {
            DWORD code = 1; GetExitCodeProcess(process,&code);
            finishedAt=GetTickCount64();lastExit=code;paused=false;setText(startButton,"start");
            extractor::markComplete(runFolder,taskMode);
            CloseHandle(process); process = nullptr; KillTimer(window,1);
            readProgress(); CloseHandle(scanPipe); scanPipe = nullptr;
            runCounters.finish(code);InvalidateRect(countersCanvas,nullptr,FALSE);
            jobLog.close(); SendMessageW(progressBar,PBM_SETRANGE32,0,100); SendMessageW(progressBar,PBM_SETPOS,code==0 ? 100 : 0,0);
            EnableWindow(startButton,TRUE);
            EnableWindow(input,TRUE); EnableWindow(output,TRUE);
            EnableWindow(inputButton,TRUE); EnableWindow(outputButton,TRUE); EnableWindow(openButton,TRUE); EnableWindow(clearOutputButton,TRUE);
            EnableWindow(taskSelector,TRUE); EnableWindow(languageSelector,TRUE);
            for (auto tile:taskTiles) EnableWindow(tile,TRUE);
            EnableWindow(mapList,TRUE); EnableWindow(mapFilter,TRUE); EnableWindow(serverSelector,taskMode!=4); EnableWindow(combineButton,(taskMode<2 && selectedServer!="l2off") || taskMode==3); EnableWindow(preferNormalButton,TRUE); EnableWindow(preferClassicButton,TRUE); EnableWindow(selectAllButton,TRUE); EnableWindow(clearMapsButton,TRUE);
            if (debugResults) {
                std::ofstream summary(runFolder/"Summary.txt",std::ios::binary); summary<<scanLog;
            } else if (!selectionPath.empty()) { std::error_code error; std::filesystem::remove(selectionPath,error); }
            const auto summary=completionText(code);
            SetWindowTextW(status,summary.first.c_str());InvalidateRect(statsCanvas,nullptr,FALSE);InvalidateRect(window,nullptr,FALSE);
            consoleLine(code?L"FAIL":L"DONE",code?theme::red:theme::green,summary.first,code?RGB(254,190,190):RGB(167,243,208));
            if(summary.second.size()>summary.first.size())consoleAppend(summary.second.substr(summary.first.size())+L"\r\n",code?RGB(253,224,150):theme::body);
            if (!verifying && code!=1 && std::filesystem::exists(manifest) && MessageBoxW(window,i18n::tr("open_output_prompt").c_str(),L"MapExtractor",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES) ShellExecuteW(window,L"open",runFolder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
            if (verifying) {
                if (verificationSimple) {
                    previewPath=runFolder.parent_path()/L"completion-preview.bmp"; renderPreview(window); previewPath.clear();
                    if(detailsPanel.window) {
                        refreshDetails(true);
                        if(detailsPanel.rows.empty())throw std::runtime_error("Live warning details disappeared at completion");
                        previewPath=runFolder.parent_path()/L"warning-details.bmp";renderPreview(detailsPanel.window);previewPath.clear();
                        DestroyWindow(detailsPanel.window);
                    }
                    if (code==2 && text(logView).find(i18n::tr("skipped_reasons"))==std::wstring::npos) throw std::runtime_error("Skipped reasons are not visible in Activity");
                }
                verificationResult = (code == 0 || (verificationSimple && code==2)) && std::filesystem::exists(manifest) ? 0 : 1;
                PostMessageW(window,WM_CLOSE,0,0);
            }
        }
        return 0;
    case WM_CLOSE:
        if (process) {
            if (verifying || MessageBoxW(window,i18n::tr("studio_close_running").c_str(),L"MapExtractor",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) { setText(status,"running"); return 0; }
            TerminateProcess(process,1); WaitForSingleObject(process,5000);
            CloseHandle(process); process=nullptr; KillTimer(window,1);
            if (scanPipe) { CloseHandle(scanPipe); scanPipe=nullptr; }
            jobLog.close();
            // Clean up now; if that fails the marker stays and the next run cleans up instead.
            try { extractor::cleanupIncomplete(runFolder,taskMode); } catch (const std::exception&) {}
        }
        DestroyWindow(window); return 0;
    case WM_DESTROY: KillTimer(window,2);if(mapChecks){ImageList_Destroy(mapChecks);mapChecks=nullptr;}PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
}

int showUi() {
    i18n::load(); extractor::allVariants=true;
    std::vector<wchar_t> settingsExecutable(32768); GetModuleFileNameW(nullptr,settingsExecutable.data(),32768);
    preferencesPath=extractor::settingsFile(std::filesystem::path(settingsExecutable.data()).parent_path());
    const auto preferences=extractor::UiPreferences::load(preferencesPath);
    selectedServer=preferences.server; combineResults=preferences.combine; debugResults=false; geoFormat=preferences.geodata; pathnodesEnabled=preferences.pathnodes;
    // The window never writes debug files. The verify path still does, unless the simple-output check is running.
    if (verifying && previewPath.empty() && !verificationSimple) { selectedServer="all"; combineResults=false; debugResults=true; }
    if (verificationSimple) { selectedServer="l2jserver"; combineResults=true; debugResults=false; }
    clientHistory = std::make_unique<extractor::ClientHistory>(preferencesPath);
    SetProcessDPIAware();
    const auto initialized = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS|ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    LoadLibraryW(L"Msftedit.dll");
    RECT workArea{};SystemParametersInfoW(SPI_GETWORKAREA,0,&workArea,0);
    {
        const auto screen=GetDC(nullptr); const float dpi=float(GetDeviceCaps(screen,LOGPIXELSY))/96; ReleaseDC(nullptr,screen);
        const float fit=std::min(float(workArea.right-workArea.left-24)/1420,float(workArea.bottom-workArea.top-24)/900);
        uiScale=std::clamp(std::min(dpi,fit),1.0f,std::max(1.0f,dpi));
    }
    Gdiplus::GdiplusStartupInput graphicsInput; ULONG_PTR graphicsToken=0; Gdiplus::GdiplusStartup(&graphicsToken,&graphicsInput,nullptr);
    if (fontInstalled(L"Segoe UI Variable Text")) { uiFace=L"Segoe UI Variable Text"; displayFace=L"Segoe UI Variable Display"; }
    for (const auto face:{L"JetBrains Mono",L"Cascadia Mono",L"Consolas"}) if (fontInstalled(face)) { monoFace=face; break; }
    background=CreateSolidBrush(theme::base); field=CreateSolidBrush(theme::inset); panel=CreateSolidBrush(theme::surface);
    headerBrush=CreateSolidBrush(theme::header); raisedBrush=CreateSolidBrush(theme::raised);
    const auto makeFont=[](int size,int weight,const std::wstring& face) { return CreateFontW(-dp(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,face.c_str()); };
    font=makeFont(14,FW_NORMAL,uiFace); titleFont=makeFont(20,FW_SEMIBOLD,displayFace); headingFont=makeFont(14,FW_BOLD,displayFace);
    labelFont=makeFont(13,FW_SEMIBOLD,uiFace); smallFont=makeFont(12,FW_SEMIBOLD,uiFace); buttonFont=makeFont(15,FW_SEMIBOLD,uiFace);
    percentFont=makeFont(24,FW_SEMIBOLD,displayFace); logFont=makeFont(13,FW_NORMAL,monoFace); monoSmallFont=makeFont(12,FW_NORMAL,monoFace);
    asciiFont=makeFont(11,FW_BOLD,monoFace); smallAsciiFont=makeFont(8,FW_BOLD,monoFace);
    WNDCLASSW activity{};activity.lpfnWndProc=activityProc;activity.hInstance=GetModuleHandleW(nullptr);activity.lpszClassName=L"911regActivity";RegisterClassW(&activity);
    WNDCLASSW choice{}; choice.lpfnWndProc=choiceProc; choice.hInstance=GetModuleHandleW(nullptr); choice.lpszClassName=L"911regChoice"; choice.hCursor=LoadCursorW(nullptr,IDC_HAND); RegisterClassW(&choice);
    WNDCLASSW lab{};lab.lpfnWndProc=labProc;lab.hInstance=GetModuleHandleW(nullptr);lab.lpszClassName=L"911regLab";RegisterClassW(&lab);
    WNDCLASSW spinner{};spinner.lpfnWndProc=spinnerProc;spinner.hInstance=GetModuleHandleW(nullptr);spinner.lpszClassName=L"911regSpinner";RegisterClassW(&spinner);
    WNDCLASSW stats{};stats.lpfnWndProc=statsProc;stats.hInstance=GetModuleHandleW(nullptr);stats.lpszClassName=L"911regStats";RegisterClassW(&stats);
    WNDCLASSW counters{};counters.lpfnWndProc=countersProc;counters.hInstance=GetModuleHandleW(nullptr);counters.lpszClassName=L"911regCounters";RegisterClassW(&counters);
    WNDCLASSW diagnostics{};diagnostics.lpfnWndProc=detailsProc;diagnostics.hInstance=GetModuleHandleW(nullptr);diagnostics.lpszClassName=L"911regDiagnostics";diagnostics.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&diagnostics);
    WNDCLASSW progress{}; progress.lpfnWndProc=progressProc; progress.hInstance=GetModuleHandleW(nullptr); progress.lpszClassName=L"911regProgress"; RegisterClassW(&progress);
    WNDCLASSW cls{}; cls.lpfnWndProc = windowProc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"911regMapExtractor"; cls.hbrBackground = background;
    cls.hIcon = LoadIconW(cls.hInstance,MAKEINTRESOURCEW(101));
    cls.hCursor = LoadCursorW(nullptr,IDC_ARROW);
    RegisterClassW(&cls);
    fixedWindowSize={std::min(LONG(dp(1420)),workArea.right-workArea.left-24),std::min(LONG(dp(900)),workArea.bottom-workArea.top-24)};
    HWND window = CreateWindowW(cls.lpszClassName,L"911reg MapExtractor",WS_POPUP|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN,
        workArea.left+(workArea.right-workArea.left-fixedWindowSize.cx)/2,workArea.top+(workArea.bottom-workArea.top-fixedWindowSize.cy)/2,fixedWindowSize.cx,fixedWindowSize.cy,nullptr,nullptr,cls.hInstance,nullptr);
    if (!window) return 1;
    const auto menu=GetSystemMenu(window,FALSE);DeleteMenu(menu,SC_MAXIMIZE,MF_BYCOMMAND);DeleteMenu(menu,SC_SIZE,MF_BYCOMMAND);
    SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
    if(!studioProof.empty()) {verifyStudioWindow(window);}
    else if (!previewPath.empty()) {
        SetWindowPos(window,HWND_BOTTOM,-32000,-32000,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
        ShowWindow(window,SW_SHOWNOACTIVATE); UpdateWindow(window);
        renderPreview(window); DestroyWindow(window);
    } else if (verifying) {
        SetWindowPos(window,nullptr,-32000,-32000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);ShowWindow(window,SW_SHOWNOACTIVATE);
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
        if(detailsPanel.window && (message.hwnd==detailsPanel.window || IsChild(detailsPanel.window,message.hwnd))) {
            if(message.message==WM_KEYDOWN && message.wParam==VK_ESCAPE){DestroyWindow(detailsPanel.window);continue;}
            focusInput(detailsPanel.window,message);
            if(IsDialogMessageW(detailsPanel.window,&message))continue;
            TranslateMessage(&message);DispatchMessageW(&message);continue;
        }
        focusInput(window,message);
        if(message.message==WM_KEYDOWN && message.wParam==VK_RETURN && (GetKeyState(VK_CONTROL)&0x8000) && !process){SendMessageW(window,WM_COMMAND,103,0);continue;}
        if (!IsDialogMessageW(window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    for (const auto face:{font,titleFont,logFont,headingFont,asciiFont,smallAsciiFont,labelFont,smallFont,buttonFont,percentFont,monoSmallFont}) DeleteObject(face);
    for (const auto brush:{background,field,panel,headerBrush,raisedBrush}) DeleteObject(brush);
    if (mapRows) { ImageList_Destroy(mapRows); mapRows=nullptr; }
    Gdiplus::GdiplusShutdown(graphicsToken);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return verifying ? verificationResult : int(message.wParam);
}

int verifyCompletion() {
    i18n::load();
    RunCounters counters;
    counters.consume("WARNING: repeated mesh warning",3);counters.consume("WARNING: repeated mesh warning",3);
    counters.consume("SKIPPED: 18_13.unr / StaticMeshActor1: invalid collision",3);
    counters.consume("SKIPPED: 18_13.unr / StaticMeshActor2: invalid collision",3);
    counters.consume("SKIPPED: 18_13.unr: incomplete collision",3);
    if(counters.warnings!=1 || counters.skipped!=1)throw std::runtime_error("Live diagnostics overcount repeated warnings or failed maps");
    if(counters.details[1].size()!=1 || counters.details[1][0].occurrences!=2 || counters.details[2].size()!=1 || counters.details[2][0].reason.find("StaticMeshActor2")==std::string::npos)throw std::runtime_error("Diagnostic reasons or occurrences lost");
    counters.consume("RESULT geodata 8 1 0 0 0",3);counters.finish(2);
    if(counters.errors || counters.skipped!=1)throw std::runtime_error("Partial export counted as fatal error");
    counters={};counters.consume("SKIPPED: door invalid",0);counters.consume("RESULT doors 303 2 1 1 5",0);
    if(counters.skipped!=4 || counters.warnings!=5)throw std::runtime_error("Door result counters mix duplicates with skipped items");
    counters.consume("ERROR: output is not writable",0);counters.finish(1);
    if(counters.errors!=1)throw std::runtime_error("Fatal error counted twice");
    counters={};counters.finish(1);
    if(counters.errors!=1 || counters.warnings || counters.skipped)throw std::runtime_error("Silent failed worker is not counted");
    if(counters.details[0].size()!=1)throw std::runtime_error("Silent failure has no details");
    counters={};
    counters.consume("Duplicate door IDs mean that multiple doors share an ID.",0);
    counters.consume("For stacked duplicates, only the upper placement is exported.",0);
    counters.consume("Duplicate door IDs:",0);
    counters.consume("19_26_Classic: 2 duplicate door IDs. IDs: 19260003, 19260007.",0);
    counters.consume("RESULT doors 303 0 0 0 2",0);
    if(counters.warnings!=2 || counters.details[1].size()!=1 || counters.details[1][0].reason.find("19260007")==std::string::npos || counters.details[1][0].reason.find("upper placement")==std::string::npos)throw std::runtime_error("Duplicate door warning details lost");
    counters={};counters.consume("RESULT water 100 3 0 0 0",1);
    if(counters.details[2].size()!=1)throw std::runtime_error("Summary-only skips have no fallback details");
    scanLog="SKIPPED: 20_22.unr / Mover8: Missing door ID\nRESULT doors 303 2 0 0 0\n";
    const auto skipped=completionText(2);
    if (skipped.first!=i18n::format("result_doors",303,2) || skipped.second.find(L"Mover8: Missing door ID")==std::wstring::npos) throw std::runtime_error("Skipped-item reason is not visible");
    scanLog="Duplicate door IDs mean that multiple doors share an ID.\nDuplicate door IDs:\n19_26_Classic: 1 duplicate door ID. ID: 19260003.\nRESULT doors 303 0 0 0 1\n";
    const auto duplicate=completionText(0);
    if (duplicate.first!=i18n::format("result_doors",303,0) || duplicate.second.find(L"19260003")==std::wstring::npos) throw std::runtime_error("Duplicate warning was lost");
    scanLog="11_23.unr: Terrain maximum-X edge: flat fallback used.\nRESULT geodata 1 0 0 0 0\n";
    const auto info=completionText(0);
    if(info.first!=i18n::format("result_geodata",1,0) || info.second.find(i18n::tr("result_warnings"))!=std::wstring::npos)throw std::runtime_error("Routine terrain fallback shown as warning");
    scanLog="WARNING: 11_23.unr: Corrupt neighbouring terrain; flat fallback used.\nRESULT geodata 1 0 0 0 0\n";
    const auto warning=completionText(0);
    if(warning.first!=i18n::format("result_geodata",1,0) || warning.second.find(L"flat fallback used.")==std::wstring::npos || warning.second.find(i18n::tr("result_warnings"))==std::wstring::npos)throw std::runtime_error("Terrain fallback warning lost or counted as skipped map");
    scanLog="ERROR: Output directory is not writable\n";
    if (completionText(1).second.find(L"not writable")==std::wstring::npos) throw std::runtime_error("Failure reason was lost");
    return 0;
}

int verifyUi(const wchar_t* source, const wchar_t* destination, bool doors, bool water, bool geodata, bool simple, bool pathnodes) {
    verifying = true; verificationSimple=simple;
    taskMode = pathnodes ? 4 : geodata ? 3 : water ? 1 : doors ? 0 : 2;
    selectedMapMode=false;
    singleMap = std::filesystem::is_regular_file(source);
    verificationInput = source; verificationOutput = destination;
    return showUi();
}

int previewUi(const wchar_t* destination) {
    previewPath = destination; verifying = true; verificationResult = 0;
    return showUi();
}

int verifyUiStudio(const wchar_t* directory) {
    studioProof=std::filesystem::absolute(directory);
    if(std::filesystem::exists(studioProof))throw std::runtime_error("Choose a new UI proof directory");
    std::filesystem::create_directories(studioProof);previewPath=studioProof/L"default.bmp";verifying=true;return showUi();
}
