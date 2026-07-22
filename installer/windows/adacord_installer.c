#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>

#include "resource.h"

#define APP_NAME L"Adacord Installer"
#define REPO_URL L"https://github.com/Rowtha/adacord.git"
#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_STAGE (WM_APP + 2)
#define WM_APP_FINISHED (WM_APP + 3)
#define INSTALL_PATH_CAPACITY (MAX_PATH * 4)
#define MAX_LOCAL_APP_DATA_ROOTS 64

#define ID_PATH 1001
#define ID_CLOSE_DISCORD 1003
#define ID_INSTALL 1004
#define ID_CANCEL 1005
#define ID_LOG 1006
#define ID_PROGRESS 1007
#define ID_UNINJECT 1008

enum InstallResult {
    INSTALL_FAILED = 0,
    INSTALL_SUCCEEDED = 1,
    INSTALL_CANCELLED = 2
};

enum InstallerOperation {
    OPERATION_INSTALL = 0,
    OPERATION_UNINJECT = 1
};

typedef struct WorkerArgs {
    wchar_t target[INSTALL_PATH_CAPACITY];
    BOOL closeDiscord;
    int operation;
} WorkerArgs;

typedef struct DiscordVariant {
    const wchar_t *branch;
    const wchar_t *directory;
    const wchar_t *executable;
} DiscordVariant;

typedef struct DiscordInstall {
    wchar_t path[INSTALL_PATH_CAPACITY];
    wchar_t resources[INSTALL_PATH_CAPACITY];
    const DiscordVariant *variant;
} DiscordInstall;

typedef struct FinishMessage {
    int result;
    wchar_t message[768];
} FinishMessage;

static const DiscordVariant DISCORD_VARIANTS[] = {
    { L"Stable", L"Discord", L"Discord.exe" },
    { L"PTB", L"DiscordPTB", L"DiscordPTB.exe" },
    { L"Canary", L"DiscordCanary", L"DiscordCanary.exe" },
    { L"Development", L"DiscordDevelopment", L"DiscordDevelopment.exe" }
};

static HINSTANCE g_instance;
static HWND g_window;
static HWND g_path;
static HWND g_closeDiscord;
static HWND g_install;
static HWND g_uninject;
static HWND g_cancel;
static HWND g_log;
static HWND g_progress;
static HWND g_status;
static HWND g_steps[4];
static HFONT g_titleFont;
static HFONT g_bodyFont;
static HFONT g_smallFont;
static HFONT g_monoFont;
static HBRUSH g_backgroundBrush;
static HBRUSH g_headerBrush;
static HBRUSH g_logBrush;
static HANDLE g_worker;
static HANDLE g_job;
static CRITICAL_SECTION g_jobLock;
static volatile LONG g_cancelRequested;
static BOOL g_running;
static int g_activeOperation = OPERATION_INSTALL;

static const COLORREF THEME_BACKGROUND = RGB(247, 247, 250);
static const COLORREF THEME_HEADER = RGB(35, 32, 38);
static const COLORREF THEME_TEXT = RGB(38, 35, 42);
static const COLORREF THEME_MUTED = RGB(105, 100, 111);
static const COLORREF THEME_ACCENT = RGB(218, 113, 143);
static const COLORREF THEME_LOG = RGB(27, 26, 30);

static void post_log_text(const wchar_t *text) {
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (!copy) return;
    memcpy(copy, text, (length + 1) * sizeof(wchar_t));
    if (!PostMessageW(g_window, WM_APP_LOG, 0, (LPARAM)copy)) free(copy);
}

static void post_log(const wchar_t *format, ...) {
    wchar_t buffer[4096];
    va_list args;
    va_start(args, format);
    _vsnwprintf(buffer, ARRAYSIZE(buffer) - 3, format, args);
    va_end(args);
    buffer[ARRAYSIZE(buffer) - 3] = L'\0';
    wcscat(buffer, L"\r\n");
    post_log_text(buffer);
}

static void post_stage(int stage, int progress, const wchar_t *status) {
    wchar_t *copy = _wcsdup(status);
    if (!copy) return;
    if (!PostMessageW(g_window, WM_APP_STAGE, MAKEWPARAM(stage, progress), (LPARAM)copy)) free(copy);
}

static void post_finished(int result, const wchar_t *message) {
    FinishMessage *finish = (FinishMessage *)calloc(1, sizeof(FinishMessage));
    if (!finish) return;
    finish->result = result;
    wcsncpy(finish->message, message, ARRAYSIZE(finish->message) - 1);
    if (!PostMessageW(g_window, WM_APP_FINISHED, 0, (LPARAM)finish)) free(finish);
}

static BOOL is_cancelled(void) {
    return InterlockedCompareExchange(&g_cancelRequested, 0, 0) != 0;
}

static void strip_ansi(wchar_t *text) {
    wchar_t *read = text;
    wchar_t *write = text;

    while (*read) {
        if (*read == 0x1b && read[1] == L'[') {
            read += 2;
            while (*read && !((*read >= L'A' && *read <= L'Z') || (*read >= L'a' && *read <= L'z'))) {
                read++;
            }
            if (*read) read++;
            continue;
        }
        *write++ = *read++;
    }
    *write = L'\0';
}

static void append_capture(wchar_t *capture, size_t captureCount, const wchar_t *chunk) {
    if (!capture || captureCount == 0) return;
    size_t used = wcslen(capture);
    if (used >= captureCount - 1) return;
    wcsncat(capture, chunk, captureCount - used - 1);
}

static wchar_t *quote_argument(const wchar_t *argument) {
    size_t length = wcslen(argument);
    size_t capacity = length * 2 + 3;
    wchar_t *quoted = (wchar_t *)calloc(capacity, sizeof(wchar_t));
    wchar_t *out = quoted;
    size_t slashes = 0;

    if (!quoted) return NULL;
    *out++ = L'"';

    for (const wchar_t *cursor = argument;; cursor++) {
        if (*cursor == L'\\') {
            slashes++;
            continue;
        }

        if (*cursor == L'"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) *out++ = L'\\';
            *out++ = L'"';
            slashes = 0;
            continue;
        }

        if (*cursor == L'\0') {
            for (size_t i = 0; i < slashes * 2; i++) *out++ = L'\\';
            break;
        }

        for (size_t i = 0; i < slashes; i++) *out++ = L'\\';
        slashes = 0;
        *out++ = *cursor;
    }

    *out++ = L'"';
    *out = L'\0';
    return quoted;
}

static wchar_t *build_native_command_line(const wchar_t *application, int argumentCount, const wchar_t **arguments) {
    size_t capacity = (wcslen(application) + 3) * 2;
    for (int i = 0; i < argumentCount; i++) capacity += wcslen(arguments[i]) * 2 + 4;

    wchar_t *commandLine = (wchar_t *)calloc(capacity, sizeof(wchar_t));
    wchar_t *quotedApplication = quote_argument(application);
    if (!commandLine || !quotedApplication) {
        free(commandLine);
        free(quotedApplication);
        return NULL;
    }

    wcscpy(commandLine, quotedApplication);
    free(quotedApplication);

    for (int i = 0; i < argumentCount; i++) {
        wchar_t *quoted = quote_argument(arguments[i]);
        if (!quoted) {
            free(commandLine);
            return NULL;
        }
        wcscat(commandLine, L" ");
        wcscat(commandLine, quoted);
        free(quoted);
    }

    return commandLine;
}

static BOOL run_process(
    const wchar_t *application,
    wchar_t *commandLine,
    const wchar_t *workingDirectory,
    wchar_t *capture,
    size_t captureCount,
    DWORD *exitCode
) {
    SECURITY_ATTRIBUTES security = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE readPipe = NULL;
    HANDLE writePipe = NULL;
    HANDLE nullInput = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process = { 0 };
    STARTUPINFOW startup = { 0 };
    BOOL started = FALSE;
    BOOL assigned = FALSE;

    if (capture && captureCount) capture[0] = L'\0';

    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        post_log(L"Could not create the process output pipe (error %lu).", GetLastError());
        goto cleanup;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    nullInput = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = nullInput;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;

    started = CreateProcessW(
        application,
        commandLine,
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        NULL,
        workingDirectory,
        &startup,
        &process
    );

    if (!started) {
        post_log(L"Could not start %ls (error %lu).", application, GetLastError());
        goto cleanup;
    }

    EnterCriticalSection(&g_jobLock);
    if (g_job) assigned = AssignProcessToJobObject(g_job, process.hProcess);
    LeaveCriticalSection(&g_jobLock);

    if (!assigned) {
        post_log(L"Warning: child process cancellation may be incomplete (error %lu).", GetLastError());
    }

    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    process.hThread = NULL;
    CloseHandle(writePipe);
    writePipe = NULL;
    if (nullInput != INVALID_HANDLE_VALUE) {
        CloseHandle(nullInput);
        nullInput = INVALID_HANDLE_VALUE;
    }

    char bytes[4096];
    DWORD bytesRead;
    while (ReadFile(readPipe, bytes, sizeof(bytes) - 1, &bytesRead, NULL) && bytesRead > 0) {
        bytes[bytesRead] = '\0';
        int wideLength = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)bytesRead, NULL, 0);
        UINT codePage = CP_UTF8;
        if (wideLength <= 0) {
            codePage = CP_OEMCP;
            wideLength = MultiByteToWideChar(codePage, 0, bytes, (int)bytesRead, NULL, 0);
        }
        if (wideLength <= 0) continue;

        wchar_t *wide = (wchar_t *)calloc((size_t)wideLength + 1, sizeof(wchar_t));
        if (!wide) continue;
        MultiByteToWideChar(codePage, 0, bytes, (int)bytesRead, wide, wideLength);
        strip_ansi(wide);
        post_log_text(wide);
        append_capture(capture, captureCount, wide);
        free(wide);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    if (exitCode) GetExitCodeProcess(process.hProcess, exitCode);

cleanup:
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (readPipe) CloseHandle(readPipe);
    if (writePipe) CloseHandle(writePipe);
    if (nullInput != INVALID_HANDLE_VALUE) CloseHandle(nullInput);
    return started;
}

static BOOL run_native(
    const wchar_t *application,
    int argumentCount,
    const wchar_t **arguments,
    const wchar_t *workingDirectory,
    wchar_t *capture,
    size_t captureCount,
    DWORD *exitCode
) {
    wchar_t *commandLine = build_native_command_line(application, argumentCount, arguments);
    if (!commandLine) {
        post_log(L"Out of memory while preparing a command.");
        return FALSE;
    }

    BOOL started = run_process(
        application,
        commandLine,
        workingDirectory,
        capture,
        captureCount,
        exitCode
    );
    free(commandLine);
    return started;
}

static BOOL run_pnpm(
    const wchar_t *pnpmPath,
    BOOL pnpmIsCmd,
    const wchar_t *command,
    const wchar_t *workingDirectory,
    DWORD *exitCode
) {
    if (!pnpmIsCmd) {
        const wchar_t *arguments[] = { command };
        return run_native(pnpmPath, 1, arguments, workingDirectory, NULL, 0, exitCode);
    }

    wchar_t cmdPath[MAX_PATH];
    UINT systemLength = GetSystemDirectoryW(cmdPath, ARRAYSIZE(cmdPath));
    if (!systemLength || systemLength + 9 >= ARRAYSIZE(cmdPath)) {
        post_log(L"Could not locate cmd.exe.");
        return FALSE;
    }
    wcscat(cmdPath, L"\\cmd.exe");

    size_t commandLineLength = wcslen(cmdPath) + wcslen(pnpmPath) + wcslen(command) + 32;
    wchar_t *commandLine = (wchar_t *)calloc(commandLineLength, sizeof(wchar_t));
    if (!commandLine) return FALSE;

    _snwprintf(
        commandLine,
        commandLineLength - 1,
        L"\"%ls\" /D /S /C \"\"%ls\" %ls\"",
        cmdPath,
        pnpmPath,
        command
    );
    BOOL started = run_process(
        cmdPath,
        commandLine,
        workingDirectory,
        NULL,
        0,
        exitCode
    );
    free(commandLine);
    return started;
}

static BOOL locate_program(const wchar_t *name, wchar_t *path, DWORD pathCount) {
    DWORD result = SearchPathW(NULL, name, NULL, pathCount, path, NULL);
    return result > 0 && result < pathCount;
}

static BOOL path_is_directory(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL path_exists(const wchar_t *path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL copy_path(wchar_t *destination, size_t destinationCount, const wchar_t *source) {
    size_t length = wcslen(source);
    if (length >= destinationCount) return FALSE;
    memcpy(destination, source, (length + 1) * sizeof(wchar_t));
    return TRUE;
}

static BOOL join_path(
    wchar_t *destination,
    size_t destinationCount,
    const wchar_t *left,
    const wchar_t *right
) {
    if (!left[0] || !right[0]) return FALSE;

    size_t leftLength = wcslen(left);
    BOOL leftHasSlash = left[leftLength - 1] == L'\\' || left[leftLength - 1] == L'/';
    BOOL rightHasSlash = right[0] == L'\\' || right[0] == L'/';
    const wchar_t *separator = leftHasSlash || rightHasSlash ? L"" : L"\\";
    if (leftHasSlash && rightHasSlash) right++;

    int length = _snwprintf(
        destination,
        destinationCount,
        L"%ls%ls%ls",
        left,
        separator,
        right
    );
    destination[destinationCount - 1] = L'\0';
    return length >= 0 && (size_t)length < destinationCount;
}

static BOOL resolve_discord_install(
    const wchar_t *installPath,
    const DiscordVariant *variant,
    DiscordInstall *install
) {
    wchar_t pattern[INSTALL_PATH_CAPACITY];
    wchar_t appPath[INSTALL_PATH_CAPACITY];
    wchar_t resourcesPath[INSTALL_PATH_CAPACITY];
    wchar_t executablePath[INSTALL_PATH_CAPACITY];
    wchar_t latestApp[MAX_PATH] = L"";
    wchar_t latestResources[INSTALL_PATH_CAPACITY] = L"";
    WIN32_FIND_DATAW entry;

    if (!path_is_directory(installPath) ||
        !join_path(pattern, ARRAYSIZE(pattern), installPath, L"app-*")) {
        return FALSE;
    }

    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return FALSE;

    do {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            _wcsnicmp(entry.cFileName, L"app-", 4) != 0 ||
            !join_path(appPath, ARRAYSIZE(appPath), installPath, entry.cFileName) ||
            !join_path(resourcesPath, ARRAYSIZE(resourcesPath), appPath, L"resources") ||
            !join_path(executablePath, ARRAYSIZE(executablePath), appPath, variant->executable) ||
            !path_is_directory(resourcesPath) ||
            !path_exists(executablePath)) {
            continue;
        }

        if (!latestApp[0] || _wcsicmp(entry.cFileName, latestApp) > 0) {
            if (!copy_path(latestApp, ARRAYSIZE(latestApp), entry.cFileName) ||
                !copy_path(latestResources, ARRAYSIZE(latestResources), resourcesPath)) {
                continue;
            }
        }
    } while (FindNextFileW(find, &entry));

    FindClose(find);
    if (!latestApp[0]) return FALSE;

    if (!copy_path(install->path, ARRAYSIZE(install->path), installPath) ||
        !copy_path(install->resources, ARRAYSIZE(install->resources), latestResources)) {
        return FALSE;
    }
    install->variant = variant;
    return TRUE;
}

static BOOL find_running_discord(DiscordInstall *install) {
    for (size_t variantIndex = 0; variantIndex < ARRAYSIZE(DISCORD_VARIANTS); variantIndex++) {
        const DiscordVariant *variant = &DISCORD_VARIANTS[variantIndex];
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) continue;

        PROCESSENTRY32W entry = { 0 };
        entry.dwSize = sizeof(entry);
        BOOL hasEntry = Process32FirstW(snapshot, &entry);

        while (hasEntry) {
            if (_wcsicmp(entry.szExeFile, variant->executable) == 0) {
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (process) {
                    wchar_t imagePath[INSTALL_PATH_CAPACITY];
                    DWORD imagePathLength = ARRAYSIZE(imagePath);
                    if (QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLength)) {
                        wchar_t *slash = wcsrchr(imagePath, L'\\');
                        if (slash) {
                            *slash = L'\0';
                            slash = wcsrchr(imagePath, L'\\');
                            if (slash) {
                                *slash = L'\0';
                                if (resolve_discord_install(imagePath, variant, install)) {
                                    CloseHandle(process);
                                    CloseHandle(snapshot);
                                    return TRUE;
                                }
                            }
                        }
                    }
                    CloseHandle(process);
                }
            }
            hasEntry = Process32NextW(snapshot, &entry);
        }

        CloseHandle(snapshot);
    }
    return FALSE;
}

static BOOL add_local_app_data_root(
    wchar_t roots[][INSTALL_PATH_CAPACITY],
    size_t *rootCount,
    const wchar_t *path
) {
    if (!path || !path[0] || *rootCount >= MAX_LOCAL_APP_DATA_ROOTS) return FALSE;

    wchar_t normalized[INSTALL_PATH_CAPACITY];
    if (!copy_path(normalized, ARRAYSIZE(normalized), path)) return FALSE;

    size_t length = wcslen(normalized);
    while (length > 3 && (normalized[length - 1] == L'\\' || normalized[length - 1] == L'/')) {
        normalized[--length] = L'\0';
    }

    for (size_t i = 0; i < *rootCount; i++) {
        if (_wcsicmp(roots[i], normalized) == 0) return TRUE;
    }

    if (!copy_path(roots[*rootCount], INSTALL_PATH_CAPACITY, normalized)) return FALSE;
    (*rootCount)++;
    return TRUE;
}

static BOOL get_shell_local_app_data(wchar_t *path, size_t pathCount) {
    HWND shellWindow = GetShellWindow();
    if (!shellWindow) return FALSE;

    DWORD processId = 0;
    GetWindowThreadProcessId(shellWindow, &processId);
    if (!processId) return FALSE;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return FALSE;

    HANDLE token = NULL;
    BOOL openedToken = OpenProcessToken(process, TOKEN_QUERY | TOKEN_IMPERSONATE, &token);
    CloseHandle(process);
    if (!openedToken) return FALSE;

    HRESULT result = SHGetFolderPathW(
        NULL,
        CSIDL_LOCAL_APPDATA,
        token,
        SHGFP_TYPE_CURRENT,
        path
    );
    CloseHandle(token);
    if (FAILED(result)) return FALSE;

    path[pathCount - 1] = L'\0';
    return path[0] != L'\0';
}

static void collect_profile_local_app_data_roots(
    wchar_t roots[][INSTALL_PATH_CAPACITY],
    size_t *rootCount
) {
    HKEY profilesKey = NULL;
    LONG openResult = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
        0,
        KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_WOW64_64KEY,
        &profilesKey
    );
    if (openResult != ERROR_SUCCESS) return;

    for (DWORD index = 0; *rootCount < MAX_LOCAL_APP_DATA_ROOTS; index++) {
        wchar_t subkeyName[256];
        DWORD subkeyLength = ARRAYSIZE(subkeyName);
        LONG enumResult = RegEnumKeyExW(
            profilesKey,
            index,
            subkeyName,
            &subkeyLength,
            NULL,
            NULL,
            NULL,
            NULL
        );
        if (enumResult == ERROR_NO_MORE_ITEMS) break;
        if (enumResult != ERROR_SUCCESS) continue;

        HKEY profileKey = NULL;
        if (RegOpenKeyExW(profilesKey, subkeyName, 0, KEY_QUERY_VALUE, &profileKey) != ERROR_SUCCESS) {
            continue;
        }

        wchar_t profilePath[INSTALL_PATH_CAPACITY];
        DWORD profilePathBytes = sizeof(profilePath);
        DWORD profilePathType = 0;
        LONG queryResult = RegQueryValueExW(
            profileKey,
            L"ProfileImagePath",
            NULL,
            &profilePathType,
            (BYTE *)profilePath,
            &profilePathBytes
        );
        RegCloseKey(profileKey);
        if (queryResult != ERROR_SUCCESS ||
            (profilePathType != REG_SZ && profilePathType != REG_EXPAND_SZ)) {
            continue;
        }

        profilePath[ARRAYSIZE(profilePath) - 1] = L'\0';
        wchar_t expandedProfilePath[INSTALL_PATH_CAPACITY];
        const wchar_t *resolvedProfilePath = profilePath;
        if (profilePathType == REG_EXPAND_SZ) {
            DWORD expandedLength = ExpandEnvironmentStringsW(
                profilePath,
                expandedProfilePath,
                ARRAYSIZE(expandedProfilePath)
            );
            if (!expandedLength || expandedLength > ARRAYSIZE(expandedProfilePath)) continue;
            resolvedProfilePath = expandedProfilePath;
        }

        wchar_t localAppData[INSTALL_PATH_CAPACITY];
        if (join_path(
            localAppData,
            ARRAYSIZE(localAppData),
            resolvedProfilePath,
            L"AppData\\Local"
        )) {
            add_local_app_data_root(roots, rootCount, localAppData);
        }
    }

    RegCloseKey(profilesKey);
}

static BOOL find_discord_under_root(const wchar_t *root, DiscordInstall *install) {
    for (size_t i = 0; i < ARRAYSIZE(DISCORD_VARIANTS); i++) {
        wchar_t installPath[INSTALL_PATH_CAPACITY];
        if (join_path(
            installPath,
            ARRAYSIZE(installPath),
            root,
            DISCORD_VARIANTS[i].directory
        ) && resolve_discord_install(installPath, &DISCORD_VARIANTS[i], install)) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL find_discord_install(DiscordInstall *install) {
    if (find_running_discord(install)) return TRUE;

    wchar_t roots[MAX_LOCAL_APP_DATA_ROOTS][INSTALL_PATH_CAPACITY];
    size_t rootCount = 0;
    wchar_t path[INSTALL_PATH_CAPACITY];

    if (get_shell_local_app_data(path, ARRAYSIZE(path))) {
        add_local_app_data_root(roots, &rootCount, path);
    }

    if (SUCCEEDED(SHGetFolderPathW(
        NULL,
        CSIDL_LOCAL_APPDATA,
        NULL,
        SHGFP_TYPE_CURRENT,
        path
    ))) {
        add_local_app_data_root(roots, &rootCount, path);
    }

    DWORD environmentLength = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        path,
        ARRAYSIZE(path)
    );
    if (environmentLength > 0 && environmentLength < ARRAYSIZE(path)) {
        add_local_app_data_root(roots, &rootCount, path);
    }

    environmentLength = GetEnvironmentVariableW(L"USERPROFILE", path, ARRAYSIZE(path));
    if (environmentLength > 0 && environmentLength < ARRAYSIZE(path)) {
        wchar_t localAppData[INSTALL_PATH_CAPACITY];
        if (join_path(localAppData, ARRAYSIZE(localAppData), path, L"AppData\\Local")) {
            add_local_app_data_root(roots, &rootCount, localAppData);
        }
    }

    collect_profile_local_app_data_roots(roots, &rootCount);
    for (size_t i = 0; i < rootCount; i++) {
        if (find_discord_under_root(roots[i], install)) return TRUE;
    }

    const wchar_t *machineRoots[] = {
        L"ProgramFiles",
        L"ProgramW6432",
        L"ProgramFiles(x86)",
        L"ProgramData"
    };
    for (size_t i = 0; i < ARRAYSIZE(machineRoots); i++) {
        environmentLength = GetEnvironmentVariableW(
            machineRoots[i],
            path,
            ARRAYSIZE(path)
        );
        if (environmentLength > 0 &&
            environmentLength < ARRAYSIZE(path) &&
            find_discord_under_root(path, install)) {
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL discord_patch_state_matches(const DiscordInstall *install, BOOL shouldBePatched) {
    DiscordInstall current;
    if (!resolve_discord_install(install->path, install->variant, &current)) return FALSE;

    wchar_t appAsar[INSTALL_PATH_CAPACITY];
    wchar_t originalAsar[INSTALL_PATH_CAPACITY];
    if (!join_path(appAsar, ARRAYSIZE(appAsar), current.resources, L"app.asar") ||
        !join_path(originalAsar, ARRAYSIZE(originalAsar), current.resources, L"_app.asar")) {
        return FALSE;
    }

    BOOL hasAppAsar = path_exists(appAsar);
    BOOL hasOriginalAsar = path_exists(originalAsar);
    return shouldBePatched
        ? hasAppAsar && hasOriginalAsar
        : hasAppAsar && !hasOriginalAsar;
}

static BOOL run_adacord_injector(
    const wchar_t *nodePath,
    const wchar_t *workingDirectory,
    const wchar_t *operation,
    const wchar_t *discordPath,
    DWORD *exitCode
) {
    wchar_t scriptPath[INSTALL_PATH_CAPACITY];
    if (!join_path(
        scriptPath,
        ARRAYSIZE(scriptPath),
        workingDirectory,
        L"scripts\\runInstaller.mjs"
    ) || !path_exists(scriptPath)) {
        post_log(L"Could not locate scripts\\runInstaller.mjs.");
        return FALSE;
    }

    const wchar_t *arguments[] = {
        scriptPath,
        L"--",
        operation,
        L"--location",
        discordPath
    };
    return run_native(
        nodePath,
        ARRAYSIZE(arguments),
        arguments,
        workingDirectory,
        NULL,
        0,
        exitCode
    );
}

static BOOL directory_is_empty(const wchar_t *path) {
    wchar_t pattern[MAX_PATH * 4];
    WIN32_FIND_DATAW entry;
    HANDLE find;

    _snwprintf(pattern, ARRAYSIZE(pattern) - 1, L"%ls\\*", path);
    find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return TRUE;

    do {
        if (wcscmp(entry.cFileName, L".") && wcscmp(entry.cFileName, L"..")) {
            FindClose(find);
            return FALSE;
        }
    } while (FindNextFileW(find, &entry));

    FindClose(find);
    return TRUE;
}

static BOOL ensure_parent_directory(const wchar_t *target) {
    wchar_t parent[MAX_PATH * 4];
    wcsncpy(parent, target, ARRAYSIZE(parent) - 1);
    parent[ARRAYSIZE(parent) - 1] = L'\0';

    wchar_t *slash = wcsrchr(parent, L'\\');
    if (!slash) slash = wcsrchr(parent, L'/');
    if (!slash) return TRUE;
    if (slash == parent + 2 && parent[1] == L':') slash++;
    *slash = L'\0';
    if (!parent[0] || path_is_directory(parent)) return TRUE;

    int result = SHCreateDirectoryExW(NULL, parent, NULL);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

static BOOL repository_matches(const wchar_t *remote) {
    wchar_t normalized[2048];
    wcsncpy(normalized, remote, ARRAYSIZE(normalized) - 1);
    normalized[ARRAYSIZE(normalized) - 1] = L'\0';
    CharLowerBuffW(normalized, (DWORD)wcslen(normalized));
    return wcsstr(normalized, L"rowtha/adacord") != NULL;
}

static BOOL create_worker_job(void) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { 0 };
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return FALSE;

    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        return FALSE;
    }

    EnterCriticalSection(&g_jobLock);
    g_job = job;
    LeaveCriticalSection(&g_jobLock);
    return TRUE;
}

static void close_worker_job(void) {
    EnterCriticalSection(&g_jobLock);
    HANDLE job = g_job;
    g_job = NULL;
    LeaveCriticalSection(&g_jobLock);
    if (job) CloseHandle(job);
}

static void fail_worker(const wchar_t *message) {
    post_log(L"\r\nERROR: %ls", message);
    post_finished(INSTALL_FAILED, message);
}

static void close_discord_processes(DWORD *exitCode) {
    wchar_t taskkillPath[MAX_PATH * 4];
    if (!locate_program(L"taskkill.exe", taskkillPath, ARRAYSIZE(taskkillPath))) return;

    post_log(L"\r\nClosing running Discord processes...");
    for (size_t i = 0; i < ARRAYSIZE(DISCORD_VARIANTS); i++) {
        const wchar_t *killArgs[] = { L"/F", L"/T", L"/IM", DISCORD_VARIANTS[i].executable };
        run_native(taskkillPath, 4, killArgs, NULL, NULL, 0, exitCode);
    }
}

static DWORD WINAPI operation_worker(LPVOID parameter) {
    WorkerArgs *args = (WorkerArgs *)parameter;
    wchar_t gitPath[MAX_PATH * 4];
    wchar_t nodePath[MAX_PATH * 4];
    wchar_t pnpmPath[MAX_PATH * 4];
    BOOL pnpmIsCmd = FALSE;
    DWORD exitCode = 1;
    wchar_t capture[4096];
    wchar_t gitDirectory[MAX_PATH * 4];
    wchar_t packageJson[MAX_PATH * 4];
    DiscordInstall discord;

    if (!create_worker_job()) {
        free(args);
        fail_worker(L"Could not create the installer process group.");
        return 1;
    }

    if (args->operation == OPERATION_UNINJECT) {
        post_log(L"Adacord Uninject");
        post_log(L"Target: %ls", args->target);
        post_log(L"");
        post_stage(1, 10, L"Locating the existing installation...");

        _snwprintf(packageJson, ARRAYSIZE(packageJson) - 1, L"%ls\\package.json", args->target);
        if (GetFileAttributesW(packageJson) == INVALID_FILE_ATTRIBUTES) {
            fail_worker(L"No Adacord installation was found beside this executable. Install Adacord first.");
            goto done;
        }

        post_stage(2, 25, L"Checking prerequisites...");
        if (!locate_program(L"node.exe", nodePath, ARRAYSIZE(nodePath))) {
            fail_worker(L"Node.js was not found on PATH. Install Node.js 22 or newer, then try again.");
            goto done;
        }

        post_log(L"$ node --version");
        const wchar_t *nodeVersionArgs[] = { L"--version" };
        if (!run_native(nodePath, 1, nodeVersionArgs, NULL, capture, ARRAYSIZE(capture), &exitCode) || exitCode != 0) {
            if (is_cancelled()) goto cancelled;
            fail_worker(L"Node.js is installed but could not be started.");
            goto done;
        }
        wchar_t *versionStart = capture;
        while (*versionStart && (*versionStart < L'0' || *versionStart > L'9')) versionStart++;
        if (wcstol(versionStart, NULL, 10) < 22) {
            fail_worker(L"Adacord requires Node.js 22 or newer.");
            goto done;
        }

        post_stage(2, 35, L"Locating Discord...");
        if (!find_discord_install(&discord)) {
            fail_worker(
                L"No Discord desktop installation was found for the active Windows user or in the standard install locations."
            );
            goto done;
        }
        post_log(L"Discord: %ls", discord.variant->branch);
        post_log(L"Discord path: %ls", discord.path);

        if (is_cancelled()) goto cancelled;

        post_stage(3, 55, args->closeDiscord ? L"Closing Discord..." : L"Preparing to uninject...");
        if (args->closeDiscord) close_discord_processes(&exitCode);

        if (is_cancelled()) goto cancelled;

        post_stage(4, 75, L"Removing Adacord from Discord...");
        post_log(
            L"\r\n$ node scripts\\runInstaller.mjs -- --uninstall --location \"%ls\"",
            discord.path
        );
        if (!run_adacord_injector(
            nodePath,
            args->target,
            L"--uninstall",
            discord.path,
            &exitCode
        ) || exitCode != 0) {
            if (is_cancelled()) goto cancelled;
            fail_worker(L"Uninject failed. Make sure Discord is fully closed, then review the log and try again.");
            goto done;
        }
        if (!discord_patch_state_matches(&discord, FALSE)) {
            fail_worker(
                L"The uninjector exited without an error, but Discord still appears to be patched. No success was reported."
            );
            goto done;
        }

        if (is_cancelled()) goto cancelled;

        post_stage(5, 100, L"Uninject complete");
        post_log(L"\r\nSUCCESS: Adacord was removed from Discord. Restart Discord to finish.");
        post_finished(INSTALL_SUCCEEDED, L"Adacord was uninjected successfully.\r\n\r\nRestart Discord now to finish.");
        goto done;
    }

    post_log(L"Adacord Installer");
    post_log(L"Target: %ls", args->target);
    post_log(L"Repository: %ls", REPO_URL);
    post_log(L"");
    post_stage(0, 2, L"Checking prerequisites...");

    if (!locate_program(L"git.exe", gitPath, ARRAYSIZE(gitPath))) {
        fail_worker(L"Git was not found on PATH. Install Git for Windows, then run this installer again.");
        goto done;
    }
    if (!locate_program(L"node.exe", nodePath, ARRAYSIZE(nodePath))) {
        fail_worker(L"Node.js was not found on PATH. Install Node.js 22 or newer, then run this installer again.");
        goto done;
    }
    if (locate_program(L"pnpm.exe", pnpmPath, ARRAYSIZE(pnpmPath))) {
        pnpmIsCmd = FALSE;
    } else if (locate_program(L"pnpm.cmd", pnpmPath, ARRAYSIZE(pnpmPath))) {
        pnpmIsCmd = TRUE;
    } else {
        fail_worker(L"pnpm was not found on PATH. Install it with \"npm install -g pnpm\", then run this installer again.");
        goto done;
    }

    post_log(L"$ git --version");
    const wchar_t *gitVersionArgs[] = { L"--version" };
    if (!run_native(gitPath, 1, gitVersionArgs, NULL, NULL, 0, &exitCode) || exitCode != 0) {
        if (is_cancelled()) goto cancelled;
        fail_worker(L"Git is installed but could not be started.");
        goto done;
    }

    post_log(L"$ node --version");
    const wchar_t *nodeVersionArgs[] = { L"--version" };
    if (!run_native(nodePath, 1, nodeVersionArgs, NULL, capture, ARRAYSIZE(capture), &exitCode) || exitCode != 0) {
        if (is_cancelled()) goto cancelled;
        fail_worker(L"Node.js is installed but could not be started.");
        goto done;
    }
    wchar_t *versionStart = capture;
    while (*versionStart && (*versionStart < L'0' || *versionStart > L'9')) versionStart++;
    long nodeMajor = wcstol(versionStart, NULL, 10);
    if (nodeMajor < 22) {
        fail_worker(L"Adacord requires Node.js 22 or newer.");
        goto done;
    }

    post_log(L"$ pnpm --version");
    if (!run_pnpm(pnpmPath, pnpmIsCmd, L"--version", NULL, &exitCode) || exitCode != 0) {
        if (is_cancelled()) goto cancelled;
        fail_worker(L"pnpm is installed but could not be started.");
        goto done;
    }

    if (is_cancelled()) goto cancelled;

    post_stage(0, 5, L"Locating Discord...");
    if (!find_discord_install(&discord)) {
        fail_worker(
            L"No Discord desktop installation was found for the active Windows user or in the standard install locations."
        );
        goto done;
    }
    post_log(L"Discord: %ls", discord.variant->branch);
    post_log(L"Discord path: %ls", discord.path);

    if (is_cancelled()) goto cancelled;

    _snwprintf(gitDirectory, ARRAYSIZE(gitDirectory) - 1, L"%ls\\.git", args->target);
    post_stage(1, 10, L"Downloading or updating source...");

    if (path_is_directory(gitDirectory)) {
        post_log(L"\r\n$ git -C \"%ls\" remote get-url origin", args->target);
        const wchar_t *remoteArgs[] = { L"-C", args->target, L"remote", L"get-url", L"origin" };
        if (!run_native(gitPath, 5, remoteArgs, NULL, capture, ARRAYSIZE(capture), &exitCode) || exitCode != 0) {
            if (is_cancelled()) goto cancelled;
            fail_worker(L"The existing checkout has no readable origin remote.");
            goto done;
        }
        if (!repository_matches(capture)) {
            fail_worker(L"The selected folder contains a different Git repository. Choose an empty folder or an Adacord checkout.");
            goto done;
        }

        post_log(L"$ git -C \"%ls\" pull --ff-only origin main", args->target);
        const wchar_t *pullArgs[] = { L"-C", args->target, L"pull", L"--ff-only", L"origin", L"main" };
        if (!run_native(gitPath, 6, pullArgs, NULL, NULL, 0, &exitCode) || exitCode != 0) {
            if (is_cancelled()) goto cancelled;
            fail_worker(L"git pull failed. Resolve any local changes shown in the log and try again.");
            goto done;
        }
    } else {
        if (path_is_directory(args->target) && !directory_is_empty(args->target)) {
            fail_worker(L"The selected folder is not empty and is not an Adacord Git checkout.");
            goto done;
        }
        if (!ensure_parent_directory(args->target)) {
            fail_worker(L"Could not create the parent installation folder.");
            goto done;
        }

        post_log(L"\r\n$ git clone --depth=1 %ls \"%ls\"", REPO_URL, args->target);
        const wchar_t *cloneArgs[] = { L"clone", L"--depth=1", REPO_URL, args->target };
        if (!run_native(gitPath, 4, cloneArgs, NULL, NULL, 0, &exitCode) || exitCode != 0) {
            if (is_cancelled()) goto cancelled;
            fail_worker(L"git clone failed. Check the network and the log, then try again.");
            goto done;
        }
    }

    if (is_cancelled()) goto cancelled;

    post_stage(2, 35, L"Installing dependencies...");
    post_log(L"\r\n$ pnpm install");
    if (!run_pnpm(pnpmPath, pnpmIsCmd, L"install", args->target, &exitCode) || exitCode != 0) {
        if (is_cancelled()) goto cancelled;
        fail_worker(L"pnpm install failed. Review the log for the package manager error.");
        goto done;
    }

    if (is_cancelled()) goto cancelled;

    post_stage(3, 58, L"Building Adacord...");
    post_log(L"\r\n$ pnpm build");
    if (!run_pnpm(pnpmPath, pnpmIsCmd, L"build", args->target, &exitCode) || exitCode != 0) {
        if (is_cancelled()) goto cancelled;
        fail_worker(L"pnpm build failed. Review the log for the compiler error.");
        goto done;
    }

    if (is_cancelled()) goto cancelled;

    post_stage(4, 82, L"Injecting into Discord...");
    if (args->closeDiscord) close_discord_processes(&exitCode);

    post_log(
        L"\r\n$ node scripts\\runInstaller.mjs -- --install --location \"%ls\"",
        discord.path
    );
    if (!run_adacord_injector(
        nodePath,
        args->target,
        L"--install",
        discord.path,
        &exitCode
    ) || exitCode != 0) {
        if (is_cancelled()) goto cancelled;
        fail_worker(L"Injection failed. Make sure Discord is fully closed, then review the log and try again.");
        goto done;
    }
    if (!discord_patch_state_matches(&discord, TRUE)) {
        fail_worker(
            L"The injector exited without an error, but the Discord patch files were not found. No success was reported."
        );
        goto done;
    }

    if (is_cancelled()) goto cancelled;

    post_stage(5, 100, L"Installation complete");
    post_log(L"\r\nSUCCESS: Adacord was installed. Restart Discord to finish.");
    post_finished(INSTALL_SUCCEEDED, L"Adacord was installed successfully.\r\n\r\nRestart Discord now to load Adacord.");
    goto done;

cancelled:
    if (args->operation == OPERATION_UNINJECT) {
        post_log(L"\r\nUninject cancelled.");
        post_finished(INSTALL_CANCELLED, L"The uninject operation was cancelled.");
    } else {
        post_log(L"\r\nInstallation cancelled.");
        post_finished(INSTALL_CANCELLED, L"The installation was cancelled.");
    }

done:
    close_worker_job();
    free(args);
    return 0;
}

static void set_controls_running(BOOL running) {
    g_running = running;
    EnableWindow(g_closeDiscord, !running);
    EnableWindow(g_install, !running);
    EnableWindow(g_uninject, !running);
    ShowWindow(g_cancel, running ? SW_SHOW : SW_HIDE);
}

static void append_log_control(const wchar_t *text) {
    int currentLength = GetWindowTextLengthW(g_log);
    if (currentLength > 1500000) {
        SendMessageW(g_log, EM_SETSEL, 0, 500000);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"");
        currentLength = GetWindowTextLengthW(g_log);
    }
    SendMessageW(g_log, EM_SETSEL, currentLength, currentLength);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

static BOOL normalize_target_path(wchar_t *path, size_t pathCount) {
    wchar_t fullPath[MAX_PATH * 4];
    DWORD length = GetFullPathNameW(path, ARRAYSIZE(fullPath), fullPath, NULL);
    if (!length || length >= ARRAYSIZE(fullPath)) return FALSE;

    while (length > 3 && (fullPath[length - 1] == L'\\' || fullPath[length - 1] == L'/')) {
        fullPath[--length] = L'\0';
    }
    if (length <= 3) return FALSE;

    wcsncpy(path, fullPath, pathCount - 1);
    path[pathCount - 1] = L'\0';
    return TRUE;
}

static BOOL get_local_install_path(wchar_t *path, size_t pathCount) {
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)pathCount);
    if (!length || length >= pathCount) return FALSE;

    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) slash = wcsrchr(path, L'/');
    if (!slash) return FALSE;

    slash[1] = L'\0';
    if (wcslen(path) + wcslen(L"adacord") >= pathCount) return FALSE;
    wcscat(path, L"adacord");
    return TRUE;
}

static void begin_operation(int operation) {
    WorkerArgs *args = (WorkerArgs *)calloc(1, sizeof(WorkerArgs));
    if (!args) {
        MessageBoxW(g_window, L"Could not allocate installer state.", APP_NAME, MB_OK | MB_ICONERROR);
        return;
    }

    GetWindowTextW(g_path, args->target, ARRAYSIZE(args->target));
    if (!args->target[0] || !normalize_target_path(args->target, ARRAYSIZE(args->target))) {
        free(args);
        MessageBoxW(g_window, L"Could not resolve the installer folder.", APP_NAME, MB_OK | MB_ICONERROR);
        return;
    }
    args->closeDiscord = SendMessageW(g_closeDiscord, BM_GETCHECK, 0, 0) == BST_CHECKED;
    args->operation = operation;

    if (operation == OPERATION_UNINJECT) {
        int choice = MessageBoxW(
            g_window,
            L"Remove Adacord's injection from Discord?",
            APP_NAME,
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION
        );
        if (choice != IDYES) {
            free(args);
            return;
        }
    }

    SetWindowTextW(g_log, L"");
    g_activeOperation = operation;
    SetWindowTextW(g_status, operation == OPERATION_UNINJECT ? L"Starting uninject..." : L"Starting installation...");
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    for (int i = 0; i < 4; i++) {
        const wchar_t *installName = i == 0 ? L"1  Source" : i == 1 ? L"2  Dependencies" : i == 2 ? L"3  Build" : L"4  Inject";
        const wchar_t *uninjectName = i == 0 ? L"1  Locate" : i == 1 ? L"2  Prerequisites" : i == 2 ? L"3  Close Discord" : L"4  Uninject";
        SetWindowTextW(g_steps[i], operation == OPERATION_UNINJECT ? uninjectName : installName);
    }

    InterlockedExchange(&g_cancelRequested, 0);
    set_controls_running(TRUE);
    g_worker = CreateThread(NULL, 0, operation_worker, args, 0, NULL);
    if (!g_worker) {
        set_controls_running(FALSE);
        free(args);
        MessageBoxW(g_window, L"Could not start the installer worker.", APP_NAME, MB_OK | MB_ICONERROR);
    }
}

static void cancel_operation(void) {
    if (!g_running) return;
    int choice = MessageBoxW(
        g_window,
        g_activeOperation == OPERATION_UNINJECT ? L"Cancel the uninject operation?" : L"Cancel the current installation?",
        APP_NAME,
        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION
    );
    if (choice != IDYES) return;

    InterlockedExchange(&g_cancelRequested, 1);
    SetWindowTextW(g_status, L"Cancelling...");
    EnableWindow(g_cancel, FALSE);

    EnterCriticalSection(&g_jobLock);
    if (g_job) TerminateJobObject(g_job, ERROR_CANCELLED);
    LeaveCriticalSection(&g_jobLock);
}

static void layout_controls(int width, int height) {
    int margin = 28;
    int contentWidth = width - margin * 2;
    int footerY = height - 58;
    int logY = 392;
    int logHeight = footerY - logY - 18;

    if (contentWidth < 400) contentWidth = 400;
    if (logHeight < 100) logHeight = 100;

    MoveWindow(g_path, margin, 166, contentWidth, 32, TRUE);
    MoveWindow(g_closeDiscord, margin, 212, contentWidth, 25, TRUE);

    int stepWidth = (contentWidth - 24) / 4;
    for (int i = 0; i < 4; i++) {
        MoveWindow(g_steps[i], margin + i * (stepWidth + 8), 264, stepWidth, 24, TRUE);
    }

    MoveWindow(g_progress, margin, 306, contentWidth, 10, TRUE);
    MoveWindow(g_status, margin, 329, contentWidth, 28, TRUE);
    MoveWindow(g_log, margin, logY, contentWidth, logHeight, TRUE);
    MoveWindow(g_cancel, width - margin - 324, footerY + 8, 96, 34, TRUE);
    MoveWindow(g_uninject, width - margin - 218, footerY + 8, 104, 34, TRUE);
    MoveWindow(g_install, width - margin - 104, footerY + 8, 104, 34, TRUE);
}

static HWND create_control(
    DWORD extendedStyle,
    const wchar_t *className,
    const wchar_t *text,
    DWORD style,
    int id
) {
    return CreateWindowExW(
        extendedStyle,
        className,
        text,
        style | WS_CHILD | WS_VISIBLE,
        0,
        0,
        0,
        0,
        g_window,
        (HMENU)(INT_PTR)id,
        g_instance,
        NULL
    );
}

static void create_ui(void) {
    HWND label;
    HWND title;
    HWND subtitle;
    HWND icon;

    icon = create_control(0, WC_STATICW, L"", SS_ICON, 0);
    HICON appIcon = (HICON)LoadImageW(g_instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 56, 56, LR_DEFAULTCOLOR);
    SendMessageW(icon, STM_SETICON, (WPARAM)appIcon, 0);
    MoveWindow(icon, 28, 26, 60, 60, TRUE);

    title = create_control(0, WC_STATICW, L"Manage Adacord", SS_LEFT, 0);
    SetWindowPos(title, NULL, 100, 25, 540, 38, SWP_NOZORDER);
    SendMessageW(title, WM_SETFONT, (WPARAM)g_titleFont, TRUE);

    subtitle = create_control(0, WC_STATICW, L"Install or remove Adacord using the folder beside this executable.", SS_LEFT, 0);
    SetWindowPos(subtitle, NULL, 102, 66, 600, 25, SWP_NOZORDER);
    SendMessageW(subtitle, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);

    label = create_control(0, WC_STATICW, L"Working folder", SS_LEFT, 0);
    SetWindowPos(label, NULL, 28, 137, 260, 24, SWP_NOZORDER);
    SendMessageW(label, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);

    g_path = create_control(WS_EX_CLIENTEDGE, WC_EDITW, L"", ES_AUTOHSCROLL | ES_READONLY, ID_PATH);
    SendMessageW(g_path, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);

    g_closeDiscord = create_control(0, WC_BUTTONW, L"Close Discord automatically before inject or uninject", BS_AUTOCHECKBOX | WS_TABSTOP, ID_CLOSE_DISCORD);
    SendMessageW(g_closeDiscord, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);
    SendMessageW(g_closeDiscord, BM_SETCHECK, BST_CHECKED, 0);

    g_steps[0] = create_control(0, WC_STATICW, L"1  Source", SS_LEFT, 0);
    g_steps[1] = create_control(0, WC_STATICW, L"2  Dependencies", SS_LEFT, 0);
    g_steps[2] = create_control(0, WC_STATICW, L"3  Build", SS_LEFT, 0);
    g_steps[3] = create_control(0, WC_STATICW, L"4  Inject", SS_LEFT, 0);
    for (int i = 0; i < 4; i++) SendMessageW(g_steps[i], WM_SETFONT, (WPARAM)g_smallFont, TRUE);

    g_progress = create_control(0, PROGRESS_CLASSW, L"", PBS_SMOOTH, ID_PROGRESS);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, 100);
    SendMessageW(g_progress, PBM_SETBARCOLOR, 0, THEME_ACCENT);
    SendMessageW(g_progress, PBM_SETBKCOLOR, 0, RGB(225, 222, 228));

    g_status = create_control(0, WC_STATICW, L"Ready to install", SS_LEFT, 0);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);

    label = create_control(0, WC_STATICW, L"Installation log", SS_LEFT, 0);
    SetWindowPos(label, NULL, 28, 363, 260, 24, SWP_NOZORDER);
    SendMessageW(label, WM_SETFONT, (WPARAM)g_smallFont, TRUE);

    g_log = create_control(
        WS_EX_CLIENTEDGE,
        WC_EDITW,
        L"",
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_HSCROLL,
        ID_LOG
    );
    SendMessageW(g_log, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 2000000, 0);

    g_cancel = create_control(0, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ID_CANCEL);
    SendMessageW(g_cancel, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);
    ShowWindow(g_cancel, SW_HIDE);

    g_uninject = create_control(0, WC_BUTTONW, L"Uninject", BS_PUSHBUTTON | WS_TABSTOP, ID_UNINJECT);
    SendMessageW(g_uninject, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);

    g_install = create_control(0, WC_BUTTONW, L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_INSTALL);
    SendMessageW(g_install, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);

    wchar_t installPath[MAX_PATH * 4];
    if (get_local_install_path(installPath, ARRAYSIZE(installPath))) {
        SetWindowTextW(g_path, installPath);
    } else {
        SetWindowTextW(g_path, L"Unable to resolve executable folder");
    }
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_window = window;
            create_ui();
            return 0;

        case WM_SIZE:
            layout_controls(LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(window, NULL, TRUE);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO *info = (MINMAXINFO *)lParam;
            info->ptMinTrackSize.x = 720;
            info->ptMinTrackSize.y = 650;
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_INSTALL:
                    begin_operation(OPERATION_INSTALL);
                    return 0;
                case ID_UNINJECT:
                    begin_operation(OPERATION_UNINJECT);
                    return 0;
                case ID_CANCEL:
                    cancel_operation();
                    return 0;
            }
            break;

        case WM_APP_LOG: {
            wchar_t *text = (wchar_t *)lParam;
            append_log_control(text);
            free(text);
            return 0;
        }

        case WM_APP_STAGE: {
            int stage = LOWORD(wParam);
            int progress = HIWORD(wParam);
            wchar_t *status = (wchar_t *)lParam;
            SetWindowTextW(g_status, status);
            SendMessageW(g_progress, PBM_SETPOS, progress, 0);
            for (int i = 0; i < 4; i++) {
                wchar_t text[64];
                const wchar_t *installName = i == 0 ? L"Source" : i == 1 ? L"Dependencies" : i == 2 ? L"Build" : L"Inject";
                const wchar_t *uninjectName = i == 0 ? L"Locate" : i == 1 ? L"Prerequisites" : i == 2 ? L"Close Discord" : L"Uninject";
                const wchar_t *name = g_activeOperation == OPERATION_UNINJECT ? uninjectName : installName;
                _snwprintf(text, ARRAYSIZE(text) - 1, i + 1 < stage ? L"%d  %ls - done" : L"%d  %ls", i + 1, name);
                SetWindowTextW(g_steps[i], text);
            }
            free(status);
            return 0;
        }

        case WM_APP_FINISHED: {
            FinishMessage *finish = (FinishMessage *)lParam;
            set_controls_running(FALSE);
            EnableWindow(g_cancel, TRUE);
            if (g_worker) {
                CloseHandle(g_worker);
                g_worker = NULL;
            }

            if (finish->result == INSTALL_SUCCEEDED) {
                if (g_activeOperation == OPERATION_UNINJECT) {
                    SetWindowTextW(g_status, L"Uninject complete");
                    MessageBoxW(window, finish->message, L"Uninject complete", MB_OK | MB_ICONINFORMATION);
                } else {
                    SetWindowTextW(g_install, L"Install again");
                    MessageBoxW(window, finish->message, L"Installation complete", MB_OK | MB_ICONINFORMATION);
                }
            } else if (finish->result == INSTALL_FAILED) {
                SetWindowTextW(g_status, g_activeOperation == OPERATION_UNINJECT ? L"Uninject failed" : L"Installation failed");
                MessageBoxW(
                    window,
                    finish->message,
                    g_activeOperation == OPERATION_UNINJECT ? L"Uninject failed" : L"Installation failed",
                    MB_OK | MB_ICONERROR
                );
            } else {
                SetWindowTextW(g_status, g_activeOperation == OPERATION_UNINJECT ? L"Uninject cancelled" : L"Installation cancelled");
            }
            free(finish);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wParam;
            HWND control = (HWND)lParam;
            if (control == g_log) {
                SetBkColor(dc, THEME_LOG);
                SetTextColor(dc, RGB(239, 235, 241));
                return (LRESULT)g_logBrush;
            }
            RECT rect;
            GetClientRect(window, &rect);

            RECT controlRect;
            GetWindowRect(control, &controlRect);
            POINT point = { controlRect.left, controlRect.top };
            ScreenToClient(window, &point);
            SetBkMode(dc, TRANSPARENT);

            if (point.y < 112) {
                SetTextColor(dc, RGB(250, 248, 251));
                return (LRESULT)g_headerBrush;
            }
            SetTextColor(dc, control == g_status ? THEME_TEXT : THEME_MUTED);
            return (LRESULT)g_backgroundBrush;
        }

        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wParam;
            HWND control = (HWND)lParam;
            if (control == g_log) {
                SetBkColor(dc, THEME_LOG);
                SetTextColor(dc, RGB(239, 235, 241));
                return (LRESULT)g_logBrush;
            }
            SetBkColor(dc, RGB(255, 255, 255));
            SetTextColor(dc, THEME_TEXT);
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }

        case WM_ERASEBKGND: {
            RECT rect;
            GetClientRect(window, &rect);
            FillRect((HDC)wParam, &rect, g_backgroundBrush);
            RECT header = { 0, 0, rect.right, 112 };
            FillRect((HDC)wParam, &header, g_headerBrush);
            RECT accent = { 0, 108, rect.right, 112 };
            HBRUSH accentBrush = CreateSolidBrush(THEME_ACCENT);
            FillRect((HDC)wParam, &accent, accentBrush);
            DeleteObject(accentBrush);
            return 1;
        }

        case WM_CLOSE:
            if (g_running) {
                MessageBoxW(window, L"Cancel the installation before closing this window.", APP_NAME, MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

static void enable_dpi_awareness(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    typedef BOOL(WINAPI *SetDpiAwarenessContextFn)(HANDLE);
    union {
        FARPROC raw;
        SetDpiAwarenessContextFn typed;
    } setDpi = { GetProcAddress(user32, "SetProcessDpiAwarenessContext") };
    SetDpiAwarenessContextFn setDpiAwarenessContext = setDpi.typed;
    if (setDpiAwarenessContext) {
        setDpiAwarenessContext((HANDLE)-4);
    } else {
        SetProcessDPIAware();
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int showCommand) {
    (void)previous;
    (void)commandLine;
    g_instance = instance;
    enable_dpi_awareness();
    InitializeCriticalSection(&g_jobLock);

    INITCOMMONCONTROLSEX controls = { sizeof(INITCOMMONCONTROLSEX), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    g_backgroundBrush = CreateSolidBrush(THEME_BACKGROUND);
    g_headerBrush = CreateSolidBrush(THEME_HEADER);
    g_logBrush = CreateSolidBrush(THEME_LOG);
    g_titleFont = CreateFontW(29, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_bodyFont = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_smallFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_monoFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    WNDCLASSEXW windowClass = { 0 };
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = window_proc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.hbrBackground = g_backgroundBrush;
    windowClass.lpszClassName = L"AdacordInstallerWindow";

    if (!RegisterClassExW(&windowClass)) return 1;

    RECT desired = { 0, 0, 900, 720 };
    AdjustWindowRectEx(&desired, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int width = desired.right - desired.left;
    int height = desired.bottom - desired.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_window = CreateWindowExW(
        0,
        windowClass.lpszClassName,
        APP_NAME,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x,
        y,
        width,
        height,
        NULL,
        NULL,
        instance,
        NULL
    );
    if (!g_window) return 1;

    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    close_worker_job();
    if (g_worker) CloseHandle(g_worker);
    DeleteObject(g_titleFont);
    DeleteObject(g_bodyFont);
    DeleteObject(g_smallFont);
    DeleteObject(g_monoFont);
    DeleteObject(g_backgroundBrush);
    DeleteObject(g_headerBrush);
    DeleteObject(g_logBrush);
    DeleteCriticalSection(&g_jobLock);
    CoUninitialize();
    return (int)message.wParam;
}
