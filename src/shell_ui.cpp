#define NOMINMAX
#include "desktop_api.h"
#include "hover_reveal.h"
#include "runtime_trace.h"
#include "switch_policy.h"
#include "startup.h"
#include "ui_policy.h"
#include "window_compat.h"
#include "window_compat_checks.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <commctrl.h>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <gdiplus.h>
#include <map>
#include <memory>
#include <objidl.h>
#include <psapi.h>
#include <set>
#include <shellapi.h>
#include <vector>
#include <wincodec.h>
#include <windows.h>
#include <windowsx.h>

using namespace Gdiplus;
using namespace desklet;
namespace fs = std::filesystem;
namespace {
constexpr wchar_t MainClass[] = L"TongZhuo.Studio.v3",
                  HudClass[] = L"TongZhuo.Edge.v3",
                  EditClass[] = L"TongZhuo.Name.v3",
                  MenuClass[] = L"TongZhuo.Menu.v3";
constexpr DWORD MainStyle = WS_POPUP | WS_THICKFRAME | WS_SYSMENU |
                            WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
constexpr UINT Changed = WM_APP + 140, TrayMessage = WM_APP + 141,
               RenameMessage = WM_APP + 142;
enum {
  Create = 400,
  Settings,
  Minimize,
  Close,
  Back,
  Forward,
  Choose,
  ToggleHud,
  ToggleFullscreen,
  Range12,
  Range24,
  Range40,
  PreviewHud,
  Quit,
  AppBehavior = 600,
  ToggleStartup = 610,
  Card = 500
};
HINSTANCE instance;
HWND mainWindow{}, hudWindow{}, stripWindow{}, editWindow{}, menuWindow{},
    nameField{};
HFONT uiFont{};
DesktopApi api;
std::unique_ptr<tongzhuo::livecompat::Controller> compatibility;
constexpr UINT CompatChanged = WM_APP + 143;
bool compatRefreshPending = false;
tongzhuo::RuntimeTrace runtimeTrace;
constexpr UINT TraceWake = WM_APP + 144;
tongzhuo::SwitchGate gate;
tongzhuo::HoverReveal hover;
tongzhuo::SharedPanelState panelState;
bool ownsPanelPin = false;
tongzhuo::StartupSetting startup;
bool startupEnabled = false;
std::map<std::wstring, tongzhuo::policy::Mode> sandboxModes;
constexpr wchar_t PinFailure[] = L"无法固定到所有桌面，请重新打开同桌重试。";
bool ready = false, sandbox = false, settingsView = false, hudEnabled = false,
     hideFullscreen = true, panelPinned = false, editingNew = false,
     follow = true, renderTest = false, residentTest = false,
     inventoryDirty = true, refreshPending = false;
float dpi = 1;
float nameDpi = 1;
int clientWidth = 816, clientHeight = 620, desktopPage = 0, appPage = 0,
    range = 24, hoverCard = -1, menuKind = 0;
GUID current{}, viewed{}, editingId{}, contextId{};
DWORD notifyCookie = 0;
HWINEVENTHOOK windowEvents{};
ComPtr<IUnknown> panelView;
ULONGLONG signature = 0, previewUntil = 0;
std::wstring status;
bool hudContentVisible = false;
size_t layoutPasses = 0;
Color canvas(18, 21, 27), surface(25, 29, 37), raised(34, 40, 50),
    line(46, 53, 65), text(237, 241, 247), muted(144, 155, 172),
    accent(164, 191, 255);
struct Desk {
  ComPtr<Desktop> object;
  GUID id{};
  std::wstring name, wallpaper;
  int apps = 0, windows = 0;
};
struct App {
  HWND window{};
  std::wstring path, name;
  int count = 0;
};
struct View {
  HWND window{};
  GUID desktop{};
  DWORD pid{};
  std::wstring path;
  bool pinned = false;
};
struct Hit {
  RectF rect;
  int index = -1;
  bool create = false;
  GUID desktop{};
};
std::vector<Desk> desks;
std::vector<View> views;
std::vector<App> apps;
std::vector<Hit> hudHits;
std::map<std::wstring, std::wstring> appNames;
std::map<std::wstring, HICON> appIcons;
std::map<std::wstring, std::unique_ptr<Bitmap>> wallpapers;
std::map<std::pair<int, bool>, std::unique_ptr<Font>> fonts;
struct PaintBuffer {
  std::unique_ptr<Bitmap> image;
  int width = 0, height = 0;
  Bitmap *get(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (!image || width != w || height != h) {
      image = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
      width = w;
      height = h;
    }
    return image.get();
  }
  void release() {
    image.reset();
    width = height = 0;
  }
} mainBuffer, hudBuffer;
void releaseVisuals() {
  if (IsWindowVisible(mainWindow) && !IsIconic(mainWindow))
    return;
  if (hudContentVisible && hudWindow && IsWindowVisible(hudWindow))
    return;
  mainBuffer.release();
  hudBuffer.release();
  wallpapers.clear();
  for (auto &pair : appIcons)
    if (pair.second)
      DestroyIcon(pair.second);
  appIcons.clear();
  appNames.clear();
  fonts.clear();
}

struct Drag {
  bool down = false, active = false;
  POINT origin{};
  int from = -1, to = -1;
  GUID id{};
} drag;
int px(float v) { return (int)(v * dpi); }
int namePx(float v) { return (int)(v * nameDpi); }
bool inside(RectF r, float x, float y) {
  return x >= r.X && x < r.GetRight() && y >= r.Y && y < r.GetBottom();
}
std::wstring module() {
  wchar_t p[32768]{};
  GetModuleFileNameW(nullptr, p, 32768);
  return p;
}
fs::path settingsPath() {
  wchar_t p[32768]{};
  GetEnvironmentVariableW(L"LOCALAPPDATA", p, 32768);
  return fs::path(p) / L"PersonalTools" / L"TongZhuo" / L"settings.ini";
}
void saveSettings() {
  if (sandbox)
    return;
  auto p = settingsPath();
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  WritePrivateProfileStringW(L"Edge", L"Enabled", hudEnabled ? L"1" : L"0",
                             p.c_str());
  WritePrivateProfileStringW(L"Edge", L"HideFullscreen",
                             hideFullscreen ? L"1" : L"0", p.c_str());
  WritePrivateProfileStringW(L"Edge", L"Range", std::to_wstring(range).c_str(),
                             p.c_str());
}
void loadSettings() {
  auto p = settingsPath();
  hudEnabled = GetPrivateProfileIntW(L"Edge", L"Enabled", 0, p.c_str()) == 1;
  hideFullscreen =
      GetPrivateProfileIntW(L"Edge", L"HideFullscreen", 1, p.c_str()) == 1;
  range = GetPrivateProfileIntW(L"Edge", L"Range", 24, p.c_str());
  if (range != 12 && range != 24 && range != 40)
    range = 24;
  if (sandbox || residentTest)
    hudEnabled = false;
  startupEnabled = !sandbox && startup.enabled();
  if (startupEnabled && !sandbox && !renderTest && !residentTest) {
    auto previous = startup.command();
    auto end = previous.find(L'"', 1);
    if (previous.starts_with(L"\"") && end != std::wstring::npos &&
        previous.substr(end) == L"\" --background") {
      fs::path owned = previous.substr(1, end - 1);
      if (owned.parent_path() == fs::path(module()).parent_path() &&
          owned.filename().wstring().starts_with(L"Desklet.") &&
          owned.extension() == L".exe" && owned != fs::path(module()))
        startup.set(true, module());
    }
  }
}
std::wstring guidText(GUID id) {
  wchar_t s[64]{};
  StringFromGUID2(id, s, 64);
  return s;
}
std::wstring classOf(HWND h) {
  wchar_t s[256]{};
  GetClassNameW(h, s, 256);
  return s;
}
std::wstring pathOf(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return {};
  wchar_t s[32768]{};
  DWORD n = 32768;
  bool ok = QueryFullProcessImageNameW(h, 0, s, &n) != FALSE;
  CloseHandle(h);
  return ok ? std::wstring(s, n) : L"";
}
std::wstring friendly(std::wstring path) {
  if (appNames.count(path))
    return appNames[path];
  if (appNames.size() >= 96)
    appNames.clear();
  std::wstring name = fs::path(path).stem().wstring();
  DWORD unused = 0, n = GetFileVersionInfoSizeW(path.c_str(), &unused);
  if (n && n < 1024 * 1024) {
    std::vector<BYTE> data(n);
    if (GetFileVersionInfoW(path.c_str(), 0, n, data.data())) {
      struct T {
        WORD lang, code;
      };
      T *t{};
      UINT length = 0;
      if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation",
                         (void **)&t, &length) &&
          length >= sizeof(T)) {
        wchar_t key[128]{};
        swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                   t[0].lang, t[0].code);
        wchar_t *value{};
        if (VerQueryValueW(data.data(), key, (void **)&value, &length) &&
            length > 1)
          name = value;
      }
    }
  }
  if (name.empty())
    name = L"系统应用";
  return appNames[path] = name;
}
HICON iconOf(std::wstring path) {
  if (appIcons.count(path))
    return appIcons[path];
  if (appIcons.size() >= tongzhuo::IconCacheLimit) {
    for (auto &item : appIcons)
      if (item.second)
        DestroyIcon(item.second);
    appIcons.clear();
  }
  HICON icon = nullptr;
  if (!path.empty())
    ExtractIconExW(path.c_str(), 0, nullptr, &icon, 1);
  appIcons[path] = icon;
  return icon;
}
void round(Graphics &g, RectF r, float rad, Color fill,
           Color stroke = Color(0, 0, 0, 0)) {
  GraphicsPath p;
  float d = rad * 2;
  p.AddArc(r.X, r.Y, d, d, 180, 90);
  p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
  p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
  p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
  p.CloseFigure();
  SolidBrush b(fill);
  g.FillPath(&b, &p);
  if (stroke.GetA()) {
    Pen pen(stroke, 1);
    g.DrawPath(&pen, &p);
  }
}
void dashedCard(Graphics &g, RectF r, bool hovered) {
  round(g, r, 10, hovered ? raised : surface);
  GraphicsPath path;
  float d = 20;
  path.AddArc(r.X, r.Y, d, d, 180, 90);
  path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
  path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
  path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
  path.CloseFigure();
  Pen border(hovered ? accent : Color(86, 101, 123), 1.2f);
  const REAL pattern[] = {3, 3};
  border.SetDashPattern(pattern, 2);
  border.SetDashCap(DashCapRound);
  g.DrawPath(&border, &path);
}
void plusGlyph(Graphics &g, float x, float y, float radius, Color color) {
  Pen pen(color, 1.7f);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  g.DrawLine(&pen, x - radius, y, x + radius, y);
  g.DrawLine(&pen, x, y - radius, x, y + radius);
}
void label(Graphics &g, std::wstring s, RectF r, float size = 12,
           Color color = text, bool bold = false,
           StringAlignment align = StringAlignmentNear) {
  auto key = std::make_pair((int)(size * 2), bold);
  auto &font = fonts[key];
  if (!font)
    font = std::make_unique<Font>(L"Microsoft YaHei UI", size,
                                  bold ? FontStyleBold : FontStyleRegular,
                                  UnitPixel);
  SolidBrush b(color);
  StringFormat format;
  format.SetLineAlignment(StringAlignmentCenter);
  format.SetAlignment(align);
  format.SetTrimming(StringTrimmingEllipsisCharacter);
  format.SetFormatFlags(StringFormatFlagsNoWrap);
  g.DrawString(s.c_str(), -1, font.get(), r, &format, &b);
}
void iconDraw(Graphics &g, HICON icon, float x, float y, int size,
              float factor = 0) {
  if (!icon) {
    round(g, {x, y, (float)size, (float)size}, size * .22f, Color(62, 85, 123));
    Pen pen(Color(206, 223, 250), 1.1f);
    g.DrawRectangle(&pen, x + size * .23f, y + size * .25f, size * .54f,
                    size * .48f);
    g.DrawLine(&pen, x + size * .23f, y + size * .4f, x + size * .77f,
               y + size * .4f);
    return;
  }
  if (!factor)
    factor = dpi;
  HDC h = g.GetHDC();
  DrawIconEx(h, (int)(x * factor), (int)(y * factor), icon,
             (int)(size * factor), (int)(size * factor), 0, nullptr, DI_NORMAL);
  g.ReleaseHDC(h);
}
void monitorGlyph(Graphics &g, float x, float y, Color c) {
  Pen p(c, 1.6f);
  g.DrawRectangle(&p, x, y, 20.f, 14.f);
  g.DrawLine(&p, x + 10, y + 14, x + 10, y + 18);
  g.DrawLine(&p, x + 5, y + 18, x + 15, y + 18);
}
int indexOf(GUID id) {
  for (size_t i = 0; i < desks.size(); ++i)
    if (desks[i].id == id)
      return (int)i;
  return -1;
}
BOOL CALLBACK collect(HWND h, LPARAM) {
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid == GetCurrentProcessId() || !IsWindowVisible(h) ||
      GetWindow(h, GW_OWNER) || !GetWindowTextLengthW(h))
    return TRUE;
  LONG_PTR style = GetWindowLongPtrW(h, GWL_EXSTYLE);
  if (style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
    return TRUE;
  auto cls = classOf(h);
  if (cls.rfind(L"TongZhuo.", 0) == 0 || cls.rfind(L"Desklet.", 0) == 0 ||
      cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" ||
      cls == L"Shell_SecondaryTrayWnd" || cls == L"Windows.UI.Core.CoreWindow")
    return TRUE;
  DWORD cloak = 0;
  DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloak, sizeof(cloak));
  if (cloak & DWM_CLOAKED_APP)
    return TRUE;
  View v;
  v.window = h;
  v.pid = pid;
  v.path = pathOf(pid);
  if (FAILED(api.manager->GetWindowDesktopId(h, &v.desktop)) ||
      v.desktop == GUID{})
    return TRUE;
  v.pinned = api.pinnedWindow(h);
  views.push_back(std::move(v));
  return TRUE;
}
std::wstring keyOf(const View &v) {
  return v.path.empty() ? L"pid:" + std::to_wstring(v.pid) : v.path;
}
int appRows() {
  int top = 150 + tongzhuo::desktopCardHeight((clientWidth - 80) / 3, clientHeight) + 52;
  return std::clamp((clientHeight - 28 - top) / 40, 1, 4);
}
int appSelectorWidth() { return std::clamp(clientWidth / 3, 160, 218); }
int appSelectorX() { return clientWidth - 46 - appSelectorWidth(); }
void groupApps() {
  apps.clear();
  std::map<std::wstring, size_t> grouped;
  for (auto &v : views)
    if (v.desktop == viewed || v.pinned) {
      auto k = keyOf(v);
      if (grouped.count(k)) {
        ++apps[grouped[k]].count;
        continue;
      }
      grouped[k] = apps.size();
      apps.push_back({v.window, v.path,
                      v.path.empty() ? L"系统应用" : friendly(v.path), 1});
    }
  std::sort(apps.begin(), apps.end(),
            [](auto &a, auto &b) { return a.name < b.name; });
  appPage =
      std::clamp(appPage, 0, std::max(0, ((int)apps.size() - 1) / appRows()));
}
bool inventory(bool includeApps = true) {
  if (!ready)
    return false;
  if (!sandbox) {
    ComPtr<IObjectArray> list;
    if (FAILED(api.internal->GetDesktops(&list)) || !list)
      return false;
    UINT n = 0;
    list->GetCount(&n);
    if (!n || n > 256)
      return false;
    std::vector<Desk> next;
    for (UINT i = 0; i < n; ++i) {
      Desk d;
      if (FAILED(list->GetAt(i, IID_PPV_ARGS(&d.object))))
        continue;
      d.id = api.id(d.object.Get());
      HSTRING name{}, wall{};
      if (SUCCEEDED(d.object->GetName((void **)&name))) {
        UINT32 len = 0;
        auto raw = WindowsGetStringRawBuffer(name, &len);
        d.name.assign(raw, len);
        WindowsDeleteString(name);
      }
      if (SUCCEEDED(d.object->GetWallpaper((void **)&wall))) {
        UINT32 len = 0;
        auto raw = WindowsGetStringRawBuffer(wall, &len);
        d.wallpaper.assign(raw, len);
        WindowsDeleteString(wall);
      }
      if (d.wallpaper.empty()) {
        wchar_t path[32768]{};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, 32768, path, 0))
          d.wallpaper = path;
      }
      if (d.name.empty() || d.name.rfind(L"间桌 · ", 0) == 0)
        d.name = L"桌面 " + std::to_wstring(i + 1);
      next.push_back(std::move(d));
    }
    desks = std::move(next);
    GUID prior = current;
    current = api.currentId();
    if (prior != current) {
      int at = indexOf(current);
      if (at >= 0)
        desktopPage = at / 3;
    }
  }
  if (follow || indexOf(viewed) < 0)
    viewed = current;
  if (!includeApps)
    return true;
  if (!sandbox) {
    views.clear();
    EnumWindows(collect, 0);
    if (compatibility) {
      views.erase(std::remove_if(views.begin(), views.end(),
                                 [](const View &v) {
                                   return compatibility->isSharedWindow(
                                       v.window);
                                 }),
                  views.end());
      for (const auto &logical : compatibility->logicalViews()) {
        View v;
        v.window = (HWND)logical.window;
        v.pid = logical.pid;
        v.path = logical.path;
        v.desktop = logical.desktop;
        views.push_back(std::move(v));
      }
    }
  }
  for (auto &d : desks) {
    d.apps = d.windows = 0;
    std::set<std::wstring> unique;
    for (auto &v : views)
      if (v.desktop == d.id || v.pinned) {
        unique.insert(keyOf(v));
        ++d.windows;
      }
    d.apps = (int)unique.size();
  }
  groupApps();
  desktopPage = std::clamp(
      desktopPage, 0, std::max(0, tongzhuo::lastDesktopPage(desks.size())));
  return true;
}
ULONGLONG hash() {
  ULONGLONG value = 1469598103934665603ULL;
  auto add = [&](const std::wstring &s) {
    for (wchar_t c : s) {
      value ^= c;
      value *= 1099511628211ULL;
    }
  };
  add(guidText(current));
  add(guidText(viewed));
  add(status);
  for (auto &d : desks) {
    add(guidText(d.id));
    add(d.name);
    add(d.wallpaper);
    add(std::to_wstring(d.apps));
    add(std::to_wstring(d.windows));
  }
  for (auto &a : apps) {
    add(a.path);
    add(std::to_wstring(a.count));
  }
  return value;
}
Bitmap *wallpaper(const std::wstring &path) {
  if (path.empty())
    return nullptr;
  if (wallpapers.count(path))
    return wallpapers[path].get();
  if (wallpapers.size() >= tongzhuo::PreviewCacheLimit)
    wallpapers.clear();
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapClipper> clipper;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  auto fail = [&]() -> Bitmap * {
    wallpapers[path] = nullptr;
    return nullptr;
  };
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
      FAILED(factory->CreateDecoderFromFilename(
          path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
          &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)))
    return fail();
  UINT width = 0, height = 0;
  if (FAILED(frame->GetSize(&width, &height)) || !width || !height)
    return fail();
  const int tw = tongzhuo::PreviewWidth, th = tongzhuo::PreviewHeight;
  UINT cropWidth = width, cropHeight = height;
  if ((double)width / height > (double)tw / th)
    cropWidth = std::max(1u, (UINT)((double)height * tw / th));
  else
    cropHeight = std::max(1u, (UINT)((double)width * th / tw));
  WICRect crop{(INT)((width - cropWidth) / 2), (INT)((height - cropHeight) / 2),
               (INT)cropWidth, (INT)cropHeight};
  if (FAILED(factory->CreateBitmapClipper(&clipper)) ||
      FAILED(clipper->Initialize(frame.Get(), &crop)) ||
      FAILED(factory->CreateBitmapScaler(&scaler)) ||
      FAILED(scaler->Initialize(clipper.Get(), tw, th,
                                WICBitmapInterpolationModeFant)) ||
      FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
    return fail();
  auto previewImage = std::make_unique<Bitmap>(tw, th, PixelFormat32bppPARGB);
  BitmapData pixels{};
  Rect area(0, 0, tw, th);
  if (previewImage->LockBits(&area, ImageLockModeWrite, PixelFormat32bppPARGB,
                             &pixels) != Ok)
    return fail();
  HRESULT copied =
      converter->CopyPixels(nullptr, (UINT)pixels.Stride,
                            (UINT)pixels.Stride * th, (BYTE *)pixels.Scan0);
  previewImage->UnlockBits(&pixels);
  if (FAILED(copied))
    return fail();
  auto *p = previewImage.get();
  wallpapers[path] = std::move(previewImage);
  return p;
}
void thumbnail(Graphics &g, const Desk &d, RectF r, bool /*selected*/,
               float iconScale = 0) {
  GraphicsState saved = g.Save();
  GraphicsPath clip;
  float rad = 10, diam = rad * 2;
  clip.AddArc(r.X, r.Y, diam, diam, 180, 90);
  clip.AddArc(r.GetRight() - diam, r.Y, diam, diam, 270, 90);
  clip.AddArc(r.GetRight() - diam, r.GetBottom() - diam, diam, diam, 0, 90);
  clip.AddArc(r.X, r.GetBottom() - diam, diam, diam, 90, 90);
  clip.CloseFigure();
  g.SetClip(&clip);
  LinearGradientBrush gradient(r, Color(31, 52, 99), Color(56, 39, 91),
                               LinearGradientModeForwardDiagonal);
  g.FillRectangle(&gradient, r);
  if (auto *image = wallpaper(d.wallpaper)) {
    float factor = std::max(r.Width / (float)tongzhuo::PreviewWidth,
                            r.Height / (float)tongzhuo::PreviewHeight);
    RectF dest(r.X + (r.Width - tongzhuo::PreviewWidth * factor) / 2,
               r.Y + (r.Height - tongzhuo::PreviewHeight * factor) / 2,
               tongzhuo::PreviewWidth * factor,
               tongzhuo::PreviewHeight * factor);
    g.DrawImage(image, dest);
  }
  SolidBrush shade(Color(70, 8, 13, 24));
  g.FillRectangle(&shade, r);
  std::set<std::wstring> seen;
  int n = 0;
  int iconLimit = std::clamp((int)((r.Width - 20) / 27), 0, 5);
  for (auto &v : views)
    if (n < iconLimit && (v.desktop == d.id || v.pinned) && !v.path.empty() &&
        seen.insert(v.path).second) {
      round(g, {r.X + 10 + n * 27, r.GetBottom() - 32, 23, 23}, 6,
            Color(210, 23, 30, 45));
      iconDraw(g, iconOf(v.path), r.X + 13 + n * 27, r.GetBottom() - 29, 17,
               iconScale);
      if (++n >= iconLimit)
        break;
    }
  g.Restore(saved);
}
HRESULT frame(HWND h) {
  DWORD corner = DWMWCP_ROUND;
  DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                        sizeof(corner));
  BOOL dark = TRUE;
  DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  COLORREF color = DWMWA_COLOR_NONE;
  return DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &color, sizeof(color));
}
bool panelVisible() {
  return (mainWindow && IsWindowVisible(mainWindow) && !IsIconic(mainWindow)) ||
         (hudContentVisible && hudWindow && IsWindowVisible(hudWindow));
}
void schedule() {
  inventoryDirty = true;
  if (mainWindow && panelVisible() && !refreshPending) {
    refreshPending = true;
    SetTimer(mainWindow, 91, 180, nullptr);
  }
}
void layout();
void updateHudTimer();
void closeMenu();
void showMain();
void showHud(tongzhuo::Reveal state, HMONITOR monitor);
void openName(bool create, GUID id);
void updateAppActions();
void openAppBehavior(int row);
void refresh(bool force = false) {
  if (inventoryDirty || force) {
    inventory();
    inventoryDirty = false;
  }
  auto next = hash();
  if (force || next != signature) {
    signature = next;
    for (int i = 0; i < 3; ++i) {
      int at = desktopPage * 3 + i;
      HWND child = GetDlgItem(mainWindow, Card + i);
      if (child && at < (int)desks.size()) {
        wchar_t value[256]{};
        GetWindowTextW(child, value, 256);
        if (desks[at].name != value)
          SetWindowTextW(child, desks[at].name.c_str());
      }
    }
    layout();
    updateAppActions();
    InvalidateRect(mainWindow, nullptr, FALSE);
    EnumChildWindows(
        mainWindow,
        [](HWND h, LPARAM) -> BOOL {
          if (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE)
            InvalidateRect(h, nullptr, FALSE);
          return TRUE;
        },
        0);
    if (hudWindow && IsWindowVisible(hudWindow))
      InvalidateRect(hudWindow, nullptr, FALSE);
  }
  updateHudTimer();
}

class Listener final : public DesktopNotification {
  LONG refs = 1;

public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
    if (!out)
      return E_POINTER;
    if (id == IID_IUnknown || id == __uuidof(DesktopNotification)) {
      *out = (DesktopNotification *)this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return (ULONG)InterlockedIncrement(&refs);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = InterlockedDecrement(&refs);
    if (!n)
      delete this;
    return (ULONG)n;
  }
  HRESULT signal(bool wallpaper = false) {
    if (mainWindow)
      PostMessageW(mainWindow, Changed, wallpaper ? 1 : 2, 0);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Created(Desktop *) override { return signal(); }
  HRESULT STDMETHODCALLTYPE DestroyBegin(Desktop *, Desktop *) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DestroyFailed(Desktop *, Desktop *) override {
    return signal();
  }
  HRESULT STDMETHODCALLTYPE Destroyed(Desktop *, Desktop *) override {
    return signal();
  }
  HRESULT STDMETHODCALLTYPE Moved(Desktop *, INT64, INT64) override {
    return signal();
  }
  HRESULT STDMETHODCALLTYPE NameChanged(Desktop *, HSTRING) override {
    return signal();
  }
  HRESULT STDMETHODCALLTYPE ViewChanged(IUnknown *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE CurrentChanged(Desktop *,
                                           Desktop *target) override {
    runtimeTrace.current(api.id(target), tongzhuo::RuntimeTrace::now());
    if (mainWindow)
      PostMessageW(mainWindow, TraceWake, 0, 0);
    if (mainWindow)
      PostMessageW(mainWindow, CompatChanged, 0, 0);
    return signal();
  }
  HRESULT STDMETHODCALLTYPE WallpaperChanged(Desktop *, HSTRING) override {
    return signal(true);
  }
  HRESULT STDMETHODCALLTYPE Switched(Desktop *target) override {
    runtimeTrace.switched(api.id(target), tongzhuo::RuntimeTrace::now());
    if (mainWindow)
      PostMessageW(mainWindow, TraceWake, 1, 0);
    return signal();
  }
  HRESULT STDMETHODCALLTYPE RemoteConnected(Desktop *) override {
    return signal();
  }
};
ComPtr<Listener> listener;
void CALLBACK eventHook(HWINEVENTHOOK, DWORD e, HWND h, LONG object, LONG child,
                        DWORD, DWORD) {
  if (!h || object != OBJID_WINDOW || child)
    return;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  if (pid == GetCurrentProcessId())
    return;
  if (mainWindow && compatibility) {
    int reason = compatibility->relevantEvent(h);
    if (reason == 2 && !compatibility->busy() &&
        (e == EVENT_OBJECT_SHOW || e == EVENT_OBJECT_HIDE))
      compatibility->observe(h);
    if (reason)
      PostMessageW(mainWindow, CompatChanged, reason, 0);
  }
  if (!panelVisible()) {
    inventoryDirty = true;
    return;
  }
  if (e == EVENT_OBJECT_SHOW || e == EVENT_OBJECT_HIDE ||
      e == EVENT_OBJECT_DESTROY)
    PostMessageW(mainWindow, Changed, 0, 0);
}
bool userSwitch(GUID id) {
  if (compatibility && compatibility->busy())
    return false;
  int at = indexOf(id);
  if (at < 0)
    return false;
  auto object = desks[at].object;
  GUID now = sandbox ? current : api.currentId();
  bool ok = gate.userSelect(now, id, [&](const GUID &target) {
    if (sandbox) {
      current = target;
      return true;
    }
    runtimeTrace.begin(now, target);
    bool prepared = !compatibility || compatibility->enter(target);
    runtimeTrace.prepared(
        prepared,
        compatibility
            ? compatibility->windowTimings()
            : std::vector<tongzhuo::livecompat::Controller::WindowTiming>{});
    if (!prepared) {
      compatibility->enter(now);
      SetTimer(mainWindow, 99, 50, nullptr);
      status = L"窗口准备未完成，本次没有切换桌面。";
      return false;
    }
    runtimeTrace.switchBegin();
    bool switched = api.switchTo(object.Get());
    runtimeTrace.switchReturned(switched);
    SetTimer(mainWindow, 98, 25, nullptr);
    SetTimer(mainWindow, 99, 50, nullptr);
    if (!switched && compatibility)
      compatibility->enter(now);
    return switched;
  });
  if (ok)
    status.clear();
  else if (status.empty())
    status = L"Windows 未能切换桌面，请重试。";
  follow = true;
  viewed = id;
  previewUntil = 0;
  hover.dismiss();
  showHud(tongzhuo::Reveal::Hidden, nullptr);
  schedule();
  return ok;
}
void userReorder(GUID id, int target) {
  int from = indexOf(id);
  if (from < 0 || target < 0 || target >= (int)desks.size() || from == target)
    return;
  bool ok = true;
  if (sandbox) {
    auto item = desks[from];
    desks.erase(desks.begin() + from);
    desks.insert(desks.begin() + target, item);
  } else {
    auto object = desks[from].object;
    ok = SUCCEEDED(api.internal->MoveDesktop(object.Get(), (UINT)target));
  }
  status = ok ? L"已更新桌面顺序" : L"此系统暂未完成排序，请重试。";
  refresh(true);
}
void renameOrCreate() {
  wchar_t name[128]{};
  GetWindowTextW(nameField, name, 128);
  std::wstring value = name;
  auto first = value.find_first_not_of(L" \t\r\n"),
       last = value.find_last_not_of(L" \t\r\n");
  if (first == std::wstring::npos) {
    SetFocus(nameField);
    return;
  }
  value = value.substr(first, last - first + 1);
  bool ok = false;
  GUID target = editingId;
  if (editingNew) {
    Desk d;
    if (sandbox) {
      d.id = {0x70000000 + (unsigned)desks.size(), 0, 0, {}};
      d.name = value;
      desks.push_back(d);
      ok = true;
    } else if (api.create(d.object)) {
      d.id = api.id(d.object.Get());
      ok = api.name(d.object.Get(), value.c_str());
      d.name = value;
      desks.push_back(d);
    }
    target = d.id;
  } else {
    int at = indexOf(target);
    if (at >= 0) {
      if (sandbox) {
        desks[at].name = value;
        ok = true;
      } else
        ok = api.name(desks[at].object.Get(), value.c_str());
    }
  }
  if (ok) {
    bool created = editingNew;
    DestroyWindow(editWindow);
    editWindow = nullptr;
    EnableWindow(mainWindow, TRUE);
    if (created)
      userSwitch(target);
    refresh(true);
  } else
    status = L"Windows 未能保存桌面名称。";
}
LRESULT CALLBACK cardSubclass(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                              DWORD_PTR) {
  int slot = GetDlgCtrlID(h) - Card, at = desktopPage * 3 + slot;
  switch (m) {
  case WM_LBUTTONDOWN:
    if (at < (int)desks.size()) {
      RECT r{};
      GetClientRect(h, &r);
      POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (p.x > r.right - px(37) && p.y < px(37)) {
        editingId = desks[at].id;
        PostMessageW(mainWindow, RenameMessage, 0, 0);
        return 0;
      }
      drag = {};
      drag.down = true;
      drag.from = drag.to = at;
      drag.id = desks[at].id;
      GetCursorPos(&drag.origin);
    }
    break;
  case WM_MOUSEMOVE:
    if (drag.down && (w & MK_LBUTTON)) {
      POINT p;
      GetCursorPos(&p);
      if (abs(p.x - drag.origin.x) > px(7) || abs(p.y - drag.origin.y) > px(7))
        drag.active = true;
      if (drag.active) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        for (int i = 0; i < 3; ++i) {
          HWND other = GetDlgItem(mainWindow, Card + i);
          RECT r{};
          GetWindowRect(other, &r);
          if (PtInRect(&r, p) && desktopPage * 3 + i < (int)desks.size())
            drag.to = desktopPage * 3 + i;
          InvalidateRect(other, nullptr, FALSE);
        }
        return 0;
      }
    }
    break;
  case WM_LBUTTONUP:
    if (drag.active) {
      GUID id = drag.id;
      int target = drag.to;
      drag = {};
      SendMessageW(h, BM_SETSTATE, FALSE, 0);
      ReleaseCapture();
      userReorder(id, target);
      return 0;
    }
    drag.down = false;
    break;
  case WM_CAPTURECHANGED:
    if (drag.down) {
      drag = {};
      for (int i = 0; i < 3; ++i)
        InvalidateRect(GetDlgItem(mainWindow, Card + i), nullptr, FALSE);
    }
    break;
  case WM_RBUTTONUP:
    if (at < (int)desks.size()) {
      editingId = desks[at].id;
      PostMessageW(mainWindow, RenameMessage, 0, 0);
    }
    return 0;
  case WM_KEYDOWN:
    if (w == VK_F2 && at < (int)desks.size()) {
      editingId = desks[at].id;
      PostMessageW(mainWindow, RenameMessage, 0, 0);
      return 0;
    }
    break;
  case WM_MOUSELEAVE:
    hoverCard = -1;
    break;
  }
  return DefSubclassProc(h, m, w, l);
}
LRESULT CALLBACK hoverButton(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR,
                             DWORD_PTR) {
  if (message == WM_ERASEBKGND)
    return 1;
  if (message == WM_MOUSEMOVE && !GetPropW(h, L"TongZhuo.Hover")) {
    SetPropW(h, L"TongZhuo.Hover", (HANDLE)1);
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, h, 0};
    TrackMouseEvent(&tracking);
    InvalidateRect(h, nullptr, FALSE);
  } else if (message == WM_MOUSELEAVE || message == WM_KILLFOCUS ||
             message == WM_ENABLE) {
    RemovePropW(h, L"TongZhuo.Hover");
    InvalidateRect(h, nullptr, FALSE);
  } else if (message == WM_NCDESTROY)
    RemovePropW(h, L"TongZhuo.Hover");
  return DefSubclassProc(h, message, w, l);
}
void buttonGlyph(Graphics &g, int id, RectF rect, Color color) {
  Pen pen(color, 1.5f);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  float x = rect.X + rect.Width / 2, y = rect.Y + rect.Height / 2;
  if (id == Close) {
    g.DrawLine(&pen, x - 4, y - 4, x + 4, y + 4);
    g.DrawLine(&pen, x + 4, y - 4, x - 4, y + 4);
  } else if (id == Minimize)
    g.DrawLine(&pen, x - 5, y, x + 5, y);
  else if (id == Back || id == Forward || (id == Settings && settingsView)) {
    float sign = id == Forward ? 1.f : -1.f;
    g.DrawLine(&pen, x - sign * 2, y - 4, x + sign * 2, y);
    g.DrawLine(&pen, x + sign * 2, y, x - sign * 2, y + 4);
  } else if (id == Settings) {
    // Eight rounded teeth and a central bore, drawn as vectors at the current
    // DPI.
    std::array<PointF, 48> teeth{};
    constexpr float pi = 3.14159265358979323846f;
    constexpr float angles[] = {-22.5f, -14.f, -9.f, 9.f, 14.f, 22.5f};
    constexpr float radii[] = {6.1f, 6.1f, 8.f, 8.f, 6.1f, 6.1f};
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 6; ++j) {
        float theta = (-90 + i * 45 + angles[j]) * pi / 180;
        teeth[i * 6 + j] = {x + std::cos(theta) * radii[j],
                            y + std::sin(theta) * radii[j]};
      }
    GraphicsPath gear;
    gear.AddLines(teeth.data(), (INT)teeth.size());
    gear.CloseFigure();
    pen.SetLineJoin(LineJoinRound);
    g.DrawPath(&pen, &gear);
    g.DrawEllipse(&pen, x - 2.6f, y - 2.6f, 5.2f, 5.2f);
  } else {
    // Pencil: avoid a font-dependent Unicode fallback glyph.
    g.DrawLine(&pen, x - 4, y + 4, x + 3, y - 3);
    g.DrawLine(&pen, x - 2, y + 5, x + 5, y - 2);
    g.DrawLine(&pen, x + 3, y - 3, x + 5, y - 2);
    g.DrawLine(&pen, x - 4, y + 4, x - 5, y + 6);
  }
}
HWND button(int id, std::wstring caption, int x, int y, int width, int height) {
  HWND h = GetDlgItem(mainWindow, id);
  if (!h) {
    h = CreateWindowW(L"BUTTON", caption.c_str(),
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                      px((float)x), px((float)y), px((float)width),
                      px((float)height), mainWindow, (HMENU)(INT_PTR)id,
                      instance, nullptr);
    SetWindowSubclass(h, hoverButton, 2, 0);
    if (id >= Card && id < Card + 3)
      SetWindowSubclass(h, cardSubclass, 1, 0);
  } else {
    wchar_t old[256]{};
    GetWindowTextW(h, old, 256);
    if (caption != old)
      SetWindowTextW(h, caption.c_str());
    RECT r{};
    GetWindowRect(h, &r);
    MapWindowPoints(nullptr, mainWindow, (POINT *)&r, 2);
    if (r.left != px((float)x) || r.top != px((float)y) ||
        r.right - r.left != px((float)width) ||
        r.bottom - r.top != px((float)height))
      SetWindowPos(h, nullptr, px((float)x), px((float)y), px((float)width),
                   px((float)height), SWP_NOZORDER | SWP_NOACTIVATE);
  }
  return h;
}
void setVisible(HWND h, bool visible) {
  if (h && ((GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0) != visible)
    ShowWindow(h, visible ? SW_SHOWNA : SW_HIDE);
}
auto appMode(const App &app) {
  if (sandbox) {
    auto found = sandboxModes.find(app.path);
    return found == sandboxModes.end() ? tongzhuo::policy::Mode::Automatic
                                       : found->second;
  }
  return compatibility ? compatibility->selectedMode(app.window)
                       : tongzhuo::policy::Mode::Automatic;
}
std::wstring appModeLabel(const App &app) {
  auto mode = appMode(app);
  if (mode != tongzhuo::policy::Mode::Automatic)
    return tongzhuo::policy::label(mode);
  int kind = !sandbox && compatibility
                 ? compatibility->effectiveKind(app.window) : 0;
  return kind == 1 ? L"自动 · 独立展开 / 收起"
       : kind == 2 ? L"自动 · 独立窗口" : L"自动 · 跟随 Windows";
}
void updateSettingControls() {
  for (int id : {ToggleFullscreen, Range12, Range24, Range40}) {
    EnableWindow(GetDlgItem(mainWindow, id), hudEnabled);
    InvalidateRect(GetDlgItem(mainWindow, id), nullptr, FALSE);
  }
}
void updateAppActions() {
  int y = 150 + tongzhuo::desktopCardHeight((clientWidth - 80) / 3, clientHeight) + 52;
  for (int i = 0; i < 4; ++i) {
    int at = appPage * appRows() + i;
    bool available = at < (int)apps.size();
    HWND action = button(AppBehavior + i,
                         available ? appModeLabel(apps[at]) : L"桌面行为",
                         appSelectorX(), y + i * 40 + 5, appSelectorWidth(), 30);
    EnableWindow(action, available &&
        (sandbox || (compatibility && compatibility->canConfigure(apps[at].window))));
    setVisible(action, !settingsView && i < appRows() &&
                           appPage * appRows() + i < (int)apps.size() &&
                           y + (i + 1) * 40 <= clientHeight - 28);
  }
}
void layout() {
  static int oldWidth = -1, oldHeight = -1, oldPage = -1;
  static float oldDpi = 0;
  static bool oldSettings = false;
  static size_t oldCount = 0;
  RECT r{};
  GetClientRect(mainWindow, &r);
  clientWidth = (int)(r.right / dpi);
  clientHeight = (int)(r.bottom / dpi);
  appPage =
      std::clamp(appPage, 0, std::max(0, ((int)apps.size() - 1) / appRows()));
  bool geometry =
      tongzhuo::needsLayout(oldWidth < 0, oldWidth, oldHeight, clientWidth,
                            clientHeight, oldPage, desktopPage, oldSettings,
                            settingsView, oldCount, desks.size()) ||
      oldDpi != dpi;
  if (!geometry)
    return;
  ++layoutPasses;
  oldWidth = clientWidth;
  oldHeight = clientHeight;
  oldPage = desktopPage;
  oldSettings = settingsView;
  oldCount = desks.size();
  oldDpi = dpi;
  updateAppActions();
  button(Settings, settingsView ? L"返回桌面" : L"设置", clientWidth - 148, 18,
         36, 32);
  button(Minimize, L"收起到托盘", clientWidth - 99, 18, 32, 32);
  button(Close, L"关闭面板", clientWidth - 55, 18, 32, 32);
  int cw = (clientWidth - 80) / 3;
  int cardHeight = tongzhuo::desktopCardHeight(cw, clientHeight);
  button(Create, L"新建桌面", clientWidth - 158, 96, 130, 36);
  button(Back, L"上一组桌面", clientWidth - 228, 96, 28, 36);
  button(Forward, L"下一组桌面", clientWidth - 195, 96, 28, 36);
  button(Choose, L"查看桌面", clientWidth - 218, 150 + cardHeight + 12, 190,
         32);
  setVisible(GetDlgItem(mainWindow, Quit), false);
  for (int i = 0; i < 3; ++i) {
    int at = desktopPage * 3 + i;
    HWND h = GetDlgItem(mainWindow, Card + i);
    if (at <= (int)desks.size())
      h = button(Card + i,
                 at == (int)desks.size() ? L"新建桌面" : desks[at].name,
                 28 + i * (cw + 12), 150, cw, cardHeight);
    if (h)
      setVisible(h, !settingsView && at <= (int)desks.size());
  }
  for (int id : {Create, Back, Forward, Choose})
    setVisible(GetDlgItem(mainWindow, id), !settingsView);
  EnableWindow(GetDlgItem(mainWindow, Back), desktopPage > 0);
  EnableWindow(GetDlgItem(mainWindow, Forward),
               (desktopPage + 1) * 3 <= (int)desks.size());
  button(ToggleHud, hudEnabled ? L"关闭顶部快捷切换" : L"开启顶部快捷切换",
         clientWidth - 105, 160, 64, 32);
  button(ToggleFullscreen, hideFullscreen ? L"关闭全屏隐藏" : L"开启全屏隐藏",
         clientWidth - 105, 304, 64, 32);
  button(Range12, L"12 像素", clientWidth - 326, 232, 86, 34);
  button(Range24, L"24 像素", clientWidth - 228, 232, 86, 34);
  button(Range40, L"40 像素", clientWidth - 130, 232, 86, 34);
  button(ToggleStartup, startupEnabled ? L"关闭开机自启" : L"开启开机自启",
         clientWidth - 105, 398, 64, 32);
  setVisible(GetDlgItem(mainWindow, PreviewHud), false);
  for (int id :
       {ToggleHud, ToggleFullscreen, Range12, Range24, Range40, ToggleStartup})
    setVisible(GetDlgItem(mainWindow, id), settingsView);
  updateSettingControls();
}
void renderButton(Graphics &g, DRAWITEMSTRUCT *d) {
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  g.ScaleTransform(dpi, dpi);
  float w = (d->rcItem.right - d->rcItem.left) / dpi,
        h = (d->rcItem.bottom - d->rcItem.top) / dpi;
  int id = (int)d->CtlID;
  bool pressed = (d->itemState & ODS_SELECTED) != 0,
       disabled = (d->itemState & ODS_DISABLED) != 0;
  bool hovered = !disabled && GetPropW(d->hwndItem, L"TongZhuo.Hover");
  g.Clear(id >= AppBehavior && id < AppBehavior + 4 ? surface : canvas);
  if (id >= Card && id < Card + 3) {
    int at = desktopPage * 3 + id - Card;
    if (at == (int)desks.size()) {
      dashedCard(g, {1, 1, w - 2, h - 2}, hovered);
      plusGlyph(g, w / 2, 70, 10, hovered ? accent : muted);
      label(g, L"新建桌面", {0, 98, w, 30}, 12, muted, false,
            StringAlignmentCenter);
      return;
    }
    if (at > (int)desks.size())
      return;
    auto &desk = desks[at];
    bool selected = desk.id == current;
    Color stroke = selected ? accent : line;
    if (drag.active && drag.to == at)
      stroke = Color(155, 222, 196);
    round(g, {1, 1, w - 2, h - 2}, 13,
          selected  ? Color(31, 40, 57)
          : hovered ? raised
                    : surface,
          stroke);
    label(g, desk.name, {14, 8, w - 55, 28}, 13, text, true);
    buttonGlyph(g, 0, {w - 34, 8, 24, 26}, hovered ? text : muted);
    auto preview = tongzhuo::cardPreview(w, h);
    float previewH = preview.height;
    thumbnail(g, desk, {preview.x, preview.y, preview.width, previewH}, false);
    if (selected)
      round(
          g,
          {w * .32f, 44 + previewH + (h - 44 - previewH - 3) / 2, w * .36f, 3},
          1.5f, accent);
  } else if (id >= AppBehavior && id < AppBehavior + 4) {
    round(g, {1, 1, w - 2, h - 2}, 7, hovered ? raised : surface, line);
    wchar_t caption[128]{};
    GetWindowTextW(d->hwndItem, caption, 128);
    label(g, caption, {10, 0, w - 34, h}, 11, disabled ? muted : text);
    Pen arrow(disabled ? Color(77, 86, 100) : muted, 1.3f);
    g.DrawLine(&arrow, w - 21, h / 2 - 2, w - 17, h / 2 + 2);
    g.DrawLine(&arrow, w - 17, h / 2 + 2, w - 13, h / 2 - 2);
  } else if (id == ToggleHud || id == ToggleFullscreen || id == ToggleStartup) {
    bool enabled = id == ToggleHud ? hudEnabled
                   : id == ToggleStartup ? startupEnabled : hideFullscreen;
    if (disabled) {
      round(g, {7, 4, w - 14, h - 8}, 12, Color(38, 44, 54));
      SolidBrush b(Color(90, 101, 116));
      g.FillEllipse(&b, enabled ? w - 29 : 11.f, 8.f, 16.f, 16.f);
      return;
    }
    round(g, {7, 4, w - 14, h - 8}, 12, enabled ? accent : Color(61, 70, 88));
    SolidBrush b(enabled ? Color(28, 36, 59) : Color(200, 209, 226));
    g.FillEllipse(&b, enabled ? w - 29 : 11.f, 8.f, 16.f, 16.f);
  } else {
    bool primary = id == Create || id == PreviewHud;
    bool selected = id == Range12 && range == 12 ||
                    id == Range24 && range == 24 ||
                    id == Range40 && range == 40;
    Color fill = disabled             ? canvas
                 : primary              ? (pressed   ? Color(117, 144, 222)
                                         : hovered ? Color(163, 184, 255)
                                                   : accent)
                 : selected           ? Color(48, 65, 105)
                 : pressed || hovered ? raised
                                      : canvas;
    round(g, {1, 1, w - 2, h - 2}, 8, fill,
          selected ? (disabled ? line : accent) : Color(0, 0, 0, 0));
    std::wstring value = id == Create       ? L"＋ 新建桌面"
                         : id == Settings   ? (settingsView ? L"‹" : L"⚙")
                         : id == Minimize   ? L"−"
                         : id == Close      ? L"×"
                         : id == Back       ? L"‹"
                         : id == Forward    ? L"›"
                         : id == Quit       ? L"退出同桌"
                         : id == PreviewHud ? L"预览快捷切换"
                         : id == Range12    ? L"近"
                         : id == Range24    ? L"标准"
                         : id == Range40    ? L"宽"
                                            : L"";
    if (id == Choose) {
      int at = indexOf(viewed);
      value = (at >= 0 ? desks[at].name : L"当前桌面") + L"  ⌄";
      round(g, {1, 1, w - 2, h - 2}, 8, surface, line);
    }
    if (id == Settings || id == Close || id == Minimize || id == Back ||
        id == Forward)
      buttonGlyph(g, id, {0, 0, w, h},
                  disabled  ? Color(74, 84, 105)
                  : hovered ? text
                            : muted);
    else
      label(g, value, {0, 0, w, h}, 12.f,
            disabled   ? Color(74, 84, 105)
            : primary  ? Color(20, 28, 48)
            : selected ? accent
                       : muted,
            false, StringAlignmentCenter);
  }
  if (d->itemState & ODS_FOCUS)
    round(g, {4, 4, w - 8, h - 8}, 6, Color(0, 0, 0, 0), Color(108, 128, 193));
}
void drawButton(DRAWITEMSTRUCT *d) {
  const int width = d->rcItem.right - d->rcItem.left,
            height = d->rcItem.bottom - d->rcItem.top;
  if (width <= 0 || height <= 0)
    return;
  Bitmap buffer(width, height, PixelFormat32bppPARGB);
  {
    Graphics g(&buffer);
    renderButton(g, d);
  }
  Graphics out(d->hDC);
  out.DrawImage(&buffer, d->rcItem.left, d->rcItem.top, width, height);
}
void paintMain(HDC dc) {
  RECT r{};
  GetClientRect(mainWindow, &r);
  Bitmap *buffer = mainBuffer.get(r.right, r.bottom);
  Graphics g(buffer);
  g.Clear(canvas);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  g.ScaleTransform(dpi, dpi);
  round(g, {.5f, .5f, clientWidth - 1.f, clientHeight - 1.f}, 11,
        Color(0, 0, 0, 0), line);
  round(g, {28, 20, 30, 30}, 9, Color(128, 150, 245));
  monitorGlyph(g, 33, 26, Color(24, 32, 55));
  label(g, L"同桌", {70, 17, 160, 36}, 21, text, true);
  Pen sep(line, 1);
  g.DrawLine(&sep, 28.f, 70.f, (float)clientWidth - 28, 70.f);
  if (settingsView) {
    label(g, L"设置", {28, 92, 400, 33}, 23, text, true);
    round(g, {28, 138, (float)clientWidth - 56, 220}, 13, Color(0, 0, 0, 0),
          line);
    label(g, L"顶部快捷切换", {44, 151, (float)clientWidth - 180, 27}, 14, text,
          true);
    label(g, L"靠近顶边显示小条，再靠近展开桌面",
          {44, 181, (float)clientWidth - 160, 23}, 11, muted);
    Pen divider(line, 1);
    g.DrawLine(&divider, 44.f, 216.f, (float)clientWidth - 44, 216.f);
    g.DrawLine(&divider, 44.f, 286.f, (float)clientWidth - 44, 286.f);
    Color dependent = hudEnabled ? text : Color(89, 100, 117);
    label(g, L"唤出距离", {56, 234, 250, 27}, 13, dependent);
    label(g, L"全屏时隐藏", {56, 306, 350, 28}, 13, dependent);
    round(g, {28, 382, (float)clientWidth - 56, 64}, 13, Color(0, 0, 0, 0),
          line);
    label(g, L"开机自启", {44, 399, 350, 28}, 14, text, true);
  } else {
    label(g, L"桌面", {28, 94, 300, 30}, 23, text, true);
    label(g, L"点击切换 · 拖动排序 · F2 重命名",
          {28, 126, (float)clientWidth - 56, 19}, 10.5f, muted);
    float heading =
        (float)(150 + tongzhuo::desktopCardHeight((clientWidth - 80) / 3, clientHeight) + 12);
    label(g, L"桌面应用", {28, heading, 300, 36}, 15, text, true);
    float y = heading + 40;
    float listHeight = std::max(0.f, std::min(160.f, clientHeight - 28 - y));
    round(g, {28, y, (float)clientWidth - 56, listHeight}, 12, surface, line);
    if (apps.empty()) {
      label(g, L"这个桌面还没有打开的应用",
            {48, y + 52, (float)clientWidth - 96, 32}, 17, text, true,
            StringAlignmentCenter);
      label(g, L"打开应用后会自动显示在这里",
            {48, y + 89, (float)clientWidth - 96, 24}, 11, muted, false,
            StringAlignmentCenter);
    }
    for (int i = 0; i < 4; ++i) {
      int at = appPage * appRows() + i;
      if (at >= (int)apps.size())
        break;
      float row = y + i * 40.f;
      if (row + 40 > y + listHeight)
        break;
      if (i)
        g.DrawLine(&sep, 44.f, row, (float)clientWidth - 44, row);
      iconDraw(g, iconOf(apps[at].path), 44, row + 11, 22);
      bool showCount = clientWidth >= 680;
      label(g, apps[at].name,
            {80, row + 5, (float)appSelectorX() - 94 - (showCount ? 94 : 0), 33}, 12);
      if (showCount)
        label(g, std::to_wstring(apps[at].count) + L" 个窗口",
              {(float)appSelectorX() - 94, row + 5, 80, 33}, 11, muted, false,
              StringAlignmentFar);
    }
  }
  Graphics out(dc);
  g.Flush(FlushIntentionSync);
  out.DrawImage(buffer, 0, 0);
}

struct MenuItem {
  std::wstring label;
  int command = 0;
  GUID id{};
};
std::vector<MenuItem> menuItems;
int menuHover = -1, menuScroll = 0;
float menuScale = 1;
int menuVisibleRows = 1;
HWND behaviorTarget{};
std::wstring behaviorPath;
float monitorScale(HMONITOR monitor);
void closeMenu() {
  if (menuWindow) {
    HWND h = menuWindow;
    menuWindow = nullptr;
    DestroyWindow(h);
  }
}
void paintMenu(Graphics &g, int width, int height) {
  g.Clear(surface);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  g.ScaleTransform(menuScale, menuScale);
  float logicalWidth = width / menuScale, logicalHeight = height / menuScale;
  round(g, {.5f, .5f, logicalWidth - 1, logicalHeight - 1}, 10, surface, line);
  menuVisibleRows = std::max(1, (int)((logicalHeight - 12) / 36));
  for (int row = 0; row < menuVisibleRows; ++row) {
    int at = menuScroll + row;
    if (at >= (int)menuItems.size())
      break;
    float y = 6 + row * 36.f;
    if (at == menuHover && menuItems[at].command)
      round(g, {6, y, logicalWidth - 12, 34}, 7, raised);
    label(g, menuItems[at].label, {17, y, logicalWidth - 34, 34}, 12,
          !menuItems[at].command                        ? muted
          : menuItems[at].id == viewed && menuKind == 1 ? accent
                                                        : text);
  }
}
int menuHit(int pixelY) {
  float y = pixelY / menuScale;
  if (y < 6)
    return -1;
  int row = (int)((y - 6) / 36), at = menuScroll + row;
  return row < menuVisibleRows && at < (int)menuItems.size() ? at : -1;
}
void selectMenuItem(int hit) {
  if (hit < 0 || hit >= (int)menuItems.size())
    return;
  auto item = menuItems[hit];
  if (!item.command)
    return;
  closeMenu();
  if (item.command == 1) {
    viewed = item.id;
    follow = viewed == current;
    appPage = 0;
    refresh(true);
  } else if (item.command == 2)
    showMain();
  else if (item.command == 3) {
    settingsView = true;
    showMain();
    refresh(true);
  } else if (item.command == 4)
    userSwitch(item.id);
  else if (item.command == 5)
    DestroyWindow(mainWindow);
  else if (item.command >= 20 && item.command <= 23) {
    auto mode = (tongzhuo::policy::Mode)(item.command - 20);
    bool ok = sandbox ||
              (compatibility && compatibility->setMode(behaviorTarget, mode));
    if (sandbox)
      sandboxModes[behaviorPath] = mode;
    status =
        ok ? std::wstring(L"桌面行为已设置：") + tongzhuo::policy::label(mode)
           : L"无法安全应用此行为，窗口或恢复记录需要重新检查。";
    inventoryDirty = true;
    refresh(true);
  }
}
LRESULT CALLBACK menuProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT p;
    HDC dc = BeginPaint(h, &p);
    RECT r{};
    GetClientRect(h, &r);
    Bitmap b(std::max(1L, r.right), std::max(1L, r.bottom),
             PixelFormat32bppPARGB);
    {
      Graphics g(&b);
      paintMenu(g, r.right, r.bottom);
    }
    Graphics out(dc);
    out.DrawImage(&b, 0, 0);
    EndPaint(h, &p);
    return 0;
  }
  case WM_MOUSEMOVE: {
    int hit = menuHit(GET_Y_LPARAM(l));
    if (hit != menuHover) {
      menuHover = hit;
      InvalidateRect(h, nullptr, FALSE);
    }
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
    TrackMouseEvent(&track);
    return 0;
  }
  case WM_MOUSELEAVE:
    menuHover = -1;
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  case WM_LBUTTONUP:
    selectMenuItem(menuHit(GET_Y_LPARAM(l)));
    return 0;
  case WM_MOUSEWHEEL:
    menuScroll =
        std::clamp(menuScroll + (GET_WHEEL_DELTA_WPARAM(w) < 0 ? 1 : -1), 0,
                   std::max(0, (int)menuItems.size() - menuVisibleRows));
    menuHover = -1;
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  case WM_KEYDOWN:
    if (w == VK_ESCAPE) {
      closeMenu();
      return 0;
    }
    if (w == VK_UP || w == VK_DOWN) {
      menuHover = std::clamp(menuHover + (w == VK_DOWN ? 1 : -1), 0,
                             (int)menuItems.size() - 1);
      if (menuHover < menuScroll)
        menuScroll = menuHover;
      else if (menuHover >= menuScroll + menuVisibleRows)
        menuScroll = menuHover - menuVisibleRows + 1;
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    }
    if (w == VK_RETURN)
      selectMenuItem(menuHover);
    return 0;
  case WM_ACTIVATE:
    if (LOWORD(w) == WA_INACTIVE)
      closeMenu();
    return 0;
  case WM_DESTROY:
    if (menuWindow == h)
      menuWindow = nullptr;
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}
void populateMenu(bool trayMenu) {
  menuItems.clear();
  menuKind = trayMenu ? 2 : 1;
  menuScroll = 0;
  menuHover = -1;
  if (trayMenu) {
    menuItems.push_back({L"打开同桌", 2, {}});
    menuItems.push_back({L"设置", 3, {}});
    for (auto &d : desks)
      menuItems.push_back(
          {(d.id == current ? L"● " : L"   ") + d.name, 4, d.id});
    menuItems.push_back({L"退出同桌", 5, {}});
  } else
    for (auto &d : desks)
      menuItems.push_back(
          {d.name + (d.id == current ? L" · 当前" : L""), 1, d.id});
}
void openMenu(bool trayMenu = false) {
  closeMenu();
  inventory(false);
  populateMenu(trayMenu);
  POINT anchor{};
  GetCursorPos(&anchor);
  if (trayMenu) {
    NOTIFYICONIDENTIFIER identifier{sizeof(identifier)};
    identifier.hWnd = mainWindow;
    identifier.uID = 1;
    RECT icon{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &icon)))
      anchor = {icon.right, icon.top};
  } else {
    RECT r{};
    GetWindowRect(GetDlgItem(mainWindow, Choose), &r);
    anchor = {r.right, r.bottom};
  }
  MONITORINFO monitor{sizeof(monitor)};
  HMONITOR target = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(target, &monitor))
    return;
  menuScale = monitorScale(target);
  int width = (int)(250 * menuScale),
      height = (int)((menuItems.size() * 36 + 12) * menuScale),
      gap = (int)(8 * menuScale);
  tongzhuo::PopupBounds bounds;
  if (trayMenu)
    bounds = tongzhuo::popupAbove(
        anchor.x, anchor.y, width, height, monitor.rcWork.left,
        monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom, gap);
  else {
    bounds =
        tongzhuo::popupAbove(anchor.x, anchor.y + height + gap * 2, width,
                             height, monitor.rcWork.left, monitor.rcWork.top,
                             monitor.rcWork.right, monitor.rcWork.bottom, gap);
    if (bounds.y < anchor.y)
      bounds = tongzhuo::popupAbove(
          anchor.x, anchor.y, width, height, monitor.rcWork.left,
          monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom, gap);
  }
  menuVisibleRows = std::max(1, (int)((bounds.height / menuScale - 12) / 36));
  menuWindow =
      CreateWindowExW(WS_EX_TOOLWINDOW, MenuClass, L"同桌 · 选择桌面", WS_POPUP,
                      bounds.x, bounds.y, bounds.width, bounds.height,
                      mainWindow, nullptr, instance, nullptr);
  frame(menuWindow);
  ShowWindow(menuWindow, SW_SHOWNORMAL);
  SetForegroundWindow(menuWindow);
  SetFocus(menuWindow);
}
void populateBehaviorMenu(const std::wstring &name,
                          tongzhuo::policy::Mode selected) {
  menuItems.push_back({name + L" · 桌面行为", 0, {}});
  for (auto mode :
       {tongzhuo::policy::Mode::Automatic, tongzhuo::policy::Mode::Separate,
        tongzhuo::policy::Mode::Shared, tongzhuo::policy::Mode::Native})
    menuItems.push_back({std::wstring(selected == mode ? L"●  " : L"    ") +
                             tongzhuo::policy::label(mode),
                         20 + (int)mode,
                         {}});
  menuItems.push_back({L"独立窗口需要应用自身支持新建", 0, {}});
}
void openAppBehavior(int row) {
  int at = appPage * appRows() + row;
  if (at < 0 || at >= (int)apps.size())
    return;
  auto app = apps[at];
  if (!sandbox && (!compatibility || !IsWindow(app.window) ||
                   !compatibility->canConfigure(app.window))) {
    status = L"此宿主没有独立应用标识，保留 Windows 原有行为。";
    InvalidateRect(mainWindow, nullptr, FALSE);
    return;
  }
  closeMenu();
  behaviorTarget = app.window;
  behaviorPath = app.path;
  menuKind = 3;
  menuScroll = 0;
  menuHover = -1;
  menuItems.clear();
  auto selected = appMode(app);
  populateBehaviorMenu(app.name, selected);
  RECT anchor{};
  GetWindowRect(GetDlgItem(mainWindow, AppBehavior + row), &anchor);
  MONITORINFO monitor{sizeof(monitor)};
  auto target = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  GetMonitorInfoW(target, &monitor);
  menuScale = monitorScale(target);
  int width = (int)(330 * menuScale),
      height = (int)((menuItems.size() * 36 + 12) * menuScale);
  auto bounds = tongzhuo::popupAbove(
      anchor.right, anchor.top, width, height, monitor.rcWork.left,
      monitor.rcWork.top, monitor.rcWork.right, monitor.rcWork.bottom,
      (int)(6 * menuScale));
  menuVisibleRows = std::max(1, (int)((bounds.height / menuScale - 12) / 36));
  menuWindow =
      CreateWindowExW(WS_EX_TOOLWINDOW, MenuClass, L"同桌 · 应用桌面行为",
                      WS_POPUP, bounds.x, bounds.y, bounds.width, bounds.height,
                      mainWindow, nullptr, instance, nullptr);
  frame(menuWindow);
  ShowWindow(menuWindow, SW_SHOWNORMAL);
  SetForegroundWindow(menuWindow);
  SetFocus(menuWindow);
}
void cancelName() {
  if (editWindow) {
    HWND old = editWindow;
    editWindow = nullptr;
    DestroyWindow(old);
  }
  EnableWindow(mainWindow, TRUE);
  if (IsWindowVisible(mainWindow))
    SetForegroundWindow(mainWindow);
}
void paintNameWindow(HWND h, HDC dc) {
  RECT r;
  GetClientRect(h, &r);
  Bitmap b(std::max(1L, r.right), std::max(1L, r.bottom),
           PixelFormat32bppPARGB);
  Graphics g(&b);
  g.Clear(canvas);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  g.ScaleTransform(nameDpi, nameDpi);
  label(g, editingNew ? L"新建桌面" : L"重命名桌面", {24, 17, 352, 34}, 21,
        text, true);
  label(g, sandbox ? L"仅修改测试数据" : L"名称会同步到 Windows",
        {24, 52, 352, 22}, 11, muted);
  round(g, {24, 82, 352, 52}, 9, surface, line);
  Graphics out(dc);
  out.DrawImage(&b, 0, 0);
}
void layoutName(HWND h) {
  struct Item { int id, x, y, width, height; };
  for (auto item : {Item{10, 38, 96, 324, 25}, Item{1, 158, 164, 86, 36},
                    Item{2, 256, 164, 120, 36}})
    SetWindowPos(GetDlgItem(h, item.id), nullptr, namePx((float)item.x),
                  namePx((float)item.y), namePx((float)item.width),
                  namePx((float)item.height), SWP_NOZORDER | SWP_NOACTIVATE);
  auto font = CreateFontW(-namePx(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
  if (font) {
    SendMessageW(nameField, WM_SETFONT, (WPARAM)font, TRUE);
    if (uiFont)
      DeleteObject(uiFont);
    uiFont = font;
  }
}
LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
  case WM_NCCALCSIZE:
    return 0;
  case WM_NCPAINT:
    return 0;
  case WM_NCACTIVATE:
    return DefWindowProcW(h, m, w, -1);
  case WM_CREATE: {
    if (!renderTest)
      nameDpi = GetDpiForWindow(h) / 96.f;
    frame(h);
    nameField = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        namePx(38), namePx(96), namePx(324), namePx(25), h, (HMENU)10, instance, nullptr);
    SendMessageW(nameField, WM_SETFONT, (WPARAM)uiFont, TRUE);
    SendMessageW(nameField, EM_SETLIMITTEXT, 64, 0);
    CreateWindowW(L"BUTTON", L"取消",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, namePx(158),
                  namePx(164), namePx(86), namePx(36), h, (HMENU)1, instance, nullptr);
    CreateWindowW(L"BUTTON", editingNew ? L"创建" : L"保存",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, namePx(256),
                  namePx(164), namePx(120), namePx(36), h, (HMENU)2, instance, nullptr);
    layoutName(h);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_PRINTCLIENT:
    paintNameWindow(h, (HDC)w);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(h, &ps);
    paintNameWindow(h, dc);
    EndPaint(h, &ps);
    return 0;
  }
  case WM_DRAWITEM: {
    auto *d = (DRAWITEMSTRUCT *)l;
    Graphics g(d->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.ScaleTransform(nameDpi, nameDpi);
    float ww = (d->rcItem.right - d->rcItem.left) / nameDpi,
          hh = (d->rcItem.bottom - d->rcItem.top) / nameDpi;
    g.Clear(canvas);
    round(g, {1, 1, ww - 2, hh - 2}, 8, d->CtlID == 2 ? accent : raised);
    label(g, d->CtlID == 2 ? (editingNew ? L"创建" : L"保存") : L"取消",
          {0, 0, ww, hh}, 12, d->CtlID == 2 ? Color(22, 29, 48) : text, false,
          StringAlignmentCenter);
    return TRUE;
  }
  case WM_CTLCOLOREDIT: {
    HDC dc = (HDC)w;
    SetTextColor(dc, RGB(237, 241, 249));
    SetBkColor(dc, RGB(25, 30, 42));
    static HBRUSH brush = CreateSolidBrush(RGB(25, 30, 42));
    return (LRESULT)brush;
  }
  case WM_COMMAND:
    if (LOWORD(w) == 1)
      cancelName();
    else if (LOWORD(w) == 2)
      renameOrCreate();
    return 0;
  case WM_CLOSE:
    cancelName();
    return 0;
  case WM_DPICHANGED: {
    nameDpi = HIWORD(w) / 96.f;
    auto *bounds = (RECT *)l;
    SetWindowPos(h, nullptr, bounds->left, bounds->top,
                  bounds->right - bounds->left, bounds->bottom - bounds->top,
                  SWP_NOZORDER | SWP_NOACTIVATE);
    layoutName(h);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    return 0;
  }
  case WM_NCHITTEST: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    if (p.y < namePx(75))
      return HTCAPTION;
    return HTCLIENT;
  }
  }
  return DefWindowProcW(h, m, w, l);
}
void openName(bool create, GUID id) {
  if (editWindow)
    return;
  closeMenu();
  editingNew = create;
  editingId = id;
  nameDpi = dpi;
  RECT owner{};
  GetClientRect(mainWindow, &owner);
  MapWindowPoints(mainWindow, nullptr, (POINT *)&owner, 2);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(mainWindow, MONITOR_DEFAULTTONEAREST),
                  &monitor);
  auto bounds = tongzhuo::popupCentered(
      owner.left, owner.top, owner.right, owner.bottom, namePx(400), namePx(226),
      monitor.rcWork.left, monitor.rcWork.top, monitor.rcWork.right,
      monitor.rcWork.bottom);
  int x = bounds.x, y = bounds.y;
  EnableWindow(mainWindow, FALSE);
  editWindow =
      CreateWindowExW(WS_EX_TOOLWINDOW, EditClass,
                      create ? L"同桌 · 新建桌面" : L"同桌 · 重命名",
                      WS_POPUP | WS_CLIPCHILDREN, x, y, namePx(400),
                      namePx(226), mainWindow, nullptr, instance, nullptr);
  std::wstring initial = create ? L"桌面 " + std::to_wstring(desks.size() + 1)
                         : indexOf(id) >= 0 ? desks[indexOf(id)].name
                                            : L"";
  SetWindowTextW(nameField, initial.c_str());
  if (!renderTest) {
    ShowWindow(editWindow, SW_SHOWNORMAL);
    SetForegroundWindow(editWindow);
    SetFocus(nameField);
  }
  SendMessageW(nameField, EM_SETSEL, 0, -1);
}

float hudScale = 1;
int hudWidth = 0, hudHeight = 0;
HMONITOR hudMonitor{};
tongzhuo::Reveal shown = tongzhuo::Reveal::Hidden;
bool hudTimer = false;
int hudHover = -1, hudPressed = -1, hudPage = 0, hudInterval = 0;

float monitorScale(HMONITOR monitor) {
  using GetDpi = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
  static HMODULE module =
      LoadLibraryExW(L"Shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  static auto function =
      module ? (GetDpi)GetProcAddress(module, "GetDpiForMonitor") : nullptr;
  UINT x = 96, y = 96;
  if (function && SUCCEEDED(function(monitor, 0, &x, &y)))
    return x / 96.f;
  return dpi;
}
void paintStrip(Graphics &g, int width, int height, float scale) {
  g.Clear(Color(34, 41, 53));
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.ScaleTransform(scale, scale);
  round(g, {13, (height / scale - 3) / 2, width / scale - 26, 3}, 1.5f, accent);
}
void paintHud(Graphics &g) {
  if (shown == tongzhuo::Reveal::Strip) {
    paintStrip(g, hudWidth, hudHeight, hudScale);
    return;
  }
  g.Clear(surface);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  g.ScaleTransform(hudScale, hudScale);
  hudHits.clear();
  const float width = hudWidth / hudScale, height = hudHeight / hudScale;
  round(g, {.5f, .5f, width - 1, height - 1}, 14, surface, line);
  int count = std::min(3, std::max(0, (int)desks.size() - hudPage * 3));
  float tile = tongzhuo::hudTileWidth(width, count);
  const float step = tile + 8;
  for (int i = 0; i < count; ++i) {
    int at = hudPage * 3 + i;
    auto &d = desks[at];
    RectF box(10 + i * step, 8, tile, 132);
    bool selected = d.id == current;
    round(g, box, 10,
          selected        ? Color(34, 44, 63)
          : hudHover == i ? raised
                          : surface,
          selected ? Color(95, 121, 174) : Color(0, 0, 0, 0));
    label(g, d.name, {box.X + 10, box.Y + 4, box.Width - 30, 23}, 11.5f, text,
          true);
    if (selected) {
      SolidBrush dot(accent);
      g.FillEllipse(&dot, box.GetRight() - 17, box.Y + 13, 5.f, 5.f);
    }
    float previewH = tongzhuo::previewHeight(box.Width - 14);
    thumbnail(g, d, {box.X + 7, box.Y + 32, box.Width - 14, previewH}, false,
              hudScale);
    if (selected)
      round(g,
            {box.X + box.Width * .32f,
             box.Y + 32 + previewH + (box.Height - 32 - previewH - 3) / 2,
             box.Width * .36f, 3},
            1.5f, accent);
    hudHits.push_back({box, at, false, d.id});
  }
  RectF create(10 + count * step, 8, tile, 132);
  dashedCard(g, create, hudHover == count);
  plusGlyph(g, create.X + create.Width / 2, create.Y + create.Height / 2, 10,
            hudHover == count ? accent : muted);
  label(g, L"新建桌面", {create.X + 8, create.Y + 94, create.Width - 16, 22},
        10.5f, muted, false, StringAlignmentCenter);
  hudHits.push_back({create, -1, true});
  if (desks.size() > 3)
    label(g,
          std::to_wstring(hudPage + 1) + L" / " +
              std::to_wstring(tongzhuo::lastHudPage(desks.size()) + 1) +
              L"   ·   滚轮翻页",
          {14, 143, width - 28, 18}, 9.5f, muted, false, StringAlignmentCenter);
}
void showHud(tongzhuo::Reveal state, HMONITOR monitor) {
  if ((residentTest || renderTest) && state != tongzhuo::Reveal::Hidden)
    return;
  if (state == tongzhuo::Reveal::Hidden) {
    hudContentVisible = false;
    bool wasShown = shown != state;
    shown = state;
    hudMonitor = nullptr;
    hudHover = hudPressed = -1;
    if (wasShown) {
      if (hudWindow)
        ShowWindow(hudWindow, SW_HIDE);
      if (stripWindow)
        ShowWindow(stripWindow, SW_HIDE);
    }
    hudBuffer.release();
    if (wasShown)
      SetTimer(mainWindow, 95, 1500, nullptr);
    return;
  }
  if (!monitor || (state == shown && monitor == hudMonitor))
    return;
  MONITORINFO info{sizeof(info)};
  if (!GetMonitorInfoW(monitor, &info))
    return;
  if (state == tongzhuo::Reveal::Panel && shown != tongzhuo::Reveal::Panel) {
    inventoryDirty = true;
    inventory();
    inventoryDirty = false;
    hudPage = std::clamp(indexOf(current) / 3, 0,
                         tongzhuo::lastHudPage(desks.size()));
  }
  hudContentVisible = state == tongzhuo::Reveal::Panel;
  shown = state;
  hudMonitor = monitor;
  hudHover = hudPressed = -1;
  hudScale = monitorScale(monitor);
  bool strip = state == tongzhuo::Reveal::Strip;
  int n = std::min(3, std::max(0, (int)desks.size() - hudPage * 3));
  int logical =
      strip
          ? 72
          : tongzhuo::hudLogicalWidth(
                n,
                (int)((info.rcMonitor.right - info.rcMonitor.left) / hudScale) -
                    24);
  int width = (int)(logical * hudScale),
      height = (int)((strip              ? 9
                      : desks.size() > 3 ? 166
                                         : 148) *
                     hudScale);
  if (!strip) {
    hudWidth = width;
    hudHeight = height;
  }
  int x = info.rcMonitor.left +
          (info.rcMonitor.right - info.rcMonitor.left - width) / 2;
  int y = info.rcMonitor.top + (int)((strip ? 2 : 6) * hudScale);
  HWND &popup = strip ? stripWindow : hudWindow;
  if (!popup) {
    popup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, HudClass,
        strip ? L"同桌 · 顶部感应条" : L"同桌 · 顶部快捷切换", WS_POPUP, x, y,
        width, height, mainWindow, nullptr, instance, nullptr);
    frame(popup);
  }
  int radius = (int)((strip ? 9 : 28) * hudScale);
  SetWindowPos(popup, HWND_TOPMOST, x, y, width, height,
               SWP_NOACTIVATE | SWP_NOREDRAW);
  SetWindowRgn(popup,
               CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius),
               FALSE);
  InvalidateRect(popup, nullptr, FALSE);
  UpdateWindow(popup);
  // Two small native popups: no visible resizing of the old strip surface.
  ShowWindow(popup, SW_SHOWNOACTIVATE);
  HWND other = strip ? hudWindow : stripWindow;
  if (other)
    ShowWindow(other, SW_HIDE);
}
bool fullscreen(HMONITOR monitor) {
  HWND foreground = GetForegroundWindow();
  if (!foreground || foreground == mainWindow || foreground == hudWindow ||
      foreground == editWindow)
    return false;
  RECT r{};
  GetWindowRect(foreground, &r);
  MONITORINFO i{sizeof(i)};
  GetMonitorInfoW(monitor, &i);
  return r.left <= i.rcMonitor.left && r.top <= i.rcMonitor.top &&
         r.right >= i.rcMonitor.right && r.bottom >= i.rcMonitor.bottom;
}
void updateHudTimer();
void setHudInterval(int interval) {
  if (interval == hudInterval)
    return;
  hudInterval = interval;
  SetTimer(mainWindow, 94, interval, nullptr);
}
void tickHud() {
  POINT p{};
  bool valid = GetCursorPos(&p) != FALSE;
  HMONITOR monitor = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{sizeof(info)};
  if (!GetMonitorInfoW(monitor, &info))
    return;
  static HMONITOR cachedMonitor{};
  static float factor = 1;
  if (monitor != cachedMonitor) {
    cachedMonitor = monitor;
    factor = monitorScale(monitor);
  }
  int center = (info.rcMonitor.left + info.rcMonitor.right) / 2;
  bool proximity = abs(p.x - center) < (int)(150 * factor) &&
                   p.y >= info.rcMonitor.top &&
                   p.y < info.rcMonitor.top + (int)(range * factor);
  bool lip = p.y < info.rcMonitor.top + (int)(8 * factor);
  setHudInterval(
      tongzhuo::edgeInterval(proximity, shown != tongzhuo::Reveal::Hidden));
  bool down = false;
  for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
    down |= (GetAsyncKeyState(key) & 0x8000) != 0;
  bool active = proximity || shown != tongzhuo::Reveal::Hidden;
  GUITHREADINFO thread{sizeof(thread)};
  if (active)
    GetGUIThreadInfo(0, &thread);
  RECT rect{};
  if (hudWindow && shown == tongzhuo::Reveal::Panel)
    GetWindowRect(hudWindow, &rect);
  // Include the six-pixel path from the top edge to the panel; no close/reopen
  // jitter.
  if (shown == tongzhuo::Reveal::Panel)
    rect.top = info.rcMonitor.top;
  bool hit = shown == tongzhuo::Reveal::Panel && monitor == hudMonitor &&
             PtInRect(&rect, p);
  if (active && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
    previewUntil = 0;
    hover.dismiss();
    showHud(tongzhuo::Reveal::Hidden, nullptr);
    updateHudTimer();
    return;
  }
  if (previewUntil > GetTickCount64()) {
    if (down && !hit) {
      previewUntil = 0;
      hover.dismiss();
      showHud(tongzhuo::Reveal::Hidden, nullptr);
      updateHudTimer();
    }
    return;
  }
  if (previewUntil) {
    previewUntil = 0;
    hover.dismiss();
    showHud(tongzhuo::Reveal::Hidden, nullptr);
    updateHudTimer();
  }
  tongzhuo::HoverSample sample;
  sample.x = p.x;
  sample.y = p.y;
  sample.valid = valid;
  sample.proximity = proximity;
  sample.lip = lip;
  sample.inside = hit && (!down || GetCapture() == hudWindow);
  sample.down = down;
  sample.movingWindow = (thread.flags & GUI_INMOVESIZE) != 0;
  sample.suppressed =
      residentTest || (active && hideFullscreen && fullscreen(monitor)) ||
      (shown != tongzhuo::Reveal::Hidden && monitor != hudMonitor) ||
      editWindow || menuWindow;
  sample.now = GetTickCount64();
  auto next = hover.update(sample);
  showHud(next, monitor);
}
void updateHudTimer() {
  bool wanted = !renderTest && ((hudEnabled && desks.size() > 1) ||
                                previewUntil > GetTickCount64());
  if (wanted && !hudTimer) {
    hudTimer = true;
    setHudInterval(tongzhuo::EdgeIdleInterval);
  } else if (!wanted && hudTimer) {
    KillTimer(mainWindow, 94);
    hudTimer = false;
    hudInterval = 0;
    hover.reset();
    showHud(tongzhuo::Reveal::Hidden, nullptr);
  }
}
int hudHit(float x, float y) {
  for (size_t i = 0; i < hudHits.size(); ++i)
    if (inside(hudHits[i].rect, x, y))
      return (int)i;
  return -1;
}
void repaginateHud() {
  hudPage = std::clamp(hudPage, 0, tongzhuo::lastHudPage(desks.size()));
  hudHover = hudPressed = -1;
  // Keep the outer popup geometry stable while paging, including the short last
  // page.
  InvalidateRect(hudWindow, nullptr, FALSE);
  UpdateWindow(hudWindow);
}
LRESULT CALLBACK hudProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (h && h == stripWindow && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST)
    return 0;
  switch (m) {
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r{};
    GetClientRect(h, &r);
    if (h == stripWindow) {
      Bitmap buffer(std::max(1L, r.right), std::max(1L, r.bottom),
                    PixelFormat32bppPARGB);
      {
        Graphics g(&buffer);
        paintStrip(g, r.right, r.bottom, hudScale);
      }
      Graphics out(dc);
      out.DrawImage(&buffer, 0, 0);
      EndPaint(h, &ps);
      return 0;
    }
    Bitmap *buffer = hudBuffer.get(r.right, r.bottom);
    {
      Graphics g(buffer);
      paintHud(g);
    }
    Graphics out(dc);
    out.DrawImage(buffer, 0, 0);
    EndPaint(h, &ps);
    return 0;
  }
  case WM_MOUSEMOVE: {
    if (shown != tongzhuo::Reveal::Panel)
      return 0;
    int at = hudHit(GET_X_LPARAM(l) / hudScale, GET_Y_LPARAM(l) / hudScale);
    if (at != hudHover) {
      hudHover = at;
      InvalidateRect(h, nullptr, FALSE);
    }
    TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
    TrackMouseEvent(&t);
    return 0;
  }
  case WM_MOUSELEAVE:
    if (hudHover != -1) {
      hudHover = -1;
      InvalidateRect(h, nullptr, FALSE);
    }
    return 0;
  case WM_LBUTTONDOWN:
    if (shown == tongzhuo::Reveal::Panel) {
      hudPressed =
          hudHit(GET_X_LPARAM(l) / hudScale, GET_Y_LPARAM(l) / hudScale);
      if (hudPressed >= 0)
        SetCapture(h);
    }
    return 0;
  case WM_CAPTURECHANGED:
    hudPressed = -1;
    return 0;
  case WM_LBUTTONUP: {
    int selected = hudPressed;
    int hit = hudHit(GET_X_LPARAM(l) / hudScale, GET_Y_LPARAM(l) / hudScale);
    hudPressed = -1;
    if (GetCapture() == h)
      ReleaseCapture();
    if (shown != tongzhuo::Reveal::Panel || selected < 0 || selected != hit ||
        selected >= (int)hudHits.size())
      return 0;
    auto item = hudHits[selected];
    if (item.create) {
      previewUntil = 0;
      hover.dismiss();
      showHud(tongzhuo::Reveal::Hidden, nullptr);
      openName(true, {});
    } else if (indexOf(item.desktop) >= 0)
      userSwitch(item.desktop);
    return 0;
  }
  case WM_MOUSEWHEEL:
    if (shown == tongzhuo::Reveal::Panel) {
      hudPage += GET_WHEEL_DELTA_WPARAM(w) < 0 ? 1 : -1;
      repaginateHud();
    }
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

void pinMain() {
  if (sandbox || renderTest || residentTest || !panelState.opened ||
      !api.own(mainWindow))
    return;
  KillTimer(mainWindow, 92);
  if (!panelState.mayRetryPin(panelPinned) && !panelPinned)
    return;
  bool verified = false;
  if (ready && api.views && api.pinned) {
    ComPtr<IUnknown> view;
    if (SUCCEEDED(api.views->GetViewForHwnd(mainWindow, &view)) && view) {
      panelView = view;
      BOOL pinned = FALSE;
      if (SUCCEEDED(api.pinned->IsViewPinned(view.Get(), &pinned)) && pinned)
        verified = true;
      else if (SUCCEEDED(api.pinned->PinView(view.Get()))) {
        ownsPanelPin = true;
        pinned = FALSE;
        verified =
            SUCCEEDED(api.pinned->IsViewPinned(view.Get(), &pinned)) && pinned;
      }
    }
  }
  panelPinned = verified;
  if (verified) {
    panelState.pinAttempts = 0;
    if (status == PinFailure) {
      status.clear();
      InvalidateRect(mainWindow, nullptr, FALSE);
    }
    return;
  }
  ++panelState.pinAttempts;
  if (panelState.mayRetryPin(false))
    SetTimer(mainWindow, 92, 180, nullptr);
  else {
    status = PinFailure;
    InvalidateRect(mainWindow, nullptr, FALSE);
  }
}
void schedulePanelPin() {
  if (!sandbox && !renderTest && !residentTest && panelState.opened)
    SetTimer(mainWindow, 92, 120, nullptr);
}
void showMain() {
  panelState.open();
  inventoryDirty = true;
  refresh(true);
  ShowWindow(mainWindow, SW_SHOWNOACTIVATE);
  pinMain();
  BOOL here = FALSE;
  if (ready && !sandbox)
    api.manager->IsWindowOnCurrentVirtualDesktop(mainWindow, &here);
  if (here || sandbox || !ready)
    SetForegroundWindow(mainWindow);
}
void tray(bool remove = false) {
  NOTIFYICONDATAW n{sizeof(n)};
  n.hWnd = mainWindow;
  n.uID = 1;
  n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  n.uCallbackMessage = TrayMessage;
  n.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wcscpy_s(n.szTip, L"同桌 · 单击打开，右键切换桌面");
  Shell_NotifyIconW(remove ? NIM_DELETE : NIM_ADD, &n);
}
LRESULT CALLBACK mainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
  case WM_NCCALCSIZE:
    return 0;
  case WM_NCPAINT:
    return 0;
  case WM_NCACTIVATE:
    // Keep native activation bookkeeping, without letting USER32 redraw its
    // classic resize frame over our client area after another app is dragged.
    return DefWindowProcW(h, m, w, -1);
  case WM_THEMECHANGED:
  case WM_DWMCOMPOSITIONCHANGED:
  case WM_DWMCOLORIZATIONCOLORCHANGED:
    frame(h);
    InvalidateRect(h, nullptr, FALSE);
    break;
  case WM_CREATE: {
    mainWindow = h;
    dpi = GetDpiForWindow(h) / 96.f;
    frame(h);
    if (!sandbox)
      ready = api.connect();
    loadSettings();
    if (!ready)
      status = sandbox ? L"" : L"暂时无法连接系统桌面";
    refresh(true);
    if (!sandbox && !residentTest)
      tray();
    if (!sandbox && !renderTest && !residentTest && ready) {
      runtimeTrace.initialize(settingsPath().parent_path() / L"diagnostics" /
                              L"switch-latency.json");
      compatibility = std::make_unique<tongzhuo::livecompat::Controller>(api);
      compatibility->loadPolicies(settingsPath().parent_path() /
                                  L"app-behaviors.ini");
      GUID run{};
      CoCreateGuid(&run);
      auto recovery = settingsPath().parent_path() / L"compat-sessions" /
                      (tongzhuo::livecompat::text(run) + L".bin");
      if (!compatibility->start(recovery)) {
        compatibility.reset();
        status = L"窗口兼容模块未能连接系统。";
      } else
        SetTimer(h, 96, 1500, nullptr);
    }
    if (!sandbox && !residentTest && ready && api.notifications) {
      listener.Attach(new Listener());
      api.notifications->Register(listener.Get(), &notifyCookie);
    }
    if (!sandbox && !residentTest)
      windowEvents = SetWinEventHook(
          EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, eventHook, 0, 0,
          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    paintMain(dc);
    runtimeTrace.painted();
    EndPaint(h, &ps);
    return 0;
  }
  case WM_DRAWITEM:
    drawButton((DRAWITEMSTRUCT *)l);
    return TRUE;
  case WM_SIZE:
    if (w == SIZE_MINIMIZED) {
      mainBuffer.release();
      releaseVisuals();
    } else {
      layout();
      InvalidateRect(h, nullptr, FALSE);
    }
    return 0;
  case WM_GETMINMAXINFO: {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &monitor);
    auto *bounds = (MINMAXINFO *)l;
    LONG workWidth = monitor.rcWork.right - monitor.rcWork.left;
    LONG workHeight = monitor.rcWork.bottom - monitor.rcWork.top;
    bounds->ptMinTrackSize = {std::min<LONG>(px(480), workWidth),
                              std::min<LONG>(px(440), workHeight)};
    bounds->ptMaxPosition = {monitor.rcWork.left - monitor.rcMonitor.left,
                             monitor.rcWork.top - monitor.rcMonitor.top};
    bounds->ptMaxSize = {workWidth, workHeight};
    return 0;
  }
  case WM_NCHITTEST: {
    POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    ScreenToClient(h, &p);
    RECT r;
    GetClientRect(h, &r);
    int b = IsZoomed(h) ? 0 : px(6);
    if (p.y < b)
      return p.x < b ? HTTOPLEFT : p.x > r.right - b ? HTTOPRIGHT : HTTOP;
    if (p.y > r.bottom - b)
      return p.x < b             ? HTBOTTOMLEFT
             : p.x > r.right - b ? HTBOTTOMRIGHT
                                 : HTBOTTOM;
    if (p.x < b)
      return HTLEFT;
    if (p.x > r.right - b)
      return HTRIGHT;
    if (p.y < px(68) && p.x < r.right - px(170))
      return HTCAPTION;
    return HTCLIENT;
  }
  case TraceWake:
    if (w == 1 && compatibility && !compatibility->busy())
      compatibility->requestCurrentRedraw();
    SetTimer(h, 98, 25, nullptr);
    return 0;
  case CompatChanged:
    if (compatibility && w == 1)
      compatibility->invalidateDiscovery();
    if (compatibility && !compatibility->busy() && !compatRefreshPending) {
      SetTimer(h, 97, 60, nullptr);
      compatRefreshPending = true;
    }
    return 0;
  case Changed:
    if (w == 2)
      schedulePanelPin();
    if (w == 1)
      wallpapers.clear();
    if (w == 2 && !panelVisible()) {
      inventory(false);
      inventoryDirty = true;
      updateHudTimer();
    } else
      schedule();
    return 0;
  case RenameMessage:
    openName(false, editingId);
    return 0;
  case WM_TIMER:
    if (w == 98) {
      if (!compatibility || !compatibility->busy()) {
        auto repairToken = runtimeTrace.repairToken();
        if (repairToken && compatibility)
          runtimeTrace.repaired(
              compatibility->repairShellVisibility(repairToken));
        if (!runtimeTrace.poll(compatibility.get())) {
          KillTimer(h, 98);
          SetTimer(h, 99, 50, nullptr);
        }
      }
      return 0;
    }
    if (w == 99) {
      if (!compatibility || !compatibility->busy()) {
        KillTimer(h, 99);
        runtimeTrace.flush();
      }
      return 0;
    }
    if (w == 96 || w == 97) {
      if (w == 97) {
        KillTimer(h, 97);
        compatRefreshPending = false;
      }
      if (compatibility && !compatibility->busy())
        compatibility->sync();
      return 0;
    }
    if (w == 91) {
      KillTimer(h, 91);
      refreshPending = false;
      if (!drag.down && !editWindow && panelVisible())
        refresh();
      else if (drag.down || editWindow)
        schedule();
    } else if (w == 92) {
      KillTimer(h, 92);
      pinMain();
    } else if (w == 94)
      tickHud();
    else if (w == 95) {
      KillTimer(h, 95);
      releaseVisuals();
    }
    return 0;
  case WM_ACTIVATE:
    frame(h);
    if (LOWORD(w) != WA_INACTIVE)
      schedule();
    return 0;
  case WM_COMMAND: {
    int id = LOWORD(w);
    if (id == Create)
      openName(true, {});
    else if (id == Settings) {
      settingsView = !settingsView;
      refresh(true);
    } else if (id == Minimize)
      SendMessageW(h, WM_CLOSE, 0, 0);
    else if (id == Close)
      SendMessageW(h, WM_CLOSE, 0, 0);
    else if (id == Quit)
      DestroyWindow(h);
    else if (id == Back || id == Forward) {
      desktopPage += id == Back ? -1 : 1;
      desktopPage =
          std::clamp(desktopPage, 0, tongzhuo::lastDesktopPage(desks.size()));
      layout();
      for (int i = 0; i < 3; ++i)
        InvalidateRect(GetDlgItem(h, Card + i), nullptr, FALSE);
    } else if (id == Choose)
      openMenu();
    else if (id == ToggleHud) {
      hudEnabled = !hudEnabled;
      saveSettings();
      updateHudTimer();
      updateSettingControls();
      InvalidateRect(h, nullptr, FALSE);
      InvalidateRect(GetDlgItem(h, id), nullptr, FALSE);
    } else if (id == ToggleFullscreen) {
      if (!hudEnabled)
        return 0;
      hideFullscreen = !hideFullscreen;
      saveSettings();
      InvalidateRect(GetDlgItem(h, id), nullptr, FALSE);
    } else if (id == Range12 || id == Range24 || id == Range40) {
      if (!hudEnabled)
        return 0;
      range = id == Range12 ? 12 : id == Range24 ? 24 : 40;
      saveSettings();
      for (int k : {Range12, Range24, Range40})
        InvalidateRect(GetDlgItem(h, k), nullptr, FALSE);
    } else if (id == ToggleStartup) {
      bool next = !startupEnabled;
      if (sandbox || startup.set(next, module()))
        startupEnabled = next;
      else
        MessageBoxW(h, L"开机自启设置未保存，请重试。", L"同桌", MB_OK | MB_ICONERROR);
      InvalidateRect(GetDlgItem(h, id), nullptr, FALSE);
    } else if (id == PreviewHud) {
      POINT p;
      GetCursorPos(&p);
      hover.reset();
      previewUntil = GetTickCount64() + 6000;
      showHud(tongzhuo::Reveal::Panel,
              MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST));
      updateHudTimer();
    } else if (id >= AppBehavior && id < AppBehavior + 4)
      openAppBehavior(id - AppBehavior);
    else if (id >= Card && id < Card + 3) {
      int at = desktopPage * 3 + id - Card;
      if (at < (int)desks.size())
        userSwitch(desks[at].id);
      else if (at == (int)desks.size())
        openName(true, {});
    }
    return 0;
  }
  case WM_MOUSEWHEEL:
    if (!settingsView) {
      appPage += GET_WHEEL_DELTA_WPARAM(w) < 0 ? 1 : -1;
      appPage = std::clamp(appPage, 0,
                           std::max(0, ((int)apps.size() - 1) / appRows()));
      updateAppActions();
      InvalidateRect(h, nullptr, FALSE);
    }
    return 0;
  case TrayMessage:
    if (l == WM_LBUTTONUP)
      showMain();
    else if (l == WM_RBUTTONUP)
      openMenu(true);
    return 0;
  case WM_DPICHANGED: {
    dpi = HIWORD(w) / 96.f;
    auto *r = (RECT *)l;
    closeMenu();
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &monitor);
    auto bounds = tongzhuo::fitPanel(r->left, r->top, r->right - r->left,
        r->bottom - r->top, monitor.rcWork.left, monitor.rcWork.top,
        monitor.rcWork.right, monitor.rcWork.bottom);
    mainBuffer.release();
    SetWindowPos(h, nullptr, bounds.x, bounds.y, bounds.width,
                 bounds.height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    layout();
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    return 0;
  }
  case WM_CLOSE:
    panelState.close();
    KillTimer(h, 92);
    closeMenu();
    ShowWindow(h, SW_HIDE);
    mainBuffer.release();
    releaseVisuals();
    return 0;
  case WM_DESTROY:
    KillTimer(h, 98);
    KillTimer(h, 99);
    runtimeTrace.shutdown();
    KillTimer(h, 96);
    KillTimer(h, 97);
    if (compatibility) {
      compatibility->stop();
      compatibility.reset();
    }
    closeMenu();
    if (windowEvents)
      UnhookWinEvent(windowEvents);
    if (notifyCookie && api.notifications)
      api.notifications->Unregister(notifyCookie);
    listener.Reset();
    KillTimer(h, 91);
    KillTimer(h, 92);
    KillTimer(h, 94);
    KillTimer(h, 95);
    if (ownsPanelPin && api.pinned && panelView)
      api.pinned->UnpinView(panelView.Get());
    panelView.Reset();
    if (hudWindow)
      DestroyWindow(hudWindow);
    if (stripWindow)
      DestroyWindow(stripWindow);
    if (!sandbox && !residentTest)
      tray(true);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int checks() {
  std::vector<std::pair<std::string, bool>> results;
  auto check = [&](std::string n, bool ok) { results.push_back({n, ok}); };
  tongzhuo::SwitchGate g;
  int called = 0;
  for (int i = 0; i < 1000; ++i)
    g.userSelect(1, 1, [&](int) {
      ++called;
      return true;
    });
  check("same_desktop_never_switches", called == 0);
  g.userSelect(1, 2, [&](int) {
    ++called;
    return true;
  });
  check("one_click_one_switch", called == 1);
  using namespace tongzhuo;
  HoverReveal h;
  HoverSample s;
  s.valid = true;
  s.proximity = true;
  s.now = 1;
  h.update(s);
  s.x = 1;
  s.now = 20;
  h.update(s);
  s.now = 120;
  check("near_reveals_strip", h.update(s) == Reveal::Strip);
  s.lip = true;
  s.now = 130;
  h.update(s);
  s.now = 280;
  check("closer_reveals_panel", h.update(s) == Reveal::Panel);
  s.inside = true;
  s.down = true;
  s.now = 290;
  check("click_inside_keeps_panel", h.update(s) == Reveal::Panel);
  s.down = false;
  s.now = 300;
  check("release_inside_keeps_panel", h.update(s) == Reveal::Panel);
  s.inside = false;
  s.now = 310;
  h.update(s);
  s.now = 740;
  check("leave_delay_hides", h.update(s) == Reveal::Hidden);
  h.reset();
  s = {};
  s.proximity = true;
  s.lip = true;
  s.down = true;
  s.now = 1;
  h.update(s);
  s.x = 1;
  s.now = 100;
  h.update(s);
  check("external_drag_cannot_open", h.state == Reveal::Hidden);
  s.down = false;
  s.now = 500;
  h.update(s);
  s.now = 900;
  check("drag_release_stationary_stays_hidden", h.update(s) == Reveal::Hidden);
  s.x = 2;
  s.now = 910;
  h.update(s);
  s.now = 1100;
  check("fresh_movement_can_rearm", h.update(s) != Reveal::Hidden);
  s.suppressed = true;
  s.now = 1200;
  check("fullscreen_suppresses", h.update(s) == Reveal::Hidden);
  h.reset();
  s = {};
  s.proximity = true;
  s.movingWindow = true;
  s.x = 1;
  s.now = 1;
  h.update(s);
  s.x = 2;
  s.now = 400;
  check("window_drag_suppresses", h.update(s) == Reveal::Hidden);
  h.dismiss();
  s = {};
  s.proximity = true;
  s.lip = true;
  s.now = 1;
  h.update(s);
  for (int i = 0; i < 20; ++i) {
    s.x = i;
    s.now += 100;
    h.update(s);
  }
  check("dismiss_blocks_until_zone_exit", h.state == Reveal::Hidden);
  s.proximity = false;
  s.lip = false;
  s.x = 100;
  s.now += 100;
  h.update(s);
  s.proximity = true;
  s.lip = true;
  s.x = 101;
  s.now += 100;
  h.update(s);
  s.now += 100;
  h.update(s);
  s.now += 150;
  check("zone_reentry_reveals_again", h.update(s) == Reveal::Panel);
  s.inside = false;
  s.proximity = false;
  s.lip = false;
  s.now += 10;
  h.update(s);
  s.now += 200;
  check("short_gap_does_not_flicker", h.update(s) == Reveal::Panel);
  s.inside = true;
  s.now += 100;
  check("reentry_cancels_close", h.update(s) == Reveal::Panel);
  s.inside = false;
  s.now += 10;
  h.update(s);
  s.now += 430;
  check("second_leave_hides_after_delay", h.update(s) == Reveal::Hidden);
  h.reset();
  s = {};
  s.proximity = true;
  s.lip = true;
  s.now = 1;
  h.update(s);
  s.now = 1000;
  check("stationary_start_never_opens", h.update(s) == Reveal::Hidden);
  check("idle_sensor_uses_slow_interval", edgeInterval(false, false) == 120);
  check("near_sensor_uses_fast_interval", edgeInterval(true, false) == 35);
  check("unchanged_inventory_skips_layout",
        !needsLayout(false, 816, 620, 816, 620, 0, 0, false, false, 3, 3));
  check("dpi_or_size_layout_allowed",
        needsLayout(false, 816, 620, 900, 620, 0, 0, false, false, 3, 3));
  check("trailing_create_tile_page_reachable",
        lastDesktopPage(3) == 1 && lastHudPage(3) == 0);
  SharedPanelState shared;
  shared.open();
  shared.pinAttempts = 9;
  check("pin_retry_allows_registration_delay", shared.mayRetryPin(false));
  ++shared.pinAttempts;
  check("pin_retry_bounded", !shared.mayRetryPin(false));
  shared.close();
  check("closed_panel_does_not_retry_pin", !shared.mayRetryPin(false));
  shared.open();
  check("explicit_open_retries_again", shared.mayRetryPin(false));
  check("already_pinned_no_retry", !shared.mayRetryPin(true));
  auto tray = tongzhuo::popupAbove(1900, 1040, 250, 240, 0, 0, 1920, 1040, 8);
  check("tray_menu_grows_up",
        tray.y + tray.height == 1032 && tray.x + tray.width == 1900);
  auto negative =
      tongzhuo::popupAbove(-30, 1040, 250, 240, -1920, 0, 0, 1040, 8);
  check("tray_menu_negative_monitor", negative.x >= -1920 &&
                                          negative.x + negative.width <= 0 &&
                                          negative.y + negative.height <= 1032);
  auto tall = tongzhuo::popupAbove(1900, 1040, 250, 2400, 0, 0, 1920, 1040, 8);
  check("tray_menu_clamped_to_workarea",
        tall.y >= 0 && tall.y + tall.height <= 1040);
  check("preview_ratio_is_16_9",
        std::abs(tongzhuo::previewHeight(225.f) * 16.f - 225.f * 9.f) < .01f);
  auto compactPreview = tongzhuo::cardPreview(400, (float)tongzhuo::desktopCardHeight(400, 440));
  check("low_height_preview_stays_16_9_inside_card",
        std::abs(compactPreview.width * 9 - compactPreview.height * 16) < .01f &&
        compactPreview.y + compactPreview.height + 18 <= 170.01f);
  auto fitted = tongzhuo::fitPanel(-2500, -10, 1800, 1300, -1920, 0, 0, 1040);
  check("dpi_suggested_bounds_fit_negative_monitor_workarea",
        fitted.x >= -1920 && fitted.y >= 0 &&
        fitted.x + fitted.width <= 0 && fitted.y + fitted.height <= 1040);
  auto centered =
      tongzhuo::popupCentered(100, 200, 900, 800, 400, 226, 0, 0, 1920, 1080);
  check("rename_centers_in_owner_not_monitor",
        centered.x == 300 && centered.y == 387);
  check("desktop_connection", api.connect());
  check("native_pinning_interfaces_available", api.views && api.pinned);
  ready = api.internal != nullptr;
  GUID before = api.currentId();
  for (int i = 0; i < 3; ++i)
    inventory();
  check("readonly_refresh_never_switches",
        api.currentId() == before && gate.switchCalls == 0);
  check("native_desktops_detected", !desks.empty());
  check("own_app_excluded",
        std::none_of(views.begin(), views.end(),
                     [](auto &v) { return v.pid == GetCurrentProcessId(); }));
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory,
                       sizeof(memory));
  fs::path dir = fs::path(module()).parent_path().parent_path() / L"artifacts";
  fs::create_directories(dir);
  std::ofstream f(dir / L"tongzhuo-checks.json");
  f << "{\n\"scope\":\"pure hover/switch tests and read-only real inventory; "
       "no desktop mutations\",\n\"private_bytes\":"
    << memory.PrivateUsage
    << ",\n\"working_set_bytes\":" << memory.WorkingSetSize
    << ",\n\"checks\":[\n";
  bool pass = true;
  for (size_t i = 0; i < results.size(); ++i) {
    f << "{\"name\":\"" << results[i].first
      << "\",\"pass\":" << (results[i].second ? "true" : "false") << "}"
      << (i + 1 < results.size() ? "," : "") << "\n";
    pass &= results[i].second;
  }
  f << "]}\n";
  return pass ? 0 : 20;
}
#include "ui_verification.h"
} // namespace
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR args, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    return 10;
  std::wstring command = args ? args : L"";
  int compatResult = tongzhuo::livecompat::dispatch(command);
  if (compatResult != -1) {
    CoUninitialize();
    return compatResult;
  }
  if (command == L"--check")
    return checks();
  renderTest = command == L"--render-test";
  residentTest = command == L"--resident-check";
  sandbox = renderTest || command == L"--ui-sandbox";
  if (!command.empty() && !sandbox && !residentTest &&
      command != L"--background")
    return 2;
  HANDLE mutex = CreateMutexW(nullptr, FALSE,
                              residentTest ? L"Local\\TongZhuo.Resident.v4"
                              : renderTest ? L"Local\\TongZhuo.Render.v4"
                              : sandbox    ? L"Local\\TongZhuo.Sandbox.v3"
                                           : L"Local\\TongZhuo.Controller.v3");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (renderTest || residentTest) {
      CloseHandle(mutex);
      return 3;
    }
    HWND old = FindWindowW(MainClass, sandbox ? L"同桌 · 界面验证" : L"同桌");
    if (old) {
      DWORD pid = 0;
      GetWindowThreadProcessId(old, &pid);
      auto running = pathOf(pid);
      if (!sandbox && !running.empty() &&
          fs::path(running) != fs::path(module()))
        MessageBoxW(nullptr,
                    L"旧版本同桌仍在运行。请先从系统托盘菜单选择“退出同桌”，再"
                    L"打开新版。",
                    L"同桌", MB_OK | MB_ICONINFORMATION);
      else
        PostMessageW(old, TrayMessage, 0, WM_LBUTTONUP);
    }
    CloseHandle(mutex);
    return 0;
  }
  instance = inst;
  dpi = GetDpiForSystem() / 96.f;
  ULONG_PTR token{};
  GdiplusStartupInput input;
  GdiplusStartup(&token, &input, nullptr);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  uiFont = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
  auto reg = [&](const wchar_t *name, WNDPROC proc) {
    WNDCLASSW c{};
    c.hInstance = inst;
    c.lpszClassName = name;
    c.lpfnWndProc = proc;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&c);
  };
  reg(MainClass, mainProc);
  reg(HudClass, hudProc);
  reg(EditClass, editProc);
  reg(MenuClass, menuProc);
  if (sandbox)
    seedFixtures();
  POINT cursor{};
  GetCursorPos(&cursor);
  MONITORINFO monitor{sizeof(monitor)};
  HMONITOR launchMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  GetMonitorInfoW(launchMonitor, &monitor);
  dpi = monitorScale(launchMonitor);
  int width = std::min<int>(px(816), monitor.rcWork.right -
                                         monitor.rcWork.left - px(24)),
      height = std::min<int>(px(620), monitor.rcWork.bottom -
                                          monitor.rcWork.top - px(24));
  int left = monitor.rcWork.left +
             (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
      top = monitor.rcWork.top +
            (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
  mainWindow =
      CreateWindowExW(0, MainClass, sandbox ? L"同桌 · 界面验证" : L"同桌",
                      MainStyle, left, top, width,
                      height, nullptr, nullptr, inst, nullptr);
  if (!mainWindow)
    return 11;
  SetWindowPos(mainWindow, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                    SWP_FRAMECHANGED);
  int result = 0;
  if (renderTest || residentTest) {
    result = renderTest ? renderChecks() : residentChecks();
    DestroyWindow(mainWindow);
  } else if (command != L"--background")
    showMain();
  MSG m{};
  while (!renderTest && !residentTest && GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (editWindow && m.message == WM_KEYDOWN &&
        (m.wParam == VK_RETURN || m.wParam == VK_ESCAPE)) {
      if (m.wParam == VK_ESCAPE || GetFocus() == GetDlgItem(editWindow, 1))
        cancelName();
      else
        renameOrCreate();
      continue;
    }
    if (editWindow && IsDialogMessageW(editWindow, &m))
      continue;
    if (!IsDialogMessageW(mainWindow, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  for (auto &[path, icon] : appIcons)
    if (icon)
      DestroyIcon(icon);
  wallpapers.clear();
  fonts.clear();
  mainBuffer.release();
  hudBuffer.release();
  DeleteObject(uiFont);
  api.notifications.Reset();
  api.views.Reset();
  api.pinned.Reset();
  api.origin.Reset();
  api.manager.Reset();
  api.internal.Reset();
  GdiplusShutdown(token);
  CloseHandle(mutex);
  CoUninitialize();
  return result;
}
