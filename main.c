#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "parson.h"

#include <stdarg.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winhttp.lib")

/* ---- debug log (writes to cltray.log next to exe, enabled with --debug) ---- */
static BOOL g_debug = FALSE;

static void DbgLog(const char *fmt, ...) {
    if (!g_debug) return;
    static char logPath[MAX_PATH] = {0};
    if (!logPath[0]) {
        GetModuleFileNameA(NULL, logPath, MAX_PATH);
        char *dot = strrchr(logPath, '.');
        if (dot) strcpy(dot, ".log");
        else strcat(logPath, ".log");
    }
    FILE *f = fopen(logPath, "a");
    if (!f) return;
    /* timestamp */
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ---- dark mode support (undocumented uxtheme ordinals) ---- */
typedef BOOL (WINAPI *AllowDarkModeForWindowFn)(HWND, BOOL);
typedef DWORD (WINAPI *SetPreferredAppModeFn)(DWORD);
typedef void (WINAPI *FlushMenuThemesFn)(void);

static void EnableDarkMode(void) {
    HMODULE hUx = LoadLibrary(L"uxtheme.dll");
    if (!hUx) return;

    /* ordinal 135: SetPreferredAppMode(AllowDark=1) */
    SetPreferredAppModeFn SetMode =
        (SetPreferredAppModeFn)GetProcAddress(hUx, MAKEINTRESOURCEA(135));
    if (SetMode) SetMode(1);

    /* ordinal 136: FlushMenuThemes */
    FlushMenuThemesFn Flush =
        (FlushMenuThemesFn)GetProcAddress(hUx, MAKEINTRESOURCEA(136));
    if (Flush) Flush();
}

static void AllowDarkForWindow(HWND hWnd) {
    HMODULE hUx = GetModuleHandle(L"uxtheme.dll");
    if (!hUx) return;

    /* ordinal 133: AllowDarkModeForWindow */
    AllowDarkModeForWindowFn Allow =
        (AllowDarkModeForWindowFn)GetProcAddress(hUx, MAKEINTRESOURCEA(133));
    if (Allow) Allow(hWnd, TRUE);
}

#define WM_TRAYICON      (WM_USER + 1)
#define ID_TRAYICON      1
#define ID_EXIT          2000
#define ID_HOVER         2001
#define ID_ROUNDED       2002
#define ID_SONNET        2003
#define IDT_UPDATE       1
#define IDT_SPINNER      2
#define WM_FETCHDONE     (WM_USER + 2)

#define MAX_BARS         3
#define POPUP_W          340
#define POPUP_H_2        158
#define POPUP_H_3        204

static HINSTANCE g_hInst;
static HWND      g_hWndPopup;
static NOTIFYICONDATA g_nid;
static BOOL      g_showOnHover;
static BOOL      g_roundedCorners;
static BOOL      g_showSonnet;
static BOOL      g_menuOpen;
static BOOL      g_fetching;
static int       g_spinnerFrame;
static int       g_barCount = 2;
static int       g_values[MAX_BARS];
static const wchar_t *g_labels[MAX_BARS] = {
    L"Session", L"All models", L"Sonnet"
};
static COLORREF g_colors[MAX_BARS] = {
    RGB(66,133,244), RGB(244,180,0), RGB(171,71,188)
};
static wchar_t g_details[MAX_BARS][64];
static wchar_t g_statusMsg[128];

/* ---- file reading helper ---- */
static char *ReadFileToStringA(const wchar_t *path) {
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(hFile); return NULL; }
    char *buf = (char *)malloc(size + 1);
    if (!buf) { CloseHandle(hFile); return NULL; }
    DWORD bytesRead;
    if (!ReadFile(hFile, buf, size, &bytesRead, NULL)) {
        CloseHandle(hFile); free(buf); return NULL;
    }
    CloseHandle(hFile);
    buf[bytesRead] = '\0';
    return buf;
}

/* ---- run a command and capture stdout ---- */
static BOOL RunCommandCapture(const wchar_t *cmdLine, char *outBuf, int outBufSize) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return FALSE;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    wchar_t cmd[512];
    lstrcpynW(cmd, cmdLine, 512);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return FALSE;
    }
    CloseHandle(hWritePipe);

    DWORD totalRead = 0, bytesRead;
    while (totalRead < (DWORD)(outBufSize - 1)) {
        if (!ReadFile(hReadPipe, outBuf + totalRead,
                      (DWORD)(outBufSize - 1 - totalRead), &bytesRead, NULL) || bytesRead == 0)
            break;
        totalRead += bytesRead;
    }
    outBuf[totalRead] = '\0';

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    /* trim trailing whitespace */
    while (totalRead > 0 && (outBuf[totalRead-1] == '\n' || outBuf[totalRead-1] == '\r'
                             || outBuf[totalRead-1] == ' '))
        outBuf[--totalRead] = '\0';

    return totalRead > 0;
}

/* ---- get default WSL distro name from registry ---- */
static BOOL GetDefaultWSLDistro(wchar_t *distro, int distroChars) {
    HKEY hLxss;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",
            0, KEY_READ, &hLxss) != ERROR_SUCCESS)
        return FALSE;

    wchar_t defaultGuid[64];
    DWORD size = sizeof(defaultGuid);
    if (RegGetValueW(hLxss, NULL, L"DefaultDistribution", RRF_RT_REG_SZ,
                     NULL, defaultGuid, &size) != ERROR_SUCCESS) {
        RegCloseKey(hLxss);
        return FALSE;
    }

    HKEY hSub;
    if (RegOpenKeyExW(hLxss, defaultGuid, 0, KEY_READ, &hSub) != ERROR_SUCCESS) {
        RegCloseKey(hLxss);
        return FALSE;
    }

    size = (DWORD)(distroChars * sizeof(wchar_t));
    BOOL ok = RegGetValueW(hSub, NULL, L"DistributionName", RRF_RT_REG_SZ,
                           NULL, distro, &size) == ERROR_SUCCESS;

    RegCloseKey(hSub);
    RegCloseKey(hLxss);
    return ok;
}

/* ---- find and read credentials file ---- */
static char *FindAndReadCredentials(void) {
    wchar_t path[MAX_PATH];
    char pathA[MAX_PATH];
    char *content;

    DbgLog("--- FindAndReadCredentials ---");

    /* primary: %USERPROFILE%\.claude\.credentials.json */
    DWORD len = GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH);
    if (len > 0 && len < MAX_PATH - 40) {
        lstrcatW(path, L"\\.claude\\.credentials.json");
        WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, MAX_PATH, NULL, NULL);
        DbgLog("Trying native path: %s", pathA);
        content = ReadFileToStringA(path);
        if (content) {
            DbgLog("Found credentials (native), %d bytes", (int)strlen(content));
            return content;
        }
        DbgLog("Native path not found");
    } else {
        DbgLog("USERPROFILE not set or too long (len=%lu)", len);
    }

    /* WSL fallback: construct UNC path via registry distro + wsl home */
    wchar_t distro[128];
    char homeA[256];
    BOOL gotDistro = GetDefaultWSLDistro(distro, 128);
    if (gotDistro) {
        char distroA[128];
        WideCharToMultiByte(CP_UTF8, 0, distro, -1, distroA, 128, NULL, NULL);
        DbgLog("WSL distro from registry: %s", distroA);
    } else {
        DbgLog("No WSL distro found in registry");
    }

    if (gotDistro &&
        RunCommandCapture(L"wsl.exe -e sh -c \"echo $HOME\"", homeA, sizeof(homeA))) {
        DbgLog("WSL home: %s", homeA);
        wchar_t homeW[256];
        MultiByteToWideChar(CP_UTF8, 0, homeA, -1, homeW, 256);
        for (int i = 0; homeW[i]; i++)
            if (homeW[i] == L'/') homeW[i] = L'\\';
        wsprintfW(path, L"\\\\wsl.localhost\\%s%s\\.claude\\.credentials.json",
                  distro, homeW);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, pathA, MAX_PATH, NULL, NULL);
        DbgLog("Trying WSL UNC path: %s", pathA);
        content = ReadFileToStringA(path);
        if (content) {
            DbgLog("Found credentials (WSL UNC), %d bytes", (int)strlen(content));
            return content;
        }
        DbgLog("WSL UNC path not found");
    }

    /* last resort: ask wsl to cat the file directly */
    DbgLog("Trying wsl cat fallback");
    char catBuf[4096];
    if (RunCommandCapture(
            L"wsl.exe -e sh -c \"cat $HOME/.claude/.credentials.json\"",
            catBuf, sizeof(catBuf))) {
        DbgLog("Got credentials via wsl cat, %d bytes", (int)strlen(catBuf));
        content = _strdup(catBuf);
        if (content) return content;
    }

    DbgLog("No credentials found anywhere");
    return NULL;
}

/* ---- WinHTTP fetch helper (returns HTTP status, 0 on error) ---- */
static int FetchUrl(const char *path, const char *token,
                    char *responseBuf, DWORD bufSize) {
    int result = 0;

    DbgLog("FetchUrl: GET https://api.anthropic.com%s", path);
    DbgLog("FetchUrl: token = %.20s...%s", token,
           strlen(token) > 20 ? "(truncated)" : "");

    HINTERNET hSession = WinHttpOpen(L"CLTray/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { DbgLog("FetchUrl: WinHttpOpen failed (%lu)", GetLastError()); return 0; }

    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.anthropic.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        DbgLog("FetchUrl: WinHttpConnect failed (%lu)", GetLastError());
        WinHttpCloseHandle(hSession);
        return 0;
    }

    wchar_t wPath[512];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, 512);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        DbgLog("FetchUrl: WinHttpOpenRequest failed (%lu)", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return 0;
    }

    /* set headers matching Claude Code's actual requests */
    wchar_t wToken[1024];
    MultiByteToWideChar(CP_UTF8, 0, token, -1, wToken, 1024);

    wchar_t hdrs[4096];
    wsprintfW(hdrs,
        L"Authorization: Bearer %s\r\n"
        L"Accept: application/json, text/plain, */*\r\n"
        L"Content-Type: application/json\r\n"
        L"User-Agent: claude-code/2.1.50\r\n"
        L"anthropic-beta: oauth-2025-04-20\r\n",
        wToken);
    WinHttpAddRequestHeaders(hRequest, hdrs, (DWORD)-1,
        WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DbgLog("FetchUrl: WinHttpSendRequest failed (%lu)", GetLastError());
        goto cleanup;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        DbgLog("FetchUrl: WinHttpReceiveResponse failed (%lu)", GetLastError());
        goto cleanup;
    }

    /* read status code */
    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    result = (int)statusCode;
    DbgLog("FetchUrl: HTTP %d", result);

    /* read body */
    {
        DWORD totalRead = 0, bytesAvail, bytesRead;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvail) && bytesAvail > 0) {
            if (totalRead + bytesAvail >= bufSize - 1) break;
            WinHttpReadData(hRequest, responseBuf + totalRead,
                            bytesAvail, &bytesRead);
            totalRead += bytesRead;
        }
        responseBuf[totalRead] = '\0';
        DbgLog("FetchUrl: body %lu bytes", totalRead);
        /* log first 500 chars of response for debugging */
        if (totalRead > 0) {
            char preview[501];
            int n = totalRead < 500 ? (int)totalRead : 500;
            memcpy(preview, responseBuf, n);
            preview[n] = '\0';
            DbgLog("FetchUrl: body preview: %s", preview);
        }
    }

cleanup:
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

/* ---- parse ISO 8601 timestamp → detail text ---- */
/* showAbsolute: FALSE → "Resets in X hr Y min", TRUE → "Resets Friday 16:00" */
static void FormatResetTime(const char *iso, wchar_t *out, int outSize, BOOL showAbsolute) {
    (void)outSize;
    if (!iso || !*iso) { wsprintfW(out, L"Starts when a message is sent"); return; }

    int year, month, day, hour, minute, second;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &year, &month, &day,
               &hour, &minute, &second) < 6) {
        wsprintfW(out, L"--");
        return;
    }

    /* parse as UTC */
    SYSTEMTIME resetUTC = {0};
    resetUTC.wYear   = (WORD)year;
    resetUTC.wMonth  = (WORD)month;
    resetUTC.wDay    = (WORD)day;
    resetUTC.wHour   = (WORD)hour;
    resetUTC.wMinute = (WORD)minute;
    resetUTC.wSecond = (WORD)second;

    FILETIME resetFT, nowFT;
    SystemTimeToFileTime(&resetUTC, &resetFT);
    GetSystemTimeAsFileTime(&nowFT);

    ULARGE_INTEGER resetUI, nowUI;
    resetUI.LowPart  = resetFT.dwLowDateTime;
    resetUI.HighPart = resetFT.dwHighDateTime;
    nowUI.LowPart    = nowFT.dwLowDateTime;
    nowUI.HighPart   = nowFT.dwHighDateTime;

    if (resetUI.QuadPart <= nowUI.QuadPart) {
        wsprintfW(out, L"Resetting soon");
        return;
    }

    if (showAbsolute) {
        /* convert UTC → local time for display */
        FILETIME localFT;
        SYSTEMTIME localST;
        FileTimeToLocalFileTime(&resetFT, &localFT);
        FileTimeToSystemTime(&localFT, &localST);

        static const wchar_t *dayNames[] = {
            L"Sunday", L"Monday", L"Tuesday", L"Wednesday",
            L"Thursday", L"Friday", L"Saturday"
        };
        wsprintfW(out, L"Resets %s %d:%02d",
                  dayNames[localST.wDayOfWeek],
                  localST.wHour, localST.wMinute);
    } else {
        ULONGLONG diffSec = (resetUI.QuadPart - nowUI.QuadPart) / 10000000ULL;
        int hrs  = (int)(diffSec / 3600);
        int mins = (int)((diffSec % 3600) / 60);

        if (hrs > 0)
            wsprintfW(out, L"Resets in %d hr %d min", hrs, mins);
        else
            wsprintfW(out, L"Resets in %d min", mins);
    }
}

/* ---- fetch real usage data from Claude.ai ---- */
static void FetchUsageData(void) {
    DbgLog("=== FetchUsageData start ===");

    char *creds = FindAndReadCredentials();
    if (!creds) {
        DbgLog("ERROR: no credentials found");
        lstrcpyW(g_statusMsg, L"No credentials found.\nCheck Windows or WSL\n~/.claude/.credentials.json");
        return;
    }

    JSON_Value *credRoot = json_parse_string(creds);
    free(creds);
    if (!credRoot) {
        DbgLog("ERROR: failed to parse credentials JSON");
        lstrcpyW(g_statusMsg, L"Invalid credentials file.");
        return;
    }

    const char *token = json_object_dotget_string(
        json_object(credRoot), "claudeAiOauth.accessToken");
    if (!token || !*token) {
        DbgLog("ERROR: no accessToken in credentials");
        json_value_free(credRoot);
        lstrcpyW(g_statusMsg, L"No access token in credentials.");
        return;
    }

    DbgLog("Token found: %.20s...", token);

    char tokenBuf[1024];
    strncpy(tokenBuf, token, sizeof(tokenBuf) - 1);
    tokenBuf[sizeof(tokenBuf) - 1] = '\0';
    json_value_free(credRoot);

    char response[32768];

    /* fetch usage — single endpoint, no org ID needed */
    DbgLog("Fetching /api/oauth/usage");
    int status = FetchUrl("/api/oauth/usage", tokenBuf, response, sizeof(response));
    if (status == 401) {
        DbgLog("ERROR: 401 — token expired");
        lstrcpyW(g_statusMsg, L"Token expired.\nRe-authenticate with Claude Code.");
        return;
    }
    if (status == 0) {
        DbgLog("ERROR: connection failed");
        lstrcpyW(g_statusMsg, L"Connection failed.\nCheck your internet connection.");
        return;
    }
    if (status != 200) {
        DbgLog("ERROR: /api/oauth/usage returned %d", status);
        wsprintfW(g_statusMsg, L"API error (%d).", status);
        return;
    }

    JSON_Value *usageRoot = json_parse_string(response);
    if (!usageRoot) {
        DbgLog("ERROR: failed to parse usage JSON");
        lstrcpyW(g_statusMsg, L"Invalid API response.");
        return;
    }

    /* success — clear any previous error */
    g_statusMsg[0] = L'\0';

    JSON_Object *usage = json_object(usageRoot);

    static const char *keys[] = {
        "five_hour", "seven_day", "seven_day_sonnet"
    };

    for (int i = 0; i < g_barCount; i++) {
        char dotUtil[64], dotReset[64];
        snprintf(dotUtil,  sizeof(dotUtil),  "%s.utilization", keys[i]);
        snprintf(dotReset, sizeof(dotReset), "%s.resets_at",   keys[i]);

        double util = json_object_dotget_number(usage, dotUtil);
        int pct = (int)(util + 0.5);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        g_values[i] = pct;

        const char *resetStr = json_object_dotget_string(usage, dotReset);
        DbgLog("Bar %d (%s): util=%.4f pct=%d reset=%s",
               i, keys[i], util, pct, resetStr ? resetStr : "(null)");
        FormatResetTime(resetStr, g_details[i], 64, /* showAbsolute */ i == 1);
    }

    json_value_free(usageRoot);
    DbgLog("=== FetchUsageData done ===");
}

/* ---- background fetch thread ---- */
static DWORD WINAPI FetchThread(LPVOID param) {
    (void)param;
    FetchUsageData();
    PostMessage(g_hWndPopup, WM_FETCHDONE, 0, 0);
    return 0;
}

static void StartAsyncFetch(void) {
    if (g_fetching) return;
    g_fetching = TRUE;
    g_spinnerFrame = 0;
    SetTimer(g_hWndPopup, IDT_SPINNER, 30, NULL);
    InvalidateRect(g_hWndPopup, NULL, FALSE);
    HANDLE hThread = CreateThread(NULL, 0, FetchThread, NULL, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

/* ---- settings persistence (registry) ---- */
#define REG_KEY L"Software\\CLTray"

static void LoadSettings(void) {
    DWORD val, size = sizeof(val);
    if (RegGetValue(HKEY_CURRENT_USER, REG_KEY, L"ShowOnHover",
                    RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS)
        g_showOnHover = (BOOL)val;
    else
        g_showOnHover = TRUE;

    size = sizeof(val);
    if (RegGetValue(HKEY_CURRENT_USER, REG_KEY, L"RoundedCorners",
                    RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS)
        g_roundedCorners = (BOOL)val;
    else {
        /* default ON for Windows 11+ (build >= 22000), OFF otherwise */
        OSVERSIONINFOW ovi = { sizeof(ovi) };
        GetVersionExW(&ovi);
        g_roundedCorners = (ovi.dwBuildNumber >= 22000);
    }

    size = sizeof(val);
    if (RegGetValue(HKEY_CURRENT_USER, REG_KEY, L"ShowSonnet",
                    RRF_RT_REG_DWORD, NULL, &val, &size) == ERROR_SUCCESS)
        g_showSonnet = (BOOL)val;
    else
        g_showSonnet = FALSE;

    g_barCount = g_showSonnet ? 3 : 2;
}

#define POPUP_H_ERR      100

static int PopupH(void) {
    if (g_statusMsg[0]) return POPUP_H_ERR;
    return g_showSonnet ? POPUP_H_3 : POPUP_H_2;
}

static void ApplyRoundedCorners(void) {
    DWORD pref = g_roundedCorners ? 2 /* DWMWCP_ROUND */ : 1 /* DWMWCP_DONOTROUND */;
    DwmSetWindowAttribute(g_hWndPopup, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                          &pref, sizeof(pref));
}

static void UpdateTrayTip(void) {
    if (g_showOnHover)
        g_nid.szTip[0] = L'\0';
    else
        lstrcpy(g_nid.szTip, L"CLTray Monitor");
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

static void SaveSettings(void) {
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0,
                       KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD val = g_showOnHover ? 1 : 0;
        RegSetValueEx(hKey, L"ShowOnHover", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = g_roundedCorners ? 1 : 0;
        RegSetValueEx(hKey, L"RoundedCorners", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        val = g_showSonnet ? 1 : 0;
        RegSetValueEx(hKey, L"ShowSonnet", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
        RegCloseKey(hKey);
    }
    g_barCount = g_showSonnet ? 3 : 2;
    UpdateTrayTip();
    ApplyRoundedCorners();
    /* resize popup */
    SetWindowPos(g_hWndPopup, NULL, 0, 0, POPUP_W, PopupH(),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---- tray icon helpers ---- */
static void TrayAdd(HWND hWnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hWnd;
    g_nid.uID              = ID_TRAYICON;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIcon(g_hInst, MAKEINTRESOURCE(1));
    lstrcpy(g_nid.szTip, L"CLTray Monitor");
    Shell_NotifyIcon(NIM_ADD, &g_nid);
}

static void TrayRemove(void) {
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
}

/* ---- paint the bar graph (horizontal) ---- */
static void PaintBars(HWND hWnd, HDC hdc) {
    RECT rc;
    GetClientRect(hWnd, &rc);

    /* background */
    HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 30));
    FillRect(hdc, &rc, hBg);
    DeleteObject(hBg);

    int cw = rc.right - rc.left;

    int margin   = 12;
    int captionH = 24;
    int labelW   = 84;
    int pctW     = 56;
    int gap      = 4;
    int barH     = 26;
    int detailH  = 16;
    int rowH     = barH + detailH + gap;
    int barLeft  = margin + labelW;
    int barRight = cw - margin - pctW;
    int topY     = margin + captionH + 14;

    SetBkMode(hdc, TRANSPARENT);
    HFONT hFontCaption = CreateFont(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT hFontLabel = CreateFont(20, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT hFontPct = CreateFont(26, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT hFontSmall = CreateFont(13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    /* caption */
    HFONT hOld = SelectObject(hdc, hFontCaption);
    SetTextColor(hdc, RGB(220, 220, 220));
    RECT capRc = { margin, 4, cw - margin, 4 + captionH };
    DrawText(hdc, L"Claude usage monitor", -1, &capRc, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    /* separator line */
    HPEN hPen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_WINDOWFRAME));
    HPEN hOldPen = SelectObject(hdc, hPen);
    MoveToEx(hdc, 0, 4 + captionH + 2, NULL);
    LineTo(hdc, cw, 4 + captionH + 2);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);

    SelectObject(hdc, hFontLabel);

    /* spinner in upper-right while fetching */
    if (g_fetching) {
        int cx = cw - margin - 8;
        int cy = 4 + captionH / 2;
        int r = 6;
        HPEN hSpinPen = CreatePen(PS_SOLID, 2, RGB(180, 180, 180));
        HPEN hOldSpin = SelectObject(hdc, hSpinPen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        int startAngle = g_spinnerFrame * 45;  /* 8 frames × 45° = 360° */
        int sweepAngle = 90;
        int x1 = cx - r, y1 = cy - r, x2 = cx + r, y2 = cy + r;
        /* Arc needs radial endpoints for start/end */
        double pi = 3.14159265358979;
        double sa = startAngle * pi / 180.0;
        double ea = (startAngle + sweepAngle) * pi / 180.0;
        int sx = cx + (int)(r * 2 * cos(sa));
        int sy = cy - (int)(r * 2 * sin(sa));
        int ex = cx + (int)(r * 2 * cos(ea));
        int ey = cy - (int)(r * 2 * sin(ea));
        Arc(hdc, x1, y1, x2, y2, sx, sy, ex, ey);
        SelectObject(hdc, hOldSpin);
        DeleteObject(hSpinPen);
    }

    /* if there's a status message, show it instead of bars */
    if (g_statusMsg[0]) {
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, RGB(180, 180, 180));
        /* measure text height, then center vertically between separator and bottom */
        int sepY = 4 + captionH + 2;
        RECT measure = { margin, 0, cw - margin, 1000 };
        int textH = DrawText(hdc, g_statusMsg, -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        int areaH = rc.bottom - sepY;
        int msgY = sepY + (areaH - textH) / 2;
        RECT msgRc = { margin, msgY, cw - margin, msgY + textH };
        DrawText(hdc, g_statusMsg, -1, &msgRc, DT_CENTER | DT_WORDBREAK);
        SelectObject(hdc, hOld);
        DeleteObject(hFontCaption);
        DeleteObject(hFontLabel);
        DeleteObject(hFontPct);
        DeleteObject(hFontSmall);
        return;
    }

    for (int i = 0; i < g_barCount; i++) {
        int y = topY + i * rowH;

        /* label on the left */
        int lblYOff = (i == 1 || i == 2) ? -6 : 0;
        RECT lblRc = { margin, y + lblYOff, margin + labelW - 10, y + barH + lblYOff };
        SetTextColor(hdc, g_colors[i]);
        DrawText(hdc, g_labels[i], -1, &lblRc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

        /* subtitle "(weekly)" under All models and Sonnet labels */
        if (i == 1 || i == 2) {
            SelectObject(hdc, hFontSmall);
            RECT subRc = { margin, y + barH - 12, margin + labelW - 10, y + barH + detailH - 12 };
            SetTextColor(hdc, RGB(190, 190, 190));
            DrawText(hdc, L"(weekly)", -1, &subRc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(hdc, hFontLabel);
        }

        /* bar background (dark slot) */
        RECT slot = { barLeft, y, barRight, y + barH };
        HBRUSH hSlot = CreateSolidBrush(RGB(55, 55, 55));
        FillRect(hdc, &slot, hSlot);
        DeleteObject(hSlot);

        /* filled portion */
        int barArea = barRight - barLeft;
        int fillW = (int)((barArea * g_values[i]) / 100.0);
        RECT bar = { barLeft, y, barLeft + fillW, y + barH };
        HBRUSH hBar = CreateSolidBrush(g_colors[i]);
        FillRect(hdc, &bar, hBar);
        DeleteObject(hBar);

        /* percentage on the right */
        wchar_t pct[8];
        wsprintfW(pct, L"%d%%", g_values[i]);
        SelectObject(hdc, hFontPct);
        RECT pctRc = { barRight + 2, y, cw - margin - 2, y + barH };
        SetTextColor(hdc, RGB(220, 220, 220));
        DrawText(hdc, pct, -1, &pctRc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, hFontLabel);

        /* detail text below bar */
        if (g_details[i][0]) {
            SelectObject(hdc, hFontSmall);
            RECT detRc = { barLeft, y + barH, barRight, y + barH + detailH };
            SetTextColor(hdc, RGB(190, 190, 190));
            DrawText(hdc, g_details[i], -1, &detRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            SelectObject(hdc, hFontLabel);
        }
    }

    SelectObject(hdc, hOld);
    DeleteObject(hFontCaption);
    DeleteObject(hFontLabel);
    DeleteObject(hFontPct);
    DeleteObject(hFontSmall);
}

/* ---- position popup near tray area ---- */
static void ShowPopupNearTray(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);

    int x = pt.x - POPUP_W / 2;
    int y;

    /* detect which edge the taskbar is on and anchor accordingly */
    if (pt.y >= mi.rcWork.bottom)        /* taskbar at bottom */
        y = mi.rcWork.bottom - PopupH() - 4;
    else if (pt.y <= mi.rcWork.top)      /* taskbar at top */
        y = mi.rcWork.top + 4;
    else if (pt.x >= mi.rcWork.right)    /* taskbar at right */
        y = pt.y - PopupH() / 2;
    else                                 /* taskbar at left or fallback */
        y = pt.y - PopupH() / 2;

    /* clamp x to work area */
    if (x + POPUP_W > mi.rcWork.right)  x = mi.rcWork.right  - POPUP_W - 4;
    if (x < mi.rcWork.left)             x = mi.rcWork.left + 4;

    /* clamp y to work area */
    if (y < mi.rcWork.top)              y = mi.rcWork.top + 4;
    if (y + PopupH() > mi.rcWork.bottom) y = mi.rcWork.bottom - PopupH() - 4;

    SetWindowPos(hWnd, HWND_TOPMOST, x, y, POPUP_W, PopupH(),
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(hWnd);

    /* fetch in background — spinner will animate until done */
    StartAsyncFetch();

    SetTimer(hWnd, IDT_UPDATE, 60000, NULL);
}

/* ---- popup window proc ---- */
static LRESULT CALLBACK PopupProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = SelectObject(memDC, memBmp);
        PaintBars(hWnd, memDC);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == IDT_UPDATE) {
            StartAsyncFetch();
        } else if (wParam == IDT_SPINNER) {
            g_spinnerFrame = (g_spinnerFrame + 1) % 8;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_FETCHDONE:
        g_fetching = FALSE;
        KillTimer(hWnd, IDT_SPINNER);
        /* resize popup in case status changed (error vs normal) */
        SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, POPUP_W, PopupH(),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        StartAsyncFetch();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            /* hide when user clicks away */
            ShowWindow(hWnd, SW_HIDE);
            KillTimer(hWnd, IDT_UPDATE);
            KillTimer(hWnd, IDT_SPINNER);
        }
        return 0;
    case WM_KILLFOCUS:
        ShowWindow(hWnd, SW_HIDE);
        KillTimer(hWnd, IDT_UPDATE);
        KillTimer(hWnd, IDT_SPINNER);
        return 0;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

/* ---- hidden message-only window proc (owns tray icon) ---- */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            if (IsWindowVisible(g_hWndPopup)) {
                ShowWindow(g_hWndPopup, SW_HIDE);
                KillTimer(g_hWndPopup, IDT_UPDATE);
            } else {
                ShowPopupNearTray(g_hWndPopup);
            }
        } else if (lParam == WM_MOUSEMOVE) {
            if (g_showOnHover && !g_menuOpen && !IsWindowVisible(g_hWndPopup)) {
                ShowPopupNearTray(g_hWndPopup);
            }
        } else if (lParam == WM_RBUTTONUP) {
            /* hide popup and suppress hover while menu is open */
            ShowWindow(g_hWndPopup, SW_HIDE);
            KillTimer(g_hWndPopup, IDT_UPDATE);
            g_menuOpen = TRUE;

            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING | (g_showOnHover ? MF_CHECKED : 0),
                       ID_HOVER, L"Show on hover");
            AppendMenu(hMenu, MF_STRING | (g_roundedCorners ? MF_CHECKED : 0),
                       ID_ROUNDED, L"Rounded corners");
            AppendMenu(hMenu, MF_STRING | (g_showSonnet ? MF_CHECKED : 0),
                       ID_SONNET, L"Show Sonnet bar");
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_EXIT, L"Exit");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);
            g_menuOpen = FALSE;
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_HOVER) {
            g_showOnHover = !g_showOnHover;
            SaveSettings();
        } else if (LOWORD(wParam) == ID_ROUNDED) {
            g_roundedCorners = !g_roundedCorners;
            SaveSettings();
        } else if (LOWORD(wParam) == ID_SONNET) {
            g_showSonnet = !g_showSonnet;
            SaveSettings();
        } else if (LOWORD(wParam) == ID_EXIT) {
            PostQuitMessage(0);
        }
        return 0;
    case WM_DESTROY:
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    (void)hPrev; (void)nShow;
    g_hInst = hInstance;

    if (lpCmd && wcsstr(lpCmd, L"--debug"))
        g_debug = TRUE;

    EnableDarkMode();
    LoadSettings();

    /* register hidden owner window class */
    WNDCLASS wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = L"CLTrayOwner";
    RegisterClass(&wc);

    HWND hWndOwner = CreateWindowEx(0, L"CLTrayOwner", L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    AllowDarkForWindow(hWndOwner);

    /* register popup window class */
    WNDCLASS pc = {0};
    pc.lpfnWndProc   = PopupProc;
    pc.hInstance      = hInstance;
    pc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    pc.hbrBackground  = CreateSolidBrush(RGB(30, 30, 30));
    pc.lpszClassName  = L"CLTrayPopup";
    RegisterClass(&pc);

    g_hWndPopup = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"CLTrayPopup", L"CLTray Monitor",
        WS_POPUP | WS_BORDER,
        0, 0, POPUP_W, PopupH(),
        NULL, NULL, hInstance, NULL);

    ApplyRoundedCorners();

    TrayAdd(hWndOwner);
    UpdateTrayTip();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
