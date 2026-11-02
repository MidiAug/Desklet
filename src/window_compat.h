#pragma once
#include "app_policy.h"
#include "compat_core.h"
#include "desktop_api.h"
#include <algorithm>
#include <chrono>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <vector>

// Production adapter. No fixture IPC, activation launcher, desktop switching,
// process injection, taskbar pinning or changes to Windows multitasking
// settings.
namespace tongzhuo::livecompat {
namespace fs = std::filesystem;
namespace cc = tongzhuo::compat;
namespace apppolicy = tongzhuo::policy;
using namespace desklet;
struct PerfStats {
  bool enabled = false;
  size_t imageQueries = 0, showRequests = 0, masks = 0, appIdWrites = 0,
         scans = 0;
  double imageMs = 0, scanMs = 0, waitMs = 0, persistMs = 0;
  std::vector<double> transitions, maintenance;
  void write(std::ostream &out) const {
    out << "{\"image_queries\":" << imageQueries
        << ",\"show_requests\":" << showRequests
        << ",\"region_masks\":" << masks << ",\"appid_writes\":" << appIdWrites
        << ",\"scans\":" << scans << ",\"image_ms\":" << imageMs
        << ",\"scan_ms\":" << scanMs << ",\"wait_ms\":" << waitMs
        << ",\"persist_ms\":" << persistMs;
    auto array = [&](const char *name, const auto &v) {
      out << ",\"" << name << "\":[";
      for (size_t i = 0; i < v.size(); ++i)
        out << (i ? "," : "") << v[i];
      out << "]";
    };
    array("transition_ms", transitions);
    array("maintenance_ms", maintenance);
    out << "}";
  }
};
inline PerfStats perf;
struct Timer {
  double *sum{};
  std::chrono::steady_clock::time_point started =
      std::chrono::steady_clock::now();
  explicit Timer(double &value) {
    if (perf.enabled)
      sum = &value;
  }
  double elapsed() const {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
  }
  ~Timer() {
    if (sum)
      *sum += elapsed();
  }
};
inline std::wstring text(GUID g) {
  wchar_t s[64]{};
  StringFromGUID2(g, s, 64);
  return s;
}
inline GUID guid(const std::wstring &s) {
  GUID g{};
  CLSIDFromString(s.c_str(), &g);
  return g;
}
inline std::wstring lower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), towlower);
  return s;
}
inline std::string jsonString(const std::wstring &s) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string utf(n, 0), out = "\"";
  if (n)
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), utf.data(), n,
                        nullptr, nullptr);
  for (unsigned char c : utf) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 32)
      out += '?';
    else
      out += (char)c;
  }
  return out + '"';
}
inline ULONGLONG birth(HANDLE p) {
  FILETIME a{}, b{}, c{}, d{};
  return GetProcessTimes(p, &a, &b, &c, &d)
             ? (ULONGLONG(a.dwHighDateTime) << 32) | a.dwLowDateTime
             : 0;
}
inline std::wstring readProcessImage(HANDLE p) {
  wchar_t s[32768]{};
  DWORD n = 32768;
  bool ok = QueryFullProcessImageNameW(p, 0, s, &n) != FALSE;
  return ok ? std::wstring(s, n) : L"";
}
inline std::wstring processImage(HANDLE p) {
  Timer timer(perf.imageMs);
  if (perf.enabled)
    ++perf.imageQueries;
  return readProcessImage(p);
}
inline std::wstring image(DWORD pid) {
  Timer timer(perf.imageMs);
  if (perf.enabled)
    ++perf.imageQueries;
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p)
    return {};
  auto value = readProcessImage(p);
  CloseHandle(p);
  return value;
}
inline std::wstring cls(HWND h) {
  wchar_t s[256]{};
  GetClassNameW(h, s, 256);
  return s;
}
inline bool candidateClass(const std::wstring &c) {
  return c == L"Chrome_WidgetWin_1" || c == L"WeChatMainWndForPC" ||
         c == L"Qt51514QWindowIcon" || c == L"Qt51515QWindowIcon" ||
         c == L"mmui::MainWindow" || c == L"TXGuiFoundation";
}
inline bool wait(const std::function<bool()> &predicate, DWORD ms = 900) {
  Timer timer(perf.waitMs);
  auto end = GetTickCount64() + ms;
  do {
    if (predicate())
      return true;
    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) {
        PostQuitMessage((int)m.wParam);
        return false;
      }
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  } while (GetTickCount64() < end);
  return predicate();
}
inline std::wstring appId(HWND h) {
  ComPtr<IPropertyStore> p;
  PROPVARIANT v{};
  std::wstring r;
  if (SUCCEEDED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&p))) &&
      SUCCEEDED(p->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR &&
      v.pwszVal)
    r = v.pwszVal;
  PropVariantClear(&v);
  return r;
}
inline bool setAppId(HWND h, const std::wstring &s) {
  if (perf.enabled)
    ++perf.appIdWrites;
  ComPtr<IPropertyStore> p;
  PROPVARIANT v{};
  if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&p))))
    return false;
  if (!s.empty() && FAILED(InitPropVariantFromString(s.c_str(), &v)))
    return false;
  bool ok =
      SUCCEEDED(p->SetValue(PKEY_AppUserModel_ID, v)) && SUCCEEDED(p->Commit());
  PropVariantClear(&v);
  return ok && appId(h) == s;
}
// A fixed-size recovery record contains only window identity/placement and the
// original AppID. No titles, document paths, command lines or application data.
struct Backup {
  UINT_PTR hwnd{};
  DWORD pid{};
  ULONGLONG created{};
  GUID home{};
  WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
  BOOL visible{}, browser{};
  wchar_t path[2048]{}, windowClass[256]{}, id[512]{};
  DWORD regionBytes{};
  BYTE region[8192]{};
};
struct Mask {
  HWND h{};
  HRGN original{};
  bool custom = false, ok = false;
  RECT before{};
  explicit Mask(HWND window) : h(window) {
    if (perf.enabled)
      ++perf.masks;
    GetWindowRect(h, &before);
    original = CreateRectRgn(0, 0, 0, 0);
    custom = original && GetWindowRgn(h, original) != ERROR;
    HRGN empty = CreateRectRgn(0, 0, 0, 0);
    ok = empty && SetWindowRgn(h, empty, FALSE);
    if (!ok && empty)
      DeleteObject(empty);
  }
  ~Mask() {
    if (ok && IsWindow(h)) {
      HRGN restore = custom ? original : nullptr;
      RECT after{};
      GetWindowRect(h, &after);
      LONG w = before.right - before.left, ht = before.bottom - before.top;
      if (custom && w > 0 && ht > 0 &&
          (w != after.right - after.left || ht != after.bottom - after.top)) {
        DWORD n = GetRegionData(original, 0, nullptr);
        std::vector<BYTE> data(n);
        if (n && GetRegionData(original, n, (RGNDATA *)data.data())) {
          XFORM scale{float(after.right - after.left) / w,  0, 0,
                      float(after.bottom - after.top) / ht, 0, 0};
          HRGN scaled = ExtCreateRegion(&scale, n, (RGNDATA *)data.data());
          if (scaled) {
            DeleteObject(original);
            original = scaled;
            restore = original;
          }
        }
      }
      if (SetWindowRgn(h, restore, TRUE) && restore)
        original = nullptr;
    }
    if (original)
      DeleteObject(original);
  }
};
inline bool valid(const Backup &b) {
  HWND h = (HWND)b.hwnd;
  DWORD pid{};
  if (!IsWindow(h) || !GetWindowThreadProcessId(h, &pid) || pid != b.pid ||
      cls(h) != b.windowClass)
    return false;
  HANDLE p =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!p)
    return false;
  bool ok = WaitForSingleObject(p, 0) == WAIT_TIMEOUT && birth(p) == b.created;
  CloseHandle(p);
  return ok && lower(image(pid)) == lower(b.path);
}
inline cc::Intent capture(HWND h) {
  WINDOWPLACEMENT p{sizeof(p)};
  GetWindowPlacement(h, &p);
  cc::Intent v;
  v.x = p.rcNormalPosition.left;
  v.y = p.rcNormalPosition.top;
  v.width = p.rcNormalPosition.right - v.x;
  v.height = p.rcNormalPosition.bottom - v.y;
  v.show = !IsWindowVisible(h) ? cc::Show::Hidden
           : IsIconic(h)       ? cc::Show::Minimized
           : IsZoomed(h)       ? cc::Show::Maximized
                               : cc::Show::Normal;
  return v;
}
inline bool show(HWND h, const cc::Intent &v) {
  auto actual = capture(h);
  if (actual.show == v.show &&
      (v.show == cc::Show::Hidden ||
       (actual.x == v.x && actual.y == v.y && actual.width == v.width &&
        actual.height == v.height)))
    return true;
  if (perf.enabled)
    ++perf.showRequests;
  if (v.show == cc::Show::Hidden) {
    ShowWindowAsync(h, SW_HIDE);
    return wait([&] { return !IsWindow(h) || !IsWindowVisible(h); });
  }
  WINDOWPLACEMENT p{sizeof(p)};
  GetWindowPlacement(h, &p);
  p.flags |= WPF_ASYNCWINDOWPLACEMENT;
  if (v.show == cc::Show::Normal)
    p.flags &= ~WPF_RESTORETOMAXIMIZED;
  p.rcNormalPosition = {v.x, v.y, v.x + v.width, v.y + v.height};
  p.showCmd = v.show == cc::Show::Minimized || v.show == cc::Show::Maximized
                  ? SW_SHOWMINNOACTIVE
                  : SW_SHOWNOACTIVATE;
  if (v.show == cc::Show::Maximized)
    p.flags |= WPF_RESTORETOMAXIMIZED;
  if (!SetWindowPlacement(h, &p))
    return false;
  // SW_MAXIMIZE can activate a foreign window. Retain maximize using a
  // no-activation placement followed by SW_SHOWNOACTIVATE instead.
  ShowWindowAsync(h, v.show == cc::Show::Minimized ? SW_SHOWMINNOACTIVE
                                                   : SW_SHOWNOACTIVATE);
  return wait([&] {
    if (!IsWindow(h) || !IsWindowVisible(h))
      return false;
    return v.show == cc::Show::Maximized   ? IsZoomed(h) != FALSE
           : v.show == cc::Show::Minimized ? IsIconic(h) != FALSE
                                           : !IsIconic(h) && !IsZoomed(h);
  });
}
inline GUID nativeHome(DesktopApi &api, HWND h) {
  GUID g{};
  if (api.manager)
    api.manager->GetWindowDesktopId(h, &g);
  return g;
}
inline HRESULT clearCurrentShellCloak(DesktopApi &api, HWND h) {
  DWORD cloak{};
  if (!IsWindowVisible(h) || nativeHome(api, h) != api.currentId() ||
      FAILED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloak, sizeof(cloak))) ||
      cloak != DWM_CLOAKED_SHELL)
    return E_ACCESSDENIED;
  ComPtr<IUnknown> view;
  ComPtr<ApplicationViewVisibility> visibility;
  HRESULT hr = api.views ? api.views->GetViewForHwnd(h, &view) : E_NOINTERFACE;
  if (FAILED(hr) || !view)
    return hr;
  hr = view.As(&visibility);
  if (FAILED(hr))
    return hr;
  HWND thumbnail{};
  hr = visibility->GetThumbnailWindow(&thumbnail);
  if (FAILED(hr) || thumbnail != h)
    return E_NOINTERFACE;
  // SetCloak(AVCT_DEFAULT, 0): remove Shell cloak, not application-owned cloak.
  // Flags verified against primary winspaces shell_cloak usage. Never SetFocus.
  return visibility->SetCloak(1, 0);
}
inline bool move(DesktopApi &api, HWND h, GUID target) {
  ComPtr<Desktop> d;
  ComPtr<IUnknown> view;
  if (!api.views || FAILED(api.internal->Find(&target, &d)) || !d)
    return false;
  if (!wait([&] {
        view.Reset();
        return SUCCEEDED(api.views->GetViewForHwnd(h, &view)) && view &&
               nativeHome(api, h) != GUID{};
      }))
    return false;
  if (nativeHome(api, h) != target &&
      FAILED(api.internal->MoveView(view.Get(), d.Get())))
    return false;
  return wait([&] { return nativeHome(api, h) == target; });
}
// Restore only this run's records. The companion reads the file only after its
// exact parent process exits; it never loads old enrollment/recovery records.
inline bool restore(DesktopApi &api, const Backup &b) {
  if (b.regionBytes > sizeof(b.region))
    return false;
  if (!valid(b))
    return true;
  HWND h = (HWND)b.hwnd;
  if (b.browser)
    return setAppId(h, b.id);
  HRGN empty = CreateRectRgn(0, 0, 0, 0);
  bool masked = empty && SetWindowRgn(h, empty, FALSE);
  if (!masked) {
    if (empty)
      DeleteObject(empty);
    return false;
  }
  auto v = capture(h);
  v.show = cc::Show::Normal;
  bool ok = show(h, v);
  ComPtr<Desktop> home;
  GUID target = b.home;
  if (FAILED(api.internal->Find(&target, &home)))
    target = api.currentId();
  if (ok)
    ok = move(api, h, target);
  if (ok) {
    v.x = b.placement.rcNormalPosition.left;
    v.y = b.placement.rcNormalPosition.top;
    v.width = b.placement.rcNormalPosition.right - v.x;
    v.height = b.placement.rcNormalPosition.bottom - v.y;
    v.show = !b.visible                                ? cc::Show::Hidden
             : b.placement.showCmd == SW_SHOWMINIMIZED ? cc::Show::Minimized
             : b.placement.showCmd == SW_SHOWMAXIMIZED ? cc::Show::Maximized
                                                       : cc::Show::Normal;
    ok = show(h, v);
  }
  if (ok && b.visible)
    ok = move(api, h, target);
  // Recover the region saved BEFORE mutation, never the interrupted zero mask.
  HRGN region = b.regionBytes ? ExtCreateRegion(nullptr, b.regionBytes,
                                                (const RGNDATA *)b.region)
                              : nullptr;
  if (!SetWindowRgn(h, region, TRUE) && region)
    DeleteObject(region);
  // Restoring a non-empty region can finish a deferred Shell registration.
  // Do not delete the recovery journal on an instantaneous pre-registration ID.
  if (ok && b.visible) {
    auto stableHome = [&] {
      ULONGLONG stable = 0;
      return wait(
          [&] {
            if (!valid(b) || !IsWindowVisible(h) ||
                nativeHome(api, h) != target) {
              stable = 0;
              return false;
            }
            if (!stable)
              stable = GetTickCount64();
            return GetTickCount64() - stable >= 40;
          },
          700);
    };
    if (!stableHome())
      ok = move(api, h, target) && stableHome();
  }
  return ok;
}
inline int guardian(DWORD pid, ULONGLONG created, const fs::path &file) {
  HANDLE parent =
      OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (parent) {
    if (birth(parent) != created) {
      CloseHandle(parent);
      return 2;
    }
    WaitForSingleObject(parent, INFINITE);
    CloseHandle(parent);
  }
  std::ifstream in(file, std::ios::binary);
  if (!in)
    return 0;
  DWORD magic{}, count{};
  in.read((char *)&magic, sizeof(magic));
  in.read((char *)&count, sizeof(count));
  if (magic != 0x545a4334 || count > 256)
    return 3;
  std::vector<Backup> records(count);
  if (!in.read((char *)records.data(), sizeof(Backup) * count))
    return 4;
  in.close();
  DesktopApi api;
  if (!api.connect())
    return 5;
  bool ok = true;
  for (const auto &b : records)
    ok = restore(api, b) && ok;
  if (ok) {
    std::error_code ec;
    fs::remove(file, ec);
  }
  return ok ? 0 : 6;
}

class Controller final : public cc::Windows {
public:
  struct LogicalView {
    cc::WindowId window{};
    GUID desktop{};
    DWORD pid{};
    std::wstring path;
    cc::Show show = cc::Show::Hidden;
  };
  struct WindowTiming {
    cc::WindowId window{};
    std::wstring exe;
    std::wstring failureStage;
    bool browser = false, ok = false, masked = false;
    double totalMs = 0, registerMs = 0, moveMs = 0, stateMs = 0;
  };
  struct ShellRepair {
    cc::WindowId window{};
    std::wstring exe;
    DWORD before{}, after{};
    HRESULT result = E_FAIL;
    double ms = 0;
  };

private:
  struct Entry {
    Backup backup;
    std::wstring home, appliedId;
    std::unique_ptr<cc::Core> core;
    std::shared_ptr<void> process;
    std::wstring identity;
    bool allowUnpin = false, pendingBind = false;
    bool touched = false, failed = false;
    uint64_t lastShellRepair = 0;
  };
  DesktopApi &api;
  std::map<cc::WindowId, Entry> entries;
  std::wstring current;
  bool working = false, active = false;
  bool discovering = false;
  bool discoveryDirty = true;
  ULONGLONG lastDiscovery = 0;
  std::map<DWORD, std::wstring> discoveryImages;
  apppolicy::Store policies;
  std::wstring windowIdentity(HWND h, const std::wstring &path = {}) {
    DWORD pid{};
    GetWindowThreadProcessId(h, &pid);
    auto id = appId(h);
    auto resolved = path.empty() ? discoveryImage(pid) : path;
    if (id.empty() && lower(fs::path(resolved).filename().wstring()) ==
                          L"applicationframehost.exe")
      return {};
    return apppolicy::identity(resolved, id,
                               id.empty() ? apppolicy::family(pid) : L"");
  }
  std::wstring discoveryImage(DWORD pid) {
    if (!discovering)
      return image(pid);
    auto at = discoveryImages.find(pid);
    if (at != discoveryImages.end())
      return at->second;
    return discoveryImages.emplace(pid, image(pid)).first->second;
  }
  fs::path journal;
  HANDLE guardProcess{};
  // Tests inject a PID/creation-time/nonce predicate, never a global allow
  // flag.
  std::function<int(HWND)> testPolicy;
  size_t failures = 0, skippedBrowser = 0;
  std::vector<WindowTiming> lastTimings;
  WindowTiming *timing{};
  bool mayLocalizeBrowser(const Entry &e) const {
    return e.allowUnpin ||
           (e.backup.browser &&
            (bool(testPolicy) ||
             lower(fs::path(e.backup.path).filename().wstring()) ==
                 L"chrome.exe"));
  }
  bool localizeBrowser(Entry &e) {
    if (!mayLocalizeBrowser(e))
      return true;
    if (!api.pinned || !api.views)
      return false;
    BOOL app = FALSE, window = FALSE;
    if (!std::wstring(e.backup.id).empty() &&
        FAILED(api.pinned->IsAppPinned(e.backup.id, &app)))
      return false;
    GUID target = guid(e.home);
    ComPtr<Desktop> source;
    // Pinned windows can report Shell's shared-desktop sentinel, which is not
    // any desktop returned by GetDesktops/Find. Never use it as a real home.
    bool realHome = SUCCEEDED(api.internal->Find(&target, &source)) && source;
    // Shell may discard the view of a hidden shared HWND. Its authenticated
    // enrollment and real cached home remain valid; transfer will temporarily
    // register it under a mask. Requiring a view here blocks that recovery.
    ComPtr<IUnknown> view;
    if (FAILED(api.views->GetViewForHwnd((HWND)e.backup.hwnd, &view)) || !view)
      return hiddenViewMayRegister(!IsWindowVisible((HWND)e.backup.hwnd),
                                   bool(e.core), e.pendingBind, realHome, app);
    if (FAILED(api.pinned->IsViewPinned(view.Get(), &window)))
      return hiddenViewMayRegister(!IsWindowVisible((HWND)e.backup.hwnd),
                                   bool(e.core), e.pendingBind, realHome, app);
    if (!app && !window && realHome)
      return true;
    if (!realHome)
      target = api.currentId();
    if (!touch(e))
      return false;
    // These are virtual-desktop pins, not fixed taskbar shortcuts. Chrome's
    // all-desktops flag conflicts with the requested per-desktop behavior.
    if (app && FAILED(api.pinned->UnpinApp(e.backup.id)))
      return false;
    if (FAILED(api.pinned->IsViewPinned(view.Get(), &window)))
      return false;
    if (window && FAILED(api.pinned->UnpinView(view.Get())))
      return false;
    app = std::wstring(e.backup.id).empty() ? FALSE : TRUE;
    window = TRUE;
    bool cleared = (std::wstring(e.backup.id).empty() ||
                    SUCCEEDED(api.pinned->IsAppPinned(e.backup.id, &app))) &&
                   !app &&
                   SUCCEEDED(api.pinned->IsViewPinned(view.Get(), &window)) &&
                   !window;
    // Shell can assign an unpinned view to the active desktop. Preserve its
    // recorded source desktop instead of adopting that side effect as home.
    bool placed = cleared && move(api, (HWND)e.backup.hwnd, target);
    if (placed)
      e.home = text(target);
    return placed;
  }
  bool ensureGuardian() {
    if (guardProcess)
      return WaitForSingleObject(guardProcess, 0) == WAIT_TIMEOUT;
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, 32768);
    std::wstring args = L"\"" + std::wstring(exe) + L"\" --compat-guardian " +
                        std::to_wstring(GetCurrentProcessId()) + L" " +
                        std::to_wstring(birth(GetCurrentProcess())) + L" \"" +
                        journal.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, args.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
      return false;
    CloseHandle(pi.hThread);
    guardProcess = pi.hProcess;
    return true;
  }
  bool persist() {
    Timer timer(perf.persistMs);
    if (journal.empty())
      return false;
    std::error_code ec;
    fs::create_directories(journal.parent_path(), ec);
    auto temp = journal;
    temp += L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
      return false;
    DWORD magic = 0x545a4334, count = 0, n{};
    for (auto &[id, e] : entries)
      if (e.touched)
        ++count;
    bool ok =
        WriteFile(f, &magic, sizeof(magic), &n, nullptr) && n == sizeof(magic);
    ok = WriteFile(f, &count, sizeof(count), &n, nullptr) &&
         n == sizeof(count) && ok;
    for (auto &[id, e] : entries)
      if (e.touched)
        ok = WriteFile(f, &e.backup, sizeof(Backup), &n, nullptr) &&
             n == sizeof(Backup) && ok;
    ok = FlushFileBuffers(f) && ok;
    CloseHandle(f);
    if (!ok)
      return false;
    return MoveFileExW(temp.c_str(), journal.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
           FALSE;
  }
  bool touch(Entry &e) {
    if (!active || !ensureGuardian())
      return false;
    if (e.touched)
      return true;
    e.touched = true;
    if (persist())
      return true;
    e.touched = false;
    return false;
  }
  int policy(HWND h) {
    int fixtureDefault = -1;
    if (testPolicy) {
      fixtureDefault = testPolicy(h);
      if (!fixtureDefault)
        return 0;
    }
    DWORD pid{};
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == GetCurrentProcessId())
      return 0;
    auto c = cls(h);
    if (GetWindow(h, GW_OWNER) || GetWindowLongPtrW(h, GWL_EXSTYLE) &
                                      (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
      return 0;
    if (c == L"Progman" || c == L"WorkerW" || c == L"Shell_TrayWnd" ||
        c == L"Shell_SecondaryTrayWnd" ||
        GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD)
      return 0;
    if (!candidateClass(c) && policies.empty() && fixtureDefault < 0)
      return 0;
    auto path = lower(discoveryImage(pid)),
         name = fs::path(path).filename().wstring();
    if (!policies.empty()) {
      auto selected = policies.get(windowIdentity(h, path));
      if (selected == apppolicy::Mode::Native)
        return 0;
      if (selected == apppolicy::Mode::Shared)
        return 1;
      if (selected == apppolicy::Mode::Separate)
        return 2;
    }
    if (fixtureDefault >= 0)
      return fixtureDefault;
    if (name == L"chrome.exe" || name == L"msedge.exe" ||
        name == L"brave.exe" || name == L"vivaldi.exe")
      return c == L"Chrome_WidgetWin_1" ? 2 : 0;
    // Explicit app families, never infer single-instance from a window count.
    if (name == L"codex.exe" ||
        (name == L"chatgpt.exe" &&
         path.find(L"\\openai.codex_") != std::wstring::npos))
      return c == L"Chrome_WidgetWin_1" ? 1 : 0;
    if (name == L"wechat.exe" || name == L"weixin.exe")
      return c == L"WeChatMainWndForPC" || c == L"Qt51514QWindowIcon" ||
                     c == L"Qt51515QWindowIcon" || c == L"mmui::MainWindow"
                 ? 1
                 : 0;
    if (name == L"qq.exe")
      return c == L"TXGuiFoundation" || c == L"Chrome_WidgetWin_1" ? 1 : 0;
    return 0;
  }

public:
  static bool hiddenViewMayRegister(bool hidden, bool bound, bool pending,
                                    bool realHome, bool appPinned) {
    return hidden && bound && !pending && realHome && !appPinned;
  }
  explicit Controller(DesktopApi &value) : api(value) {}
  ~Controller() {
    if (guardProcess)
      CloseHandle(guardProcess);
  }
  bool enabled() const { return active; }
  bool busy() const { return working; }
  const auto &windowTimings() const { return lastTimings; }
  void loadPolicies(const fs::path &file) { policies.open(file); }
  bool canConfigure(HWND h) { return !windowIdentity(h).empty(); }
  apppolicy::Mode selectedMode(HWND h) {
    return policies.get(windowIdentity(h));
  }
  int effectiveKind(HWND h) { return policy(h); }
  bool setMode(HWND h, apppolicy::Mode mode) {
    if (working || !IsWindow(h))
      return false;
    auto key = windowIdentity(h);
    auto previous = policies.get(key);
    if (!policies.set(key, mode))
      return false;
    working = true;
    std::vector<cc::WindowId> replace;
    for (auto &[id, e] : entries)
      if (valid(id) && windowIdentity((HWND)id) == key) {
        e.identity = key;
        auto name = lower(fs::path(e.backup.path).filename().wstring());
        e.allowUnpin =
            mode != apppolicy::Mode::Automatic ||
            (e.backup.browser && (name == L"chrome.exe" || bool(testPolicy))) ||
            (!e.backup.browser && name == L"qq.exe");
        int next = policy((HWND)id), old = e.backup.browser ? 2 : 1;
        if (next == old)
          continue;
        if (e.touched) {
          auto release = e.backup;
          if (!release.browser) {
            GUID actual = nativeHome(api, (HWND)id);
            release.home = actual == GUID{} ? guid(e.home) : actual;
            release.visible = IsWindowVisible((HWND)id);
            GetWindowPlacement((HWND)id, &release.placement);
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            release.regionBytes = 0;
            if (region && GetWindowRgn((HWND)id, region) != ERROR) {
              release.regionBytes = GetRegionData(region, 0, nullptr);
              if (release.regionBytes > sizeof(release.region)) {
                DeleteObject(region);
                working = false;
                policies.set(key, previous);
                return false;
              }
              GetRegionData(region, release.regionBytes,
                            (RGNDATA *)release.region);
            }
            if (region)
              DeleteObject(region);
          }
          if (!restore(api, release)) {
            working = false;
            policies.set(key, previous);
            return false;
          }
        }
        replace.push_back(id);
      }
    std::vector<cc::WindowId> previouslyTouched;
    for (auto id : replace)
      if (entries.at(id).touched) {
        previouslyTouched.push_back(id);
        entries.at(id).touched = false;
      }
    if (!replace.empty() && !journal.empty() && fs::exists(journal) &&
        !persist()) {
      for (auto id : previouslyTouched)
        entries.at(id).touched = true;
      working = false;
      policies.set(key, previous);
      return false;
    }
    for (auto id : replace)
      entries.erase(id);
    working = false;
    discoveryDirty = true;
    scan();
    return sync();
  }
  bool isSharedWindow(HWND h) {
    auto at = entries.find((cc::WindowId)h);
    return at != entries.end() && at->second.core && valid(at->first);
  }
  std::vector<LogicalView> logicalViews() {
    std::vector<LogicalView> out;
    for (auto &[id, e] : entries) {
      if (!e.core || !valid(id))
        continue;
      auto intents = e.core->savedIntents();
      // Current physical state may have changed through the app's own launcher
      // since the last desktop transition; stored inactive intents stay intact.
      intents[e.core->workspace()] = capture(id);
      for (auto &[desktop, intent] : intents)
        if (intent.show != cc::Show::Hidden)
          out.push_back(
              {id, guid(desktop), e.backup.pid, e.backup.path, intent.show});
    }
    return out;
  }
  bool nativeReady(GUID target) {
    if (working || api.currentId() != target)
      return false;
    for (auto &[id, e] : entries) {
      if (!valid(id))
        continue;
      if (e.core) {
        auto desired = e.core->intent(text(target));
        if (desired.show == cc::Show::Hidden) {
          if (IsWindowVisible((HWND)id))
            return false;
          continue;
        }
        if (nativeHome(api, (HWND)id) != target || !IsWindowVisible((HWND)id))
          return false;
        DWORD cloak{};
        if (FAILED(DwmGetWindowAttribute((HWND)id, DWMWA_CLOAKED, &cloak,
                                         sizeof(cloak))) ||
            cloak)
          return false;
      } else if (e.backup.browser && nativeHome(api, (HWND)id) == target &&
                 IsWindowVisible((HWND)id)) {
        DWORD cloak{};
        if (FAILED(DwmGetWindowAttribute((HWND)id, DWMWA_CLOAKED, &cloak,
                                         sizeof(cloak))) ||
            cloak)
          return false;
      }
    }
    return true;
  }
  void requestCurrentRedraw() {
    GUID here = api.currentId();
    for (auto &[id, e] : entries)
      if (e.core && valid(id) && IsWindowVisible((HWND)id) &&
          nativeHome(api, (HWND)id) == here)
        RedrawWindow((HWND)id, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
  }
  std::vector<ShellRepair> repairShellVisibility(uint64_t sequence) {
    std::vector<ShellRepair> out;
    if (working || !sequence)
      return out;
    GUID here = api.currentId();
    for (auto &[id, e] : entries) {
      if (!e.core || !valid(id) || e.lastShellRepair == sequence ||
          e.core->workspace() != text(here) || !IsWindowVisible((HWND)id) ||
          nativeHome(api, (HWND)id) != here)
        continue;
      DWORD cloak{};
      if (FAILED(DwmGetWindowAttribute((HWND)id, DWMWA_CLOAKED, &cloak,
                                       sizeof(cloak))) ||
          cloak != DWM_CLOAKED_SHELL)
        continue;
      e.lastShellRepair = sequence;
      ShellRepair r;
      r.window = id;
      r.exe = fs::path(e.backup.path).filename().wstring();
      r.before = cloak;
      double ignored = 0;
      Timer duration(ignored);
      r.result = clearCurrentShellCloak(api, (HWND)id);
      r.ms = duration.elapsed();
      DwmGetWindowAttribute((HWND)id, DWMWA_CLOAKED, &r.after, sizeof(r.after));
      if (SUCCEEDED(r.result))
        RedrawWindow((HWND)id, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
      out.push_back(r);
    }
    return out;
  }
  int relevantEvent(HWND h) const {
    if (entries.count((cc::WindowId)h))
      return 2;
    if (GetWindow(h, GW_OWNER) ||
        GetWindowLongPtrW(h, GWL_EXSTYLE) &
            (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) ||
        GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD)
      return 0;
    auto c = cls(h);
    if (candidateClass(c))
      return 1;
    if (policies.empty() || c.empty() || !GetWindowTextLengthW(h) ||
        c == L"Progman" || c == L"WorkerW" || c == L"Shell_TrayWnd" ||
        c == L"Shell_SecondaryTrayWnd")
      return 0;
    return 1;
  }
  void invalidateDiscovery() { discoveryDirty = true; }
  size_t count() const { return entries.size(); }
  size_t errors() const { return failures; }
  size_t browserSkips() const { return skippedBrowser; }
  void inventoryReport(std::ostream &out) {
    out << "{\"mode\":\"read_only_no_window_mutations\",\"recognized_windows\":"
        << entries.size()
        << ",\"browsers_without_explicit_appid\":" << skippedBrowser
        << ",\"windows\":[";
    bool first = true;
    for (auto &[id, e] : entries) {
      if (!first)
        out << ",";
      first = false;
      auto name = fs::path(e.backup.path).filename().wstring();
      auto c = std::wstring(e.backup.windowClass);
      out << "{\"exe\":" << jsonString(name) << ",\"class\":" << jsonString(c)
          << ",\"kind\":\""
          << (e.backup.browser ? "browser" : "shared_main_window") << "\"}";
    }
    out << "],\"candidates\":[";
    struct Scan {
      Controller *self;
      std::ostream *out;
      bool first = true;
    } scan{this, &out};
    EnumWindows(
        [](HWND h, LPARAM data) -> BOOL {
          auto &s = *(Scan *)data;
          DWORD pid{};
          GetWindowThreadProcessId(h, &pid);
          auto name = lower(fs::path(image(pid)).filename().wstring());
          if (name != L"codex.exe" && name != L"chatgpt.exe" &&
              name != L"weixin.exe" && name != L"wechat.exe" &&
              name != L"qq.exe" && name != L"chrome.exe" &&
              name != L"msedge.exe")
            return TRUE;
          if (!s.first)
            *s.out << ",";
          s.first = false;
          HRGN r = CreateRectRgn(0, 0, 0, 0);
          bool custom = r && GetWindowRgn(h, r) != ERROR;
          if (r)
            DeleteObject(r);
          BOOL appPinned = FALSE, here = FALSE;
          auto model = appId(h);
          if (s.self->api.pinned && !model.empty())
            s.self->api.pinned->IsAppPinned(model.c_str(), &appPinned);
          s.self->api.manager->IsWindowOnCurrentVirtualDesktop(h, &here);
          DWORD cloaked = 0;
          DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
          ComPtr<IUnknown> shellView;
          HRESULT viewResult = s.self->api.views->GetViewForHwnd(h, &shellView);
          *s.out << "{\"exe\":" << jsonString(name)
                 << ",\"hwnd\":" << (UINT_PTR)h
                 << ",\"class\":" << jsonString(cls(h))
                 << ",\"visible\":" << (IsWindowVisible(h) ? "true" : "false")
                 << ",\"owned_popup\":"
                 << (GetWindow(h, GW_OWNER) ? "true" : "false")
                 << ",\"extended_style\":" << GetWindowLongPtrW(h, GWL_EXSTYLE)
                 << ",\"custom_region\":" << (custom ? "true" : "false")
                 << ",\"pinned\":"
                 << (s.self->api.pinnedWindow(h) ? "true" : "false")
                 << ",\"app_pinned\":" << (appPinned ? "true" : "false")
                 << ",\"on_current\":" << (here ? "true" : "false")
                 << ",\"cloaked\":" << cloaked << ",\"desktop\":"
                 << jsonString(text(nativeHome(s.self->api, h)))
                 << ",\"shell_view_available\":"
                 << (SUCCEEDED(viewResult) && shellView ? "true" : "false")
                 << ",\"shell_view_hresult\":" << (long)viewResult
                 << ",\"current\":" << jsonString(text(s.self->api.currentId()))
                 << ",\"home_known\":"
                 << (nativeHome(s.self->api, h) != GUID{} ? "true" : "false")
                 << ",\"policy\":" << s.self->policy(h) << "}";
          return TRUE;
        },
        (LPARAM)&scan);
    out << "]}\n";
  }
  bool start(fs::path file, std::function<int(HWND)> filter = {}) {
    if (!api.internal || !api.views || !api.manager)
      return false;
    journal = std::move(file);
    testPolicy = std::move(filter);
    current = text(api.currentId());
    active = true;
    scan();
    return true;
  }
  void scan(bool force = true) {
    if (!active || working || discovering)
      return;
    if (!force && !discoveryDirty && GetTickCount64() - lastDiscovery < 1500)
      return;
    Timer timer(perf.scanMs);
    if (perf.enabled)
      ++perf.scans;
    for (auto at = entries.begin(); at != entries.end();) {
      if (!valid(at->first))
        at = entries.erase(at);
      else
        ++at;
    }
    skippedBrowser = 0;
    discovering = true;
    discoveryImages.clear();
    EnumWindows(
        [](HWND h, LPARAM value) -> BOOL {
          auto &self = *(Controller *)value;
          if (self.entries.count((cc::WindowId)h) ||
              (!IsWindowVisible(h) && !GetWindowTextLengthW(h)))
            return TRUE;
          int kind = self.policy(h);
          if (!kind)
            return TRUE;
          // Keep intentionally pinned non-Chrome apps alone. Chrome is the
          // explicitly requested repair; tests allow only their nonce windows.
          DWORD candidatePid{};
          GetWindowThreadProcessId(h, &candidatePid);
          bool chrome = lower(fs::path(self.discoveryImage(candidatePid))
                                  .filename()
                                  .wstring()) == L"chrome.exe";
          auto identity =
              self.windowIdentity(h, self.discoveryImage(candidatePid));
          bool explicitMode =
              self.policies.get(identity) != apppolicy::Mode::Automatic;
          auto executable = lower(
              fs::path(self.discoveryImage(candidatePid)).filename().wstring());
          bool allowUnpin = (kind == 2 && (chrome || bool(self.testPolicy))) ||
                            explicitMode ||
                            (kind == 1 && executable == L"qq.exe");
          bool pinned = self.api.pinnedWindow(h);
          if (!kind || (pinned && !allowUnpin))
            return TRUE;
          GUID home = nativeHome(self.api, h);
          if (home == GUID{})
            return TRUE;
          Entry e;
          e.identity = identity;
          e.allowUnpin = allowUnpin;
          e.pendingBind = kind == 1 && pinned;
          auto &b = e.backup;
          b.hwnd = (UINT_PTR)h;
          GetWindowThreadProcessId(h, &b.pid);
          HANDLE p = OpenProcess(
              PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, b.pid);
          if (!p)
            return TRUE;
          b.created = birth(p);
          e.process = std::shared_ptr<void>(
              p, [](void *handle) { CloseHandle(handle); });
          auto path = processImage(p), c = cls(h), appid = appId(h);
          if (path.size() >= 2048 || c.size() >= 256 || appid.size() >= 512)
            return TRUE;
          if (kind == 2 && appid.empty() && !explicitMode) {
            ++self.skippedBrowser;
            return TRUE;
          }
          HRGN region = CreateRectRgn(0, 0, 0, 0);
          bool custom = region && GetWindowRgn(h, region) != ERROR;
          if (kind == 1 && custom) {
            b.regionBytes = GetRegionData(region, 0, nullptr);
            if (!b.regionBytes || b.regionBytes > sizeof(b.region)) {
              DeleteObject(region);
              return TRUE;
            }
            if (GetRegionData(region, b.regionBytes, (RGNDATA *)b.region) !=
                b.regionBytes) {
              DeleteObject(region);
              return TRUE;
            }
          }
          if (region)
            DeleteObject(region);
          wcscpy_s(b.path, path.c_str());
          wcscpy_s(b.windowClass, c.c_str());
          wcscpy_s(b.id, appid.c_str());
          b.home = home;
          b.visible = IsWindowVisible(h);
          b.browser = kind == 2;
          GetWindowPlacement(h, &b.placement);
          e.home = text(home);
          e.appliedId = appid;
          auto id = (cc::WindowId)h;
          self.entries.emplace(id, std::move(e));
          auto &added = self.entries.at(id);
          if (kind == 1 && !added.pendingBind) {
            added.core = std::make_unique<cc::Core>(self, L"");
            if (!added.core->bindSingle(id, text(home)))
              self.entries.erase(id);
          }
          return TRUE;
        },
        (LPARAM)this);
    discovering = false;
    discoveryImages.clear();
    lastDiscovery = GetTickCount64();
    discoveryDirty = false;
  }
  // Called before our user's switch, and from desktop notifications for
  // Win+Tab. Failure aborts our switch; neither path ever performs a corrective
  // switch.
  bool enter(GUID target) {
    double unused = 0;
    Timer duration(unused);
    bool changed = text(target) != current;
    if (!active)
      return true;
    if (working || target == GUID{})
      return false;
    scan(false);
    working = true;
    lastTimings.clear();
    lastTimings.reserve(entries.size());
    bool ok = true;
    auto next = text(target);
    for (auto &[id, e] : entries) {
      if (!valid(id))
        continue;
      WindowTiming record;
      record.window = id;
      record.exe = fs::path(e.backup.path).filename().wstring();
      record.browser = e.backup.browser;
      lastTimings.push_back(record);
      timing = &lastTimings.back();
      double ignored = 0;
      Timer operation(ignored);
      bool done = true;
      if (!localizeBrowser(e)) {
        ++failures;
        e.failed = true;
        ok = false;
        timing->totalMs = operation.elapsed();
        timing->ok = false;
        timing->failureStage = L"pin_preflight";
        timing = nullptr;
        continue;
      }
      if (e.pendingBind) {
        e.core = std::make_unique<cc::Core>(*this, L"");
        done = e.core->bindSingle(id, e.home);
        e.pendingBind = !done;
        if (!done)
          timing->failureStage = L"shared_bind";
      }
      if (e.backup.browser) {
        auto home = nativeHome(api, (HWND)id);
        if (home == GUID{}) {
          done = false;
        } else {
          auto wanted = home == target ? std::wstring(e.backup.id)
                                       : std::wstring(e.backup.id) +
                                             L".TongZhuo.Parked." + text(home);
          if (*e.backup.id && appId((HWND)id) != wanted)
            done = touch(e) && setAppId((HWND)id, wanted);
        }
      } else if (e.core && done)
        done = e.core->enter(next);
      if (!done) {
        if (timing->failureStage.empty())
          timing->failureStage = e.backup.browser ? L"window_group" : L"window_transfer";
        ++failures;
        e.failed = true;
        ok = false;
      } else
        e.failed = false;
      timing->totalMs = operation.elapsed();
      timing->ok = done;
      timing = nullptr;
    }
    current = next;
    working = false;
    if (perf.enabled && ok)
      (changed ? perf.transitions : perf.maintenance)
          .push_back(duration.elapsed());
    return ok;
  }
  bool sync() {
    if (!active || working)
      return true;
    GUID now = api.currentId();
    if (text(now) != current)
      return enter(now);
    // Enter is idempotent for existing windows; it also stages new windows.
    return enter(now);
  }
  void observe(HWND h) {
    if (working)
      return;
    auto at = entries.find((cc::WindowId)h);
    if (at == entries.end() || !at->second.core)
      return;
    auto &e = at->second;
    if (e.core->workspace() != text(api.currentId()))
      return;
    if (nativeHome(api, h) == api.currentId() ||
        (!IsWindowVisible(h) && e.home == text(api.currentId())))
      e.core->observeSingle(e.core->workspace(), e.core->generation, false,
                            capture((cc::WindowId)h));
  }
  bool stop() {
    active = false;
    working = true;
    bool ok = true;
    for (auto &[id, e] : entries)
      if (e.touched)
        ok = restore(api, e.backup) && ok;
    working = false;
    if (ok) {
      std::error_code ec;
      fs::remove(journal, ec);
    }
    entries.clear();
    return ok;
  }
  bool valid(cc::WindowId id) override {
    auto at = entries.find(id);
    if (at == entries.end())
      return false;
    const auto &e = at->second;
    const auto &b = e.backup;
    DWORD pid{};
    // A retained process handle identifies the exact process lifetime, so
    // HWND/PID/class checks cannot accidentally authorize a recycled PID.
    // The executable path was read from this same handle at enrollment.
    return e.process && IsWindow((HWND)id) &&
           GetWindowThreadProcessId((HWND)id, &pid) && pid == b.pid &&
           cls((HWND)id) == b.windowClass &&
           WaitForSingleObject(e.process.get(), 0) == WAIT_TIMEOUT &&
           birth(e.process.get()) == b.created;
  }
  cc::DesktopId desktop(cc::WindowId id) override {
    if (!valid(id))
      return {};
    auto &e = entries.at(id);
    auto live = nativeHome(api, (HWND)id);
    if (live != GUID{}) {
      e.home = text(live);
      return e.home;
    }
    return !IsWindowVisible((HWND)id) ? e.home : L"";
  }
  cc::DesktopId activeDesktop() override { return text(api.currentId()); }
  cc::Intent capture(cc::WindowId id) override {
    return valid(id) ? livecompat::capture((HWND)id) : cc::Intent{};
  }
  bool transfer(cc::WindowId id, const cc::DesktopId &target,
                const cc::Intent &desired) override {
    if (!valid(id))
      return false;
    auto &e = entries.at(id);
    if (!touch(e))
      return false;
    HWND h = (HWND)id;
    auto leaving = livecompat::capture(h);
    auto source = desktop(id);
    // A visible window moving to an inactive desktop is cloaked by Shell.
    // A visible window being expanded here is intended to be exposed. Neither
    // case needs a zero region, which can disrupt GPU-backed surfaces.
    bool needsMask = !IsWindowVisible(h) || (guid(target) == api.currentId() &&
                                             desired.show == cc::Show::Hidden);
    std::unique_ptr<Mask> mask;
    if (needsMask)
      mask = std::make_unique<Mask>(h);
    if (timing)
      timing->masked = needsMask;
    if (mask && !mask->ok)
      return false;
    // Only a hidden window needs to be temporarily registered with Shell.
    // Visible normal/maximized windows keep their state during the move.
    bool ok = true;
    if (!IsWindowVisible(h)) {
      double ignored = 0;
      Timer duration(ignored);
      auto registered = desired.show == cc::Show::Hidden ? leaving : desired;
      if (registered.show == cc::Show::Hidden)
        registered.show = cc::Show::Normal;
      ok = show(h, registered);
      if (timing)
        timing->registerMs = duration.elapsed();
    }
    double ignored = 0;
    Timer movement(ignored);
    if (ok)
      ok = move(api, h, guid(target));
    if (timing)
      timing->moveMs = movement.elapsed();
    if (ok) {
      Timer state(ignored);
      e.home = target;
      ok = show(h, desired);
      if (timing)
        timing->stateMs = state.elapsed();
    }
    // Showing a hidden view can register it asynchronously. Verify the final
    // native home before exposing any pixels, even in the optimized path.
    if (ok && desired.show != cc::Show::Hidden &&
        nativeHome(api, h) != guid(target))
      ok = move(api, h, guid(target));
    if (!ok) {
      if (!mask)
        mask = std::make_unique<Mask>(h);
      auto registered = leaving;
      registered.show = cc::Show::Normal;
      bool rolledBack = show(h, registered) && move(api, h, guid(source));
      if (rolledBack) {
        show(h, leaving);
        e.home = source;
      } else {
        auto hidden = leaving;
        hidden.show = cc::Show::Hidden;
        show(h, hidden);
      }
    }
    mask.reset();
    if (ok && desired.show != cc::Show::Hidden)
      RedrawWindow(h, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    return ok;
  }
  bool stageHidden(cc::WindowId id, const cc::DesktopId &to) override {
    if (!valid(id))
      return false;
    auto &e = entries.at(id);
    if (!touch(e))
      return false;
    HWND h = (HWND)id;
    auto leaving = livecompat::capture(h);
    auto home = desktop(id);
    Mask mask(h);
    if (!mask.ok)
      return false;
    auto visible = leaving;
    visible.show = cc::Show::Normal;
    bool ok = show(h, visible) && move(api, h, guid(to));
    if (ok) {
      e.home = to;
      auto hidden = leaving;
      hidden.show = cc::Show::Hidden;
      ok = show(h, hidden);
    }
    if (!ok) {
      if (IsWindowVisible(h))
        move(api, h, guid(home));
      show(h, leaving);
      e.home = home;
    }
    return ok;
  }
  bool apply(cc::WindowId id, const cc::Intent &v, bool focus) override {
    if (!valid(id) || focus)
      return false; // The real app owns user activation.
    auto &e = entries.at(id);
    if (!touch(e))
      return false;
    auto intended = e.home;
    if (v.show == cc::Show::Hidden)
      return show((HWND)id, v);
    HWND h = (HWND)id;
    Mask mask(h);
    if (!mask.ok)
      return false;
    bool ok = show(h, v) && move(api, h, guid(intended));
    if (!ok) {
      auto hidden = v;
      hidden.show = cc::Show::Hidden;
      show(h, hidden);
    }
    return ok;
  }
  bool group(cc::WindowId, const std::wstring &) override { return true; }
  cc::WindowId createMulti(const cc::DesktopId &) override { return 0; }
};
} // namespace tongzhuo::livecompat
