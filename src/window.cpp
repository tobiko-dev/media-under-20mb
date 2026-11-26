#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
#include "media.hpp"
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

using namespace drop;
namespace {
constexpr wchar_t windowClass[]=L"Under20.MainWindow";
constexpr wchar_t regKey[]=L"Software\\Under20 Media Compressor";
constexpr UINT updateMsg=WM_APP+1, finishMsg=WM_APP+2, approvalMsg=WM_APP+3, startMsg=WM_APP+4;
enum ID { Drop=101, Keep, Queue, Cancel, Open, Locate, Detail, Meter };
enum class Status { Waiting, Working, Saved, Skipped, Cancelled, Failed };
const COLORREF paper=RGB(246,248,246), ink=RGB(26,42,37), soft=RGB(78,99,89), green=RGB(23,89,65),
    mint=RGB(232,244,235), border=RGB(166,192,174), white=RGB(255,255,255);
HWND win{},zone{},box{},list{},cancelButton{},openButton{},locateButton{},detail{},meter{};
HFONT normal{},heading{},title{},small{},medium{};
HBRUSH pageBrush{};
HINSTANCE appInstance{};
int dpi=96, active=-1;
bool closing=false;
Tools tools;
std::thread worker;
struct Job {
    fs::path input;
    bool keep=false;
    std::atomic_bool cancel{false};
    Status status=Status::Waiting;
    int percent=0;
    std::wstring phase;
    Result result;
};
std::vector<std::shared_ptr<Job>> jobs;
struct Update { int id,percent; std::wstring phase; };
struct Finished { int id; Result result; };
struct Approval { int id; std::wstring text; };

int px(int n) { return MulDiv(n,dpi,96); }
std::wstring env(const wchar_t* key) {
    DWORD n=GetEnvironmentVariableW(key,nullptr,0); if (!n) return {};
    std::wstring out(n,0); DWORD used=GetEnvironmentVariableW(key,out.data(),n); out.resize(used); return out;
}
fs::path exeFolder() {
    std::wstring p(32768,0); auto n=GetModuleFileNameW(nullptr,p.data(),(DWORD)p.size());
    p.resize(n); return fs::path(p).parent_path();
}
bool file(const fs::path& p) { std::error_code err; return fs::is_regular_file(p,err); }
Tools pairAt(const fs::path& directory) {
    if (directory.empty() || !directory.is_absolute()) return {};
    auto encoder=directory/L"ffmpeg.exe",probe=directory/L"ffprobe.exe";
    return file(encoder)&&file(probe)?Tools{encoder,probe}:Tools{};
}
std::vector<fs::path> toolDirectories() {
    std::vector<fs::path> dirs{exeFolder(),exeFolder()/L"bin"};
    auto paths=env(L"PATH"); size_t pos=0;
    while (pos<paths.size()) {
        auto end=paths.find(L';',pos); std::wstring item=paths.substr(pos,end==paths.npos?end:end-pos);
        if (item.size()>1 && item.front()==L'"' && item.back()==L'"') item=item.substr(1,item.size()-2);
        if (!item.empty()) dirs.emplace_back(item);
        if (end==paths.npos) break;
        pos=end+1;
    }
    for (auto place: {std::make_pair(L"ProgramFiles",L"ffmpeg\\bin"),
                      std::make_pair(L"USERPROFILE",L"scoop\\apps\\ffmpeg\\current\\bin"),
                      std::make_pair(L"LOCALAPPDATA",L"Microsoft\\WinGet\\Links"),
                      std::make_pair(L"ChocolateyInstall",L"bin")}) {
        auto root=env(place.first); if (!root.empty()) dirs.push_back(fs::path(root)/place.second);
    }
    auto drive=env(L"SystemDrive"); if (!drive.empty()) dirs.emplace_back(drive+L"\\ffmpeg\\bin");
    return dirs;
}
fs::path findBinary(const std::vector<fs::path>& dirs,const wchar_t* name) {
    for (auto& dir:dirs) {
        if (dir.is_absolute() && file(dir/name)) return dir/name;
    }
    return {};
}
fs::path savedBinary(const wchar_t* value) {
    wchar_t path[32768]{}; DWORD bytes=sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER,regKey,value,RRF_RT_REG_SZ,nullptr,path,&bytes)==ERROR_SUCCESS &&
        fs::path(path).is_absolute() && file(path)) return path;
    return {};
}
Tools discover() {
    auto enc=savedBinary(L"FFmpeg"), probe=savedBinary(L"FFprobe");
    if (!enc.empty() && !probe.empty()) return {enc,probe};
    auto dirs=toolDirectories();
    for (auto& dir:dirs) { auto found=pairAt(dir); if (!found.ffmpeg.empty()) return found; }
    if (enc.empty()) enc=findBinary(dirs,L"ffmpeg.exe");
    if (probe.empty()) probe=findBinary(dirs,L"ffprobe.exe");
    return enc.empty()||probe.empty()?Tools{}:Tools{enc,probe};
}
fs::path pickTool(bool probe) {
    std::wstring buffer(32768,0); OPENFILENAMEW pick{}; pick.lStructSize=sizeof(pick); pick.hwndOwner=win;
    pick.lpstrTitle=probe?L"Locate your installed ffprobe.exe":L"Locate your installed ffmpeg.exe";
    pick.lpstrFilter=probe?L"FFprobe executable\0ffprobe.exe\0\0":L"FFmpeg executable\0ffmpeg.exe\0\0";
    pick.lpstrFile=buffer.data(); pick.nMaxFile=(DWORD)buffer.size();
    pick.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    return GetOpenFileNameW(&pick)?fs::path(buffer.c_str()):fs::path();
}
bool chooseTools() {
    auto enc=pickTool(false); if (enc.empty()) return false;
    auto dirs=toolDirectories(); dirs.insert(dirs.begin(),enc.parent_path());
    auto probe=findBinary(dirs,L"ffprobe.exe");
    if (probe.empty()) probe=pickTool(true);
    if (probe.empty()) return false;
    tools={enc,probe}; HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER,regKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)==ERROR_SUCCESS) {
        RegSetValueExW(key,L"FFmpeg",0,REG_SZ,(const BYTE*)enc.c_str(),(DWORD)((enc.native().size()+1)*sizeof(wchar_t)));
        RegSetValueExW(key,L"FFprobe",0,REG_SZ,(const BYTE*)probe.c_str(),(DWORD)((probe.native().size()+1)*sizeof(wchar_t)));
        RegCloseKey(key);
    }
    ShowWindow(locateButton,SW_HIDE); InvalidateRect(win,nullptr,FALSE); return true;
}
void useFont(HWND ctrl,HFONT f) { SendMessageW(ctrl,WM_SETFONT,(WPARAM)f,TRUE); }
HFONT font(int pts,int weight) {
    return CreateFontW(-MulDiv(pts,dpi,72),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
}
void fonts() {
    for (auto f:{normal,small,medium,title,heading}) if (f) DeleteObject(f);
    normal=font(10,FW_NORMAL); small=font(9,FW_NORMAL); medium=font(10,FW_SEMIBOLD);
    title=font(21,FW_SEMIBOLD); heading=font(14,FW_SEMIBOLD);
    for (auto h:{box,list,cancelButton,openButton,locateButton,detail}) useFont(h,normal);
    useFont(zone,heading); useFont(ListView_GetHeader(list),medium);
}
HWND child(const wchar_t* type,const wchar_t* text,DWORD style,int id,DWORD ex=0) {
    return CreateWindowExW(ex,type,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,win,(HMENU)(INT_PTR)id,appInstance,nullptr);
}
void move(HWND h,int x,int y,int w,int ht) { MoveWindow(h,px(x),px(y),px(w),px(ht),TRUE); }
int logical(int n) { return MulDiv(n,96,dpi); }
void layout() {
    RECT r{}; GetClientRect(win,&r); int w=logical(r.right),ht=logical(r.bottom);
    int m=24, inner=w-2*m;
    move(zone,m,87,inner,143); move(box,w-m-166,44,166,26); move(locateButton,w-m-167,10,167,30);
    move(list,m,272,inner,std::max(45,ht-412));
    move(detail,m+1,ht-124,inner-2,66);
    move(cancelButton,w-m-237,ht-46,80,30); move(openButton,w-m-148,ht-46,148,30);
    move(meter,m,ht-36,std::max(110,inner-267),9);
    RECT lr{}; GetClientRect(list,&lr); int available=logical(lr.right)-4-GetSystemMetrics(SM_CXVSCROLL)*96/dpi;
    int before=78,after=78,pct=76,remaining=available-before-after-pct;
    int name=remaining*52/100;
    int widths[]={name,remaining-name,pct,before,after};
    for (int i=0;i<5;++i) ListView_SetColumnWidth(list,i,px(std::max(30,widths[i])));
    InvalidateRect(win,nullptr,TRUE);
}
void solid(HDC dc,RECT r,COLORREF c) { HBRUSH b=CreateSolidBrush(c); FillRect(dc,&r,b); DeleteObject(b); }
void roundBox(HDC dc,RECT rect,COLORREF bg,COLORREF edge,int round=10) {
    HPEN pen=CreatePen(PS_SOLID,px(1),edge); HBRUSH brush=CreateSolidBrush(bg);
    HGDIOBJ p=SelectObject(dc,pen),b=SelectObject(dc,brush);
    RoundRect(dc,rect.left,rect.top,rect.right-1,rect.bottom-1,px(round),px(round));
    SelectObject(dc,p); SelectObject(dc,b); DeleteObject(pen); DeleteObject(brush);
}
void words(HDC dc,std::wstring s,RECT r,HFONT f,COLORREF c,UINT align=DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS) {
    HGDIOBJ old=SelectObject(dc,f); SetTextColor(dc,c); SetBkMode(dc,TRANSPARENT);
    DrawTextW(dc,s.c_str(),(int)s.size(),&r,align|DT_NOPREFIX); SelectObject(dc,old);
}
void strokes(HDC dc,int cx,int cy,COLORREF color,int scale) {
    auto x=[&](int dx){return cx+px(dx*scale)/2;}; auto y=[&](int dy){return cy+px(dy*scale)/2;};
    auto line=[&](std::initializer_list<POINT> p){Polyline(dc,p.begin(),(int)p.size());};
    HPEN pen=CreatePen(PS_SOLID,std::max(2,px(scale)),color); auto old=SelectObject(dc,pen);
    line({{x(0),y(-9)},{x(0),y(3)}}); line({{x(-5),y(-2)},{x(0),y(3)},{x(5),y(-2)}});
    line({{x(-9),y(3)},{x(-9),y(9)},{x(9),y(9)},{x(9),y(3)}});
    SelectObject(dc,old); DeleteObject(pen);
}
void drawZone(DRAWITEMSTRUCT* d) {
    RECT r=d->rcItem; int cx=(r.left+r.right)/2;
    solid(d->hDC,r,paper); roundBox(d->hDC,r,(d->itemState&ODS_SELECTED)?RGB(218,235,221):mint,border,16);
    RECT badge{cx-px(19),px(17),cx+px(19),px(55)}; roundBox(d->hDC,badge,green,green,11);
    strokes(d->hDC,cx,px(36),white,2);
    RECT first{px(5),px(62),r.right-px(5),px(91)};
    words(d->hDC,L"Drop audio or video here",first,heading,ink,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    RECT sub{px(5),px(96),r.right-px(5),px(120)};
    words(d->hDC,L"Click to choose files  ·  Copies save beside each original",sub,normal,soft,DT_CENTER|DT_SINGLELINE|DT_VCENTER);
    if (d->itemState&ODS_FOCUS) { InflateRect(&r,-px(5),-px(5)); DrawFocusRect(d->hDC,&r); }
}
void paint() {
    PAINTSTRUCT ps{}; HDC dc=BeginPaint(win,&ps); RECT client{}; GetClientRect(win,&client);
    FillRect(dc,&ps.rcPaint,pageBrush);
    RECT logo{px(24),px(23),px(67),px(66)}; roundBox(dc,logo,green,green,10); strokes(dc,px(46),px(44),white,2);
    RECT name{px(79),px(11),client.right-px(200),px(52)}; words(dc,L"Under 20",name,title,ink);
    RECT sub{px(80),px(49),client.right-px(198),px(74)};
    words(dc,L"Local media compressor",sub,normal,soft);
    if (!tools.ffmpeg.empty()) {
        RECT cap{client.right-px(204),px(14),client.right-px(25),px(39)};
        words(dc,L"Limit   < 20,000,000 bytes",cap,small,soft,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    }
    RECT q{px(24),px(240),px(170),px(268)}; words(dc,L"Queue",q,heading,ink);
    int done=0,left=0; for (auto& j:jobs) {
        if (j->status==Status::Waiting || j->status==Status::Working) ++left;
        if (j->status==Status::Saved) ++done;
    }
    std::wstring counts=jobs.empty()?L"0 files":std::to_wstring(jobs.size())+(jobs.size()==1?L" file":L" files")+
        L"  ·  "+std::to_wstring(done)+L" saved"+(left?L"  ·  "+std::to_wstring(left)+L" pending":L"");
    RECT count{px(170),px(241),client.right-px(25),px(269)};
    words(dc,counts,count,small,soft,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
    EndPaint(win,&ps);
}
int selected() { return ListView_GetNextItem(list,-1,LVNI_SELECTED); }
void setText(int id,int col,const std::wstring& value) {
    ListView_SetItemText(list,id,col,const_cast<wchar_t*>(value.c_str()));
}
std::wstring phaseFor(Job& j) {
    switch (j.status) {
        case Status::Waiting:return L"Queued";
        case Status::Working:return j.cancel.load()?L"Canceling…":j.phase;
        case Status::Saved:return L"Saved";
        case Status::Skipped:return L"Skipped";
        case Status::Cancelled:return L"Canceled";
        default:return L"Couldn’t finish";
    }
}
void display(int id) {
    auto& j=*jobs.at(id);
    setText(id,1,phaseFor(j)); setText(id,2,j.status==Status::Working?std::to_wstring(j.percent)+L"%":j.status==Status::Saved?L"100%":L"—");
    setText(id,3,j.result.before?wide(sizeText(j.result.before)):L"—");
    setText(id,4,j.status==Status::Saved?wide(sizeText(j.result.after)):L"—");
}
void details() {
    int id=selected(); std::wstring text; int progress=0;
    bool mayCancel=false,mayOpen=false;
    if (id<0 || id>=(int)jobs.size()) text=tools.ffmpeg.empty()?L"Locate your installed FFmpeg to start.":L"Ready for files.";
    else {
        auto& j=*jobs[id]; progress=j.status==Status::Saved?100:j.percent;
        mayCancel=(j.status==Status::Waiting||j.status==Status::Working)&&!j.cancel.load();
        mayOpen=!j.result.path.empty() && (j.status==Status::Saved || j.status==Status::Skipped);
        if (j.status==Status::Saved)
            text=j.result.path.filename().wstring()+L"\r\n"+wide(j.result.message)+L"  ·  "+std::to_wstring(j.result.after)+L" bytes";
        else if (j.status==Status::Working || j.status==Status::Waiting)
            text=j.input.filename().wstring()+L"\r\n"+phaseFor(j)+(j.keep?L"  ·  Keep WAV for audio":L"");
        else text=j.input.filename().wstring()+L"\r\n"+wide(j.result.message);
    }
    int length=GetWindowTextLengthW(detail); std::wstring prev(length+1,0); GetWindowTextW(detail,prev.data(),length+1); prev.resize(length);
    if (text!=prev) SetWindowTextW(detail,text.c_str());
    SendMessageW(meter,PBM_SETPOS,progress,0); EnableWindow(cancelButton,mayCancel); EnableWindow(openButton,mayOpen);
}
void changed() { details(); RECT r{}; GetClientRect(win,&r); r.top=px(231);r.bottom=px(272);InvalidateRect(win,&r,FALSE); }
void pickRow(int id) {
    ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_SetItemState(list,id,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED); ListView_EnsureVisible(list,id,FALSE);
}
void enqueue(const std::vector<fs::path>& paths) {
    if (closing || paths.empty()) return;
    bool keep=SendMessageW(box,BM_GETCHECK,0,0)==BST_CHECKED;
    int first=(int)jobs.size();
    for (auto& path:paths) {
        auto j=std::make_shared<Job>(); j->input=path; j->keep=keep; std::error_code ec;
        auto size=fs::file_size(path,ec); if (!ec) j->result.before=size;
        int id=(int)jobs.size(); jobs.push_back(j);
        auto name=path.filename().wstring(); LVITEMW item{}; item.mask=LVIF_TEXT; item.iItem=id; item.pszText=name.data();
        ListView_InsertItem(list,&item); display(id);
    }
    if (selected()<0) pickRow(first);
    changed(); PostMessageW(win,startMsg,0,0);
}
void pickFiles() {
    std::vector<wchar_t> buff(262144,0);
    OPENFILENAMEW dlg{}; dlg.lStructSize=sizeof(dlg); dlg.hwndOwner=win; dlg.lpstrFile=buff.data();
    dlg.nMaxFile=(DWORD)buff.size(); dlg.lpstrTitle=L"Choose audio or video files";
    dlg.lpstrFilter=L"All files\0*.*\0Common media\0*.mp4;*.mkv;*.mov;*.webm;*.avi;*.wav;*.flac;*.mp3;*.m4a;*.aac;*.ogg;*.opus\0\0";
    dlg.Flags=OFN_EXPLORER|OFN_ALLOWMULTISELECT|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dlg)) return;
    fs::path base(buff.data()); const wchar_t* p=buff.data()+wcslen(buff.data())+1; std::vector<fs::path> files;
    if (!*p) files.push_back(base);
    else for (;*p;p+=wcslen(p)+1) files.push_back(base/p);
    enqueue(files);
}
void takeDrop(HDROP drop) {
    std::vector<fs::path> files; UINT n=DragQueryFileW(drop,0xffffffff,nullptr,0);
    for (UINT i=0;i<n;++i) {
        UINT len=DragQueryFileW(drop,i,nullptr,0); std::wstring p(len+1,0);
        if (DragQueryFileW(drop,i,p.data(),len+1)) {p.resize(len); files.emplace_back(p);}
    }
    DragFinish(drop); enqueue(files);
}
LRESULT CALLBACK zoneSubclass(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    if (m==WM_DROPFILES) {takeDrop((HDROP)w);return 0;}
    if (m==WM_NCDESTROY) RemoveWindowSubclass(h,zoneSubclass,1);
    return DefSubclassProc(h,m,w,l);
}
void reveal() {
    int id=selected(); if (id<0 || jobs[id]->result.path.empty()) return;
    auto path=jobs[id]->result.path; PIDLIST_ABSOLUTE item{};
    if (SUCCEEDED(SHParseDisplayName(path.c_str(),nullptr,&item,0,nullptr))) {
        PIDLIST_ABSOLUTE folder=ILCloneFull(item);
        if (folder && ILRemoveLastID(folder)) {
            PCUITEMID_CHILD childId=ILFindLastID(item);
            auto hr=SHOpenFolderAndSelectItems(folder,1,&childId,0);
            CoTaskMemFree(folder); CoTaskMemFree(item); if (SUCCEEDED(hr)) return;
        } else { if (folder) CoTaskMemFree(folder); CoTaskMemFree(item); }
    }
    ShellExecuteW(win,L"open",path.parent_path().c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}
void startNext() {
    if (closing || active!=-1 || tools.ffmpeg.empty()) return;
    int id=0; while (id<(int)jobs.size() && jobs[id]->status!=Status::Waiting) ++id;
    if (id==(int)jobs.size()) return;
    active=id; auto j=jobs[id]; j->status=Status::Working; j->phase=L"Starting"; display(id); changed();
    HWND target=win; auto binaries=tools;
    worker=std::thread([j,id,target,binaries] {
        Hooks hooks{j->cancel,[=](std::string phase,int pc) {
            auto e=new Update{id,pc,wide(phase)}; if (!PostMessageW(target,updateMsg,0,(LPARAM)e)) delete e;
        },[=](std::string question) {
            Approval a{id,wide(question)}; return SendMessageW(target,approvalMsg,0,(LPARAM)&a)==IDYES;
        }};
        auto r=compress(j->input,j->keep,binaries,hooks);
        auto e=new Finished{id,std::move(r)}; if (!PostMessageW(target,finishMsg,0,(LPARAM)e)) delete e;
    });
}
void cancelRow(int id) {
    if (id<0) return;
    auto& j=*jobs[id]; if (j.status!=Status::Waiting && j.status!=Status::Working) return;
    j.cancel.store(true);
    if (j.status==Status::Waiting) {j.status=Status::Cancelled;j.result.message="Canceled before processing";}
    display(id); changed();
}
void setup() {
    pageBrush=CreateSolidBrush(paper);
    zone=child(L"BUTTON",L"Drop audio or video here, or click to choose files",BS_OWNERDRAW|WS_TABSTOP,Drop,WS_EX_ACCEPTFILES);
    SetWindowSubclass(zone,zoneSubclass,1,0); DragAcceptFiles(zone,TRUE); DragAcceptFiles(win,TRUE);
    box=child(L"BUTTON",L"Keep WAV (audio only)",WS_TABSTOP|BS_AUTOCHECKBOX,Keep);
    list=child(WC_LISTVIEWW,L"Compression queue",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_BORDER,Queue);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
    SetWindowTheme(list,L"Explorer",nullptr); ListView_SetBkColor(list,white); ListView_SetTextColor(list,ink);
    const wchar_t* cols[]={L"File",L"Status",L"Progress",L"Before",L"After"};
    for (int i=0;i<5;++i) { LVCOLUMNW c{}; c.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_FMT; c.cx=90;
        c.pszText=const_cast<wchar_t*>(cols[i]); c.fmt=i>=2?LVCFMT_RIGHT:LVCFMT_LEFT; ListView_InsertColumn(list,i,&c); }
    detail=child(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,Detail);
    cancelButton=child(L"BUTTON",L"Cancel",BS_PUSHBUTTON|WS_TABSTOP,Cancel);
    openButton=child(L"BUTTON",L"Open file location",BS_PUSHBUTTON|WS_TABSTOP,Open);
    locateButton=child(L"BUTTON",L"Locate FFmpeg…",BS_PUSHBUTTON|WS_TABSTOP,Locate);
    meter=child(PROGRESS_CLASSW,L"Selected file progress",PBS_SMOOTH,Meter);
    tools=discover(); if (!tools.ffmpeg.empty()) ShowWindow(locateButton,SW_HIDE);
    fonts(); layout(); details();
}
LRESULT CALLBACK wndProc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    switch (message) {
    case WM_CREATE: { win=hwnd; HDC dc=GetDC(win);dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(win,dc);setup();return 0; }
    case WM_GETMINMAXINFO: { auto m=(MINMAXINFO*)l;m->ptMinTrackSize={px(640),px(525)};return 0; }
    case WM_SIZE: if (list && w!=SIZE_MINIMIZED) layout();return 0;
    case WM_DPICHANGED: { dpi=HIWORD(w);fonts();RECT* r=(RECT*)l;
        SetWindowPos(win,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);layout();return 0; }
    case WM_PAINT: paint();return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: { HDC dc=(HDC)w;SetTextColor(dc,ink);SetBkColor(dc,paper);return (LRESULT)pageBrush;}
    case WM_DRAWITEM: if (((DRAWITEMSTRUCT*)l)->CtlID==Drop) {drawZone((DRAWITEMSTRUCT*)l);return TRUE;}break;
    case WM_DROPFILES: takeDrop((HDROP)w);return 0;
    case WM_NOTIFY: if (((NMHDR*)l)->hwndFrom==list && ((NMHDR*)l)->code==LVN_ITEMCHANGED && detail) details();break;
    case WM_COMMAND:
        switch (LOWORD(w)) {
            case Drop: if (HIWORD(w)==BN_CLICKED) pickFiles();return 0;
            case Cancel: cancelRow(selected());return 0;
            case Open: reveal();return 0;
            case Locate: if (chooseTools()) {changed();PostMessageW(win,startMsg,0,0);}return 0;
        }
        break;
    case startMsg: startNext();return 0;
    case updateMsg: {
        std::unique_ptr<Update> e((Update*)l); auto& j=*jobs[e->id];j.percent=e->percent;j.phase=std::move(e->phase);
        display(e->id);if (selected()==e->id) details();return 0;
    }
    case approvalMsg: {
        auto a=(Approval*)l; if (closing || jobs[a->id]->cancel.load()) return IDNO;
        std::wstring text=jobs[a->id]->input.filename().wstring()+L"\n\n"+a->text;
        return MessageBoxW(win,text.c_str(),L"Approve a change to this copy",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION);
    }
    case finishMsg: {
        std::unique_ptr<Finished> e((Finished*)l);if (worker.joinable()) worker.join();
        auto& j=*jobs[e->id];j.result=std::move(e->result);active=-1;
        switch (j.result.state) {
            case Result::Saved: j.status=Status::Saved;j.percent=100;break;
            case Result::Skipped:j.status=Status::Skipped;j.percent=0;break;
            case Result::Canceled:j.status=Status::Cancelled;j.percent=0;break;
            case Result::Failed:j.status=Status::Failed;j.percent=0;break;
        }
        display(e->id);changed();if (closing) DestroyWindow(win);else PostMessageW(win,startMsg,0,0);return 0;
    }
    case WM_CLOSE:
        if (active>=0) {
            closing=true;for (int i=0;i<(int)jobs.size();++i) cancelRow(i);
            EnableWindow(zone,FALSE);SetWindowTextW(win,L"Under 20 · Canceling…");return 0;
        }
        DestroyWindow(win);return 0;
    case WM_DESTROY:
        DragAcceptFiles(hwnd,FALSE);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    appInstance=instance;
    INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_LISTVIEW_CLASSES|ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};InitCommonControlsEx(&cc);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance;wc.lpfnWndProc=wndProc;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.lpszClassName=windowClass;
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(100));wc.hIconSm=wc.hIcon;
    if (!RegisterClassExW(&wc)) return 1;
    HDC screen=GetDC(nullptr);dpi=GetDeviceCaps(screen,LOGPIXELSX);ReleaseDC(nullptr,screen);
    HWND main=CreateWindowExW(WS_EX_CONTROLPARENT,windowClass,L"Under 20 · Media compressor",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,px(840),px(645),nullptr,nullptr,instance,nullptr);
    if (!main) return 1;
    ShowWindow(main,show);UpdateWindow(main);
    // Explorer can also drop files onto the executable itself.
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (argv) {std::vector<fs::path> paths;for (int i=1;i<argc;++i) paths.emplace_back(argv[i]);LocalFree(argv);enqueue(paths);}
    MSG msg{};while (GetMessageW(&msg,nullptr,0,0)>0) {
        if (!IsDialogMessageW(main,&msg)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    for (auto f:{normal,small,medium,title,heading}) if (f) DeleteObject(f);
    DeleteObject(pageBrush);CoUninitialize();return (int)msg.wParam;
}
