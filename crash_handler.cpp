#include "buildsettings.h"

#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <winternl.h>
#include <DbgHelp.h>
#include <commctrl.h>
#include <winhttp.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>

#ifndef defer
struct defer_dummy
{
};
template <class F>
struct deferrer
{
  F f;
  ~deferrer() { f(); }
};
template <class F>
deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(LINE) zz_defer##LINE
#define DEFER(LINE) DEFER_(LINE)
#define defer auto DEFER(__LINE__) = defer_dummy{} *[&]()
#endif // defer

#include <Shlobj.h>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Oleaut32.lib")
#pragma comment(lib, "Shell32.lib")
bool view_file_in_system_file_browser(const wchar_t *wfullpath)
{
  bool res = false;
  if (wfullpath)
  {
    { // Good version of the functionality
      const wchar_t *wpath = wfullpath;
      if (!wcsncmp(wfullpath, L"\\\\?\\", 4))
      {
        wpath += 4;
      }
      if (wpath)
      {
        HRESULT hr = CoInitialize(nullptr);
        if (hr == S_OK || hr == S_FALSE)
        {
          PIDLIST_ABSOLUTE pidl;
          SFGAOF flags;
          if (SHParseDisplayName(wpath, nullptr, &pidl, 0, &flags) == S_OK)
          {
            if (SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0) == S_OK)
            {
              res = true;
            }
            CoTaskMemFree(pidl);
          }
          CoUninitialize();
        }
      }
    }
    if (!res)
    { // Worse Fallback if good version fails
      WCHAR wcmd[65536];
      wnsprintfW(wcmd, ARRAYSIZE(wcmd), L"explorer /select,\"%s\"", wfullpath);

      if (_wsystem(wcmd) >= 0)
      {
        res = true;
      }
    }
  }
  return res;
}


#define DISCORD_WEBHOOK_ENDPOINT "/api/webhooks/922366591751053342/pR59Za5xL1HcvGSrxJ7hb1UCa85zfdtZyDet15I_CZgrY9RkAq73uAQ2Obo1Zi9QBvvX"

#define ErrBox(msg, flags) MessageBoxW(nullptr, L"" msg, L"Fatal Error", flags | MB_SYSTEMMODAL | MB_SETFOREGROUND)
#define fatal_init_error(s) ErrBox("The game had a fatal error and must close.\n\"" s "\"\nPlease report this to team@happenlance.com or the Happenlance Discord.", MB_OK | MB_ICONERROR)

extern "C" {
// TODO: Internationalization for error messages.
#pragma comment(lib, "DbgHelp.lib")
#pragma comment(lib, "ComCtl32.lib")
#pragma comment(lib, "WinHTTP.lib")
#pragma comment(lib, "Shlwapi.lib")
void do_crash_handler()
{

  // Get the command line
  int argc = 0;
  wchar_t *cmd = GetCommandLineW();
  if (!cmd || !cmd[0])
  {
    return; // Error: just run the app without a crash handler.
  }
  wchar_t **wargv = CommandLineToArgvW(cmd, &argc); // Passing nullptr here crashes!
  if (!wargv || !wargv[0])
  {
    return; // Error: just run the app without a crash handler.
  }

  // Parse the command line for -no-crash-handler
  bool crashHandler = true;
  for (int i = 0; i < argc; ++i)
  {
    if (!wcscmp(wargv[i], L"-no-crash-handler"))
    {
      crashHandler = false;
    }
  }
  if (!crashHandler)
  { // We already *are* the subprocess - continue with the main program!
    return;
  }

  // Concatenate -no-crash-handler onto the command line for the subprocess
  int cmdLen = 0;
  while (cmd[cmdLen])
  { // could use wcslen() here, but Clang ASan's wcslen() can be bugged sometimes
    cmdLen++;
  }
  const wchar_t *append = L" -no-crash-handler";
  int appendLen = 0;
  while (append[appendLen])
  {
    appendLen++;
  }
  wchar_t *cmdNew = (wchar_t *)calloc(cmdLen + appendLen + 1, sizeof(wchar_t)); // @Leak
  if (!cmdNew)
  {
    return; // Error: just run the app without a crash handler.
  }
  memcpy(cmdNew, cmd, cmdLen * sizeof(wchar_t));
  memcpy(cmdNew + cmdLen, append, appendLen * sizeof(wchar_t));

// Crash handler loop: run the program until it succeeds or the user chooses not to restart it
restart:;

  // Parameters for starting the subprocess
  STARTUPINFOW siw = {};
  siw.cb = sizeof(siw);
  siw.dwFlags = STARTF_USESTDHANDLES;
  siw.hStdInput = GetStdHandle(STD_INPUT_HANDLE); // @Leak: CloseHandle()
  siw.hStdOutput = GetStdHandle(STD_ERROR_HANDLE);
  siw.hStdError = GetStdHandle(STD_OUTPUT_HANDLE);
  PROCESS_INFORMATION pi = {}; // @Leak: CloseHandle()

  // Launch suspended, then read-modify-write the PEB (see below), then resume -p 2022-03-04
  if (!CreateProcessW(nullptr, cmdNew, nullptr, nullptr, true,
                      CREATE_SUSPENDED | DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &siw, &pi))
  {
    // If we couldn't create a subprocess, then just run the program without a crash handler.
    // That's not great, but it's presumably better than stopping the user from running at all!
    return;
  }

  // NOTE: SteamAPI_Init() takes WAY longer On My Machine(tm) when a debugger is present.
  //       (The DLL file steam_api64.dll does indeed call IsDebuggerPresent() sometimes.)
  //       It's clear that Steam does extra niceness for us when debugging, but we DO NOT
  //       want this to destroy our load times; I measure 3.5x slowdown (0.6s -> 2.1s).
  //       The only way I know to trick the child process into thinking it is free of a
  //       debugger is to clear the BeingDebugged byte in the Process Environment Block.
  //       If we are unable to perform this advanced maneuver, we will gracefully step back
  //       and allow Steam to ruin our loading times. -p 2022-03-04
  auto persuade_process_no_debugger_is_present = [](HANDLE hProcess)
  {
    // Load NTDLL
    HMODULE ntdll = LoadLibraryA("ntdll.dll");
    if (!ntdll)
      return;

    // Get NtQueryInformationProcess function
    auto NtQueryInformationProcess =
        (/*__kernel_entry*/ NTSTATUS(*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG))
            GetProcAddress(ntdll, "NtQueryInformationProcess");
    if (!NtQueryInformationProcess)
      return;

    // Query process information to find the PEB address
    PROCESS_BASIC_INFORMATION pbi = {};
    DWORD queryBytesRead = 0;
    if (NtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), &queryBytesRead) != 0 || queryBytesRead != sizeof(pbi))
      return;

    // Read the PEB of the child process
    PEB peb = {};
    SIZE_T processBytesRead = NULL;
    if (!ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), &processBytesRead) || processBytesRead != sizeof(peb))
      return;
    printf("Child process's peb.BeingDebugged is %d, setting to 0...\n", peb.BeingDebugged);

    // Gaslight the child into believing we are not watching
    peb.BeingDebugged = 0;

    // Write back the modified PEB
    SIZE_T processBytesWritten = NULL;
    if (!WriteProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), &processBytesWritten) || processBytesWritten != sizeof(peb))
      return;
  };
  persuade_process_no_debugger_is_present(pi.hProcess);

  // Helper function to destroy the subprocess
  auto exit_child = [&]
  {
    TerminateProcess(pi.hProcess, 1);                  // Terminate before detaching, so you don't see Windows Error Reporting.
    DebugActiveProcessStop(GetProcessId(pi.hProcess)); // Detach
    WaitForSingleObject(pi.hProcess, 2000);            // Wait for child to die, but not forever.
  };

  // Kick off the subprocess
  if (ResumeThread(pi.hThread) != 1)
  {
    exit_child();
    fatal_init_error("Could not start main game thread");
    ExitProcess(1); // @Note: could potentially "return;" here instead if you wanted.
  }

  // Debugger loop: catch (and ignore) all debug events until the program exits or hits a last-chance exception
  char filename[65536] = {0};
  WCHAR wfilename[65536] = {0};
  HANDLE file = nullptr;
  for (;;)
  {

    // Get debug event
    DEBUG_EVENT de = {};
    if (!WaitForDebugEvent(&de, INFINITE))
    {
      exit_child();
      fatal_init_error("Waiting for debug event failed");
      ExitProcess(1);
    }

    // If the process exited, nag about failure, or silently exit on success
    if (de.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT && de.dwProcessId == pi.dwProcessId)
    {

      // If the process exited unsuccessfully, prompt to restart it
      // @Todo: in these cases, no dump can be made, so upload just the stdout log and profiling trace
      if (de.u.ExitThread.dwExitCode != 0)
      {

        // Terminate & detach just to be safe
        exit_child();

        // Prompt to restart
        MessageBeep(MB_ICONINFORMATION); // MB_ICONQUESTION makes no sound
        if (MessageBoxW(nullptr,
                        L"The game had a fatal error and must close.\n"
                        "Unfortunately, a crash report could not be generated. Sorry!\n"
                        "Please report this to team@happenlance.com or the Happenlance Discord.\n"
                        "Restart the game?\n",
                        L"Fatal Error",
                        MB_YESNO | MB_ICONQUESTION | MB_SYSTEMMODAL | MB_SETFOREGROUND) == IDYES)
        {
          goto restart;
        }
      }

      // Bubble up the failure code - this is where successful program runs will end up!
      ExitProcess(de.u.ExitThread.dwExitCode);
    }

    // If the process had some other debug stuff, we don't care.
    if (de.dwDebugEventCode != EXCEPTION_DEBUG_EVENT)
    {
      ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_CONTINUE);
      continue;
    }

    // Skip first-chance exceptions or exceptions for processes we don't care about (shouldn't ever happen).
    if (de.u.Exception.dwFirstChance || de.dwProcessId != GetProcessId(pi.hProcess))
    {
      ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_EXCEPTION_NOT_HANDLED);
      continue;
    }

// By here, we have hit a real, last-chance exception. This is a crash we should generate a dump for.
#define crash_report_failure(str)                                               \
  ErrBox(                                                                       \
      "The game had a fatal error and must close.\n"                            \
      "A crash report could not be produced:\n\"" str "\"\n"                    \
      "Please report this to team@happenlance.com or the Happenlance Discord.", \
      MB_OK | MB_ICONERROR);

    // Create crash dump filename
    snprintf(filename, sizeof(filename), "./CrashDump_%d.dmp", (int)time(NULL));

    // Convert filename to UTF-16
    MultiByteToWideChar(CP_UTF8, 0, filename, -1, wfilename, ARRAYSIZE(wfilename));

    // Create crash dump file
    file = CreateFileW(wfilename, GENERIC_WRITE | GENERIC_READ, 0, nullptr,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
      exit_child();
      crash_report_failure("The crash dump file could not be created. Sorry!");
      ExitProcess(1);
    }

    // Generate exception pointers out of excepting thread context
    CONTEXT c = {};
    if (HANDLE thread = OpenThread(THREAD_ALL_ACCESS, true, de.dwThreadId))
    {
      c.ContextFlags = CONTEXT_ALL;
      GetThreadContext(thread, &c);
      CloseHandle(thread);
    }
    EXCEPTION_POINTERS ep = {};
    ep.ExceptionRecord = &de.u.Exception.ExceptionRecord;
    ep.ContextRecord = &c;
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = de.dwThreadId;
    mei.ExceptionPointers = &ep;
    mei.ClientPointers = false;

    // You could add some others here, but these should be good.
    int flags = MiniDumpNormal | MiniDumpWithHandleData | MiniDumpScanMemory | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo | MiniDumpIgnoreInaccessibleMemory;

    // Write minidump
    if (!MiniDumpWriteDump(pi.hProcess, GetProcessId(pi.hProcess), file,
                           (MINIDUMP_TYPE)flags, &mei, nullptr, nullptr))
    {
      exit_child();
      crash_report_failure("The crash dump could not be written. Sorry!");
      ExitProcess(1);
    }

    // @Todo: ZIP compress the crash dump files here, with graceful fallback to uncompressed dumps.

    // Cleanup: Destroy subprocess now that we have a dump.
    // Note that we want to do this before doing any blocking interface dialogs,
    // because otherwise you would leave an arbitrarily broken program lying around
    // longer than you need to.
    exit_child();
    break;
  }

  // Prompt to upload crash dump
  int res = 0;
  bool uploaded = false;
  if (!(res = ErrBox(
            "The game had a fatal error and must close.\n"
            "Send anonymous crash report?\n"
            "This will go directly to the developers on Discord,\n"
            "and help fix the problem.",
            MB_YESNO | MB_ICONERROR)))
    ExitProcess(1);

  // Upload crash dump
  if (res == IDYES)
  {

    // Setup window class for progress window
    WNDCLASSEXW wcex = {sizeof(wcex)};
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpszClassName = L"bar";
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wcex.hCursor = LoadCursor(GetModuleHandleA(nullptr), IDC_ARROW);
    wcex.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT
    {
      return m == WM_QUIT || m == WM_CLOSE || m == WM_DESTROY ? 0 : DefWindowProcW(h, m, w, l);
    };
    wcex.hInstance = GetModuleHandleA(nullptr);
    if (!RegisterClassExW(&wcex))
    {
      ExitProcess(1);
    }
    HWND hWnd = nullptr;
    HWND ctrl = nullptr;

    // Initialize common controls for progress bar
    INITCOMMONCONTROLSEX iccex = {sizeof(iccex)};
    iccex.dwICC = ICC_PROGRESS_CLASS;
    if (InitCommonControlsEx(&iccex))
    {

      // Create progress window and progress bar child-window
      hWnd = CreateWindowExW(0, wcex.lpszClassName, L"Uploading...",
                             WS_SYSMENU | WS_CAPTION | WS_VISIBLE, CW_USEDEFAULT, SW_SHOW,
                             320, 80, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
      ctrl = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                             WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 10, 10,
                             280, 20, hWnd, (HMENU)12345, GetModuleHandleA(nullptr), nullptr);
    }
    else
    {
      ExitProcess(1);
    }

    // Infinite loop: Attempt to upload the crash dump until the user cancels or it succeeds
    do
    {

      // Position the progress window to the centre of the screen
      RECT r;
      GetWindowRect(hWnd, &r);
      int ww = r.right - r.left, wh = r.bottom - r.top;
      int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
      SetWindowPos(hWnd, HWND_TOP, (sw - ww) / 2, (sh - wh) / 2, 0, 0, SWP_NOSIZE);

      // Helper function to set the loading bar to a certain position.
      auto update_loading_bar = [&](float amt)
      {
        if (hWnd && ctrl)
        {
          SendMessageW(ctrl, PBM_SETPOS, (WPARAM)(amt * 100), 0);
          ShowWindow(hWnd, SW_SHOW);
          UpdateWindow(hWnd);
          MSG msg = {};
          while (PeekMessageW(&msg, nullptr, 0, 0, 1) > 0)
          {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
          }
        }
      };

      auto try_upload = [&]() -> bool
      {
        float x = 0;
        update_loading_bar(x);

        // Build MIME multipart-form payload
        static char body[1 << 23]; // ouch that's a big static buffer!!!
        const char *bodyPrefix =
            "--19024605111143684786787635207\r\n"
            "Content-Disposition: form-data; name=\"payload_json\"\r\n\r\n{\"content\":\""
            "Astris v"
#define STRINGIZE_(N) #N
#define STRINGIZE(N) STRINGIZE_(N)
            STRINGIZE(GIT_RELEASE_TAG) " Crash Report"
            "\"}\r\n--19024605111143684786787635207\r\n"
            "Content-Disposition: form-data; name=\"files[0]\"; filename=\"";
        const char *bodyInfix = "\"\r\n"
                                "Content-Type: application/octet-stream\r\n"
                                "\r\n";
        const char *bodyPostfix = "\r\n--19024605111143684786787635207--\r\n";

        // Printf the prefix, filename, infix
        int headerLen = snprintf(body, sizeof(body), "%s%s%s", bodyPrefix, filename, bodyInfix);
        if (headerLen != strlen(bodyPrefix) + strlen(filename) + strlen(bodyInfix))
          return false;
        update_loading_bar(x += 0.1f);

        // Get crash dump file size
        LARGE_INTEGER fileSizeInt = {};
        GetFileSizeEx(file, &fileSizeInt);
        uint64_t fileSize = fileSizeInt.QuadPart;
        if (fileSize >= 8000000)
          return false; // discord limit
        int bodyLen = headerLen + (int)fileSize + (int)(sizeof(bodyPostfix)-1);
        if (bodyLen >= sizeof(body))
          return false; // buffer overflow
        update_loading_bar(x += 0.1f);

        // Seek file to start
        if (SetFilePointer(file, 0, nullptr, FILE_BEGIN) != 0)
          return false;

        // Copy entire file into the space after the body infix
        DWORD bytesRead = 0;
        if (!ReadFile(file, body + headerLen, (DWORD)fileSize, &bytesRead, nullptr))
          return false;
        if (bytesRead != fileSize)
          return false;
        update_loading_bar(x += 0.1f);

        // Print the body postfix after the data file (overflow already checked)
        strncpy(body + headerLen + fileSize, bodyPostfix, sizeof(body) - headerLen - fileSize);
        update_loading_bar(x += 0.1f);

        // Windows HTTPS stuff from here on out...
        HINTERNET hSession = WinHttpOpen(L"Discord Crashdump Webhook",
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession)
          return false;
        defer { WinHttpCloseHandle(hSession); };
        update_loading_bar(x += 0.1f);

        // Connect to domain
        HINTERNET hConnect = WinHttpConnect(hSession, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect)
          return false;
        defer { WinHttpCloseHandle(hConnect); };
        update_loading_bar(x += 0.1f);

        // Begin POST request to the discord webhook endpoint
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST",
                                                L"" DISCORD_WEBHOOK_ENDPOINT,
                                                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest)
          return false;
        defer { WinHttpCloseHandle(hRequest); };
        update_loading_bar(x += 0.1f);

        // Send request once - don't handle auth challenge, credentials, reauth, redirects
        const wchar_t ContentType[] = L"Content-Type: multipart/form-data; boundary=19024605111143684786787635207";
        if (!WinHttpSendRequest(hRequest, ContentType, ARRAYSIZE(ContentType),
                                body, bodyLen, bodyLen, 0))
          return false;
        update_loading_bar(x += 0.1f);

        // Wait for response
        if (!WinHttpReceiveResponse(hRequest, nullptr))
          return false;
        update_loading_bar(x += 0.1f);

        // Pull headers from response
        DWORD dwStatusCode, dwSize = sizeof(dwStatusCode);
        if (!WinHttpQueryHeaders(hRequest,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 nullptr, &dwStatusCode, &dwSize, nullptr))
          return false;
        if (dwStatusCode != 200)
          return false;
        update_loading_bar(x += 0.1f);

        return true;
      };
      res = 0;
      uploaded = try_upload();
      if (!uploaded)
      {
        if (!(res = MessageBoxW(hWnd, L"Sending failed. Retry?", L"Fatal Error", MB_RETRYCANCEL | MB_ICONWARNING | MB_SYSTEMMODAL)))
          ExitProcess(1);
      }
    } while (res == IDRETRY);

    // Cleanup
    if (hWnd)
    {
      DestroyWindow(hWnd);
    }
    UnregisterClassW(wcex.lpszClassName, GetModuleHandleA(nullptr));
  }

  // Cleanup
  CloseHandle(file);

  // Prompt to restart
  MessageBeep(MB_ICONINFORMATION); // MB_ICONQUESTION makes no sound
  if (MessageBoxW(nullptr, uploaded ? L"Thank you for sending the crash report.\n"
                                      "You can send more info to the #bugs channel\n"
                                      "in the Astris Discord.\n"
                                      "Restart the game?\n"
                           : view_file_in_system_file_browser(wfilename) ? L"The crash report folder has been opened.\n"
                                                                          "You can send the report to the #bugs channel\n"
                                                                          "in the Astris Discord.\n"
                                                                          "Restart the game?\n"
                                                                        : L"The crash report can be found in the program installation directory.\n"
                                                                          "You can send the report to the #bugs channel\n"
                                                                          "in the Astris Discord.\n"
                                                                          "Restart the game?\n",
                  L"Fatal Error",
                  MB_YESNO | MB_ICONQUESTION | MB_SYSTEMMODAL | MB_SETFOREGROUND) == IDYES)
  {
    goto restart;
  }

  // Return 1 because the game crashed, not because the crash report failed
  ExitProcess(1);
}
}
