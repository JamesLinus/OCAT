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

#include <Windows.h>
#include <appmodel.h>
#include "Config\BlackList.h"
#include "Recording\Capturing.h"
#include "Utility\Constants.h"
#include "Utility\FileDirectory.h"
#include "Logging\MessageLog.h"
#include "Utility\ProcessHelper.h"
#include "hook_manager.hpp"
#include "Overlay\OverlayMessage.h"

#include <Psapi.h>
#include <assert.h>

#pragma data_seg("SHARED")
HWND sharedFrontendWindow = NULL;
#pragma data_seg()
#pragma comment(linker, "/section:SHARED,RWS")

HMODULE g_module_handle = NULL;
HHOOK g_hook = NULL;
std::wstring g_dllDirectory;
BlackList g_blackList;
bool g_uwpApp = false;

extern "C" __declspec(dllexport) LRESULT CALLBACK
    GlobalHookProc(int code, WPARAM wParam, LPARAM lParam)
{
  return CallNextHookEx(NULL, code, wParam, lParam);
}

typedef LONG (WINAPI *PGetPackageFamilyName) (HANDLE, UINT32*, PWSTR);

bool UWPApp()
{
  auto handle = GetCurrentProcess();
  if (handle) {
    UINT32 length = 0;

    PGetPackageFamilyName packageFamilyName = reinterpret_cast<PGetPackageFamilyName>(GetProcAddress(GetModuleHandle(L"kernel32.dll"), "GetPackageFamilyName"));
    if (packageFamilyName == NULL)
    {
      return false;
    }
    else
    {
      auto rc = packageFamilyName(handle, &length, NULL);
      if (rc == APPMODEL_ERROR_NO_PACKAGE)
        return false;
      else
        return true;
    }
  }
  return false;
}

void InitLogging()
{
  // dont start logging if a uwp app is injected
  g_uwpApp = UWPApp();
  if (!g_uwpApp) {
    g_dllDirectory = g_fileDirectory.GetDirectoryW(FileDirectory::DIR_BIN);
    const auto logDir = g_fileDirectory.GetDirectoryW(FileDirectory::DIR_LOG);
#if _WIN64
    g_messageLog.Start(logDir + L"GameOverlayLog", L"GameOverlay64");
#else
    g_messageLog.Start(logDir + L"GameOverlayLog", L"GameOverlay32");
#endif
  }
}

void SetFrontendWindow()
{
  sharedFrontendWindow = FindWindow(NULL, L"OCAT");
  if (!sharedFrontendWindow)
  {
    g_messageLog.Log(MessageLog::LOG_ERROR, "main", "Could not find OCAT window handle", GetLastError());
  }
}

void SendDllStateMessage(OverlayMessageType messageType)
{
  if (sharedFrontendWindow == NULL)
  {
    SetFrontendWindow();
  }

  OverlayMessage::PostFrontendMessage(sharedFrontendWindow, messageType, GetCurrentProcessId());
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID lpReserved)
{
  UNREFERENCED_PARAMETER(lpReserved);
  switch (fdwReason) {
    case DLL_PROCESS_ATTACH: {
      g_module_handle = hModule;
      DisableThreadLibraryCalls(hModule);

      // Register modules for hooking
      wchar_t system_path_buffer[MAX_PATH];
      GetSystemDirectoryW(system_path_buffer, MAX_PATH);
      const std::wstring system_path(system_path_buffer);

      g_blackList.Load();
      const std::wstring processName = GetProcessNameFromHandle(GetCurrentProcess());
      if (!processName.empty()) {
        if (!g_blackList.Contains(processName)) {
          
          g_messageLog.Log(MessageLog::LOG_INFO, "GameOverlay", "Install process hooks for Vulkan");
          InitLogging();
          SendDllStateMessage(OVERLAY_AttachDll);
          if (!GameOverlay::installCreateProcessHook()) {
            g_messageLog.Log(MessageLog::LOG_ERROR, "GameOverlay", "Failed to install process hooks for Vulkan");
          }

          // Register modules for hooking
          g_messageLog.Log(MessageLog::LOG_INFO, "GameOverlay", "Register module for D3D");
          wchar_t system_path_buffer[MAX_PATH];
          GetSystemDirectoryW(system_path_buffer, MAX_PATH);
          const std::wstring system_path(system_path_buffer);
          if (!GameOverlay::register_module(system_path + L"\\dxgi.dll")) {
            g_messageLog.Log(MessageLog::LOG_ERROR, "GameOverlay", "Failed to register module for D3D");
          } 
        }
        else {
          g_messageLog.Log(MessageLog::LOG_INFO, "GameOverlay", L"Process '" + processName + L"' is on blacklist -> Ignore");
        }
      }
      break;
    }
    case DLL_PROCESS_DETACH: {
      if (lpReserved == NULL) {
        g_messageLog.Log(MessageLog::LOG_INFO, "GameOverlay", L"Detach because DLL load failed or FreeLibrary called");
      }
      else {
        g_messageLog.Log(MessageLog::LOG_INFO, "GameOverlay", L"Detach because process is terminating");
      }

      // Uninstall and clean up all hooks before unloading
      SendDllStateMessage(OVERLAY_DetachDll);
      GameOverlay::uninstall_hook();
      break;
    }
  }

  return TRUE;
}
