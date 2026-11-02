#pragma once
#include <algorithm>
#include <appmodel.h>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

namespace tongzhuo::policy {
enum class Mode { Automatic = 0, Native = 1, Separate = 2, Shared = 3 };
inline std::wstring baseId(std::wstring id) {
  auto at = id.find(L".TongZhuo.Parked.");
  if (at != std::wstring::npos)
    id.resize(at);
  return id;
}
inline std::wstring identity(const std::wstring &path, const std::wstring &id,
                             const std::wstring &family = {}) {
  if (!baseId(id).empty())
    return L"A:" + baseId(id);
  if (!family.empty())
    return L"P:" + family;
  if (path.empty())
    return {};
  auto canonical = path;
  std::transform(canonical.begin(), canonical.end(), canonical.begin(),
                 towlower);
  return L"X:" + canonical;
}
inline std::wstring family(DWORD pid) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p)
    return {};
  UINT32 n = 0;
  LONG result = GetPackageFamilyName(p, &n, nullptr);
  std::wstring out;
  if (result == ERROR_INSUFFICIENT_BUFFER && n && n < 1024) {
    out.resize(n);
    if (GetPackageFamilyName(p, &n, out.data()) == ERROR_SUCCESS)
      out.resize(n - 1);
    else
      out.clear();
  }
  CloseHandle(p);
  return out;
}
inline const wchar_t *label(Mode mode) {
  switch (mode) {
  case Mode::Separate:
    return L"各桌面独立窗口";
  case Mode::Shared:
    return L"各桌面独立展开 / 收起";
  case Mode::Native:
    return L"跟随 Windows";
  default:
    return L"自动推荐";
  }
}
class Store {
  std::filesystem::path file;
  std::map<std::wstring, Mode> values;
  static std::wstring section(const std::wstring &key) {
    uint64_t hash = 1469598103934665603ULL;
    for (wchar_t c : key) {
      hash ^= (unsigned)c;
      hash *= 1099511628211ULL;
    }
    wchar_t s[32]{};
    swprintf_s(s, L"App%016llX", (unsigned long long)hash);
    return s;
  }

public:
  void open(std::filesystem::path path) {
    file = std::move(path);
    values.clear();
    if (!std::filesystem::exists(file))
      return;
    std::vector<wchar_t> sections(65536);
    GetPrivateProfileSectionNamesW(sections.data(), (DWORD)sections.size(),
                                   file.c_str());
    for (const wchar_t *s = sections.data(); *s; s += wcslen(s) + 1) {
      wchar_t key[4096]{};
      GetPrivateProfileStringW(s, L"Identity", L"", key, 4096, file.c_str());
      int mode = GetPrivateProfileIntW(s, L"Mode", 0, file.c_str());
      if (*key && mode >= 1 && mode <= 3)
        values[key] = (Mode)mode;
    }
  }
  bool empty() const { return values.empty(); }
  Mode get(const std::wstring &key) const {
    auto at = values.find(key);
    return at == values.end() ? Mode::Automatic : at->second;
  }
  bool set(const std::wstring &key, Mode mode) {
    if (file.empty() || key.empty() || key.size() >= 4096 ||
        key.find_first_of(L"\r\n") != std::wstring::npos)
      return false;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec)
      return false;
    if (!std::filesystem::exists(file)) {
      std::ofstream initial(file, std::ios::binary);
      const unsigned char bom[] = {0xff, 0xfe};
      initial.write((const char *)bom, 2);
      initial.close();
      if (!initial)
        return false;
    }
    auto s = section(key);
    wchar_t existing[4096]{};
    GetPrivateProfileStringW(s.c_str(), L"Identity", L"", existing, 4096,
                             file.c_str());
    if (*existing && key != existing)
      return false;
    if (mode == Mode::Automatic) {
      if (!WritePrivateProfileStringW(s.c_str(), nullptr, nullptr,
                                      file.c_str()))
        return false;
      values.erase(key);
      return true;
    }
    if (!WritePrivateProfileStringW(s.c_str(), L"Identity", key.c_str(),
                                    file.c_str()) ||
        !WritePrivateProfileStringW(s.c_str(), L"Mode",
                                    std::to_wstring((int)mode).c_str(),
                                    file.c_str()))
      return false;
    values[key] = mode;
    return true;
  }
};
} // namespace tongzhuo::policy
