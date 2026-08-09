/**
 * core.h - 核心模块
 * 注册表读写、核心算法、文件操作、控制台基础设施
 */

#ifndef CORE_H
#define CORE_H

#include <windows.h>
#include "RebootWipe.h"

/* ============================================================
 * 控制台基础设施
 * ============================================================ */

void SaveConsoleColor(void);
void SetConsoleColor(WORD attr);
void ResetConsoleColor(void);

#define WPRINTF_RED(fmt, ...) do { \
    SetConsoleColor(CONSOLE_RED); \
    wprintf(fmt, __VA_ARGS__); \
    ResetConsoleColor(); \
} while(0)

#define WPRINTF_RED0(fmt) do { \
    SetConsoleColor(CONSOLE_RED); \
    wprintf(fmt); \
    ResetConsoleColor(); \
} while(0)

/* ============================================================
 * 注册表 I/O
 * ============================================================ */

LONG RW_OpenRegKey(REGSAM access, HKEY* hKey);
LONG RW_CreateRegKey(REGSAM access, HKEY* hKey);
LONG ReadRegistryData(BYTE** buffer, DWORD* size);
LONG WriteRegistryData(const BYTE* buffer, DWORD size);
LONG BackupData(const BYTE* data, DWORD size, wchar_t** path);

/* ============================================================
 * 核心算法
 * ============================================================ */

LONG ParseMultiSzData(const BYTE* data, DWORD size,
                      PendingOperation** operations, int* count);
LONG InjectSkipPrefix(BYTE** buffer, DWORD* size, int index);
LONG EraseEntry(BYTE** buffer, DWORD* size, int index);

/* ============================================================
 * 文件操作
 * ============================================================ */

wchar_t* TrimPath(wchar_t* str);
LONG AddToDeleteList(const wchar_t* filePath);
LONG ReadPathsFromFile(const wchar_t* filePath, wchar_t*** paths, int* count);
int AddMultipleToDeleteList(const wchar_t* const* paths, int count);
BOOL IsDirectoryPath(const wchar_t* path);
BOOL IsDirectoryNonEmpty(const wchar_t* dirPath);
LONG ExpandDirectoryPaths(const wchar_t* const* inputPaths, int inputCount,
                          wchar_t*** outPaths, int* outCount,
                          BOOL interactive);

#endif /* CORE_H */
