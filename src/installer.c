#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>
#include <commctrl.h>
#include <wchar.h>

#include "resource.h"

#define APP_NAME L"SuperLightBattery"
#define APP_EXE L"SuperLightBattery.exe"
#define LAUNCHER_EXE L"SuperLightBatteryLauncher.exe"
#define INSTALLER_EXE L"SuperLightBatteryInstaller.exe"
#define STARTUP_SHORTCUT L"SuperLightBattery.lnk"
#define TRAY_WINDOW_CLASS L"SuperLightBatteryTrayWindow"
#define RUN_KEY_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_APPROVED_RUN_KEY_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define STARTUP_APPROVED_FOLDER_KEY_PATH L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder"

#define INSTALLER_CLASS_NAME L"SuperLightBatteryInstallerWindow"
#define ID_BTN_INSTALL   1001
#define ID_BTN_UNINSTALL 1002
#define ID_BTN_CLOSE     1003

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
typedef HANDLE DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
typedef BOOL (WINAPI *SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);
typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);

typedef struct InstallerWindow {
    HWND hwnd;
    HWND title;
    HWND pathLabel;
    HWND pathValue;
    HWND statusLabel;
    HWND installBtn;
    HWND uninstallBtn;
    HWND closeBtn;
    HFONT titleFont;
    HFONT bodyFont;
    HFONT statusFont;
    UINT dpi;
    WCHAR installDir[MAX_PATH];
} InstallerWindow;

static void EnablePerMonitorDpiAwareness(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetProcessDpiAwarenessContextFn p = NULL;

    if (user32) {
        p = (SetProcessDpiAwarenessContextFn)
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    }

    if (p && p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return;
    }

    if (user32) {
        typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
        SetProcessDPIAwareFn legacy = (SetProcessDPIAwareFn)
            GetProcAddress(user32, "SetProcessDPIAware");

        if (legacy) {
            legacy();
        }
    }
}

static UINT GetWindowDpiSafe(HWND hwnd)
{
    static GetDpiForWindowFn p = NULL;
    static BOOL resolved = FALSE;

    if (!resolved) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            p = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
        }
        resolved = TRUE;
    }

    if (p && hwnd) {
        UINT dpi = p(hwnd);
        if (dpi > 0) {
            return dpi;
        }
    }

    return 96;
}

static int Scale(int value, UINT dpi)
{
    return MulDiv(value, (int)dpi, 96);
}

static BOOL RemoveFileName(WCHAR *path)
{
    WCHAR *lastSlash = wcsrchr(path, L'\\');
    WCHAR *lastForwardSlash = wcsrchr(path, L'/');
    WCHAR *last = lastSlash;

    if (lastForwardSlash && (!last || lastForwardSlash > last)) {
        last = lastForwardSlash;
    }

    if (!last) {
        return FALSE;
    }

    *last = L'\0';
    return TRUE;
}

static BOOL GetModuleDirectory(WCHAR *dir, size_t dirChars)
{
    DWORD len = GetModuleFileNameW(NULL, dir, (DWORD)dirChars);

    if (len == 0 || len >= dirChars) {
        return FALSE;
    }

    return RemoveFileName(dir);
}

static BOOL GetInstallDirectory(WCHAR *dir, size_t dirChars)
{
    WCHAR localAppData[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, (DWORD)ARRAYSIZE(localAppData));

    if (len == 0 || len >= ARRAYSIZE(localAppData)) {
        return FALSE;
    }

    return SUCCEEDED(StringCchPrintfW(dir, dirChars, L"%s\\%s", localAppData, APP_NAME));
}

static BOOL SamePathNoCase(const WCHAR *a, const WCHAR *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

static BOOL CopyIfNeeded(const WCHAR *source, const WCHAR *destination)
{
    if (SamePathNoCase(source, destination)) {
        return TRUE;
    }

    return CopyFileW(source, destination, FALSE);
}

static void RequestTrayExit(void)
{
    HWND hwnd = FindWindowW(TRAY_WINDOW_CLASS, NULL);

    if (!hwnd) {
        hwnd = FindWindowExW(HWND_MESSAGE, NULL, TRAY_WINDOW_CLASS, NULL);
    }

    if (hwnd) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        Sleep(400);
    }
}

static BOOL GetStartupShortcutPath(WCHAR *path, size_t pathChars)
{
    WCHAR startupDir[MAX_PATH];

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, SHGFP_TYPE_CURRENT, startupDir))) {
        return FALSE;
    }

    return SUCCEEDED(StringCchPrintfW(path, pathChars, L"%s\\%s", startupDir, STARTUP_SHORTCUT));
}

static BOOL WriteStartupShortcut(const WCHAR *launcherPath, const WCHAR *installDir)
{
    WCHAR shortcutPath[MAX_PATH];
    IShellLinkW *link = NULL;
    IPersistFile *persist = NULL;
    HRESULT hr;
    BOOL ok = FALSE;

    if (!GetStartupShortcutPath(shortcutPath, ARRAYSIZE(shortcutPath))) {
        return FALSE;
    }

    hr = CoInitialize(NULL);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return FALSE;
    }

    if (SUCCEEDED(CoCreateInstance(
            &CLSID_ShellLink,
            NULL,
            CLSCTX_INPROC_SERVER,
            &IID_IShellLinkW,
            (void **)&link))) {
        if (SUCCEEDED(link->lpVtbl->SetPath(link, launcherPath)) &&
            SUCCEEDED(link->lpVtbl->SetWorkingDirectory(link, installDir)) &&
            SUCCEEDED(link->lpVtbl->SetIconLocation(link, launcherPath, 0)) &&
            SUCCEEDED(link->lpVtbl->SetShowCmd(link, SW_HIDE)) &&
            SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&persist))) {
            ok = SUCCEEDED(persist->lpVtbl->Save(persist, shortcutPath, TRUE));
        }
    }

    if (persist) {
        persist->lpVtbl->Release(persist);
    }
    if (link) {
        link->lpVtbl->Release(link);
    }
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }

    return ok;
}

static void DeleteStartupShortcut(void)
{
    WCHAR shortcutPath[MAX_PATH];

    if (GetStartupShortcutPath(shortcutPath, ARRAYSIZE(shortcutPath))) {
        (void)DeleteFileW(shortcutPath);
    }
}

static BOOL StartupShortcutExists(void)
{
    WCHAR shortcutPath[MAX_PATH];

    return GetStartupShortcutPath(shortcutPath, ARRAYSIZE(shortcutPath)) &&
        GetFileAttributesW(shortcutPath) != INVALID_FILE_ATTRIBUTES;
}

static void DeleteRunKey(void)
{
    HKEY key;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        (void)RegDeleteValueW(key, APP_NAME);
        RegCloseKey(key);
    }
}

static void DeleteStartupApprovalValue(const WCHAR *keyPath, const WCHAR *valueName)
{
    HKEY key;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        (void)RegDeleteValueW(key, valueName);
        RegCloseKey(key);
    }
}

static BOOL IsInstalled(void)
{
    WCHAR installDir[MAX_PATH];
    WCHAR destApp[MAX_PATH];
    WCHAR destLauncher[MAX_PATH];
    WCHAR destInstaller[MAX_PATH];

    if (!GetInstallDirectory(installDir, ARRAYSIZE(installDir)) ||
        FAILED(StringCchPrintfW(destApp, ARRAYSIZE(destApp), L"%s\\%s", installDir, APP_EXE)) ||
        FAILED(StringCchPrintfW(destLauncher, ARRAYSIZE(destLauncher), L"%s\\%s", installDir, LAUNCHER_EXE)) ||
        FAILED(StringCchPrintfW(destInstaller, ARRAYSIZE(destInstaller), L"%s\\%s", installDir, INSTALLER_EXE))) {
        return FALSE;
    }

    if (GetFileAttributesW(destApp) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(destLauncher) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(destInstaller) == INVALID_FILE_ATTRIBUTES) {
        return FALSE;
    }

    return StartupShortcutExists();
}

static BOOL LaunchTrayFromDirectory(const WCHAR *dir)
{
    WCHAR toolPath[MAX_PATH];
    WCHAR command[MAX_PATH * 2];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    BOOL ok;

    if (FAILED(StringCchPrintfW(toolPath, ARRAYSIZE(toolPath), L"%s\\%s", dir, APP_EXE)) ||
        FAILED(StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\" --tray", toolPath))) {
        return FALSE;
    }

    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    ok = CreateProcessW(
        NULL,
        command,
        NULL,
        NULL,
        FALSE,
        CREATE_NO_WINDOW,
        NULL,
        dir,
        &startup,
        &process);

    if (ok) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }

    return ok;
}

static BOOL DoInstall(WCHAR *errorMessage, size_t errorMessageChars, BOOL *trayStarted)
{
    WCHAR sourceDir[MAX_PATH];
    WCHAR sourceApp[MAX_PATH];
    WCHAR sourceLauncher[MAX_PATH];
    WCHAR sourceInstaller[MAX_PATH];
    WCHAR installDir[MAX_PATH];
    WCHAR destApp[MAX_PATH];
    WCHAR destLauncher[MAX_PATH];
    WCHAR destInstaller[MAX_PATH];

    if (errorMessage && errorMessageChars > 0) {
        errorMessage[0] = L'\0';
    }
    if (trayStarted) {
        *trayStarted = FALSE;
    }

    if (!GetModuleDirectory(sourceDir, ARRAYSIZE(sourceDir)) ||
        !GetInstallDirectory(installDir, ARRAYSIZE(installDir)) ||
        FAILED(StringCchPrintfW(sourceApp, ARRAYSIZE(sourceApp), L"%s\\%s", sourceDir, APP_EXE)) ||
        FAILED(StringCchPrintfW(sourceLauncher, ARRAYSIZE(sourceLauncher), L"%s\\%s", sourceDir, LAUNCHER_EXE)) ||
        FAILED(StringCchPrintfW(sourceInstaller, ARRAYSIZE(sourceInstaller), L"%s\\%s", sourceDir, INSTALLER_EXE)) ||
        FAILED(StringCchPrintfW(destApp, ARRAYSIZE(destApp), L"%s\\%s", installDir, APP_EXE)) ||
        FAILED(StringCchPrintfW(destLauncher, ARRAYSIZE(destLauncher), L"%s\\%s", installDir, LAUNCHER_EXE)) ||
        FAILED(StringCchPrintfW(destInstaller, ARRAYSIZE(destInstaller), L"%s\\%s", installDir, INSTALLER_EXE))) {
        if (errorMessage) {
            (void)StringCchCopyW(errorMessage, errorMessageChars, L"Could not prepare install paths.");
        }
        return FALSE;
    }

    if (GetFileAttributesW(sourceApp) == INVALID_FILE_ATTRIBUTES) {
        if (errorMessage) {
            (void)StringCchCopyW(errorMessage, errorMessageChars, L"SuperLightBattery.exe must sit next to the installer.");
        }
        return FALSE;
    }
    if (GetFileAttributesW(sourceLauncher) == INVALID_FILE_ATTRIBUTES) {
        if (errorMessage) {
            (void)StringCchCopyW(errorMessage, errorMessageChars, L"SuperLightBatteryLauncher.exe must sit next to the installer.");
        }
        return FALSE;
    }

    if (!CreateDirectoryW(installDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (errorMessage) {
            (void)StringCchCopyW(errorMessage, errorMessageChars, L"Could not create the install directory.");
        }
        return FALSE;
    }

    RequestTrayExit();

    if (!CopyIfNeeded(sourceApp, destApp) ||
        !CopyIfNeeded(sourceLauncher, destLauncher) ||
        !CopyIfNeeded(sourceInstaller, destInstaller)) {
        if (errorMessage) {
            (void)StringCchCopyW(errorMessage, errorMessageChars, L"Could not copy SuperLightBattery files.");
        }
        return FALSE;
    }

    DeleteRunKey();
    DeleteStartupApprovalValue(STARTUP_APPROVED_RUN_KEY_PATH, APP_NAME);
    DeleteStartupShortcut();

    if (!WriteStartupShortcut(destLauncher, installDir)) {
        if (errorMessage) {
            (void)StringCchCopyW(errorMessage, errorMessageChars, L"Could not register the startup entry.");
        }
        return FALSE;
    }

    {
        BOOL launched = LaunchTrayFromDirectory(installDir);
        if (trayStarted) {
            *trayStarted = launched;
        }
    }
    return TRUE;
}

static BOOL DoUninstall(WCHAR *errorMessage, size_t errorMessageChars)
{
    WCHAR installDir[MAX_PATH];
    WCHAR destApp[MAX_PATH];
    WCHAR destLauncher[MAX_PATH];
    WCHAR destInstaller[MAX_PATH];

    if (errorMessage && errorMessageChars > 0) {
        errorMessage[0] = L'\0';
    }

    DeleteRunKey();
    DeleteStartupApprovalValue(STARTUP_APPROVED_RUN_KEY_PATH, APP_NAME);
    DeleteStartupApprovalValue(STARTUP_APPROVED_FOLDER_KEY_PATH, STARTUP_SHORTCUT);
    DeleteStartupShortcut();
    RequestTrayExit();

    if (!GetInstallDirectory(installDir, ARRAYSIZE(installDir)) ||
        FAILED(StringCchPrintfW(destApp, ARRAYSIZE(destApp), L"%s\\%s", installDir, APP_EXE)) ||
        FAILED(StringCchPrintfW(destLauncher, ARRAYSIZE(destLauncher), L"%s\\%s", installDir, LAUNCHER_EXE)) ||
        FAILED(StringCchPrintfW(destInstaller, ARRAYSIZE(destInstaller), L"%s\\%s", installDir, INSTALLER_EXE))) {
        if (errorMessage) {
            (void)StringCchCopyW(errorMessage, errorMessageChars, L"Could not prepare uninstall paths.");
        }
        return FALSE;
    }

    if (!DeleteFileW(destApp) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        (void)MoveFileExW(destApp, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    if (!DeleteFileW(destLauncher) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        (void)MoveFileExW(destLauncher, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    if (!DeleteFileW(destInstaller) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        (void)MoveFileExW(destInstaller, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    (void)RemoveDirectoryW(installDir);

    return TRUE;
}

static int CliInstall(void)
{
    WCHAR error[160];
    BOOL trayStarted = FALSE;

    if (DoInstall(error, ARRAYSIZE(error), &trayStarted)) {
        if (!trayStarted) {
            MessageBoxW(
                NULL,
                L"SuperLightBattery was installed and will start with Windows, "
                L"but the tray app could not be launched right now. It will start at next logon.",
                APP_NAME,
                MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    MessageBoxW(NULL, error[0] ? error : L"Install failed.", APP_NAME, MB_OK | MB_ICONERROR);
    return 1;
}

static int CliUninstall(void)
{
    WCHAR error[160];

    if (DoUninstall(error, ARRAYSIZE(error))) {
        return 0;
    }
    MessageBoxW(NULL, error[0] ? error : L"Uninstall failed.", APP_NAME, MB_OK | MB_ICONERROR);
    return 1;
}

static void ShowUsage(void)
{
    MessageBoxW(
        NULL,
        L"Usage:\n"
        L"  SuperLightBatteryInstaller.exe              (opens this window)\n"
        L"  SuperLightBatteryInstaller.exe --install    (silent install)\n"
        L"  SuperLightBatteryInstaller.exe --uninstall  (silent uninstall)\n",
        APP_NAME,
        MB_OK | MB_ICONINFORMATION);
}

static HFONT MakeFont(int pointSize, int weight, UINT dpi)
{
    LOGFONTW lf;
    int height;

    ZeroMemory(&lf, sizeof(lf));
    height = -MulDiv(pointSize, (int)dpi, 72);
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_TT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    (void)StringCchCopyW(lf.lfFaceName, ARRAYSIZE(lf.lfFaceName), L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

static void DestroyFonts(InstallerWindow *win)
{
    if (win->titleFont) { DeleteObject(win->titleFont); win->titleFont = NULL; }
    if (win->bodyFont) { DeleteObject(win->bodyFont); win->bodyFont = NULL; }
    if (win->statusFont) { DeleteObject(win->statusFont); win->statusFont = NULL; }
}

static void ApplyFonts(InstallerWindow *win)
{
    SendMessageW(win->title, WM_SETFONT, (WPARAM)win->titleFont, TRUE);
    SendMessageW(win->pathLabel, WM_SETFONT, (WPARAM)win->bodyFont, TRUE);
    SendMessageW(win->pathValue, WM_SETFONT, (WPARAM)win->bodyFont, TRUE);
    SendMessageW(win->statusLabel, WM_SETFONT, (WPARAM)win->statusFont, TRUE);
    SendMessageW(win->installBtn, WM_SETFONT, (WPARAM)win->bodyFont, TRUE);
    SendMessageW(win->uninstallBtn, WM_SETFONT, (WPARAM)win->bodyFont, TRUE);
    SendMessageW(win->closeBtn, WM_SETFONT, (WPARAM)win->bodyFont, TRUE);
}

static void LayoutControls(InstallerWindow *win)
{
    UINT dpi = win->dpi;
    int pad = Scale(18, dpi);
    int btnW = Scale(120, dpi);
    int btnH = Scale(34, dpi);
    int gap = Scale(10, dpi);
    int titleH = Scale(28, dpi);
    int labelH = Scale(20, dpi);
    int valueH = Scale(22, dpi);
    int statusH = Scale(40, dpi);
    int contentW;
    int y;
    RECT rc;

    GetClientRect(win->hwnd, &rc);
    contentW = rc.right - rc.left - 2 * pad;
    if (contentW < Scale(360, dpi)) {
        contentW = Scale(360, dpi);
    }

    y = pad;
    MoveWindow(win->title, pad, y, contentW, titleH, TRUE);
    y += titleH + gap;
    MoveWindow(win->pathLabel, pad, y, contentW, labelH, TRUE);
    y += labelH + Scale(2, dpi);
    MoveWindow(win->pathValue, pad, y, contentW, valueH, TRUE);
    y += valueH + gap;
    MoveWindow(win->statusLabel, pad, y, contentW, statusH, TRUE);

    {
        int btnY = rc.bottom - pad - btnH;
        int gapBtn = Scale(12, dpi);
        int rowW = btnW * 3 + gapBtn * 2;
        int startX = rc.right - pad - rowW;

        if (startX < pad) {
            startX = pad;
        }
        MoveWindow(win->installBtn, startX, btnY, btnW, btnH, TRUE);
        MoveWindow(win->uninstallBtn, startX + btnW + gapBtn, btnY, btnW, btnH, TRUE);
        MoveWindow(win->closeBtn, startX + 2 * (btnW + gapBtn), btnY, btnW, btnH, TRUE);
    }
}

static void RefreshButtons(InstallerWindow *win)
{
    BOOL installed = IsInstalled();

    EnableWindow(win->installBtn, !installed);
    EnableWindow(win->uninstallBtn, installed);
}

static void SetStatus(InstallerWindow *win, const WCHAR *text)
{
    SetWindowTextW(win->statusLabel, text);
}

static void HandleInstall(InstallerWindow *win)
{
    WCHAR error[160];
    BOOL trayStarted = FALSE;

    SetStatus(win, L"Installing...");
    EnableWindow(win->installBtn, FALSE);
    EnableWindow(win->uninstallBtn, FALSE);
    UpdateWindow(win->statusLabel);

    if (DoInstall(error, ARRAYSIZE(error), &trayStarted)) {
        if (trayStarted) {
            SetStatus(win, L"Installed. The tray app is running and will start at every logon.");
        } else {
            SetStatus(win, L"Installed. Tray could not be started now; it will start at next logon.");
        }
    } else {
        SetStatus(win, error[0] ? error : L"Install failed.");
    }
    RefreshButtons(win);
}

static void HandleUninstall(InstallerWindow *win)
{
    WCHAR error[160];

    SetStatus(win, L"Uninstalling...");
    EnableWindow(win->installBtn, FALSE);
    EnableWindow(win->uninstallBtn, FALSE);
    UpdateWindow(win->statusLabel);

    if (DoUninstall(error, ARRAYSIZE(error))) {
        SetStatus(win, L"Uninstalled. The startup entry and installed files were removed.");
    } else {
        SetStatus(win, error[0] ? error : L"Uninstall failed.");
    }
    RefreshButtons(win);
}

static void CreateControls(InstallerWindow *win, HINSTANCE instance)
{
    win->title = CreateWindowExW(0, L"STATIC", APP_NAME,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, win->hwnd, NULL, instance, NULL);
    win->pathLabel = CreateWindowExW(0, L"STATIC", L"Will install for the current user to:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, win->hwnd, NULL, instance, NULL);
    win->pathValue = CreateWindowExW(0, L"STATIC", win->installDir,
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
        0, 0, 0, 0, win->hwnd, NULL, instance, NULL);
    win->statusLabel = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, win->hwnd, NULL, instance, NULL);

    win->installBtn = CreateWindowExW(0, L"BUTTON", L"Install",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, win->hwnd, (HMENU)(LONG_PTR)ID_BTN_INSTALL, instance, NULL);
    win->uninstallBtn = CreateWindowExW(0, L"BUTTON", L"Uninstall",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, win->hwnd, (HMENU)(LONG_PTR)ID_BTN_UNINSTALL, instance, NULL);
    win->closeBtn = CreateWindowExW(0, L"BUTTON", L"Close",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, win->hwnd, (HMENU)(LONG_PTR)ID_BTN_CLOSE, instance, NULL);
}

static void CreateFontsForDpi(InstallerWindow *win)
{
    DestroyFonts(win);
    win->titleFont = MakeFont(14, FW_SEMIBOLD, win->dpi);
    win->bodyFont = MakeFont(10, FW_NORMAL, win->dpi);
    win->statusFont = MakeFont(10, FW_NORMAL, win->dpi);
}

static LRESULT CALLBACK InstallerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    InstallerWindow *win = (InstallerWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_CREATE:
        win = (InstallerWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        win->hwnd = hwnd;
        win->dpi = GetWindowDpiSafe(hwnd);
        CreateControls(win, ((CREATESTRUCTW *)lParam)->hInstance);
        CreateFontsForDpi(win);
        ApplyFonts(win);
        LayoutControls(win);
        RefreshButtons(win);
        return 0;

    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lParam;
        if (win) {
            win->dpi = HIWORD(wParam);
            SetWindowPos(hwnd, NULL,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            CreateFontsForDpi(win);
            ApplyFonts(win);
            LayoutControls(win);
        }
        return 0;
    }

    case WM_SIZE:
        if (win) {
            LayoutControls(win);
        }
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }

    case WM_COMMAND:
        if (!win) {
            break;
        }
        switch (LOWORD(wParam)) {
        case ID_BTN_INSTALL:
            HandleInstall(win);
            return 0;
        case ID_BTN_UNINSTALL:
            HandleUninstall(win);
            return 0;
        case ID_BTN_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (win) {
            DestroyFonts(win);
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int RunInstallerGui(HINSTANCE instance)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    InstallerWindow win;
    HICON appIcon;
    UINT dpi;
    int windowW;
    int windowH;

    EnablePerMonitorDpiAwareness();

    ZeroMemory(&icc, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&win, sizeof(win));
    if (!GetInstallDirectory(win.installDir, ARRAYSIZE(win.installDir))) {
        (void)StringCchCopyW(win.installDir, ARRAYSIZE(win.installDir), L"%LOCALAPPDATA%\\SuperLightBattery");
    }

    appIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = InstallerWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = INSTALLER_CLASS_NAME;
    wc.hIcon = appIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Could not register installer window.", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    dpi = 96;
    {
        HDC screenDc = GetDC(NULL);
        if (screenDc) {
            int sysDpi = GetDeviceCaps(screenDc, LOGPIXELSX);
            if (sysDpi > 0) {
                dpi = (UINT)sysDpi;
            }
            ReleaseDC(NULL, screenDc);
        }
    }

    windowW = Scale(520, dpi);
    windowH = Scale(280, dpi);

    hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        INSTALLER_CLASS_NAME,
        L"SuperLightBattery",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowW, windowH,
        NULL, NULL, instance, &win);

    if (!hwnd) {
        MessageBoxW(NULL, L"Could not create installer window.", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    if (appIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)appIcon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)appIcon);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previousInstance, PWSTR commandLine, int showCommand)
{
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int result = 0;

    (void)previousInstance;
    (void)commandLine;
    (void)showCommand;

    if (!argv) {
        return 1;
    }

    if (argc <= 1) {
        result = RunInstallerGui(instance);
    } else if (wcscmp(argv[1], L"--install") == 0) {
        result = CliInstall();
    } else if (wcscmp(argv[1], L"--uninstall") == 0) {
        result = CliUninstall();
    } else if (wcscmp(argv[1], L"--gui") == 0) {
        result = RunInstallerGui(instance);
    } else if (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0 || wcscmp(argv[1], L"/?") == 0) {
        ShowUsage();
    } else {
        MessageBoxW(NULL, L"Unknown installer option.", APP_NAME, MB_OK | MB_ICONERROR);
        result = 2;
    }

    LocalFree(argv);
    return result;
}
