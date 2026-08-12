/**
 * core.c - 核心模块实现
 * 注册表 I/O、核心算法、文件操作、控制台基础设施
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "RebootWipe.h"
#include "core.h"

/* ============================================================
 * 控制台颜色管理
 * ============================================================ */

static WORD g_originalAttrs = 0;

// 设置控制台输出文字颜色
void SetConsoleColor(WORD attr)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hOut, attr);
}

// 保存当前控制台颜色属性，供 ResetConsoleColor 恢复
void SaveConsoleColor(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hOut, &info);
    g_originalAttrs = info.wAttributes;
}

// 恢复控制台颜色为 SaveConsoleColor 保存的原始值
void ResetConsoleColor(void)
{
    SetConsoleColor(g_originalAttrs);
}

// 使用 Win32 API 安全清屏（FillConsoleOutputCharacter + SetConsoleCursorPosition）
void ClearConsole(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD written;
    COORD origin = { 0, 0 };

    if (!GetConsoleScreenBufferInfo(hOut, &info)) {
        /* Fallback: 如果无法获取信息则不做清屏 */
        return;
    }

    FillConsoleOutputCharacterW(hOut, L' ', info.dwSize.X * info.dwSize.Y,
                                origin, &written);
    FillConsoleOutputAttribute(hOut, g_originalAttrs,
                               info.dwSize.X * info.dwSize.Y, origin, &written);
    SetConsoleCursorPosition(hOut, origin);
}

/* ============================================================
 * 安全输入解析
 * ============================================================ */

// 安全的宽字符串转整数：防溢出、拒绝非数字字符，成功返回 TRUE
BOOL SafeParseInt(const wchar_t* str, int* value)
{
    const wchar_t* p;
    int sign = 1;
    long long result = 0;

    if (str == NULL || value == NULL) return FALSE;
    if (*str == L'\0') return FALSE;

    /* 跳过前导空白 */
    p = str;
    while (*p == L' ' || *p == L'\t') p++;
    if (*p == L'\0') return FALSE;

    /* 处理符号 */
    if (*p == L'-') { sign = -1; p++; }
    else if (*p == L'+') { p++; }

    if (*p == L'\0') return FALSE;

    /* 逐字符校验并累加 */
    while (*p != L'\0') {
        if (*p < L'0' || *p > L'9') {
            /* 允许尾部空格/换行 */
            if (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\r') break;
            return FALSE;
        }
        result = result * 10 + (*p - L'0');
        if (sign > 0 && result > INT_MAX) return FALSE;
        if (sign < 0 && result > (long long)INT_MAX + 1) return FALSE;
        p++;
    }

    *value = (int)(sign * result);
    return TRUE;
}

// 清空 stdin 缓冲区中所有残留字符（防止污染下一次输入）
void FlushStdin(void)
{
    wint_t ch;
    while ((ch = fgetwc(stdin)) != L'\n' && ch != WEOF) { }
}

/* ============================================================
 * 注册表 I/O
 * ============================================================ */

// 以指定权限打开注册表键，优先使用 WOW64 标志访问 64 位视图
LONG RW_OpenRegKey(REGSAM access, HKEY* hKey)
{
    LONG result;

    result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0,
                           access | REG_WOW64_FLAG, hKey);
    if (result == ERROR_SUCCESS) {
        return ERROR_SUCCESS;
    }

    result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0,
                           access, hKey);
    return result;
}

// 以指定权限打开/创建注册表键，优先尝试 WOW64 标志
LONG RW_CreateRegKey(REGSAM access, HKEY* hKey)
{
    LONG result;
    DWORD disposition;

    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0, NULL, 0,
                             access | REG_WOW64_FLAG, NULL, hKey, &disposition);
    if (result == ERROR_SUCCESS) {
        return ERROR_SUCCESS;
    }

    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0, NULL, 0,
                             access, NULL, hKey, &disposition);
    return result;
}

// 读取 PendingFileRenameOperations 注册表值，返回堆分配的原始数据
LONG ReadRegistryData(BYTE** buffer, DWORD* size)
{
    HKEY hKey = NULL;
    LONG result;
    DWORD type;
    DWORD dataSize = 0;
    BYTE* data = NULL;

    result = RW_OpenRegKey(KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        return result;
    }

    result = RegQueryValueExW(hKey, REG_VALUE_NAME, NULL, &type, NULL, &dataSize);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return result;
    }

    if (dataSize == 0) {
        RegCloseKey(hKey);
        return ERROR_FILE_NOT_FOUND;
    }

    data = (BYTE*)malloc(dataSize);
    if (data == NULL) {
        RegCloseKey(hKey);
        return ERROR_OUTOFMEMORY;
    }

    result = RegQueryValueExW(hKey, REG_VALUE_NAME, NULL, &type, data, &dataSize);
    if (result != ERROR_SUCCESS) {
        free(data);
        RegCloseKey(hKey);
        return result;
    }

    *buffer = data;
    *size = dataSize;

    RegCloseKey(hKey);
    return ERROR_SUCCESS;
}

// 写入数据到注册表，写入后立即读回验证；size=0 时删除该值
LONG WriteRegistryData(const BYTE* buffer, DWORD size)
{
    HKEY hKey = NULL;
    LONG result;

    result = RW_CreateRegKey(KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS) {
        if (result == ERROR_ACCESS_DENIED) {
            WPRINTF_RED0(L"[错误] 权限不足：请以管理员身份运行本程序。\n");
        } else if (result == ERROR_FILE_NOT_FOUND || result == ERROR_NO_MORE_ITEMS) {
            WPRINTF_RED0(L"[错误] 注册表路径不存在，且无法创建（可能被杀软拦截）。\n");
        } else {
            WPRINTF_RED(L"[错误] 打开注册表失败，错误码：%lu\n", result);
        }
        return result;
    }

    if (size == 0) {
        result = RegDeleteValueW(hKey, REG_VALUE_NAME);
    } else {
        result = RegSetValueExW(hKey, REG_VALUE_NAME, 0, REG_MULTI_SZ, buffer, size);
    }

    RegCloseKey(hKey);

    if (result == ERROR_SUCCESS) {
        BYTE* verifyBuf = NULL;
        DWORD verifySize = 0;
        result = ReadRegistryData(&verifyBuf, &verifySize);
        if (result == ERROR_SUCCESS) {
            if (verifySize != size || memcmp(verifyBuf, buffer, size) != 0) {
                free(verifyBuf);
                WPRINTF_RED0(L"[错误] 写入验证失败：数据不匹配（可能被杀软拦截）。\n");
                return ERROR_INTERNAL_ERROR;
            }
            free(verifyBuf);
        } else if (size == 0 && result == ERROR_FILE_NOT_FOUND) {
            /* 删除操作验证通过 */
        } else {
            WPRINTF_RED(L"[错误] 写入验证失败：无法读回数据（错误码：%lu）\n", result);
            return result;
        }
    } else {
        if (result == ERROR_ACCESS_DENIED) {
            WPRINTF_RED0(L"[错误] 写入被拒绝：可能是权限不足或被杀软拦截。\n");
            wprintf(L"       建议：以管理员身份运行，并将本程序加入杀软白名单。\n");
        } else if (result == ERROR_SHARING_VIOLATION) {
            WPRINTF_RED0(L"[错误] 写入失败：注册表正被其他进程占用。\n");
            wprintf(L"       请关闭相关程序后重试。\n");
        } else if (result == ERROR_NOT_ENOUGH_MEMORY) {
            WPRINTF_RED0(L"[错误] 写入失败：系统内存不足。\n");
        } else {
            WPRINTF_RED(L"[错误] 写入失败，错误码：%lu\n", result);
        }
    }

    return result;
}

// 将当前注册表数据备份到临时文件，抹除操作前自动调用
LONG BackupData(const BYTE* data, DWORD size, wchar_t** path)
{
    wchar_t tempDir[MAX_PATH];
    wchar_t* backupPath = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD bytesWritten;

    GetTempPathW(MAX_PATH, tempDir);

    backupPath = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
    if (backupPath == NULL) {
        return ERROR_OUTOFMEMORY;
    }

    swprintf_s(backupPath, MAX_PATH, L"%sRebootWipe_%d.tmp", tempDir, GetTickCount());

    hFile = CreateFileW(backupPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        free(backupPath);
        return GetLastError();
    }

    if (!WriteFile(hFile, data, size, &bytesWritten, NULL)) {
        CloseHandle(hFile);
        DeleteFileW(backupPath);
        free(backupPath);
        return GetLastError();
    }

    CloseHandle(hFile);
    *path = backupPath;
    return ERROR_SUCCESS;
}

/* ============================================================
 * 核心算法
 * ============================================================ */

// 解析 REG_MULTI_SZ 原始数据为 PendingOperation 数组，识别删除/移动/跳过类型
LONG ParseMultiSzData(const BYTE* data, DWORD size,
                      PendingOperation** operations, int* count)
{
    const wchar_t* ptr;
    const wchar_t* end;
    PendingOperation* ops = NULL;
    int opCount = 0;
    int capacity = 16;

    if (data == NULL || size == 0) {
        *operations = NULL;
        *count = 0;
        return ERROR_SUCCESS;
    }

    ops = (PendingOperation*)malloc(capacity * sizeof(PendingOperation));
    if (ops == NULL) {
        return ERROR_OUTOFMEMORY;
    }
    memset(ops, 0, capacity * sizeof(PendingOperation));

    ptr = (const wchar_t*)data;
    end = (const wchar_t*)(data + size);

    while (ptr < end) {
        const wchar_t *srcStart, *srcEnd;
        const wchar_t *dstStart, *dstEnd;
        size_t srcLen, dstLen;
        PendingOperation* op;

        srcStart = ptr;
        srcEnd = ptr;
        while (srcEnd < end && *srcEnd != L'\0') {
            srcEnd++;
        }

        if (srcEnd >= end) break;

        srcLen = (size_t)(srcEnd - srcStart);
        if (srcLen == 0) {
            ptr = srcEnd + 1;
            continue;
        }

        if (opCount >= capacity) {
            capacity *= 2;
            PendingOperation* newOps = (PendingOperation*)realloc(ops, capacity * sizeof(PendingOperation));
            if (newOps == NULL) {
                free(ops);
                return ERROR_OUTOFMEMORY;
            }
            ops = newOps;
            memset(ops + opCount, 0, (capacity - opCount) * sizeof(PendingOperation));
        }

        op = &ops[opCount];
        opCount++;

        if (srcLen >= MAX_PATH_LEN) srcLen = MAX_PATH_LEN - 1;
        wcsncpy_s(op->sourcePath, MAX_PATH_LEN, srcStart, srcLen);
        op->sourcePath[srcLen] = L'\0';

        if (srcLen >= SKIP_PREFIX_LEN &&
            op->sourcePath[0] == L'?' && op->sourcePath[1] == L'?') {
            size_t newLen = srcLen - SKIP_PREFIX_LEN;
            wcsncpy_s(op->sourcePath, MAX_PATH_LEN,
                      op->sourcePath + SKIP_PREFIX_LEN, newLen);
            op->sourcePath[newLen] = L'\0';
            op->type = OP_SKIPPED;
            srcLen = newLen;
        }

        if (srcLen >= 2 && op->sourcePath[0] == L'*') {
            size_t skipLen = 1;
            while (skipLen < srcLen &&
                   op->sourcePath[skipLen] >= L'0' &&
                   op->sourcePath[skipLen] <= L'9') {
                skipLen++;
            }
            if (skipLen < srcLen) {
                size_t newLen = srcLen - skipLen;
                wcsncpy_s(op->sourcePath, MAX_PATH_LEN,
                          op->sourcePath + skipLen, newLen);
                op->sourcePath[newLen] = L'\0';
                srcLen = newLen;
            }
        }

        if (srcLen >= 4 && op->sourcePath[0] == L'\\' &&
            op->sourcePath[1] == L'?' && op->sourcePath[2] == L'?' &&
            op->sourcePath[3] == L'\\') {
            size_t newPathLen = srcLen - 4;
            wcsncpy_s(op->sourcePath, MAX_PATH_LEN,
                      op->sourcePath + 4, newPathLen);
            op->sourcePath[newPathLen] = L'\0';
        }

        ptr = srcEnd + 1;

        if (ptr < end && *ptr != L'\0') {
            dstStart = ptr;
            dstEnd = ptr;
            while (dstEnd < end && *dstEnd != L'\0') {
                dstEnd++;
            }

            dstLen = (size_t)(dstEnd - dstStart);
            if (dstLen >= MAX_PATH_LEN) dstLen = MAX_PATH_LEN - 1;
            wcsncpy_s(op->targetPath, MAX_PATH_LEN, dstStart, dstLen);
            op->targetPath[dstLen] = L'\0';

            if (op->type != OP_SKIPPED) {
                op->type = OP_MOVE;
            }

            ptr = dstEnd + 1;
        } else {
            if (op->type != OP_SKIPPED) {
                op->type = OP_DELETE;
            }
        }

        if (ptr < end && *ptr == L'\0') {
            ptr++;
        }
    }

    *operations = ops;
    *count = opCount;
    return ERROR_SUCCESS;
}

// 在指定索引的条目前插入 ?? 前缀实现"安全跳过"，已被跳过则无操作
LONG InjectSkipPrefix(BYTE** buffer, DWORD* size, int index)
{
    BYTE* oldBuffer = *buffer;
    DWORD oldSize = *size;
    BYTE* newBuffer = NULL;
    DWORD newSize;
    const wchar_t* ptr;
    const wchar_t* end;
    int currentIdx = 0;
    size_t offset;
    size_t prefixBytes;
    LONG result;

    ptr = (const wchar_t*)oldBuffer;
    end = (const wchar_t*)(oldBuffer + oldSize);

    while (ptr < end && *ptr == L'\0') {
        ptr++;
    }

    while (ptr < end && currentIdx < index) {
        while (ptr < end && *ptr != L'\0') ptr++;
        if (ptr >= end) break;
        ptr++;

        if (ptr < end && *ptr != L'\0') {
            while (ptr < end && *ptr != L'\0') ptr++;
            if (ptr >= end) break;
            ptr++;
        }

        currentIdx++;

        if (ptr < end && *ptr == L'\0') {
            ptr++;
        }

        while (ptr < end && *ptr == L'\0' && ptr + 1 < end && *(ptr + 1) != L'\0') {
            ptr++;
        }
    }

    if (currentIdx != index || ptr >= end) {
        return ERROR_INVALID_PARAMETER;
    }

    if (ptr[0] == L'?' && ptr[1] == L'?') {
        return ERROR_SUCCESS;
    }

    prefixBytes = SKIP_PREFIX_LEN * sizeof(wchar_t);
    newSize = oldSize + (DWORD)prefixBytes;
    offset = (size_t)((const BYTE*)ptr - oldBuffer);

    newBuffer = (BYTE*)malloc(newSize);
    if (newBuffer == NULL) {
        return ERROR_OUTOFMEMORY;
    }

    memcpy(newBuffer, oldBuffer, offset);
    memcpy(newBuffer + offset, SKIP_PREFIX, prefixBytes);
    memcpy(newBuffer + offset + prefixBytes, oldBuffer + offset, oldSize - (DWORD)offset);

    result = WriteRegistryData(newBuffer, newSize);
    if (result == ERROR_SUCCESS) {
        free(oldBuffer);
        *buffer = newBuffer;
        *size = newSize;
    } else {
        free(newBuffer);
    }

    return result;
}

// 物理删除指定索引的条目，使用 memmove 防止内存重叠，自动处理末尾双空终止
LONG EraseEntry(BYTE** buffer, DWORD* size, int index)
{
    BYTE* oldBuffer = *buffer;
    DWORD oldSize = *size;
    BYTE* newBuffer = NULL;
    DWORD newSize;
    const wchar_t* ptr;
    const wchar_t* end;
    const wchar_t *entryStart, *entryEnd;
    int currentIdx = 0;
    size_t offset;
    size_t bytesToRemove;
    BOOL isLastEntry;
    LONG result;

    ptr = (const wchar_t*)oldBuffer;
    end = (const wchar_t*)(oldBuffer + oldSize);

    while (ptr < end && *ptr == L'\0') {
        ptr++;
    }

    while (ptr < end && currentIdx < index) {
        while (ptr < end && *ptr != L'\0') ptr++;
        if (ptr >= end) break;
        ptr++;

        if (ptr < end && *ptr != L'\0') {
            while (ptr < end && *ptr != L'\0') ptr++;
            if (ptr >= end) break;
            ptr++;
        }

        currentIdx++;

        if (ptr < end && *ptr == L'\0') {
            ptr++;
        }
    }

    if (currentIdx != index || ptr >= end) {
        return ERROR_INVALID_PARAMETER;
    }

    entryStart = ptr;
    offset = (size_t)((const BYTE*)entryStart - oldBuffer);

    ptr = entryStart;

    while (ptr < end && *ptr != L'\0') ptr++;
    if (ptr >= end) return ERROR_INVALID_PARAMETER;
    ptr++;

    if (ptr < end && *ptr != L'\0') {
        while (ptr < end && *ptr != L'\0') ptr++;
        if (ptr >= end) return ERROR_INVALID_PARAMETER;
        ptr++;
    }

    isLastEntry = TRUE;
    {
        const wchar_t* checkPtr = ptr;
        if (checkPtr < end && *checkPtr == L'\0') {
            checkPtr++;
        }
        while (checkPtr < end && *checkPtr == L'\0' &&
               checkPtr + 1 < end && *(checkPtr + 1) != L'\0') {
            checkPtr++;
        }
        if (checkPtr < end && *checkPtr != L'\0') {
            isLastEntry = FALSE;
        }
    }

    if (!isLastEntry) {
        if (ptr < end && *ptr == L'\0') {
            ptr++;
        }
    }

    entryEnd = ptr;
    bytesToRemove = (size_t)((const BYTE*)entryEnd - (const BYTE*)entryStart);

    newSize = oldSize - (DWORD)bytesToRemove;

    if (newSize < 2 * sizeof(wchar_t)) {
        if (newSize == 0) {
            newSize = 2 * sizeof(wchar_t);
        }
    }

    newBuffer = (BYTE*)malloc(newSize);
    if (newBuffer == NULL) {
        return ERROR_OUTOFMEMORY;
    }
    memset(newBuffer, 0, newSize);

    memcpy(newBuffer, oldBuffer, offset);

    {
        size_t copySize = oldSize - (DWORD)offset - (DWORD)bytesToRemove;
        if (copySize > 0 && entryEnd < end) {
            memmove(newBuffer + offset, (const BYTE*)entryEnd, copySize);
        }
    }

    {
        wchar_t* endPtr = (wchar_t*)(newBuffer + newSize - 2 * sizeof(wchar_t));
        if (endPtr[0] != L'\0' || endPtr[1] != L'\0') {
            wchar_t* lastStr = (wchar_t*)newBuffer;
            wchar_t* scanPtr = (wchar_t*)newBuffer;
            wchar_t* bufferEnd = (wchar_t*)(newBuffer + newSize);

            while (scanPtr < bufferEnd) {
                while (scanPtr < bufferEnd && *scanPtr != L'\0') scanPtr++;
                if (scanPtr >= bufferEnd) break;
                lastStr = scanPtr;
                scanPtr++;
            }

            if (lastStr >= (wchar_t*)newBuffer &&
                lastStr + 2 <= (wchar_t*)(newBuffer + newSize)) {
                lastStr[1] = L'\0';
            }
        }
    }

    result = WriteRegistryData(newBuffer, newSize);
    if (result == ERROR_SUCCESS) {
        free(oldBuffer);
        *buffer = newBuffer;
        *size = newSize;
    } else {
        free(newBuffer);
    }

    return result;
}

/* ============================================================
 * 路径处理
 * ============================================================ */

// 去除路径字符串首尾的空白和引号，原地修改
wchar_t* TrimPath(wchar_t* str)
{
    wchar_t* start;
    wchar_t* end;

    if (str == NULL) return NULL;

    start = str;
    while (*start == L' ' || *start == L'\t' || *start == L'"') {
        start++;
    }

    end = start + wcslen(start) - 1;
    while (end > start && (*end == L' ' || *end == L'\t' || *end == L'"')) {
        *end = L'\0';
        end--;
    }

    if (start != str) {
        wcscpy_s(str, wcslen(str) + 1, start);
    }

    return str;
}

/* ============================================================
 * 文件添加
 * ============================================================ */

// 通过 MoveFileExW(MOVEFILE_DELAY_UNTIL_REBOOT) 将路径添加到重启删除列表，写入前先验证路径存在
LONG AddToDeleteList(const wchar_t* filePath)
{
    LONG result;
    wchar_t* cleanPath;

    if (filePath == NULL || *filePath == L'\0') {
        WPRINTF_RED0(L"[错误] 文件路径不能为空。\n");
        return ERROR_INVALID_PARAMETER;
    }

    cleanPath = (wchar_t*)malloc((wcslen(filePath) + 1) * sizeof(wchar_t));
    if (cleanPath == NULL) {
        return ERROR_OUTOFMEMORY;
    }
    wcscpy_s(cleanPath, wcslen(filePath) + 1, filePath);
    TrimPath(cleanPath);

    /* 路径有效性验证：只有真实存在的文件/目录才允许写入 */
    if (!IsPathValid(cleanPath)) {
        WPRINTF_RED(L"[跳过] 路径不存在，已跳过：%s\n", filePath);
        free(cleanPath);
        return ERROR_FILE_NOT_FOUND;
    }

    result = MoveFileExW(cleanPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    free(cleanPath);

    if (result == 0) {
        result = GetLastError();
        WPRINTF_RED(L"[错误] 添加失败，错误码：%lu\n", result);
        if (result == ERROR_ACCESS_DENIED) {
            WPRINTF_RED0(L"       请确保以管理员身份运行本程序。\n");
        } else if (result == ERROR_FILE_NOT_FOUND) {
            WPRINTF_RED0(L"       文件不存在，请检查路径是否正确。\n");
        }
        return result;
    }

    wprintf(L"[成功] 已将文件添加到重启删除列表：%s\n", filePath);
    return ERROR_SUCCESS;
}

// 从 UTF-8/UTF-16 文本文件逐行读取路径并 Trim，# 开头的行为注释跳过
LONG ReadPathsFromFile(const wchar_t* filePath, wchar_t*** paths, int* count)
{
    FILE* fp = NULL;
    errno_t err;
    wchar_t* line = NULL;
    wchar_t** arr = NULL;
    int arrCount = 0;
    int capacity = 16;
    size_t i;

    *paths = NULL;
    *count = 0;

    /* 堆分配防止栈溢出（MAX_LONG_PATH_LEN ≈ 64KB） */
    line = (wchar_t*)malloc(MAX_LONG_PATH_LEN * sizeof(wchar_t));
    if (line == NULL) {
        return ERROR_OUTOFMEMORY;
    }

    err = _wfopen_s(&fp, filePath, L"r, ccs=UTF-8");
    if (err != 0 || fp == NULL) {
        free(line);
        return ERROR_FILE_NOT_FOUND;
    }

    arr = (wchar_t**)malloc(capacity * sizeof(wchar_t*));
    if (arr == NULL) {
        free(line);
        fclose(fp);
        return ERROR_OUTOFMEMORY;
    }

    while (fgetws(line, MAX_LONG_PATH_LEN, fp) != NULL) {
        wchar_t* trimmed;
        size_t len;

        len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) {
            line[--len] = L'\0';
        }

        if (len == 0) continue;
        if (line[0] == L'#') continue;

        trimmed = (wchar_t*)malloc((wcslen(line) + 1) * sizeof(wchar_t));
        if (trimmed == NULL) {
            for (i = 0; i < (size_t)arrCount; i++) free(arr[i]);
            free(arr);
            free(line);
            fclose(fp);
            return ERROR_OUTOFMEMORY;
        }
        wcscpy_s(trimmed, wcslen(line) + 1, line);
        TrimPath(trimmed);

        if (trimmed[0] == L'\0') {
            free(trimmed);
            continue;
        }

        if (arrCount >= capacity) {
            wchar_t** newArr;
            capacity *= 2;
            newArr = (wchar_t**)realloc(arr, capacity * sizeof(wchar_t*));
            if (newArr == NULL) {
                free(trimmed);
                for (i = 0; i < (size_t)arrCount; i++) free(arr[i]);
                free(arr);
                free(line);
                fclose(fp);
                return ERROR_OUTOFMEMORY;
            }
            arr = newArr;
        }

        arr[arrCount++] = trimmed;
    }

    fclose(fp);
    free(line);
    *paths = arr;
    *count = arrCount;
    return ERROR_SUCCESS;
}

// 批量调用 AddToDeleteList，带进度显示和成功/跳过/失败分类汇总
int AddMultipleToDeleteList(const wchar_t* const* paths, int count)
{
    int i;
    int success = 0;
    int skipped = 0;
    int failure = 0;

    if (paths == NULL || count <= 0) return 0;

    wprintf(L"\n  ╔══════════════════════════════════════════════════════════════╗\n");
    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  ║  批量添加重启删除任务（共 %d 项）                          ║\n", count);
    ResetConsoleColor();
    wprintf(L"  ╚══════════════════════════════════════════════════════════════╝\n\n");

    for (i = 0; i < count; i++) {
        LONG result;
        wprintf(L"  [%d/%d] ", i + 1, count);
        result = AddToDeleteList(paths[i]);
        if (result == ERROR_SUCCESS) {
            success++;
        } else if (result == ERROR_FILE_NOT_FOUND) {
            /* 路径无效，已在 AddToDeleteList 中输出 "[跳过]" */
            skipped++;
        } else {
            /* 其他写入错误，已在 AddToDeleteList 中输出 "[错误]" */
            failure++;
        }
    }

    wprintf(L"\n  ──────────────────────────────────────────────────────────\n");
    wprintf(L"  合计：");
    SetConsoleColor(CONSOLE_GREEN);
    wprintf(L"成功 %d", success);
    ResetConsoleColor();
    if (skipped > 0) {
        wprintf(L"  ");
        SetConsoleColor(CONSOLE_YELLOW);
        wprintf(L"跳过 %d", skipped);
        ResetConsoleColor();
    }
    if (failure > 0) {
        wprintf(L"  ");
        SetConsoleColor(CONSOLE_RED);
        wprintf(L"失败 %d", failure);
        ResetConsoleColor();
    }
    wprintf(L"\n\n");

    return success;
}

/* ============================================================
 * 目录递归展开
 * ============================================================ */

// 判断给定路径是否为目录（通过 GetFileAttributesW 属性判断）
BOOL IsDirectoryPath(const wchar_t* path)
{
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return FALSE;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
}

// 判断目录是否含有任何内容（绕过 . 和 ..），空目录返回 FALSE
BOOL IsDirectoryNonEmpty(const wchar_t* dirPath)
{
    WIN32_FIND_DATAW findData;
    HANDLE hFind;
    wchar_t* searchPath;
    BOOL nonEmpty = FALSE;

    searchPath = (wchar_t*)malloc(MAX_LONG_PATH_LEN * sizeof(wchar_t));
    if (searchPath == NULL) return FALSE;

    swprintf_s(searchPath, MAX_LONG_PATH_LEN, L"%s\\*", dirPath);
    hFind = FindFirstFileW(searchPath, &findData);
    free(searchPath);

    if (hFind == INVALID_HANDLE_VALUE) return FALSE;

    do {
        if (wcscmp(findData.cFileName, L".") != 0 &&
            wcscmp(findData.cFileName, L"..") != 0) {
            nonEmpty = TRUE;
            break;
        }
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
    return nonEmpty;
}

// 验证给定路径是否真实存在（文件或目录），作为写入注册表前的安全守卫
BOOL IsPathValid(const wchar_t* path)
{
    DWORD attrs;

    if (path == NULL || *path == L'\0') return FALSE;

    /* GetFileAttributesW 对文件和目录均有效，不存在返回 INVALID_FILE_ATTRIBUTES */
    attrs = GetFileAttributesW(path);
    return (attrs != INVALID_FILE_ATTRIBUTES);
}

// 内部函数：将路径拷贝追加到动态数组，容量不足时自动扩容
static LONG AddPathToArray(wchar_t*** paths, int* count, int* capacity,
                           const wchar_t* path)
{
    wchar_t* pathCopy;

    if (*count >= *capacity) {
        int newCap = (*capacity) * 2;
        wchar_t** newArr = (wchar_t**)realloc(*paths, newCap * sizeof(wchar_t*));
        if (newArr == NULL) return ERROR_OUTOFMEMORY;
        *paths = newArr;
        *capacity = newCap;
    }

    pathCopy = _wcsdup(path);
    if (pathCopy == NULL) return ERROR_OUTOFMEMORY;

    (*paths)[(*count)++] = pathCopy;
    return ERROR_SUCCESS;
}

// 内部函数：后序遍历收集目录下所有文件/子目录路径，跳过重解析点防止无限循环
static LONG CollectDirectoryPaths(const wchar_t* dirPath,
                                  wchar_t*** paths, int* count, int* capacity)
{
    WIN32_FIND_DATAW findData;
    HANDLE hFind;
    wchar_t* searchPath;
    wchar_t* fullPath;
    LONG result;

    searchPath = (wchar_t*)malloc(MAX_LONG_PATH_LEN * sizeof(wchar_t));
    if (searchPath == NULL) return ERROR_OUTOFMEMORY;

    fullPath = (wchar_t*)malloc(MAX_LONG_PATH_LEN * sizeof(wchar_t));
    if (fullPath == NULL) {
        free(searchPath);
        return ERROR_OUTOFMEMORY;
    }

    swprintf_s(searchPath, MAX_LONG_PATH_LEN, L"%s\\*", dirPath);
    hFind = FindFirstFileW(searchPath, &findData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findData.cFileName, L".") == 0 ||
                wcscmp(findData.cFileName, L"..") == 0) {
                continue;
            }

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }

            swprintf_s(fullPath, MAX_LONG_PATH_LEN, L"%s\\%s",
                       dirPath, findData.cFileName);

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                result = CollectDirectoryPaths(fullPath, paths, count, capacity);
                if (result != ERROR_SUCCESS) {
                    FindClose(hFind);
                    free(searchPath);
                    free(fullPath);
                    return result;
                }
            }

            result = AddPathToArray(paths, count, capacity, fullPath);
            if (result != ERROR_SUCCESS) {
                FindClose(hFind);
                free(searchPath);
                free(fullPath);
                return result;
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);
    }

    free(searchPath);
    free(fullPath);

    return AddPathToArray(paths, count, capacity, dirPath);
}

// 展开输入路径列表：非空目录递归展开为文件/子目录列表，交互模式下逐个确认
LONG ExpandDirectoryPaths(const wchar_t* const* inputPaths, int inputCount,
                          wchar_t*** outPaths, int* outCount,
                          BOOL interactive)
{
    wchar_t** arr = NULL;
    int count = 0;
    int capacity = 16;
    int i;
    int dirCount = 0;
    LONG result;

    *outPaths = NULL;
    *outCount = 0;

    arr = (wchar_t**)malloc(capacity * sizeof(wchar_t*));
    if (arr == NULL) return ERROR_OUTOFMEMORY;

    for (i = 0; i < inputCount; i++) {
        size_t pathLen;
        wchar_t* cleanPath;

        pathLen = wcslen(inputPaths[i]);
        cleanPath = (wchar_t*)malloc((pathLen + 1) * sizeof(wchar_t));
        if (cleanPath == NULL) {
            int j;
            for (j = 0; j < count; j++) free(arr[j]);
            free(arr);
            return ERROR_OUTOFMEMORY;
        }
        wcscpy_s(cleanPath, pathLen + 1, inputPaths[i]);
        TrimPath(cleanPath);

        if (cleanPath[0] == L'\0') {
            free(cleanPath);
            continue;
        }

        if (IsDirectoryPath(cleanPath) && IsDirectoryNonEmpty(cleanPath)) {
            if (interactive) {
                wchar_t answer[16];
                size_t alen;

                wprintf(L"\n[警告] \"%s\" 是非空目录。\n", cleanPath);
                wprintf(L"       将递归添加目录下所有文件和子目录到重启删除列表。\n");
                wprintf(L"       确认操作？(y/N): ");

                if (fgetws(answer, 16, stdin) == NULL) {
                    wprintf(L"[跳过] 已跳过目录：%s\n", cleanPath);
                    free(cleanPath);
                    continue;
                }

                alen = wcslen(answer);
                if (alen > 0 && answer[alen - 1] != L'\n') {
                    FlushStdin();
                }
                while (alen > 0 && (answer[alen - 1] == L'\n' || answer[alen - 1] == L'\r')) {
                    answer[--alen] = L'\0';
                }

                if (answer[0] != L'y' && answer[0] != L'Y') {
                    wprintf(L"[跳过] 已跳过目录：%s\n", cleanPath);
                    free(cleanPath);
                    continue;
                }
            }

            dirCount++;
            wprintf(L"[信息] 正在展开目录：%s\n", cleanPath);

            result = CollectDirectoryPaths(cleanPath, &arr, &count, &capacity);
            free(cleanPath);

            if (result != ERROR_SUCCESS) {
                int j;
                for (j = 0; j < count; j++) free(arr[j]);
                free(arr);
                return result;
            }
        } else {
            result = AddPathToArray(&arr, &count, &capacity, cleanPath);
            free(cleanPath);

            if (result != ERROR_SUCCESS) {
                int j;
                for (j = 0; j < count; j++) free(arr[j]);
                free(arr);
                return result;
            }
        }
    }

    if (dirCount > 0) {
        wprintf(L"[信息] 共展开 %d 个非空目录，展开后总计 %d 项路径\n\n",
                dirCount, count);
    }

    *outPaths = arr;
    *outCount = count;
    return ERROR_SUCCESS;
}
