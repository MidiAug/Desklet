#pragma once
#include <string>
#include <utility>
#include <windows.h>

namespace tongzhuo {
class StartupSetting {
  std::wstring key;
  static constexpr auto value = L"TongZhuo";

public:
  explicit StartupSetting(std::wstring registryKey =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run")
      : key(std::move(registryKey)) {}
  std::wstring command() const {
    wchar_t data[32768]{};
    DWORD bytes = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ,
                     nullptr, data, &bytes) != ERROR_SUCCESS)
      return {};
    return data;
  }
  bool enabled() const { return !command().empty(); }
  bool set(bool enabled, const std::wstring &exe) {
    HKEY handle{};
    if (!enabled && !this->enabled())
      return true;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS)
      return false;
    std::wstring cmd = L"\"" + exe + L"\" --background";
    LONG result = enabled
        ? RegSetValueExW(handle, value, 0, REG_SZ, (const BYTE *)cmd.c_str(),
                         (DWORD)((cmd.size() + 1) * sizeof(wchar_t)))
        : RegDeleteValueW(handle, value);
    RegCloseKey(handle);
    return result == ERROR_SUCCESS || (!enabled && result == ERROR_FILE_NOT_FOUND);
  }
};
} // namespace tongzhuo
