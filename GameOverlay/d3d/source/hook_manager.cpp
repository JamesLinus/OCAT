// Copyright 2016 Patrick Mours.All rights reserved.
//
// https://github.com/crosire/gameoverlay
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met :
//
// 1. Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and / or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY COPYRIGHT HOLDER ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.IN NO EVENT
// SHALL COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES(INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT(INCLUDING NEGLIGENCE
// OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.

#include "hook_manager.hpp"
#include <Windows.h>
#include <TlHelp32.h>

#include <assert.h>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "Overlay\DLLInjection.h"
#include "Config\BlackList.h"
#include "Recording\Capturing.h"
#include "Utility\FileDirectory.h"
#include "Logging\MessageLog.h"
#include "Overlay\VK_Environment.h"
#include "critical_section.hpp"
#include "Utility\ProcessHelper.h"

extern std::wstring g_dllDirectory;
extern BlackList g_blackList;

namespace GameOverlay {

namespace {
enum class hook_method { function_hook, vtable_hook };
struct module_export {
  hook::address address;
  const char *name;
  unsigned short ordinal;
};

HMODULE get_current_module()
{
  HMODULE handle = nullptr;
  GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&get_current_module), &handle);

  return handle;
}
std::vector<module_export> get_module_exports(HMODULE handle)
{
  std::vector<module_export> exports;
  const auto imagebase = reinterpret_cast<const BYTE *>(handle);
  const auto imageheader = reinterpret_cast<const IMAGE_NT_HEADERS *>(
      imagebase + reinterpret_cast<const IMAGE_DOS_HEADER *>(imagebase)->e_lfanew);

  if (imageheader->Signature != IMAGE_NT_SIGNATURE ||
      imageheader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size == 0) {
    return exports;
  }

  const auto exportdir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(
      imagebase +
      imageheader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
  const auto exportbase = static_cast<WORD>(exportdir->Base);

  if (exportdir->NumberOfFunctions == 0) {
    return exports;
  }

  const auto count = static_cast<size_t>(exportdir->NumberOfNames);
  exports.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    module_export symbol;
    symbol.ordinal =
        reinterpret_cast<const WORD *>(imagebase + exportdir->AddressOfNameOrdinals)[i] +
        exportbase;
    symbol.name = reinterpret_cast<const char *>(
        imagebase + reinterpret_cast<const DWORD *>(imagebase + exportdir->AddressOfNames)[i]);
    symbol.address = const_cast<void *>(reinterpret_cast<const void *>(
        imagebase + reinterpret_cast<const DWORD *>(
                        imagebase + exportdir->AddressOfFunctions)[symbol.ordinal - exportbase]));

    exports.push_back(std::move(symbol));
  }

  return exports;
}

critical_section s_cs;
std::vector<std::wstring> s_delayed_hook_paths;
std::vector<HMODULE> s_delayed_hook_modules;
std::vector<std::pair<hook, hook_method>> s_hooks;
std::unordered_map<hook::address, hook::address *> s_vtable_addresses;

bool install_hook(hook::address target, hook::address replacement, hook_method method)
{
  hook hook(target, replacement);
  hook.trampoline = target;

  hook::status status = hook::status::unknown;

  switch (method) {
    case hook_method::function_hook: {
      status = hook.install();
      break;
    }
    case hook_method::vtable_hook: {
      DWORD protection = PAGE_READWRITE;
      const auto target_address = s_vtable_addresses.at(target);

      if (VirtualProtect(target_address, sizeof(*target_address), protection, &protection)) {
        *target_address = replacement;

        VirtualProtect(target_address, sizeof(*target_address), protection, &protection);

        status = hook::status::success;
      }
      else {
        status = hook::status::memory_protection_failure;
      }
      break;
    }
  }

  if (status != hook::status::success) {
    g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "Hook installation failed");
    return false;
  }

  const critical_section::lock lock(s_cs);

  s_hooks.emplace_back(std::move(hook), method);

  g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "Successfully installed hook");
  return true;
}
bool install_hook(const HMODULE target_module, const HMODULE replacement_module, hook_method method)
{
  assert(target_module != nullptr);
  assert(replacement_module != nullptr);

  // Load export tables
  const auto target_exports = get_module_exports(target_module);
  const auto replacement_exports = get_module_exports(replacement_module);

  if (target_exports.empty()) {
    g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "No exports found");
    return false;
  }

  size_t install_count = 0;
  std::vector<std::pair<hook::address, hook::address>> matches;
  matches.reserve(replacement_exports.size());

  // Analyze export table
  for (const auto &symbol : target_exports) {
    if (symbol.name == nullptr || symbol.address == nullptr) {
      continue;
    }

    // Find appropriate replacement
    const auto it = std::find_if(replacement_exports.cbegin(), replacement_exports.cend(),
                                 [&symbol](const module_export &moduleexport) {
                                   return std::strcmp(moduleexport.name, symbol.name) == 0;
                                 });

    if (it == replacement_exports.cend()) {
      continue;
    }
    g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "Found matching function: " + std::string(symbol.name));
    matches.push_back(std::make_pair(symbol.address, it->address));
  }

  // Hook matching exports
  for (const auto &match : matches) {
    if (install_hook(match.first, match.second, method)) {
      install_count++;
    }
  }
  g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "Install count: " + std::to_string(install_count));
  return install_count != 0;
}
bool uninstall_hook(hook &hook, hook_method method)
{
  if (hook.uninstalled()) {
    return true;
  }

  hook::status status = hook::status::unknown;

  switch (method) {
    case hook_method::function_hook: {
      status = hook.uninstall();
      break;
    }
    case hook_method::vtable_hook: {
      DWORD protection = PAGE_READWRITE;
      const auto target_address = s_vtable_addresses.at(hook.target);

      if (VirtualProtect(target_address, sizeof(*target_address), protection, &protection)) {
        *target_address = hook.target;
        s_vtable_addresses.erase(hook.target);

        VirtualProtect(target_address, sizeof(*target_address), protection, &protection);

        status = hook::status::success;
      }
      else {
        status = hook::status::memory_protection_failure;
      }
      break;
    }
  }

  if (status != hook::status::success) {
    return false;
  }

  hook.trampoline = nullptr;

  return true;
}

hook find_hook(hook::address replacement)
{
  const critical_section::lock lock(s_cs);

  const auto it = std::find_if(s_hooks.cbegin(), s_hooks.cend(),
                               [replacement](const std::pair<hook, hook_method> &hook) {
                                 return hook.first.replacement == replacement;
                               });

  if (it == s_hooks.cend()) {
    return hook();
  }

  return it->first;
}
template <typename T>
inline T find_hook_trampoline_unchecked(T replacement)
{
  return reinterpret_cast<T>(find_hook(reinterpret_cast<hook::address>(replacement)).call());
}

HMODULE WINAPI HookLoadLibraryA(LPCSTR lpFileName)
{
  if (lpFileName == nullptr) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "HookLoadLibraryA", "Called with nullptr -> Abort");
    return nullptr;
  }

  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookLoadLibraryA", "Load library " + std::string(lpFileName));

  static const auto trampoline = find_hook_trampoline_unchecked(&HookLoadLibraryA);

  const HMODULE handle = trampoline(lpFileName);

  if (handle == nullptr || handle == get_current_module()) {
    return handle;
  }

  const critical_section::lock lock(s_cs);

  HookAllModules();

  const auto remove = std::remove_if(s_delayed_hook_paths.begin(), s_delayed_hook_paths.end(),
                                     [lpFileName](const std::wstring &path) {
                                       HMODULE delayed_handle = nullptr;
                                       GetModuleHandleExW(0, path.c_str(), &delayed_handle);

                                       if (delayed_handle == nullptr) {
                                         return false;
                                       }

                                       s_delayed_hook_modules.push_back(delayed_handle);

                                       return install_hook(delayed_handle, get_current_module(),
                                                           hook_method::function_hook);
                                     });

  s_delayed_hook_paths.erase(remove, s_delayed_hook_paths.end());

  return handle;
}
HMODULE WINAPI HookLoadLibraryExA(LPCSTR lpFileName, HANDLE hFile, DWORD dwFlags)
{
  if (lpFileName == nullptr) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "HookLoadLibraryExA", "Called with nullptr -> Abort");
    return nullptr;
  }

  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookLoadLibraryExA", "Load library " + std::string(lpFileName));

  if (dwFlags == 0) {
    return HookLoadLibraryA(lpFileName);
  }

  static const auto trampoline = find_hook_trampoline_unchecked(&HookLoadLibraryExA);

  return trampoline(lpFileName, hFile, dwFlags);
}
HMODULE WINAPI HookLoadLibraryW(LPCWSTR lpFileName)
{
  if (lpFileName == nullptr) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "HookLoadLibraryW", "Called with nullptr -> Abort");
    return nullptr;
  }

  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookLoadLibraryW", L"Load library " + std::wstring(lpFileName));

  static const auto trampoline = find_hook_trampoline_unchecked(&HookLoadLibraryW);

  const HMODULE handle = trampoline(lpFileName);

  if (handle == nullptr || handle == get_current_module()) {
    return handle;
  }

  const critical_section::lock lock(s_cs);

  HookAllModules();

  const auto remove = std::remove_if(s_delayed_hook_paths.begin(), s_delayed_hook_paths.end(),
                                     [lpFileName](const std::wstring &path) {
                                       HMODULE delayed_handle = nullptr;
                                       GetModuleHandleExW(0, path.c_str(), &delayed_handle);

                                       if (delayed_handle == nullptr) {
                                         return false;
                                       }

                                       s_delayed_hook_modules.push_back(delayed_handle);

                                       return install_hook(delayed_handle, get_current_module(),
                                                           hook_method::function_hook);
                                     });

  s_delayed_hook_paths.erase(remove, s_delayed_hook_paths.end());

  return handle;
}
HMODULE WINAPI HookLoadLibraryExW(LPCWSTR lpFileName, HANDLE hFile, DWORD dwFlags)
{
  if (lpFileName == nullptr) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "HookLoadLibraryExW", "Called with nullptr -> Abort");
    return nullptr;
  }

  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookLoadLibraryExW", L"Load library " + std::wstring(lpFileName));

  if (dwFlags == 0) {
    return HookLoadLibraryW(lpFileName);
  }

  static const auto trampoline = find_hook_trampoline_unchecked(&HookLoadLibraryExW);

  return trampoline(lpFileName, hFile, dwFlags);
}

void Inject(DWORD processID)
{
  if (processID) {
    auto module = GetModuleHandle(g_overlayLibName.c_str());
    if (!module) {
      g_messageLog.Log(MessageLog::LOG_ERROR, "Hook Manager", "Inject - GetModuleHandle failed",
                       GetLastError());
      return;
    }
    s_delayed_hook_modules.push_back(module);

    InjectDLL(processID, g_fileDirectory.GetDirectoryW(FileDirectory::DIR_BIN));
  }
}

std::wstring GetProcessName(LPCTSTR lpApplicationName, LPTSTR lpCommandLine)
{
  std::wstring path;
  size_t processStart = 0;
  size_t processSize = 0;
  if (lpApplicationName) {
    path = std::wstring(lpApplicationName);
    processStart = path.find_last_of('\\') + 1;
    processSize = path.size() - processStart;
  }
  else if (lpCommandLine) {
    path = std::wstring(lpCommandLine);
    const auto exePathEnd = path.find_first_of(L'"', 1);
    processStart = path.find_last_of('\\', exePathEnd) + 1;
    processSize = exePathEnd - processStart;
  }

  return path.substr(processStart, processSize);
}

bool IsDLLInjectionProcess(const std::wstring &processName)
{
  return (processName.compare(0, 11, L"DLLInjector") == 0);
}

void EnableVulkan(VK_Environment& vkEnv, const std::wstring& processName)
{
  const auto blacklisted = g_blackList.Contains(processName);
  if (!blacklisted) {
    vkEnv.SetVKEnvironment(g_dllDirectory);
  }
}

BOOL WINAPI HookCreateProcessA(_In_opt_ LPCTSTR lpApplicationName, _Inout_opt_ LPTSTR lpCommandLine,
                               _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                               _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                               _In_ BOOL bInheritHandles, _In_ DWORD dwCreationFlags,
                               _In_opt_ LPVOID lpEnvironment, _In_opt_ LPCTSTR lpCurrentDirectory,
                               _In_ LPSTARTUPINFO lpStartupInfo,
                               _Out_ LPPROCESS_INFORMATION lpProcessInformation)
{
  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookCreateProcessA", "Entered");

  static const auto trampoline = find_hook_trampoline_unchecked(&HookCreateProcessA);

  const auto processName = GetProcessName(lpApplicationName, lpCommandLine);
  if (IsDLLInjectionProcess(processName)) {
    return trampoline(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
                      bInheritHandles, dwCreationFlags, NULL, lpCurrentDirectory, lpStartupInfo,
                      lpProcessInformation);
  }

  HookAllModules();

  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookCreateProcessA", "Init Vulkan");
  VK_Environment vkEnv;
  EnableVulkan(vkEnv, processName);
  const auto result = trampoline(lpApplicationName, lpCommandLine, lpProcessAttributes,
                                 lpThreadAttributes, bInheritHandles, dwCreationFlags, NULL,
                                 lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
  vkEnv.ResetVKEnvironment();
  Inject(lpProcessInformation->dwProcessId);

  return result;
}

BOOL WINAPI HookCreateProcessW(_In_opt_ LPCTSTR lpApplicationName, _Inout_opt_ LPTSTR lpCommandLine,
                               _In_opt_ LPSECURITY_ATTRIBUTES lpProcessAttributes,
                               _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
                               _In_ BOOL bInheritHandles, _In_ DWORD dwCreationFlags,
                               _In_opt_ LPVOID lpEnvironment, _In_opt_ LPCTSTR lpCurrentDirectory,
                               _In_ LPSTARTUPINFO lpStartupInfo,
                               _Out_ LPPROCESS_INFORMATION lpProcessInformation)
{
  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookCreateProcessW", "Entered");
  static const auto trampoline = find_hook_trampoline_unchecked(&HookCreateProcessW);

  const auto processName = GetProcessName(lpApplicationName, lpCommandLine);
  if (IsDLLInjectionProcess(processName)) {
    return trampoline(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
      bInheritHandles, dwCreationFlags, NULL, lpCurrentDirectory, lpStartupInfo,
      lpProcessInformation);
  }

  HookAllModules();

  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookCreateProcessW", "Init Vulkan");
  VK_Environment vkEnv;
  EnableVulkan(vkEnv, processName);
  const auto result = trampoline(lpApplicationName, lpCommandLine, lpProcessAttributes,
    lpThreadAttributes, bInheritHandles, dwCreationFlags, NULL,
    lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
  vkEnv.ResetVKEnvironment();
  Inject(lpProcessInformation->dwProcessId);

  return result;
}
}

bool installCreateProcessHook()
{
  int numHooksInstalled = 0;
  g_messageLog.Log(MessageLog::LOG_INFO, "Hook Manager", "Install hooks for CreateProcess");
  if (!GameOverlay::install_hook(reinterpret_cast<hook::address>(&::CreateProcessA),
                                 reinterpret_cast<hook::address>(&HookCreateProcessA))) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "Hook Manager",
                     "install_hook failed for CreateProcessA");
  }
  else {
    g_messageLog.Log(MessageLog::LOG_INFO, "Hook Manager",
      "Successfully installed hook for CreateProcessA");
    numHooksInstalled++;
  }

  if (!GameOverlay::install_hook(reinterpret_cast<hook::address>(&::CreateProcessW),
                                 reinterpret_cast<hook::address>(&HookCreateProcessW))) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "Hook Manager",
                     " install_hook failed for CreateProcessW");
  }
  else {
    g_messageLog.Log(MessageLog::LOG_INFO, "Hook Manager",
      "Successfully installed hook for CreateProcessW");
    numHooksInstalled++;
  }

  return numHooksInstalled > 0;
}

void HookAllModules()
{
  // https://github.com/baldurk/renderdoc/blob/master/renderdoc/os/win32/win32_hook.cpp
  // Retrieve all modules in IAT
  // Install function hook for all of them and replace them with our module handle
  g_messageLog.Log(MessageLog::LOG_DEBUG, "HookAllModules", "Entered function");

  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  // Restrict the number of retries
  for (int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());

    if (hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();
      g_messageLog.Log(MessageLog::LOG_DEBUG, "HookAllModules", "Create snapshot " + std::to_string(i) + " exited with error", err);
      
      if (err == ERROR_BAD_LENGTH)
        continue; // Retry
    }

    // Valid handle or an error other than ERROR_BAD_LENGTH occured.
    break;
  }

  if (hModuleSnap == INVALID_HANDLE_VALUE)
  {
    g_messageLog.Log(MessageLog::LOG_DEBUG, "HookAllModules", "Could not create snapshot");
    return;
  }

#ifdef UNICODE
#undef MODULEENTRY32
#undef Module32First
#undef Module32Next
#endif

  MODULEENTRY32 me32 = {};
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);
  if (success == FALSE)
  {
    g_messageLog.Log(MessageLog::LOG_DEBUG, "HookAllModules", "Could not load first module", GetLastError());
    CloseHandle(hModuleSnap);
    return;
  }

  uintptr_t ret = 0;

  // Set of modules to ignore, if the module path contains any of those in the set, we skip it.
  // overlay is a catch for our own module, our vulkan layer as well as the Steam overlay (and possible other overlays)
  std::vector<std::string> filter = { "kernel32.dll", "powrprof.dll", "gdi32.dll",
    "opengl32.dll", "nvoglv32.dll", "nvoglv64.dll", "nvcuda.dll", "cudart", "msvcr",
    "msvcp", "nv-vk", "amdvlk", "igvk", "nvopencl", "nvapi", "fraps", "vulkan-1.dll", "overlay" }; 

  for (auto& entry : filter) {
    std::transform(entry.begin(), entry.end(), entry.begin(), ::tolower);
  }
  
  do
  {
    auto szExePathString = std::string(me32.szExePath);
    std::transform(szExePathString.begin(), szExePathString.end(), szExePathString.begin(), ::tolower);

    bool skip = false;
    for (auto& entry : filter) {
      if (szExePathString.find(entry) != std::string::npos) {
        // If szExePathString contains entry we skip it
        skip = true;
        break;
      }
    }

    if (skip) {
      g_messageLog.Log(MessageLog::LOG_DEBUG, "HookAllModules", "Skip module: " + szExePathString);
      continue;
    }

    g_messageLog.Log(MessageLog::LOG_DEBUG, "HookAllModules", "Found module: " + szExePathString);
    s_delayed_hook_modules.push_back(me32.hModule);
    install_hook(me32.hModule, get_current_module(), hook_method::function_hook);
  } while (ret == 0 && Module32Next(hModuleSnap, &me32));

  CloseHandle(hModuleSnap);
}

bool install_hook(hook::address target, hook::address replacement)
{
  assert(target != nullptr);
  assert(replacement != nullptr);

  if (target == replacement) {
    g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "Target module equals replacement.");
    return false;
  }

  const hook hook = find_hook(replacement);

  if (hook.installed()) {
    bool success = target == hook.target;
    if (success) {
      g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "Hook already installed");
    }
    else {
      g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "There exists another module with the same name but a different address");
    }
    return success;
  }

  g_messageLog.Log(MessageLog::LOG_DEBUG, "install_hook", "Try install");
  return install_hook(target, replacement, hook_method::function_hook);
}
bool install_hook(hook::address vtable[], unsigned int offset, hook::address replacement)
{
  assert(vtable != nullptr);
  assert(replacement != nullptr);

  DWORD protection = PAGE_READONLY;
  hook::address &target = vtable[offset];

  if (VirtualProtect(&target, sizeof(hook::address), protection, &protection)) {
    const critical_section::lock lock(s_cs);

    const auto insert = s_vtable_addresses.emplace(target, &target);

    VirtualProtect(&target, sizeof(hook::address), protection, &protection);

    if (insert.second) {
      if (target != replacement && install_hook(target, replacement, hook_method::vtable_hook)) {
        return true;
      }

      s_vtable_addresses.erase(insert.first);
    }
    else {
      return insert.first->first == target;
    }
  }

  return false;
}
void uninstall_hook()
{
  const critical_section::lock lock(s_cs);

  // Uninstall hooks
  for (auto &hook : s_hooks) {
    uninstall_hook(hook.first, hook.second);
  }

  s_hooks.clear();

  // Free loaded modules
  for (HMODULE module : s_delayed_hook_modules) {
    FreeLibrary(module);
  }

  s_delayed_hook_modules.clear();
}

bool register_module(const std::wstring &target_path)  // Not thread-safe
{
  int numModulesRegistered = 0;
  g_messageLog.Log(MessageLog::LOG_INFO, "register_module", L"Register module for " + target_path);
  if (!install_hook(reinterpret_cast<hook::address>(&::LoadLibraryA),
                    reinterpret_cast<hook::address>(&HookLoadLibraryA))) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "register_module", 
      "Failed to install hook for LoadLibraryA");
  }
  else {
    g_messageLog.Log(MessageLog::LOG_INFO, "register_module", 
      "Successfully installed hook for LoadLibraryA");
    numModulesRegistered++;
  }
  if (!install_hook(reinterpret_cast<hook::address>(&::LoadLibraryExA),
                    reinterpret_cast<hook::address>(&HookLoadLibraryExA))) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "register_module",
                     "Failed to install hook for LoadLibraryExA");
  }
  else {
    g_messageLog.Log(MessageLog::LOG_INFO, "register_module", 
      "Successfully installed hook for LoadLibraryExA");
    numModulesRegistered++;
  }
  if (!install_hook(reinterpret_cast<hook::address>(&::LoadLibraryW),
                    reinterpret_cast<hook::address>(&HookLoadLibraryW))) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "register_module",
      "Failed to install hook for LoadLibraryW");
  }
  else {
    g_messageLog.Log(MessageLog::LOG_INFO, "register_module",
      "Successfully installed hook for LoadLibraryW");
    numModulesRegistered++;
  }
  if (!install_hook(reinterpret_cast<hook::address>(&::LoadLibraryExW),
                    reinterpret_cast<hook::address>(&HookLoadLibraryExW))) {
    g_messageLog.Log(MessageLog::LOG_ERROR, "register_module",
      "Failed to install hook for LoadLibraryExW");
  }
  else {
    g_messageLog.Log(MessageLog::LOG_INFO, "register_module",
      "Successfully installed hook for LoadLibraryExW");
    numModulesRegistered++;
  }

  HMODULE handle = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, target_path.c_str(), &handle);

  if (handle != nullptr) {
    s_delayed_hook_modules.push_back(handle);

    if (!install_hook(handle, get_current_module(), hook_method::function_hook)) {
      g_messageLog.Log(MessageLog::LOG_ERROR, "register_module",
                       L"Failed to install function hook for " + target_path);
    }
    else {
      g_messageLog.Log(MessageLog::LOG_INFO, "register_module",
        L"Successfully installed function hook for " + target_path);
    }
  }
  else {
    s_delayed_hook_paths.push_back(target_path);
  }

  return numModulesRegistered > 0;
}

hook::address find_hook_trampoline(hook::address replacement)
{
  const hook hook = find_hook(replacement);

  if (!hook.valid()) {
    return nullptr;
  }

  return hook.call();
}
}
