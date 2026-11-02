#pragma once
#include "runtime_trace.h"
#include "window_compat.h"
#include <set>

namespace tongzhuo::livecompat {
// Independent ordinary Win32 processes. They know nothing about Controller or
// Core and expose no control protocol. The single-instance launch follows the
// common FindWindow/ShowWindow pattern. Transparent windows keep native Shell
// views while presenting no test pixels on the user's screen.
inline int fixture(const std::wstring &nonce, bool single) {
  auto className = L"TongZhuo.NativeRegression." + nonce;
  if (single) {
    HWND existing = FindWindowW(className.c_str(), L"Single");
    if (existing) {
      ShowWindowAsync(existing, SW_SHOWNOACTIVATE);
      return 0;
    }
  }
  WNDCLASSW c{};
  c.hInstance = GetModuleHandleW(nullptr);
  c.lpfnWndProc = DefWindowProcW;
  c.lpszClassName = className.c_str();
  RegisterClassW(&c);
  auto model = std::wstring(single ? L"TongZhuo.Regression.Single."
                                   : L"TongZhuo.Regression.Multi.") +
               nonce;
  SetCurrentProcessExplicitAppUserModelID(model.c_str());
  HWND h =
      CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED, className.c_str(),
                      single ? L"Single" : L"Multi", WS_OVERLAPPEDWINDOW, 100,
                      100, 600, 350, nullptr, nullptr, c.hInstance, nullptr);
  SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
  if (single)
    SetWindowRgn(h, CreateRoundRectRgn(0, 0, 600, 350, 18, 18), FALSE);
  SetPropW(h, nonce.c_str(), (HANDLE)1);
  setAppId(h, model);
  ShowWindow(h, SW_SHOWNOACTIVATE);
  MSG m{};
  while (IsWindow(h) && GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}
inline int crashController(const std::wstring &nonce, HWND single, HWND multi,
                           const fs::path &file) {
  DesktopApi api;
  if (!api.connect())
    return 40;
  Backup s{}, m{};
  auto remember = [&](HWND h, Backup &b) {
    b.hwnd = (UINT_PTR)h;
    GetWindowThreadProcessId(h, &b.pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, b.pid);
    if (p) {
      b.created = birth(p);
      CloseHandle(p);
    }
    wcscpy_s(b.path, image(b.pid).c_str());
    wcscpy_s(b.windowClass, cls(h).c_str());
  };
  remember(single, s);
  remember(multi, m);
  auto policy = [&](HWND h) {
    auto *b = h == single ? &s : h == multi ? &m : nullptr;
    return b && valid(*b) && GetPropW(h, nonce.c_str()) == (HANDLE)1 &&
                   lower(image(b->pid)) == lower(image(GetCurrentProcessId()))
               ? (h == single ? 1 : 2)
               : 0;
  };
  Controller c(api);
  if (!c.start(file, policy) || c.count() != 2 || !c.enter(api.currentId()))
    return 41;
  // Deliberately bypass the destructor. The real guardian must recover.
  ExitProcess(88);
}
inline int nativeChecks() {
  PerfStats coldStats, warmStats;
  DesktopApi api;
  fs::path root =
      fs::path(image(GetCurrentProcessId())).parent_path().parent_path();
  auto report = root / L"artifacts" / L"compat-production" / L"report.json";
  fs::create_directories(report.parent_path());
  std::vector<std::pair<std::string, bool>> results;
  auto check = [&](const char *n, bool ok) { results.emplace_back(n, ok); };
  auto ruleFile = report.parent_path() / L"policy-store-test.ini";
  apppolicy::Store ruleStore;
  ruleStore.open(ruleFile);
  auto appKey = apppolicy::identity(L"C:\\Apps\\one.exe", L"User.Sample.App");
  check("app_identity_prefers_shell_id_over_filename",
        appKey ==
            apppolicy::identity(L"D:\\Renamed\\two.exe", L"User.Sample.App"));
  check("app_identity_full_path_fallback_distinguishes_same_names",
        apppolicy::identity(L"C:\\One\\same.exe", L"") !=
            apppolicy::identity(L"D:\\Two\\same.exe", L""));
  check("app_identity_normalizes_own_parked_group",
        appKey ==
            apppolicy::identity(L"", L"User.Sample.App.TongZhuo.Parked.{"
                                     L"00000000-0000-0000-0000-000000000001}"));
  check("policy_mode_saved", ruleStore.set(appKey, apppolicy::Mode::Shared));
  apppolicy::Store loadedStore;
  loadedStore.open(ruleFile);
  check("policy_mode_survives_reload",
        loadedStore.get(appKey) == apppolicy::Mode::Shared);
  GUID run{};
  CoCreateGuid(&run);
  auto nonce = text(run);
  auto startupKey = L"Software\\PersonalTools\\TongZhuo\\Tests\\" + nonce;
  tongzhuo::StartupSetting isolatedStartup(startupKey);
  check("startup_registration_uses_quoted_background_command",
        isolatedStartup.set(true, L"C:\\Test Apps\\TongZhuo.exe") &&
        isolatedStartup.command() == L"\"C:\\Test Apps\\TongZhuo.exe\" --background");
  tongzhuo::StartupSetting reloadedStartup(startupKey);
  check("startup_registration_survives_reload", reloadedStartup.enabled());
  check("startup_disable_removes_only_owned_registration",
        isolatedStartup.set(false, L"") && !reloadedStartup.enabled());
  RegDeleteKeyW(HKEY_CURRENT_USER, startupKey.c_str());
  HWND originalForeground = GetForegroundWindow();
  bool connected = api.connect();
  check("desktop_api_connected", connected);
  if (!connected)
    return 10;
  GUID original = api.currentId();
  ComPtr<Desktop> a, origin;
  api.internal->GetCurrent(&origin);
  bool made = api.create(a);
  check("one_background_desktop_created", made);
  if (!made)
    return 11;
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                          sizeof(limits));
  std::map<DWORD, ULONGLONG> owned;
  std::vector<HANDLE> processes;
  auto spawn = [&](bool single) -> HWND {
    std::wstring exe = image(GetCurrentProcessId());
    auto cmd = L"\"" + exe + L"\" --compat-fixture \"" + nonce + L"\" " +
               (single ? L"single" : L"multi");
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi))
      return nullptr;
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
      TerminateProcess(pi.hProcess, 1);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      return nullptr;
    }
    owned[pi.dwProcessId] = birth(pi.hProcess);
    processes.push_back(pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    HWND found{};
    wait(
        [&] {
          struct Search {
            DWORD pid;
            HWND h;
          } search{pi.dwProcessId, {}};
          EnumWindows(
              [](HWND h, LPARAM v) -> BOOL {
                auto &s = *(Search *)v;
                DWORD pid{};
                GetWindowThreadProcessId(h, &pid);
                if (pid == s.pid) {
                  s.h = h;
                  return FALSE;
                }
                return TRUE;
              },
              (LPARAM)&search);
          found = search.h;
          return found && IsWindowVisible(found) &&
                 nativeHome(api, found) != GUID{};
        },
        5000);
    return found;
  };
  HWND single = spawn(true), multi = spawn(false);
  HRGN originalRegion = CreateRectRgn(0, 0, 0, 0);
  if (single)
    GetWindowRgn(single, originalRegion);
  check("unmodified_cross_process_windows_registered", single && multi);
  bool setup = single && multi && move(api, single, api.id(a.Get())) &&
               move(api, multi, api.id(a.Get()));
  check("fixture_windows_staged_on_background_A", setup);
  if (setup) {
    Controller controller(api);
    auto policy = [&](HWND h) -> int {
      DWORD pid{};
      GetWindowThreadProcessId(h, &pid);
      if (!owned.count(pid) || GetPropW(h, nonce.c_str()) != (HANDLE)1)
        return 0;
      HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                             FALSE, pid);
      BOOL inJob = FALSE;
      bool ok = p && birth(p) == owned[pid] && IsProcessInJob(p, job, &inJob) &&
                inJob;
      if (p)
        CloseHandle(p);
      if (!ok || lower(image(pid)) != lower(image(GetCurrentProcessId())))
        return 0;
      return h == single ? 1 : 2;
    };
    auto baselineGroup = appId(multi);
    ComPtr<IUnknown> browserView;
    bool pinnedSetup =
        api.pinned &&
        SUCCEEDED(api.views->GetViewForHwnd(multi, &browserView)) &&
        browserView && SUCCEEDED(api.pinned->PinApp(baselineGroup.c_str())) &&
        SUCCEEDED(api.pinned->PinView(browserView.Get()));
    check("browser_all_desktops_app_and_window_pins_reproduced", pinnedSetup);
    check("production_adapter_started",
          controller.start(report.parent_path() / (nonce + L".bin"), policy));
    check("only_two_owned_windows_discovered", controller.count() == 2);
    perf = {};
    perf.enabled = true;
    bool entered = controller.enter(original);
    coldStats = perf;
    perf.enabled = false;
    check("enter_B_uses_production_code", entered);
    BOOL browserAppPinned = TRUE, browserWindowPinned = TRUE,
         browserOnCurrent = TRUE;
    if (api.pinned) {
      api.pinned->IsAppPinned(baselineGroup.c_str(), &browserAppPinned);
      if (browserView)
        api.pinned->IsViewPinned(browserView.Get(), &browserWindowPinned);
    }
    api.manager->IsWindowOnCurrentVirtualDesktop(multi, &browserOnCurrent);
    check("browser_app_all_desktops_pin_cleared", !browserAppPinned);
    check("browser_window_all_desktops_pin_cleared", !browserWindowPinned);
    check("shared_desktop_sentinel_replaced_by_real_current_desktop",
          browserOnCurrent && nativeHome(api, multi) == original);
    check("localized_browser_can_remain_on_background_A",
          move(api, multi, api.id(a.Get())) && controller.enter(original));
    api.manager->IsWindowOnCurrentVirtualDesktop(multi, &browserOnCurrent);
    check("background_browser_not_visible_on_current_desktop",
          !browserOnCurrent && nativeHome(api, multi) == api.id(a.Get()));
    check("single_hidden_on_B", !IsWindowVisible(single));
    // Hidden Shell windows may not expose a public desktop GUID. The next
    // ordinary launch verifies the actual live home after ShowWindow.
    check("background_browser_detached_from_original_group",
          appId(multi) ==
              baselineGroup + L".TongZhuo.Parked." + text(api.id(a.Get())));
    DWORD singlePid{};
    GetWindowThreadProcessId(single, &singlePid);
    std::wstring exe = image(GetCurrentProcessId());
    auto args = L"--compat-fixture \"" + nonce + L"\" single";
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    launch.lpFile = exe.c_str();
    launch.lpParameters = args.c_str();
    launch.nShow = SW_HIDE;
    bool launched = ShellExecuteExW(&launch) != FALSE;
    if (launch.hProcess) {
      WaitForSingleObject(launch.hProcess, 5000);
      CloseHandle(launch.hProcess);
    }
    check("ordinary_single_instance_launch_succeeds",
          launched && wait([&] { return IsWindowVisible(single); }));
    check("app_launch_stays_on_B",
          wait([&] { return nativeHome(api, single) == original; }, 1500) &&
              api.currentId() == original);
    DWORD afterPid{};
    GetWindowThreadProcessId(single, &afterPid);
    check("same_window_and_process_reused",
          IsWindow(single) && afterPid == singlePid);
    auto logical = controller.logicalViews();
    auto sharedCount = [&](const auto &items) {
      return std::count_if(items.begin(), items.end(), [&](const auto &v) {
        return v.window == (cc::WindowId)single;
      });
    };
    check("shared_window_listed_on_both_expanded_desktops",
          sharedCount(logical) == 2);
    check("physical_shared_window_identified_for_inventory_dedup",
          controller.isSharedWindow(single));
    bool backgroundTransfer = controller.enter(api.id(a.Get()));
    auto transferInfo = controller.windowTimings();
    auto transferAt = std::find_if(
        transferInfo.begin(), transferInfo.end(),
        [&](const auto &w) { return w.window == (cc::WindowId)single; });
    HRGN intact = CreateRectRgn(0, 0, 0, 0);
    check("visible_background_transfer_keeps_region_without_zero_mask",
          backgroundTransfer && transferAt != transferInfo.end() &&
              !transferAt->masked && intact &&
              GetWindowRgn(single, intact) != ERROR &&
              EqualRgn(intact, originalRegion));
    if (intact)
      DeleteObject(intact);
    check("visible_return_restores_without_region_mask",
          controller.enter(original) && IsWindowVisible(single) &&
              nativeHome(api, single) == original);
    ShowWindowAsync(single, SW_HIDE);
    wait([&] { return !IsWindowVisible(single); });
    check("collapsed_current_does_not_remove_inactive_expanded_list_entry",
          sharedCount(controller.logicalViews()) == 1);
    check("A_expanded_intent_restored",
          controller.enter(api.id(a.Get())) && IsWindowVisible(single) &&
              nativeHome(api, single) == api.id(a.Get()));
    check("browser_original_group_restored_on_A",
          appId(multi) == baselineGroup);
    auto maximized = livecompat::capture(single);
    maximized.show = cc::Show::Maximized;
    check("maximize_without_foreground_activation",
          show(single, maximized) &&
              wait([&] { return IsZoomed(single) != FALSE; }) &&
              GetForegroundWindow() == originalForeground);
    check("B_collapsed_intent_survives_A_replay",
          controller.enter(original) && !IsWindowVisible(single));
    check("A_maximized_intent_restored",
          controller.enter(api.id(a.Get())) && IsZoomed(single));
    check("B_hidden_after_maximized_replay",
          controller.enter(original) && !IsWindowVisible(single));
    check("no_system_switch_during_replay", api.currentId() == original);
    RuntimeTrace trace;
    auto traceFile = report.parent_path() / L"synthetic-switch-trace.json";
    trace.initialize(traceFile, true);
    trace.begin(api.id(a.Get()), original);
    trace.prepared(true, controller.windowTimings());
    trace.switchBegin();
    trace.switchReturned(true);
    trace.current(original, RuntimeTrace::now());
    trace.switched(original, RuntimeTrace::now());
    bool traceFinished = !trace.poll(&controller);
    trace.flush();
    std::ifstream traceInput(traceFile);
    std::string traceText((std::istreambuf_iterator<char>(traceInput)), {});
    check("runtime_trace_separates_phases_with_synthetic_scope",
          traceFinished &&
              traceText.find("synthetic trace test") != std::string::npos &&
              traceText.find("switch_api_ms") != std::string::npos &&
              traceText.find("native_ready_from_start_ms") !=
                  std::string::npos);
    // Another physical window of the same app is managed independently.
    HWND another = spawn(false);
    controller.invalidateDiscovery();
    bool discovered = controller.sync();
    check("event_invalidated_new_browser_window_discovered",
          discovered && another && controller.count() == 3);
    check("current_browser_keeps_original_group",
          controller.enter(original) && appId(another) == baselineGroup);
    ComPtr<IUnknown> cloakView;
    ComPtr<ApplicationViewVisibility> cloakControl;
    HWND thumbnail{};
    bool cloakInterface =
        another && SUCCEEDED(api.views->GetViewForHwnd(another, &cloakView)) &&
        cloakView && SUCCEEDED(cloakView.As(&cloakControl)) &&
        SUCCEEDED(cloakControl->GetThumbnailWindow(&thumbnail)) &&
        thumbnail == another;
    check("exact_shell_visibility_interface_and_hwnd_verified", cloakInterface);
    bool shellCloaked =
        cloakInterface && SUCCEEDED(cloakControl->SetCloak(1, 2)) && wait([&] {
          DWORD c{};
          return SUCCEEDED(DwmGetWindowAttribute(another, DWMWA_CLOAKED, &c,
                                                 sizeof(c))) &&
                 c == DWM_CLOAKED_SHELL;
        });
    check("owned_current_window_shell_cloak_reproduced", shellCloaked);
    bool uncloak = shellCloaked &&
                   SUCCEEDED(clearCurrentShellCloak(api, another)) && wait([&] {
                     DWORD c{};
                     return SUCCEEDED(DwmGetWindowAttribute(
                                another, DWMWA_CLOAKED, &c, sizeof(c))) &&
                            c == 0;
                   });
    check("shell_cloak_cleared_without_minimize_focus_or_switch",
          uncloak && GetForegroundWindow() == originalForeground &&
              api.currentId() == original);
    check("shell_uncloak_refuses_background_desktop",
          clearCurrentShellCloak(api, multi) == E_ACCESSDENIED);
    check("foreign_handle_rejected",
          !controller.valid((cc::WindowId)GetDesktopWindow()));
    GUID invalid{};
    CoCreateGuid(&invalid);
    check("missing_target_fails_without_switch",
          !controller.enter(invalid) && api.currentId() == original);
    check("failed_transition_can_recover", controller.enter(original));
    perf = {};
    perf.enabled = true;
    bool benchmarkOk = true;
    for (int i = 0; i < 8; ++i)
      benchmarkOk = controller.enter(api.id(a.Get())) &&
                    controller.enter(original) && benchmarkOk;
    for (int i = 0; i < 16; ++i)
      benchmarkOk = controller.sync() && benchmarkOk;
    warmStats = perf;
    perf.enabled = false;
    check("timed_replay_preserves_B_hidden_and_system_desktop",
          benchmarkOk && !IsWindowVisible(single) &&
              api.currentId() == original);
    if (another) {
      PostMessageW(another, WM_CLOSE, 0, 0);
      bool destroyed = wait([&] { return !IsWindow(another); });
      check("cached_process_identity_rejects_destroyed_window",
            destroyed && !controller.valid((cc::WindowId)another));
    }
    check("normal_exit_restores_records", controller.stop());
    check("single_original_home_and_visibility_restored",
          nativeHome(api, single) == api.id(a.Get()) &&
              IsWindowVisible(single));
    check("browser_appid_restored_on_exit", appId(multi) == baselineGroup);
    browserAppPinned = TRUE;
    if (api.pinned)
      api.pinned->IsAppPinned(baselineGroup.c_str(), &browserAppPinned);
    check("exit_does_not_restore_broken_global_browser_pin", !browserAppPinned);
    check("recovery_file_removed_on_clean_exit",
          !fs::exists(report.parent_path() / (nonce + L".bin")));
    {
      Controller configured(api);
      configured.loadPolicies(report.parent_path() / (nonce + L"-modes.ini"));
      auto genericGate = [&](HWND h) -> int {
        if (h != single || GetPropW(h, nonce.c_str()) != (HANDLE)1)
          return 0;
        DWORD pid{};
        GetWindowThreadProcessId(h, &pid);
        if (!owned.count(pid))
          return 0;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        bool same = p && birth(p) == owned.at(pid);
        if (p)
          CloseHandle(p);
        return same ? -1 : 0;
      };
      bool started = configured.start(
          report.parent_path() / (nonce + L"-mode-session.bin"), genericGate);
      check("unknown_application_auto_keeps_windows_behavior",
            started && configured.count() == 0);
      check("unknown_window_class_can_use_explicit_shared_policy",
            configured.setMode(single, apppolicy::Mode::Shared) &&
                configured.isSharedWindow(single));
      auto open = capture(single);
      open.show = cc::Show::Normal;
      show(single, open);
      move(api, single, original);
      ComPtr<IUnknown> pinnedShared;
      api.views->GetViewForHwnd(single, &pinnedShared);
      bool pinned =
          pinnedShared && SUCCEEDED(api.pinned->PinView(pinnedShared.Get()));
      check("explicit_shared_policy_removes_all_desktops_window_pin",
            pinned && configured.sync() && !api.pinnedWindow(single));
      check("hidden_shared_without_view_can_reregister",
            Controller::hiddenViewMayRegister(true, true, false, true, false));
      check("missing_view_recovery_requires_safe_enrollment",
            !Controller::hiddenViewMayRegister(false, true, false, true, false) &&
            !Controller::hiddenViewMayRegister(true, false, false, true, false) &&
            !Controller::hiddenViewMayRegister(true, true, true, true, false) &&
            !Controller::hiddenViewMayRegister(true, true, false, false, false) &&
            !Controller::hiddenViewMayRegister(true, true, false, true, true));
      auto collapsed = capture(single);
      collapsed.show = cc::Show::Hidden;
      show(single, collapsed);
      configured.observe(single);
      bool replay = true;
      std::ofstream replayLog(report.parent_path() / L"shared-replay.json");
      replayLog << "[";
      for (int i = 0; i < 3; ++i) {
        bool toA = configured.enter(api.id(a.Get()));
        bool visibleA = IsWindowVisible(single);
        bool back = configured.enter(original);
        bool hiddenBack = !IsWindowVisible(single);
        bool synced = configured.sync();
        if (i) replayLog << ",";
        replayLog << "{\"to_a\":" << toA << ",\"visible_a\":" << visibleA
                  << ",\"back\":" << back << ",\"hidden_back\":" << hiddenBack
                  << ",\"sync\":" << synced << "}";
        replay = toA && visibleA && back && hiddenBack && synced && replay;
      }
      replayLog << "]";
      check("explicit_shared_repeated_hidden_return_does_not_block", replay);
      show(single, open);
      move(api, single, original);
      configured.observe(single);
      check("mode_change_to_native_preserves_current_window",
            configured.setMode(single, apppolicy::Mode::Native) &&
                !configured.isSharedWindow(single) && IsWindowVisible(single) &&
                nativeHome(api, single) == original);
      check("mode_choice_is_reported_after_save",
            configured.selectedMode(single) == apppolicy::Mode::Native);
      check("mode_change_to_separate_preserves_real_hwnd_home",
            configured.setMode(single, apppolicy::Mode::Separate) &&
                configured.enter(api.id(a.Get())) &&
                nativeHome(api, single) == original);
      check("return_to_automatic_releases_explicit_adapter",
            configured.setMode(single, apppolicy::Mode::Automatic) &&
                configured.count() == 0);
      check("mode_session_exits_cleanly", configured.stop());
      move(api, single, api.id(a.Get()));
    }
    auto crashFile = report.parent_path() / (nonce + L"-crash.bin");
    auto crashCmd = L"\"" + exe + L"\" --compat-crash \"" + nonce + L"\" " +
                    std::to_wstring((UINT_PTR)single) + L" " +
                    std::to_wstring((UINT_PTR)multi) + L" \"" +
                    crashFile.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    bool crashStarted =
        CreateProcessW(exe.c_str(), crashCmd.data(), nullptr, nullptr, FALSE,
                       CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                       &si, &pi) != FALSE;
    if (crashStarted) {
      if (!AssignProcessToJobObject(job, pi.hProcess)) {
        TerminateProcess(pi.hProcess, 1);
        crashStarted = false;
      } else
        ResumeThread(pi.hThread);
      CloseHandle(pi.hThread);
      WaitForSingleObject(pi.hProcess, 10000);
      DWORD exit{};
      GetExitCodeProcess(pi.hProcess, &exit);
      check("controller_abnormal_exit_exercised", crashStarted && exit == 88);
      CloseHandle(pi.hProcess);
      check("guardian_restores_hidden_window_after_crash",
            wait(
                [&] {
                  return !fs::exists(crashFile) && IsWindowVisible(single) &&
                         nativeHome(api, single) == api.id(a.Get());
                },
                6000));
      check("guardian_restores_browser_group_after_crash",
            appId(multi) == baselineGroup);
      HRGN region = CreateRectRgn(0, 0, 0, 0);
      check("guardian_restores_original_custom_region",
            region && GetWindowRgn(single, region) != ERROR &&
                EqualRgn(region, originalRegion));
      if (region)
        DeleteObject(region);
    } else
      check("crash_controller_started", false);
  }
  // Terminate only Job-contained test processes, including blocked fixture
  // loops. No real app handles are ever sent WM_CLOSE or termination requests.
  TerminateJobObject(job, 0);
  for (HANDLE p : processes) {
    WaitForSingleObject(p, 3000);
    CloseHandle(p);
  }
  CloseHandle(job);
  if (originalRegion)
    DeleteObject(originalRegion);
  bool empty = true;
  struct Empty {
    DesktopApi *api;
    GUID id;
    bool *empty;
  } scan{&api, api.id(a.Get()), &empty};
  EnumWindows(
      [](HWND h, LPARAM v) -> BOOL {
        auto &s = *(Empty *)v;
        if (nativeHome(*s.api, h) == s.id)
          *s.empty = false;
        return TRUE;
      },
      (LPARAM)&scan);
  bool removed = empty && api.currentId() != api.id(a.Get()) &&
                 SUCCEEDED(api.internal->Remove(a.Get(), origin.Get()));
  check("only_owned_empty_background_desktop_removed", removed);
  check("system_desktop_unchanged", api.currentId() == original);
  check("foreground_window_unchanged",
        GetForegroundWindow() == originalForeground);
  std::ofstream out(report);
  out << "{\n\"scope\":\"production adapter; non-cooperating transparent owned "
         "processes; no desktop switching\",\n"
      << "\"original_taskbar_mouse_click\":\"not_executed\",\n\"real_third_"
         "party_apps\":\"not_modified\",\n\"checks\":[\n";
  bool ok = true;
  for (size_t i = 0; i < results.size(); ++i) {
    auto &[n, pass] = results[i];
    ok = ok && pass;
    out << "{\"name\":\"" << n << "\",\"pass\":" << (pass ? "true" : "false")
        << "}" << (i + 1 < results.size() ? "," : "") << "\n";
  }
  out << "],\"performance\":{\"cold\":";
  coldStats.write(out);
  out << ",\"warm\":";
  warmStats.write(out);
  out << "}}\n";
  return ok ? 0 : 20;
}
inline int dispatch(const std::wstring &command) {
  if (command.rfind(L"--compat-", 0) != 0)
    return -1;
  int n{};
  auto argv = CommandLineToArgvW(GetCommandLineW(), &n);
  std::vector<std::wstring> a;
  for (int i = 1; i < n; ++i)
    a.push_back(argv[i]);
  LocalFree(argv);
  try {
    if (a.size() == 1 && a[0] == L"--compat-repair-current-codex-cloak") {
      DesktopApi api;
      if (!api.connect() || !api.views)
        return 10;
      GUID here = api.currentId();
      ComPtr<Desktop> destination;
      if (FAILED(api.internal->Find(&here, &destination)))
        return 11;
      struct Search {
        std::vector<HWND> windows;
      } search;
      EnumWindows(
          [](HWND h, LPARAM data) -> BOOL {
            if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) ||
                cls(h) != L"Chrome_WidgetWin_1")
              return TRUE;
            DWORD pid{};
            GetWindowThreadProcessId(h, &pid);
            auto path = lower(image(pid));
            if (fs::path(path).filename() == L"chatgpt.exe" &&
                path.find(L"\\openai.codex_") != std::wstring::npos)
              ((Search *)data)->windows.push_back(h);
            return TRUE;
          },
          (LPARAM)&search);
      auto root =
          fs::path(image(GetCurrentProcessId())).parent_path().parent_path() /
          L"artifacts" / L"runtime";
      fs::create_directories(root);
      std::ofstream report(root / L"codex-shell-refresh-probe.json");
      report << "{\"operation\":\"same-current-desktop MoveView only; no "
                "minimize/focus/switch\",\"windows\":[";
      bool first = true, ok = true;
      for (HWND h : search.windows) {
        DWORD before{};
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &before, sizeof(before));
        if (before != DWM_CLOAKED_SHELL || nativeHome(api, h) != here)
          continue;
        ComPtr<IUnknown> view;
        HRESULT hr = api.views->GetViewForHwnd(h, &view);
        if (SUCCEEDED(hr) && view)
          hr = clearCurrentShellCloak(api, h);
        bool cleared =
            SUCCEEDED(hr) &&
            wait(
                [&] {
                  DWORD cloak{};
                  return SUCCEEDED(DwmGetWindowAttribute(
                             h, DWMWA_CLOAKED, &cloak, sizeof(cloak))) &&
                         cloak == 0;
                },
                1200);
        DWORD after{};
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &after, sizeof(after));
        if (!first)
          report << ",";
        first = false;
        report << "{\"hwnd\":" << (UINT_PTR)h << ",\"before\":" << before
               << ",\"after\":" << after << ",\"hresult\":" << (unsigned long)hr
               << ",\"cleared\":" << (cleared ? "true" : "false") << "}";
        ok = cleared && ok;
        RedrawWindow(h, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      bool same = api.currentId() == here;
      report << "],\"system_desktop_unchanged\":" << (same ? "true" : "false")
             << "}\n";
      return ok && same ? 0 : 21;
    }
    if (a.size() == 4 && a[0] == L"--compat-guardian")
      return guardian(std::stoul(a[1]), std::stoull(a[2]), a[3]);
    if (a.size() == 3 && a[0] == L"--compat-fixture")
      return fixture(a[1], a[2] == L"single");
    if (a.size() == 5 && a[0] == L"--compat-crash")
      return crashController(a[1], (HWND)(UINT_PTR)std::stoull(a[2]),
                             (HWND)(UINT_PTR)std::stoull(a[3]), a[4]);
    if (a.size() == 1 && a[0] == L"--compat-check")
      return nativeChecks();
    if (a.size() == 1 && a[0] == L"--compat-localize-chrome") {
      DesktopApi api;
      if (!api.connect())
        return 10;
      auto root =
          fs::path(image(GetCurrentProcessId())).parent_path().parent_path() /
          L"artifacts" / L"compat-production";
      fs::create_directories(root);
      GUID session{};
      CoCreateGuid(&session);
      Controller c(api);
      c.start(root / (text(session) + L"-chrome.bin"), [](HWND h) {
        DWORD pid{};
        GetWindowThreadProcessId(h, &pid);
        return lower(fs::path(image(pid)).filename().wstring()) ==
                           L"chrome.exe" &&
                       cls(h) == L"Chrome_WidgetWin_1" &&
                       !GetWindow(h, GW_OWNER)
                   ? 2
                   : 0;
      });
      std::ofstream out(root / L"chrome-localize.json");
      out << "{\"before\":";
      c.inventoryReport(out);
      bool ok = c.enter(api.currentId());
      ok = c.stop() && ok;
      out << ",\"after\":";
      c.inventoryReport(out);
      out << ",\"success\":" << (ok ? "true" : "false") << "}\n";
      return ok ? 0 : 21;
    }
    if (a.size() == 1 && a[0] == L"--compat-inventory") {
      DesktopApi api;
      if (!api.connect())
        return 10;
      auto root =
          fs::path(image(GetCurrentProcessId())).parent_path().parent_path() /
          L"artifacts" / L"compat-production";
      fs::create_directories(root);
      Controller c(api);
      c.start(root / L"unused-readonly.bin");
      std::ofstream out(root / L"inventory.json");
      c.inventoryReport(out);
      c.stop();
      return 0;
    }
  } catch (...) {
    return 12;
  }
  return 2;
}
} // namespace tongzhuo::livecompat
