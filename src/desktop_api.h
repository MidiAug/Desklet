#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <servprov.h>
#include <objectarray.h>
#include <winstring.h>
#include <wrl/client.h>
#include <string>
#include <inspectable.h>

// Interface identifiers / ordering cross-checked against the MIT-licensed
// Ciantic/VirtualDesktopAccessor rust branch, 2026-10-29.
// Only exact queried interfaces are called. Never guess a fallback vtable.
namespace desklet {
using Microsoft::WRL::ComPtr;
// Exact IInspectable prefix verified against the primary Win10Desktops.h
// declaration in RadAd/DesktopsMenu (slots 6..12). QI + thumbnail HWND verify
// the interface before any SetCloak call; no guessed fallback vtable.
struct __declspec(uuid("372E1D3B-38D3-42E4-A15B-8AB2B178F513")) ApplicationViewVisibility : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetFocus()=0;
    virtual HRESULT STDMETHODCALLTYPE SwitchTo()=0;
    virtual HRESULT STDMETHODCALLTYPE TryInvokeBack(IUnknown*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetThumbnailWindow(HWND*)=0;
    virtual HRESULT STDMETHODCALLTYPE GetMonitor(IUnknown**)=0;
    virtual HRESULT STDMETHODCALLTYPE GetVisibility(int*)=0;
    virtual HRESULT STDMETHODCALLTYPE SetCloak(int,int)=0;
};
struct __declspec(uuid("3F07F4BE-B107-441A-AF0F-39D82529072C")) Desktop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IUnknown*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetName(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetWallpaper(void**) = 0;
};
struct __declspec(uuid("53F5CA0B-158F-4124-900C-057158060B27")) DesktopInternal : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveView(IUnknown*, Desktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CanMove(IUnknown*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrent(Desktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAdjacent(Desktop*, int, Desktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE Switch(Desktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchAndMove(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Create(Desktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveDesktop(Desktop*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove(Desktop*, Desktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Find(GUID*, Desktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSwitchViews(Desktop*, IObjectArray**, IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetName(Desktop*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetWallpaper(Desktop*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateWallpaper(void*) = 0;
};
struct __declspec(uuid("1841C6D7-4F9D-42C0-AF41-8747538F10E5")) ViewCollection : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppId(PCWSTR,IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND,IUnknown**) = 0;
};
struct __declspec(uuid("4CE81583-1E4C-4632-A621-07A53543148F")) PinnedApps : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsAppPinned(PCWSTR,BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE PinApp(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnpinApp(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsViewPinned(IUnknown*,BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE PinView(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnpinView(IUnknown*) = 0;
};
struct __declspec(uuid("B9E5E94D-233E-49AB-AF5C-2B4541C3AADE")) DesktopNotification : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Created(Desktop*)=0;
    virtual HRESULT STDMETHODCALLTYPE DestroyBegin(Desktop*,Desktop*)=0;
    virtual HRESULT STDMETHODCALLTYPE DestroyFailed(Desktop*,Desktop*)=0;
    virtual HRESULT STDMETHODCALLTYPE Destroyed(Desktop*,Desktop*)=0;
    virtual HRESULT STDMETHODCALLTYPE Moved(Desktop*,INT64,INT64)=0;
    virtual HRESULT STDMETHODCALLTYPE NameChanged(Desktop*,HSTRING)=0;
    virtual HRESULT STDMETHODCALLTYPE ViewChanged(IUnknown*)=0;
    virtual HRESULT STDMETHODCALLTYPE CurrentChanged(Desktop*,Desktop*)=0;
    virtual HRESULT STDMETHODCALLTYPE WallpaperChanged(Desktop*,HSTRING)=0;
    virtual HRESULT STDMETHODCALLTYPE Switched(Desktop*)=0;
    virtual HRESULT STDMETHODCALLTYPE RemoteConnected(Desktop*)=0;
};
struct __declspec(uuid("0CD45E71-D927-4F15-8B0A-8FEF525337BF")) NotificationService : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Register(DesktopNotification*,DWORD*)=0;
    virtual HRESULT STDMETHODCALLTYPE Unregister(DWORD)=0;
};
class DesktopApi {
public:
    ComPtr<DesktopInternal> internal;
    ComPtr<IVirtualDesktopManager> manager;
    ComPtr<Desktop> origin;
    ComPtr<ViewCollection> views;
    ComPtr<PinnedApps> pinned;
    ComPtr<NotificationService> notifications;
    HRESULT last = E_FAIL;
    bool connect() {
        ComPtr<IServiceProvider> shell;
        const CLSID immersive = {0xC2F03A33,0x21F5,0x47FA,{0xB4,0xBB,0x15,0x63,0x62,0xA2,0xF2,0x39}};
        const GUID service = {0xC5E0CDCA,0x7B6E,0x41B2,{0x9F,0xC4,0xD9,0x39,0x75,0xCC,0x46,0x7B}};
        last = CoCreateInstance(immersive, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell));
        if (FAILED(last)) return false;
        last = shell->QueryService(service, IID_PPV_ARGS(&internal));
        if (FAILED(last)) return false;
        const GUID pinService={0xb5a399e7,0x1c87,0x46b8,{0x88,0xe9,0xfc,0x57,0x47,0xb1,0x71,0xbd}};
        shell->QueryService(__uuidof(ViewCollection),IID_PPV_ARGS(&views));
        shell->QueryService(pinService,IID_PPV_ARGS(&pinned));
        const GUID notifyService={0xA501FDEC,0x4A09,0x464C,{0xAE,0x4E,0x1B,0x9C,0x21,0xB8,0x49,0x18}};
        shell->QueryService(notifyService,IID_PPV_ARGS(&notifications));
        last = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
        if (FAILED(last)) return false;
        UINT count = 0;
        last = internal->GetCount(&count);
        if (FAILED(last) || count == 0 || count > 256) return false;
        last = internal->GetCurrent(&origin);
        return SUCCEEDED(last) && origin;
    }
    GUID id(Desktop* desktop) const { GUID g{}; if (desktop) desktop->GetId(&g); return g; }
    GUID currentId() const { ComPtr<Desktop> d; if (internal) internal->GetCurrent(&d); return id(d.Get()); }
    bool isCurrent(Desktop* desktop) const { return id(desktop) == currentId(); }
    bool own(HWND h) const { DWORD pid = 0; GetWindowThreadProcessId(h, &pid); return pid == GetCurrentProcessId(); }
    bool moveOwned(HWND h, Desktop* d) {
        if (!own(h) || !manager || !d) { last = E_ACCESSDENIED; return false; }
        GUID g = id(d);
        last = manager->MoveWindowToDesktop(h, g);
        return SUCCEEDED(last);
    }
    bool create(ComPtr<Desktop>& out) { last = internal->Create(&out); return SUCCEEDED(last) && out; }
    bool pinnedWindow(HWND h) const {
        if(!views||!pinned)return false;ComPtr<IUnknown> view;BOOL yes=FALSE;
        if(FAILED(views->GetViewForHwnd(h,&view)))return false;
        return SUCCEEDED(pinned->IsViewPinned(view.Get(),&yes))&&yes;
    }
    bool name(Desktop* d, const wchar_t* value) {
        HSTRING text{}; HRESULT h=WindowsCreateString(value,(UINT32)wcslen(value),&text);
        if(FAILED(h))return false;last=internal->SetName(d,text);WindowsDeleteString(text);return SUCCEEDED(last);
    }
    bool switchTo(Desktop* d) { last = internal->Switch(d); return SUCCEEDED(last); }
};
}
