#pragma once
#include "window_compat.h"
#include <mutex>

namespace tongzhuo {
// Bounded metadata for real user transitions. Native readiness is NOT a
// rendered-frame measurement. No titles, URLs, screenshots or document data.
class RuntimeTrace {
  struct Record {
    unsigned seq{};
    std::wstring utc;
    GUID from{}, target{};
    double started{}, switchStarted = -1;
    bool external = false, ok = false;
    double prepare = -1, switchCall = -1, currentChanged = -1,
           shellSwitched = -1, nativeReady = -1, paint = -1, ended = -1;
    std::string result;
    std::vector<livecompat::Controller::WindowTiming> windows;
    std::vector<livecompat::Controller::ShellRepair> repairs;
  };
  mutable std::mutex mutex;
  bool enabled = false, pending = false, dirty = false, synthetic = false;
  unsigned sequence = 0;
  std::filesystem::path path;
  Record active;
  std::vector<Record> records;
  void finishLocked(const char *reason) {
    if (!pending)
      return;
    active.ended = now() - active.started;
    active.result = reason;
    if (records.size() == 64)
      records.erase(records.begin());
    records.push_back(active);
    pending = false;
    dirty = true;
  }
  static void value(std::ostream &out, double n) {
    if (n < 0)
      out << "null";
    else
      out << n;
  }

public:
  static double now() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
  void initialize(std::filesystem::path file, bool test = false) {
    {
      std::lock_guard lock(mutex);
      enabled = true;
      synthetic = test;
      path = std::move(file);
      dirty = true;
    }
    flush();
  }
  void begin(GUID from, GUID target, bool external = false) {
    std::lock_guard lock(mutex);
    if (!enabled)
      return;
    finishLocked("superseded");
    active = {};
    active.seq = ++sequence;
    SYSTEMTIME stamp{};
    GetSystemTime(&stamp);
    wchar_t utc[40]{};
    swprintf_s(utc, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", stamp.wYear,
               stamp.wMonth, stamp.wDay, stamp.wHour, stamp.wMinute,
               stamp.wSecond, stamp.wMilliseconds);
    active.utc = utc;
    active.from = from;
    active.target = target;
    active.external = external;
    active.started = now();
    pending = true;
  }
  void
  prepared(bool ok,
           const std::vector<livecompat::Controller::WindowTiming> &windows) {
    std::lock_guard lock(mutex);
    if (!pending)
      return;
    active.prepare = now() - active.started;
    active.windows = windows;
    if (active.windows.size() > 32)
      active.windows.resize(32);
    if (!ok)
      finishLocked("prepare_failed");
  }
  void switchBegin() {
    std::lock_guard lock(mutex);
    if (pending)
      active.switchStarted = now();
  }
  void switchReturned(bool ok) {
    std::lock_guard lock(mutex);
    if (!pending)
      return;
    active.ok = ok;
    active.switchCall = now() - active.switchStarted;
    if (!ok)
      finishLocked("switch_failed");
  }
  void current(GUID target, double stamp) {
    {
      std::lock_guard lock(mutex);
      if (!enabled)
        return;
      if (pending && active.target == target) {
        if (active.currentChanged < 0)
          active.currentChanged = stamp - active.started;
        return;
      }
      if (pending)
        finishLocked("external_target_changed");
    }
    begin({}, target, true);
    std::lock_guard lock(mutex);
    active.currentChanged = 0;
    active.ok = true;
  }
  void switched(GUID target, double stamp) {
    std::lock_guard lock(mutex);
    if (pending && active.target == target && active.shellSwitched < 0)
      active.shellSwitched = stamp - active.started;
  }
  void painted() {
    std::lock_guard lock(mutex);
    if (pending && active.switchCall >= 0 && active.paint < 0)
      active.paint = now() - active.started;
  }
  uint64_t repairToken() const {
    std::lock_guard lock(mutex);
    return pending && active.shellSwitched >= 0 &&
                   now() - active.started - active.shellSwitched >= 75
               ? active.seq
               : 0;
  }
  void
  repaired(const std::vector<livecompat::Controller::ShellRepair> &repairs) {
    std::lock_guard lock(mutex);
    if (pending)
      active.repairs.insert(active.repairs.end(), repairs.begin(),
                            repairs.end());
  }
  bool poll(livecompat::Controller *controller) {
    GUID target{};
    {
      std::lock_guard lock(mutex);
      if (!pending)
        return false;
      target = active.target;
    }
    bool ready = controller && controller->nativeReady(target);
    std::lock_guard lock(mutex);
    if (!pending || active.target != target)
      return pending;
    if (ready && active.nativeReady < 0)
      active.nativeReady = now() - active.started;
    if (active.nativeReady >= 0 && active.shellSwitched >= 0)
      finishLocked("native_ready_and_shell_notified");
    else if (now() - active.started >= 2000)
      finishLocked("observation_deadline");
    return pending;
  }
  void flush() {
    std::vector<Record> copy;
    std::filesystem::path file;
    {
      std::lock_guard lock(mutex);
      if (!enabled || !dirty)
        return;
      copy = records;
      file = path;
      dirty = false;
    }
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto temp = file;
    temp += L".tmp";
    std::ofstream out(temp);
    out << "{\"version\":\"0.5.3\",\"pid\":" << GetCurrentProcessId()
        << ",\"scope\":\""
        << (synthetic ? "synthetic trace test; " : "actual user actions; ")
        << "native state only; renderer "
           "pixels not "
           "observed\",\"readiness_poll_ms\":25,\"capacity\":64,\"records\":[";
    for (size_t i = 0; i < copy.size(); ++i) {
      const auto &r = copy[i];
      if (i)
        out << ",";
      out << "{\"sequence\":" << r.seq << ",\"origin\":\""
          << (r.external ? "external_notification" : "tool_user_action")
          << "\",\"started_utc\":" << livecompat::jsonString(r.utc)
          << ",\"from\":" << livecompat::jsonString(livecompat::text(r.from))
          << ",\"target\":"
          << livecompat::jsonString(livecompat::text(r.target))
          << ",\"prepare_ms\":";
      value(out, r.prepare);
      out << ",\"switch_api_ms\":";
      value(out, r.switchCall);
      out << ",\"current_notification_from_start_ms\":";
      value(out, r.currentChanged);
      out << ",\"shell_switched_from_start_ms\":";
      value(out, r.shellSwitched);
      out << ",\"native_ready_from_start_ms\":";
      value(out, r.nativeReady);
      out << ",\"tool_paint_from_start_ms\":";
      value(out, r.paint);
      out << ",\"observation_ms\":";
      value(out, r.ended);
      out << ",\"result\":\"" << r.result << "\",\"windows\":[";
      for (size_t j = 0; j < r.windows.size(); ++j) {
        const auto &w = r.windows[j];
        if (j)
          out << ",";
        out << "{\"exe\":" << livecompat::jsonString(w.exe)
            << ",\"hwnd\":" << w.window << ",\"kind\":\""
            << (w.browser ? "browser" : "shared")
            << "\",\"ok\":" << (w.ok ? "true" : "false")
            << ",\"used_region_mask\":" << (w.masked ? "true" : "false")
            << ",\"failure_stage\":" << livecompat::jsonString(w.failureStage)
            << ",\"total_ms\":" << w.totalMs
            << ",\"register_ms\":" << w.registerMs
            << ",\"move_ms\":" << w.moveMs << ",\"state_ms\":" << w.stateMs
            << "}";
      }
      out << "],\"shell_repairs\":[";
      for (size_t k = 0; k < r.repairs.size(); ++k) {
        const auto &repair = r.repairs[k];
        if (k)
          out << ",";
        out << "{\"exe\":" << livecompat::jsonString(repair.exe)
            << ",\"hwnd\":" << repair.window << ",\"before\":" << repair.before
            << ",\"after\":" << repair.after
            << ",\"hresult\":" << (unsigned long)repair.result
            << ",\"ms\":" << repair.ms << "}";
      }
      out << "]}";
    }
    out << "]}\n";
    out.close();
    if (out)
      MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  void shutdown() {
    {
      std::lock_guard lock(mutex);
      finishLocked("shutdown");
    }
    flush();
  }
};
} // namespace tongzhuo
