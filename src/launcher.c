#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <stddef.h>

#define APP_NAME L"SuperLightBattery"
#define REAL_APP_EXE L"SuperLightBattery.exe"

void *memset(void *dest, int value, size_t count)
{
    unsigned char *bytes = (unsigned char *)dest;

    while (count > 0) {
        *bytes++ = (unsigned char)value;
        --count;
    }

    return dest;
}

static DWORD StringLength(const WCHAR *text)
{
    DWORD len = 0;

    while (text[len]) {
        ++len;
    }

    return len;
}

static BOOL CopyString(WCHAR *dest, DWORD destChars, const WCHAR *source)
{
    DWORD i = 0;

    if (destChars == 0) {
        return FALSE;
    }

    while (source[i]) {
        if (i + 1 >= destChars) {
            dest[0] = L'\0';
            return FALSE;
        }
        dest[i] = source[i];
        ++i;
    }

    dest[i] = L'\0';
    return TRUE;
}

static BOOL AppendString(WCHAR *dest, DWORD destChars, const WCHAR *suffix)
{
    DWORD len = StringLength(dest);
    DWORD i = 0;

    while (suffix[i]) {
        if (len + i + 1 >= destChars) {
            return FALSE;
        }
        dest[len + i] = suffix[i];
        ++i;
    }

    dest[len + i] = L'\0';
    return TRUE;
}

static BOOL GetModuleDirectory(WCHAR *dir, DWORD dirChars)
{
    DWORD len = GetModuleFileNameW(NULL, dir, dirChars);

    if (len == 0 || len >= dirChars) {
        return FALSE;
    }

    while (len > 0) {
        --len;
        if (dir[len] == L'\\') {
            dir[len] = L'\0';
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL BuildPath(WCHAR *path, DWORD pathChars, const WCHAR *dir, const WCHAR *fileName)
{
    return CopyString(path, pathChars, dir) &&
        AppendString(path, pathChars, L"\\") &&
        AppendString(path, pathChars, fileName);
}

static BOOL BuildTrayCommand(WCHAR *command, DWORD commandChars, const WCHAR *appPath)
{
    return CopyString(command, commandChars, L"\"") &&
        AppendString(command, commandChars, appPath) &&
        AppendString(command, commandChars, L"\" --tray");
}

static BOOL LaunchTray(void)
{
    WCHAR dir[MAX_PATH];
    WCHAR appPath[MAX_PATH];
    WCHAR command[MAX_PATH * 2];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    BOOL ok;

    if (!GetModuleDirectory(dir, ARRAYSIZE(dir)) ||
        !BuildPath(appPath, ARRAYSIZE(appPath), dir, REAL_APP_EXE) ||
        !BuildTrayCommand(command, ARRAYSIZE(command), appPath)) {
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

void WINAPI LauncherEntry(void)
{
    if (!LaunchTray()) {
        MessageBoxW(NULL, L"Could not start SuperLightBattery.exe.", APP_NAME, MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }

    ExitProcess(0);
}
