#pragma once

// dlssnr.log, beside the DLL. Written at start-up, when the core is patched, at feature creation, release and
// failure, and by the reporter thread (ngx_hook.cpp) -- never on the evaluate path. Each line starts with the seconds
// since the log was opened.
//
// Written with WriteFile under a lock of our own rather than through a CRT FILE. When the game exits, Windows ends
// every other thread before DllMain(DLL_PROCESS_DETACH) runs; a thread ended inside the CRT's stream or low-level file
// lock leaves that lock held for ever, and the totals written at detach would then hang the game's exit. Our lock is
// only tried at detach (LogAtExit): a line is dropped instead.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

inline HANDLE g_log = INVALID_HANDLE_VALUE;
inline SRWLOCK g_logLock = SRWLOCK_INIT;
inline LARGE_INTEGER g_logStart = {};
inline LARGE_INTEGER g_logFrequency = {};

// Starts the file afresh, first renaming the last run's to <name>.prev.log (replacing an older one): a game relaunched
// to try a setting would otherwise wipe the log of the run before, as it did in the first M4 playtest. Others may
// read it while the game runs; only we write it.
inline void LogOpen(const wchar_t* path)
{
    QueryPerformanceFrequency(&g_logFrequency);
    QueryPerformanceCounter(&g_logStart);
    const int length = lstrlenW(path);
    if (length > 4 && length + 6 <= MAX_PATH &&
        CompareStringOrdinal(path + length - 4, 4, L".log", 4, TRUE) == CSTR_EQUAL)
    {
        wchar_t previous[MAX_PATH];
        wmemcpy(previous, path, size_t(length) - 4);
        wmemcpy(previous + length - 4, L".prev.log", 10);
        MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Seconds since LogOpen.
inline double LogClock()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return g_logFrequency.QuadPart != 0 ? double(now.QuadPart - g_logStart.QuadPart) / double(g_logFrequency.QuadPart)
                                        : 0.0;
}

// One line. It is formatted first and written in one piece, so lines from different threads never interleave, and
// the caller's last-error value is kept: this also runs inside the game's calls we intercept. With `wait` false the
// line is dropped when another thread holds the lock.
inline void LogV(bool wait, const char* format, va_list args)
{
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    const DWORD lastError = GetLastError();

    char line[1024];
    int length = snprintf(line, sizeof line, "%9.3f  ", LogClock());
    const int text = vsnprintf(line + length, sizeof line - length - 1, format, args);
    if (text > 0)
        length += text < int(sizeof line) - length - 1 ? text : int(sizeof line) - length - 2;
    line[length++] = '\n';

    if (wait)
        AcquireSRWLockExclusive(&g_logLock);
    else if (!TryAcquireSRWLockExclusive(&g_logLock))
    {
        SetLastError(lastError);
        return;
    }
    DWORD written = 0;
    WriteFile(g_log, line, DWORD(length), &written, nullptr);
    ReleaseSRWLockExclusive(&g_logLock);
    SetLastError(lastError);
}

inline void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogV(true, format, args);
    va_end(args);
}

// For DllMain(DLL_PROCESS_DETACH) only: never waits for the lock, see above.
inline void LogAtExit(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    LogV(false, format, args);
    va_end(args);
}
