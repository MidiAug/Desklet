// Included inside shell_ui.cpp's anonymous namespace. All windows stay hidden;
// the same production painters draw into a DIB, with synthetic desktop data.
struct TestMemory {
  SIZE_T privateBytes = 0, workingSet = 0;
  DWORD gdi = 0, user = 0;
};
TestMemory memorySnapshot() {
  PROCESS_MEMORY_COUNTERS_EX m{};
  m.cb = sizeof(m);
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m,
                       sizeof(m));
  return {m.PrivateUsage, m.WorkingSetSize,
          GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS),
          GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)};
}
bool savePng(Bitmap &image, const fs::path &path) {
  UINT count = 0, bytes = 0;
  GetImageEncodersSize(&count, &bytes);
  std::vector<BYTE> buffer(bytes);
  auto *encoders = reinterpret_cast<ImageCodecInfo *>(buffer.data());
  if (GetImageEncoders(count, bytes, encoders) != Ok)
    return false;
  for (UINT i = 0; i < count; ++i)
    if (wcscmp(encoders[i].MimeType, L"image/png") == 0)
      return image.Save(path.c_str(), &encoders[i].Clsid, nullptr) == Ok;
  return false;
}
fs::path visualArtifacts() {
  return fs::path(module()).parent_path().parent_path() / L"artifacts" /
         L"ui-v4";
}
void seedFixtures() {
  ready = true;
  desks.clear();
  views.clear();
  auto folder = visualArtifacts();
  fs::create_directories(folder);
  auto path = folder / L"fixture-wallpaper.png";
  {
    Bitmap image(512, 288, PixelFormat32bppPARGB);
    {
      Graphics g(&image);
      g.SetSmoothingMode(SmoothingModeAntiAlias);
      RectF area(0, 0, 512, 288);
      LinearGradientBrush base(area, Color(25, 50, 82), Color(68, 88, 127),
                               LinearGradientModeForwardDiagonal);
      g.FillRectangle(&base, area);
      SolidBrush ribbon(Color(95, 125, 164, 226));
      g.FillEllipse(&ribbon, 170, -150, 480, 430);
      SolidBrush foreground(Color(190, 24, 44, 72));
      g.FillEllipse(&foreground, -130, 110, 750, 500);
    }
    savePng(image, path);
  }
  const wchar_t *names[] = {L"我的桌面", L"工作", L"专注"};
  for (unsigned i = 0; i < 3; ++i) {
    Desk d;
    d.id = {i + 1, 0, 0, {}};
    d.name = names[i];
    d.wallpaper = path.wstring();
    desks.push_back(d);
  }
  current = viewed = desks[0].id;
  const wchar_t *applications[] = {L"浏览器", L"文件资源管理器", L"记事本",
                                   L"终端"};
  for (unsigned i = 0; i < 4; ++i) {
    View v;
    v.desktop = current;
    v.pid = i + 100;
    v.path =
        L"C:\\TongZhuo-Fixtures\\" + std::wstring(applications[i]) + L".exe";
    views.push_back(v);
  }
  views.push_back(views[0]);
  View v;
  v.desktop = desks[1].id;
  v.pid = 200;
  v.path = L"C:\\TongZhuo-Fixtures\\浏览器.exe";
  views.push_back(v);
  inventory();
}
// A private DIB: no screen DC capture, no foreground/desktop changes.
bool renderOverview(const fs::path &path) {
  RECT r{};
  GetClientRect(mainWindow, &r);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = r.right;
  info.bmiHeader.biHeight = -r.bottom;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void *pixels = nullptr;
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP dib =
      CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!dc || !dib) {
    if (dc)
      DeleteDC(dc);
    return false;
  }
  HGDIOBJ old = SelectObject(dc, dib);
  paintMain(dc);
  for (HWND h = GetWindow(mainWindow, GW_CHILD); h;
       h = GetWindow(h, GW_HWNDNEXT)) {
    if (!(GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE))
      continue;
    RECT box{};
    GetWindowRect(h, &box);
    MapWindowPoints(nullptr, mainWindow, (POINT *)&box, 2);
    DRAWITEMSTRUCT item{};
    item.CtlType = ODT_BUTTON;
    item.CtlID = GetDlgCtrlID(h);
    item.hwndItem = h;
    item.hDC = dc;
    item.itemState = IsWindowEnabled(h) ? 0 : ODS_DISABLED;
    item.rcItem = {0, 0, box.right - box.left, box.bottom - box.top};
    int saved = SaveDC(dc);
    SetViewportOrgEx(dc, box.left, box.top, nullptr);
    IntersectClipRect(dc, 0, 0, item.rcItem.right, item.rcItem.bottom);
    drawButton(&item);
    RestoreDC(dc, saved);
  }
  GdiFlush();
  SelectObject(dc, old);
  bool saved = false;
  {
    Bitmap image(dib, nullptr);
    saved = path.empty() || savePng(image, path);
  }
  DeleteObject(dib);
  DeleteDC(dc);
  return saved;
}
int renderChecks() {
  auto folder = visualArtifacts();
  fs::create_directories(folder);
  std::vector<std::pair<std::string, bool>> results;
  auto check = [&](const char *name, bool ok) {
    results.push_back({name, ok});
  };
  auto cold = memorySnapshot();
  check("initial_dpi_matches_actual_window",
        std::abs(dpi * 96 - GetDpiForWindow(mainWindow)) < .01f);
  auto frameIsClient = [&] {
    RECT outer{}, client{};
    GetWindowRect(mainWindow, &outer);
    GetClientRect(mainWindow, &client);
    POINT origin{};
    ClientToScreen(mainWindow, &origin);
    return origin.x == outer.left && origin.y == outer.top &&
           client.right == outer.right - outer.left &&
           client.bottom == outer.bottom - outer.top &&
           !(GetWindowLongPtrW(mainWindow, GWL_STYLE) & WS_CAPTION);
  };
  check("initial_window_has_no_native_caption_or_client_offset", frameIsClient());
  check("dwm_system_border_suppression_supported", SUCCEEDED(frame(mainWindow)));
  bool activationHandled = true;
  for (int i = 0; i < 4; ++i) {
    activationHandled = SendMessageW(mainWindow, WM_NCACTIVATE, FALSE, 0) &&
                        activationHandled;
    activationHandled = SendMessageW(mainWindow, WM_NCACTIVATE, TRUE, 0) &&
                        activationHandled;
    activationHandled = SendMessageW(mainWindow, WM_NCPAINT, 1, 0) == 0 &&
                        activationHandled;
  }
  check("activation_repaint_preserves_custom_frame",
        activationHandled && frameIsClient() &&
        (GetWindowLongPtrW(mainWindow, GWL_STYLE) & WS_THICKFRAME));
  SendMessageW(mainWindow, WM_THEMECHANGED, 0, 0);
  check("theme_change_preserves_custom_frame", frameIsClient());
  auto resize = [&](float scale) {
    dpi = scale;
    SetWindowPos(mainWindow, nullptr, 0, 0, px(816), px(620),
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
    layout();
  };
  resize(1);
  check("overview_100", renderOverview(folder / L"overview-100.png"));
  resize(1.5f);
  check("overview_150", renderOverview(folder / L"overview-150.png"));
  openName(false, desks[0].id);
  RECT owner{}, dialog{};
  GetClientRect(mainWindow, &owner);
  MapWindowPoints(mainWindow, nullptr, (POINT *)&owner, 2);
  GetWindowRect(editWindow, &dialog);
  check("rename_actual_window_center_matches_owner",
        std::abs((dialog.left + dialog.right) - (owner.left + owner.right)) <=
                2 &&
            std::abs((dialog.top + dialog.bottom) -
                     (owner.top + owner.bottom)) <= 2);
  check("rename_render_remains_hidden", !IsWindowVisible(editWindow));
  {
    Bitmap shot(dialog.right - dialog.left, dialog.bottom - dialog.top,
                PixelFormat32bppPARGB);
    Graphics g(&shot);
    HDC dc = g.GetHDC();
    SendMessageW(editWindow, WM_PRINT, (WPARAM)dc,
                 PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    g.ReleaseHDC(dc);
    check("rename_150", savePng(shot, folder / L"rename-150.png"));
  }
  cancelName();
  SetWindowPos(mainWindow, nullptr, 0, 0, px(816), px(560),
               SWP_NOACTIVATE | SWP_NOZORDER);
  layout();
  check("compact_overview_150",
        renderOverview(folder / L"overview-compact-150.png"));
  check("compact_app_rows_fit_available_space", appRows() == 3);
  resize(1.5f);
  menuKind = 3;
  menuScale = 1.5f;
  menuScroll = 0;
  menuHover = 3;
  menuVisibleRows = 6;
  menuItems.clear();
  populateBehaviorMenu(L"浏览器", tongzhuo::policy::Mode::Automatic);
  {
    Bitmap shot(495, 342, PixelFormat32bppPARGB);
    Graphics g(&shot);
    paintMenu(g, 495, 342);
    check("app_behavior_menu_150",
          savePng(shot, folder / L"app-behavior-150.png"));
  }
  check("behavior_menu_has_explicit_modes",
        menuItems.size() == 6 && menuItems[2].command == 22 &&
            menuItems[3].command == 23 && menuItems[4].command == 21);
  behaviorPath = apps[0].path;
  selectMenuItem(3);
  wchar_t behaviorCaption[128]{};
  GetWindowTextW(GetDlgItem(mainWindow, AppBehavior), behaviorCaption, 128);
  check("select_displays_saved_current_mode",
        std::wstring(behaviorCaption) == L"各桌面独立展开 / 收起");
  check("overview_current_mode_150",
        renderOverview(folder / L"overview-current-mode-150.png"));
  menuItems.clear();
  menuKind = 0;
  menuHover = -1;
  settingsView = true;
  layout();
  check("settings_150", renderOverview(folder / L"settings-150.png"));
  check("hud_off_disables_dependent_settings",
        !IsWindowEnabled(GetDlgItem(mainWindow, ToggleFullscreen)) &&
        !IsWindowEnabled(GetDlgItem(mainWindow, Range12)) &&
        !IsWindowEnabled(GetDlgItem(mainWindow, Range24)) &&
        !IsWindowEnabled(GetDlgItem(mainWindow, Range40)));
  auto priorRange = range;
  auto priorHidden = hideFullscreen;
  SendMessageW(mainWindow, WM_COMMAND, Range40, 0);
  SendMessageW(mainWindow, WM_COMMAND, ToggleFullscreen, 0);
  check("disabled_settings_ignore_commands",
        range == priorRange && hideFullscreen == priorHidden);
  SendMessageW(mainWindow, WM_COMMAND, ToggleHud, 0);
  check("hud_on_enables_dependent_settings",
        IsWindowEnabled(GetDlgItem(mainWindow, ToggleFullscreen)) &&
        IsWindowEnabled(GetDlgItem(mainWindow, Range24)));
  check("settings_enabled_150",
        renderOverview(folder / L"settings-enabled-150.png"));
  SendMessageW(mainWindow, WM_COMMAND, ToggleHud, 0);
  check("removed_preview_and_quit_are_not_shown",
        !(GetWindowLongPtrW(GetDlgItem(mainWindow, PreviewHud), GWL_STYLE) & WS_VISIBLE) &&
        !(GetWindowLongPtrW(GetDlgItem(mainWindow, Quit), GWL_STYLE) & WS_VISIBLE));
  bool previousStartup = startupEnabled;
  SendMessageW(mainWindow, WM_COMMAND, ToggleStartup, 0);
  check("startup_toggle_works_in_sandbox_without_registry_write",
        startupEnabled != previousStartup);
  SendMessageW(mainWindow, WM_COMMAND, ToggleStartup, 0);
  settingsView = false;
  layout();
  auto open = memorySnapshot();
  shown = tongzhuo::Reveal::Panel;
  hudScale = 1.5f;
  hudPage = 0;
  hudWidth = (int)(tongzhuo::hudLogicalWidth(3, 2000) * hudScale);
  hudHeight = (int)(148 * hudScale);
  {
    Bitmap b(hudWidth, hudHeight, PixelFormat32bppPARGB);
    {
      Graphics g(&b);
      paintHud(g);
    }
    check("hud_150", savePng(b, folder / L"hud-150.png"));
  }
  check("hud_create_equal_width",
        hudHits.size() == 4 &&
            std::abs(hudHits.front().rect.Width - hudHits.back().rect.Width) <
                .01f &&
            hudHits.front().rect.Height == hudHits.back().rect.Height);
  check("hud_create_last", !hudHits.empty() && hudHits.back().create);
  {
    menuScale = 1.5f;
    populateMenu(true);
    int width = (int)(250 * menuScale),
        height = (int)((menuItems.size() * 36 + 12) * menuScale);
    Bitmap b(width, height, PixelFormat32bppPARGB);
    {
      Graphics g(&b);
      paintMenu(g, width, height);
    }
    check("tray_menu_150", savePng(b, folder / L"tray-menu-150.png"));
    check("menu_padding_not_selectable", menuHit(0) == -1);
    check("menu_scaled_row_hit", menuHit((int)(24 * menuScale)) == 0);
  }
  panelState.open();
  for (int desktop = 0; desktop < 20; ++desktop)
    mainProc(mainWindow, Changed, 2, 0);
  check("desktop_callbacks_preserve_open_intent",
        panelState.opened && !IsWindowVisible(mainWindow));
  panelState.close();
  for (int desktop = 0; desktop < 20; ++desktop)
    mainProc(mainWindow, Changed, 2, 0);
  check("desktop_callbacks_preserve_closed_intent",
        !panelState.opened && !IsWindowVisible(mainWindow));
  check("sandbox_does_not_pin_real_windows",
        !panelPinned && !ownsPanelPin && !panelView);
  shown = tongzhuo::Reveal::Strip;
  hudWidth = 108;
  hudHeight = 13;
  {
    Bitmap b(hudWidth, hudHeight, PixelFormat32bppPARGB);
    {
      Graphics g(&b);
      paintHud(g);
    }
    check("strip_150", savePng(b, folder / L"strip-150.png"));
  }
  // A spurious mouse-up (e.g. releasing an outside drag) cannot select a card.
  shown = tongzhuo::Reveal::Panel;
  hudPressed = -1;
  hudProc(nullptr, WM_LBUTTONUP, 0, MAKELPARAM(30, 30));
  check("outside_release_no_switch", gate.switchCalls == 0);
  shown = tongzhuo::Reveal::Hidden;
  struct RenderSize { int width, height; float scale; const char *name; };
  for (auto size : {RenderSize{816, 620, 1.25f, "overview-125"},
                    RenderSize{816, 620, 2.f, "overview-200"},
                    RenderSize{816, 620, 2.5f, "overview-250"},
                    RenderSize{600, 440, 1.5f, "overview-small-150"},
                    RenderSize{480, 440, 2.f, "overview-narrow-200"}}) {
    dpi = size.scale;
    SetWindowPos(mainWindow, nullptr, 0, 0, px((float)size.width),
                   px((float)size.height), SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    layout();
    check(size.name, renderOverview(folder / (std::string(size.name) + ".png")));
    RECT selector{}, client{};
    GetWindowRect(GetDlgItem(mainWindow, AppBehavior), &selector);
    MapWindowPoints(nullptr, mainWindow, (POINT *)&selector, 2);
    GetClientRect(mainWindow, &client);
    check((std::string(size.name) + "_controls_fit").c_str(),
          selector.left >= 0 && selector.right <= client.right &&
          selector.bottom <= client.bottom && appRows() >= 1);
  }
  std::vector<HMONITOR> monitors;
  EnumDisplayMonitors(nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM value) -> BOOL {
        ((std::vector<HMONITOR> *)value)->push_back(monitor);
        return TRUE;
      }, (LPARAM)&monitors);
  std::ofstream monitorReport(folder / L"monitor-dpi.json");
  monitorReport << "[";
  {
    UINT actual = GetDpiForWindow(mainWindow);
    RECT proposed{};
    GetWindowRect(mainWindow, &proposed);
    SendMessageW(mainWindow, WM_DPICHANGED, MAKELONG(actual, actual),
                  (LPARAM)&proposed);
  }
  for (size_t i = 0; i < monitors.size(); ++i) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(monitors[i], &monitor);
    SetWindowPos(mainWindow, nullptr, monitor.rcWork.left + 12,
                   monitor.rcWork.top + 12, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // The real native WM_DPICHANGED path must own the scale after a move.
    UINT actual = GetDpiForWindow(mainWindow);
    bool matches = std::abs(dpi * 96 - actual) < .01f;
    check(("monitor_" + std::to_string(i) + "_native_dpi_and_frame").c_str(),
          matches && frameIsClient());
    if (i) monitorReport << ",";
    monitorReport << "{\"monitor\":" << i << ",\"dpi\":" << actual
                  << ",\"native_event_matched\":" << (matches ? "true" : "false")
                  << ",\"caption_absent\":" << (frameIsClient() ? "true" : "false") << "}";
  }
  monitorReport << "]";
  resize(1.5f);
  renderOverview({});
  auto layoutBefore = layoutPasses;
  for (int i = 0; i < 12; ++i)
    refresh(true);
  check("stable_refresh_does_not_relayout", layoutPasses == layoutBefore);
  auto before = memorySnapshot();
  for (int i = 0; i < 40; ++i)
    renderOverview(folder / L"repeat.png");
  auto after = memorySnapshot();
  check("gdi_handles_stable", after.gdi <= before.gdi + 2);
  check("user_handles_stable", after.user <= before.user + 2);
  check("repeat_private_memory_bounded",
        after.privateBytes <= before.privateBytes + 2 * 1024 * 1024);
  check("thumbnail_cache_bounded",
        wallpapers.size() <= tongzhuo::PreviewCacheLimit);
  check("no_visible_test_window", !IsWindowVisible(mainWindow));
  for (int i = 0; i < 1000; ++i)
    mainProc(mainWindow, Changed, 0, 0);
  check("hidden_events_schedule_no_refresh", !refreshPending);
  check("all_rendering_no_desktop_switch", gate.switchCalls == 0);
  mainBuffer.release();
  hudBuffer.release();
  releaseVisuals();
  auto closed = memorySnapshot();
  check("hidden_resources_released", !mainBuffer.image && !hudBuffer.image &&
                                         wallpapers.empty() &&
                                         appIcons.empty() && fonts.empty());
  std::error_code ec;
  fs::remove(folder / L"repeat.png", ec);
  std::ofstream output(folder / L"report.json");
  output << "{\n\"scope\":\"Offscreen production painters; synthetic data; no "
            "shown windows, desktop APIs, mouse input or working-set "
            "trimming\",\n";
  auto memory = [&](const char *name, TestMemory value) {
    output << "\"" << name << "\":{\"private_bytes\":" << value.privateBytes
           << ",\"working_set_bytes\":" << value.workingSet
           << ",\"gdi_handles\":" << value.gdi
           << ",\"user_handles\":" << value.user << "},\n";
  };
  memory("cold", cold);
  memory("overview", open);
  memory("before_repeat", before);
  memory("after_repeat", after);
  memory("released", closed);
  output << "\"checks\":[\n";
  bool pass = true;
  for (size_t i = 0; i < results.size(); ++i) {
    pass &= results[i].second;
    output << "{\"name\":\"" << results[i].first
           << "\",\"pass\":" << (results[i].second ? "true" : "false") << "}"
           << (i + 1 == results.size() ? "" : ",") << "\n";
  }
  output << "]}\n";
  return pass ? 0 : 21;
}

// Native read-only inventory + offscreen warm-up. Never creates or shows a HUD.
int residentChecks() {
  auto folder = visualArtifacts();
  fs::create_directories(folder);
  auto cold = memorySnapshot();
  GUID before = api.currentId();
  bool rendered = renderOverview({});
  auto open = memorySnapshot();
  mainBuffer.release();
  hudBuffer.release();
  releaseVisuals();
  auto released = memorySnapshot();
  auto cpuTime = []() -> ULONGLONG {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
  };
  auto pump = [&](bool enabled) {
    hudEnabled = enabled;
    updateHudTimer();
    const auto start = GetTickCount64(), cpuStart = cpuTime();
    while (GetTickCount64() - start < 3000) {
      MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT,
                                  MWMO_INPUTAVAILABLE);
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    return (cpuTime() - cpuStart) / 10000.0 / (GetTickCount64() - start) *
           100.0;
  };
  double disabledCpu = pump(false), enabledCpu = pump(true);
  auto final = memorySnapshot();
  bool noPopup = !hudWindow && !stripWindow && !IsWindowVisible(mainWindow) &&
                 !panelPinned;
  bool noCalls = gate.switchCalls == 0;
  bool currentUnchanged = api.currentId() == before;
  hudEnabled = false;
  updateHudTimer();
  bool noTimer = !hudTimer;
  std::ofstream output(folder / L"resident-report.json");
  output << "{\n\"scope\":\"Native inventory, hidden real controls, offscreen "
            "rendering. HUD sensor tested with popup display suppressed. No "
            "tray, hooks, desktop writes or working-set trimming. CPU is "
            "percent of one logical core over 3 seconds per phase\",\n";
  auto memory = [&](const char *name, TestMemory value) {
    output << "\"" << name << "\":{\"private_bytes\":" << value.privateBytes
           << ",\"working_set_bytes\":" << value.workingSet << "},\n";
  };
  memory("cold_resident", cold);
  memory("overview_rendered", open);
  memory("released", released);
  memory("after_idle", final);
  output << "\"cpu_hud_disabled_percent\":" << disabledCpu
         << ",\n\"cpu_sensor_enabled_percent\":" << enabledCpu << ",\n";
  output << "\"real_desktop_count\":" << desks.size()
         << ",\n\"rendered\":" << (rendered ? "true" : "false")
         << ",\n\"no_visible_popup\":" << (noPopup ? "true" : "false")
         << ",\n\"zero_switch_calls\":" << (noCalls ? "true" : "false")
         << ",\n\"current_desktop_observed_unchanged\":"
         << (currentUnchanged ? "true" : "false")
         << ",\n\"disabled_timer_released\":" << (noTimer ? "true" : "false")
         << "}\n";
  return rendered && noPopup && noCalls && noTimer ? 0 : 22;
}
