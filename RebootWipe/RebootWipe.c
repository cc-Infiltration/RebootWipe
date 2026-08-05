/**
 * RebootWipe - Windows 重启文件操作管理器
 * 
 * 功能：管理 PendingFileRenameOperations 注册表项
 *   1. 查看 (Read)：解析并展示待处理文件操作列表
 *   2. 加入 (Add)：将文件标记为重启后删除
 *   3. 取消 (Remove)：跳过模式 / 直接抹除模式
 * 
 * 架构：
 *   - 底层 API 封装层：注册表读写、MoveFileEx 调用、内存管理
 *   - 核心业务逻辑层：REG_MULTI_SZ 解析、跳过算法、抹除算法
 *   - 用户交互层：CLI 界面
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>

/* ============================================================
 * 常量定义
 * ============================================================ */

/* 注册表路径 */
#define REG_KEY_PATH       L"SYSTEM\\CurrentControlSet\\Control\\Session Manager"
#define REG_VALUE_NAME     L"PendingFileRenameOperations"

/* WOW64 重定向标志：确保 32 位进程在 64 位系统上访问真实的 64 位注册表视图 */
#define REG_WOW64_FLAG     KEY_WOW64_64KEY

/* 路径长度限制 */
#define MAX_PATH_LEN       260

/* 带前缀 \\?\ 时 MoveFileExW 支持的最大路径长度（官方文档） */
#define MAX_LONG_PATH_LEN  32767

/* 多路径输入缓冲区最大长度（宽字符数） */
#define MAX_INPUT_LEN      8192

/* 分号分隔模式下最多解析的路径数 */
#define MAX_BATCH_PATHS    256

/* 控制台颜色（使用 Windows API 设置） */
#define CONSOLE_RED    FOREGROUND_RED | FOREGROUND_INTENSITY
#define CONSOLE_NORMAL FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE

/* 跳过前缀 */
#define SKIP_PREFIX        L"??"
#define SKIP_PREFIX_LEN    2

/* 操作类型 */
typedef enum {
    OP_DELETE   = 0,  /* 删除操作（重启后删除） */
    OP_MOVE     = 1,  /* 移动操作（重启后重命名） */
    OP_SKIPPED  = 2   /* 已跳过（带 ?? 前缀） */
} OperationType;

/* 取消模式 */
typedef enum {
    MODE_SKIP   = 0,  /* 跳过模式：注入 ?? 前缀 */
    MODE_ERASE  = 1   /* 抹除模式：物理删除条目 */
} RemoveMode;

/* ============================================================
 * 核心数据结构
 * ============================================================ */

/**
 * 待处理操作结构体
 */
typedef struct {
    OperationType type;                    /* 操作类型 */
    wchar_t sourcePath[MAX_PATH_LEN];      /* 源文件路径 */
    wchar_t targetPath[MAX_PATH_LEN];      /* 目标路径（仅移动操作有效） */
} PendingOperation;

/* ============================================================
 * 控制台颜色辅助函数
 * ============================================================ */

static WORD g_originalAttrs = 0;

static void SetConsoleColor(WORD attr)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hOut, attr);
}

static void SaveConsoleColor(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(hOut, &info);
    g_originalAttrs = info.wAttributes;
}

static void ResetConsoleColor(void)
{
    SetConsoleColor(g_originalAttrs);
}

/* 红色输出宏：设置红色 -> 输出 -> 恢复原色 */
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
 * 第一层：底层 API 封装层
 * ============================================================ */

/**
 * 打开注册表键（含 WOW64 回退机制）
 * 优先使用 KEY_WOW64_64KEY 访问 64 位视图，失败则回退到不带标志的方式
 * @param access 访问权限（KEY_READ / KEY_WRITE）
 * @param hKey   输出参数，返回注册表句柄
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG RW_OpenRegKey(REGSAM access, HKEY* hKey)
{
    LONG result;

    /* 第一优先级：使用 WOW64 标志访问 64 位注册表视图 */
    result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0,
                           access | REG_WOW64_FLAG, hKey);
    if (result == ERROR_SUCCESS) {
        return ERROR_SUCCESS;
    }

    /* 回退：不带 WOW64 标志（适用于 32 位系统或旧系统） */
    result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0,
                           access, hKey);
    return result;
}

/**
 * 打开或创建注册表键（含 WOW64 回退机制）
 * 用于写入场景，若键不存在则自动创建
 * @param access 访问权限（KEY_WRITE）
 * @param hKey   输出参数，返回注册表句柄
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG RW_CreateRegKey(REGSAM access, HKEY* hKey)
{
    LONG result;
    DWORD disposition;

    /* 第一优先级：使用 WOW64 标志打开或创建 64 位注册表视图 */
    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0, NULL, 0,
                             access | REG_WOW64_FLAG, NULL, hKey, &disposition);
    if (result == ERROR_SUCCESS) {
        return ERROR_SUCCESS;
    }

    /* 回退：不带 WOW64 标志 */
    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0, NULL, 0,
                             access, NULL, hKey, &disposition);
    return result;
}

/**
 * 读取 PendingFileRenameOperations 数据
 * @param buffer    输出参数，数据缓冲区（调用者负责 free）
 * @param size      输出参数，数据大小（字节）
 * @return 成功返回 ERROR_SUCCESS，无数据返回 ERROR_FILE_NOT_FOUND
 */
static LONG ReadRegistryData(BYTE** buffer, DWORD* size)
{
    HKEY hKey = NULL;
    LONG result;
    DWORD type;
    DWORD dataSize = 0;
    BYTE* data = NULL;

    /* 打开注册表 */
    result = RW_OpenRegKey(KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        return result;
    }

    /* 第一次调用：获取数据大小 */
    result = RegQueryValueExW(hKey, REG_VALUE_NAME, NULL, &type, NULL, &dataSize);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return result;
    }

    if (dataSize == 0) {
        RegCloseKey(hKey);
        return ERROR_FILE_NOT_FOUND;
    }

    /* 分配内存 */
    data = (BYTE*)malloc(dataSize);
    if (data == NULL) {
        RegCloseKey(hKey);
        return ERROR_OUTOFMEMORY;
    }

    /* 第二次调用：读取数据 */
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

/**
 * 写入 PendingFileRenameOperations 数据（含写入后验证）
 * @param buffer 数据缓冲区
 * @param size   数据大小（字节）
 * @return 成功返回 ERROR_SUCCESS，验证失败返回 ERROR_INTERNAL_ERROR
 */
static LONG WriteRegistryData(const BYTE* buffer, DWORD size)
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

    /* 写入后验证：立即读回数据确认写入成功 */
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
        /* 写入失败，分析原因 */
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

/**
 * 备份数据到临时文件
 * @param data     数据缓冲区
 * @param size     数据大小
 * @param path     输出参数，备份文件路径（调用者负责 free）
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG BackupData(const BYTE* data, DWORD size, wchar_t** path)
{
    wchar_t tempDir[MAX_PATH];
    wchar_t* backupPath = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    DWORD bytesWritten;

    /* 获取临时目录 */
    GetTempPathW(MAX_PATH, tempDir);

    /* 分配路径缓冲区 */
    backupPath = (wchar_t*)malloc(MAX_PATH * sizeof(wchar_t));
    if (backupPath == NULL) {
        return ERROR_OUTOFMEMORY;
    }

    /* 生成唯一文件名 */
    swprintf_s(backupPath, MAX_PATH, L"%sRebootWipe_%d.tmp", tempDir, GetTickCount());

    /* 写入文件 */
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
 * 第二层：核心业务逻辑层
 * ============================================================ */

/**
 * 解析 REG_MULTI_SZ 数据
 * 
 * 格式说明：
 *   删除操作：[源路径]\0\0
 *   移动操作：[源路径]\0[目标路径]\0
 *   终止符：连续的 \0\0
 * 
 * @param data         数据缓冲区
 * @param size         数据大小
 * @param operations   输出参数，操作数组（调用者负责 free）
 * @param count        输出参数，操作数量
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG ParseMultiSzData(const BYTE* data, DWORD size,
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

    /* 分配初始数组 */
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

        /* 找到源路径结束位置 */
        srcStart = ptr;
        srcEnd = ptr;
        while (srcEnd < end && *srcEnd != L'\0') {
            srcEnd++;
        }

        if (srcEnd >= end) break;

        srcLen = (size_t)(srcEnd - srcStart);
        if (srcLen == 0) {
            /* 空字符串，跳过 */
            ptr = srcEnd + 1;
            continue;
        }

        /* 检查是否需要扩容 */
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

        /* 提取源路径 */
        if (srcLen >= MAX_PATH_LEN) srcLen = MAX_PATH_LEN - 1;
        wcsncpy_s(op->sourcePath, MAX_PATH_LEN, srcStart, srcLen);
        op->sourcePath[srcLen] = L'\0';

        /* 检查是否为跳过项（以 ?? 开头） */
        if (srcLen >= SKIP_PREFIX_LEN &&
            op->sourcePath[0] == L'?' && op->sourcePath[1] == L'?') {
            /* 移除用户注入的 ?? 前缀 */
            size_t newLen = srcLen - SKIP_PREFIX_LEN;
            wcsncpy_s(op->sourcePath, MAX_PATH_LEN,
                      op->sourcePath + SKIP_PREFIX_LEN, newLen);
            op->sourcePath[newLen] = L'\0';
            op->type = OP_SKIPPED;
            srcLen = newLen;
        }

        /* 检查是否有 Windows 10/11 的 *N 会话标记前缀 */
        if (srcLen >= 2 && op->sourcePath[0] == L'*') {
            size_t skipLen = 1;  /* 跳过 '*' */
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

        /* 检查是否有 Windows API 的 \??\ 前缀 */
        if (srcLen >= 4 && op->sourcePath[0] == L'\\' &&
            op->sourcePath[1] == L'?' && op->sourcePath[2] == L'?' &&
            op->sourcePath[3] == L'\\') {
            size_t newPathLen = srcLen - 4;
            wcsncpy_s(op->sourcePath, MAX_PATH_LEN,
                      op->sourcePath + 4, newPathLen);
            op->sourcePath[newPathLen] = L'\0';
        }

        ptr = srcEnd + 1;

        /* 检查是否有目标路径 */
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

        /* 跳过终止符 */
        if (ptr < end && *ptr == L'\0') {
            ptr++;
        }
    }

    *operations = ops;
    *count = opCount;
    return ERROR_SUCCESS;
}

/**
 * 跳过模式：在目标条目前注入 ?? 前缀
 * @param buffer    数据缓冲区（会被重新分配）
 * @param size      数据大小（会被更新）
 * @param index     目标索引（从 0 开始）
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG InjectSkipPrefix(BYTE** buffer, DWORD* size, int index)
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

    /* 找到目标条目位置 */
    ptr = (const wchar_t*)oldBuffer;
    end = (const wchar_t*)(oldBuffer + oldSize);

    /* 跳过开头的空字符串（Windows API格式） */
    while (ptr < end && *ptr == L'\0') {
        ptr++;
    }

    while (ptr < end && currentIdx < index) {
        /* 跳过源路径 */
        while (ptr < end && *ptr != L'\0') ptr++;
        if (ptr >= end) break;
        ptr++;

        /* 跳过目标路径 */
        if (ptr < end && *ptr != L'\0') {
            while (ptr < end && *ptr != L'\0') ptr++;
            if (ptr >= end) break;
            ptr++;
        }

        currentIdx++;

        /* 跳过条目终止符 */
        if (ptr < end && *ptr == L'\0') {
            ptr++;
        }

        /* 跳过可能的空字符串分隔符 */
        while (ptr < end && *ptr == L'\0' && ptr + 1 < end && *(ptr + 1) != L'\0') {
            ptr++;
        }
    }

    if (currentIdx != index || ptr >= end) {
        return ERROR_INVALID_PARAMETER;
    }

    /* 检查是否已有 ?? 前缀 */
    if (ptr[0] == L'?' && ptr[1] == L'?') {
        return ERROR_SUCCESS;  /* 已是跳过状态 */
    }

    /* 计算新大小和偏移 */
    prefixBytes = SKIP_PREFIX_LEN * sizeof(wchar_t);
    newSize = oldSize + (DWORD)prefixBytes;
    offset = (size_t)((const BYTE*)ptr - oldBuffer);

    /* 分配新缓冲区 */
    newBuffer = (BYTE*)malloc(newSize);
    if (newBuffer == NULL) {
        return ERROR_OUTOFMEMORY;
    }

    /* 复制前半部分 */
    memcpy(newBuffer, oldBuffer, offset);

    /* 写入 ?? 前缀 */
    memcpy(newBuffer + offset, SKIP_PREFIX, prefixBytes);

    /* 复制后半部分 */
    memcpy(newBuffer + offset + prefixBytes, oldBuffer + offset, oldSize - (DWORD)offset);

    /* 写入注册表 */
    LONG result = WriteRegistryData(newBuffer, newSize);
    if (result == ERROR_SUCCESS) {
        free(oldBuffer);
        *buffer = newBuffer;
        *size = newSize;
    } else {
        free(newBuffer);
    }

    return result;
}

/**
 * 抹除模式：从内存中物理删除目标条目
 * @param buffer    数据缓冲区（会被重新分配）
 * @param size      数据大小（会被更新）
 * @param index     目标索引（从 0 开始）
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG EraseEntry(BYTE** buffer, DWORD* size, int index)
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

    /* 找到目标条目位置 */
    ptr = (const wchar_t*)oldBuffer;
    end = (const wchar_t*)(oldBuffer + oldSize);

    /* 跳过开头的空字符串（Windows API格式） */
    while (ptr < end && *ptr == L'\0') {
        ptr++;
    }

    while (ptr < end && currentIdx < index) {
        /* 跳过源路径 */
        while (ptr < end && *ptr != L'\0') ptr++;
        if (ptr >= end) break;
        ptr++;

        /* 跳过目标路径 */
        if (ptr < end && *ptr != L'\0') {
            while (ptr < end && *ptr != L'\0') ptr++;
            if (ptr >= end) break;
            ptr++;
        }

        currentIdx++;

        /* 跳过条目终止符 */
        if (ptr < end && *ptr == L'\0') {
            ptr++;
        }
    }

    if (currentIdx != index || ptr >= end) {
        return ERROR_INVALID_PARAMETER;
    }

    entryStart = ptr;
    offset = (size_t)((const BYTE*)entryStart - oldBuffer);

    /* 计算条目结束位置（源路径 + 可选目标路径 + 终止符） */
    ptr = entryStart;

    /* 跳过源路径 */
    while (ptr < end && *ptr != L'\0') ptr++;
    if (ptr >= end) return ERROR_INVALID_PARAMETER;
    ptr++;  /* 源路径后的 \0 */

    /* 跳过目标路径（如果有） */
    if (ptr < end && *ptr != L'\0') {
        while (ptr < end && *ptr != L'\0') ptr++;
        if (ptr >= end) return ERROR_INVALID_PARAMETER;
        ptr++;  /* 目标路径后的 \0 */
    }

    /* 判断是否为最后一个条目（后面只有终止符或没有数据） */
    isLastEntry = TRUE;
    {
        const wchar_t* checkPtr = ptr;
        /* 跳过终止符检查是否有更多数据 */
        if (checkPtr < end && *checkPtr == L'\0') {
            checkPtr++;
        }
        /* 跳过可能的空字符串分隔符 */
        while (checkPtr < end && *checkPtr == L'\0' && 
               checkPtr + 1 < end && *(checkPtr + 1) != L'\0') {
            checkPtr++;
        }
        /* 如果后面还有非空字符串，说明不是最后一个条目 */
        if (checkPtr < end && *checkPtr != L'\0') {
            isLastEntry = FALSE;
        }
    }

    /* 如果不是最后一个条目，条目结束位置包含终止符 */
    /* 如果是最后一个条目，条目结束位置不包含终止符（保留 \0\0 作为REG_MULTI_SZ结尾） */
    if (!isLastEntry) {
        /* 包含条目终止符 */
        if (ptr < end && *ptr == L'\0') {
            ptr++;
        }
    } else {
        /* 最后一个条目，删除后保留 \0 终止符 */
        /* 不包含条目终止符，让删除后的数据自然结尾 */
    }

    entryEnd = ptr;
    bytesToRemove = (size_t)((const BYTE*)entryEnd - (const BYTE*)entryStart);

    /* 计算新大小 */
    newSize = oldSize - (DWORD)bytesToRemove;

    /* 如果删除后数据无效，调整为最小的 REG_MULTI_SZ 格式（\0\0） */
    if (newSize < 2 * sizeof(wchar_t)) {
        if (newSize == 0) {
            /* 完全删除，返回空数据并添加 \0\0 */
            newSize = 2 * sizeof(wchar_t);
        }
    }

    /* 分配新缓冲区 */
    newBuffer = (BYTE*)malloc(newSize);
    if (newBuffer == NULL) {
        return ERROR_OUTOFMEMORY;
    }
    memset(newBuffer, 0, newSize);

    /* 复制前半部分 */
    memcpy(newBuffer, oldBuffer, offset);

    /* 复制后半部分（条目之后的数据） */
    {
        size_t copySize = oldSize - (DWORD)offset - (DWORD)bytesToRemove;
        if (copySize > 0 && entryEnd < end) {
            memmove(newBuffer + offset, (const BYTE*)entryEnd, copySize);
        }
    }

    /* 确保新数据以 \0\0 结尾 */
    {
        wchar_t* endPtr = (wchar_t*)(newBuffer + newSize - 2 * sizeof(wchar_t));
        if (endPtr[0] != L'\0' || endPtr[1] != L'\0') {
            /* 如果没有正确的终止符，添加它 */
            /* 找到字符串实际结尾 */
            wchar_t* lastStr = (wchar_t*)newBuffer;
            wchar_t* scanPtr = (wchar_t*)newBuffer;
            wchar_t* bufferEnd = (wchar_t*)(newBuffer + newSize);
            
            /* 找到最后一个字符串的结束位置 */
            while (scanPtr < bufferEnd) {
                while (scanPtr < bufferEnd && *scanPtr != L'\0') scanPtr++;
                if (scanPtr >= bufferEnd) break;
                lastStr = scanPtr;  /* 记录字符串结束位置 */
                scanPtr++;
            }
            
            /* 在最后一个字符串后添加终止符 */
            if (lastStr >= (wchar_t*)newBuffer && 
                lastStr + 2 <= (wchar_t*)(newBuffer + newSize)) {
                lastStr[1] = L'\0';
            }
        }
    }

    /* 写入注册表 */
    LONG result = WriteRegistryData(newBuffer, newSize);
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
 * 第三层：用户交互层 - 功能函数
 * ============================================================ */

/**
 * 显示待处理文件操作列表
 * @return 操作数量，失败返回 -1
 */
static int ShowPendingList(void)
{
    BYTE* data = NULL;
    DWORD size = 0;
    PendingOperation* ops = NULL;
    int count = 0;
    LONG result;
    int i;

    result = ReadRegistryData(&data, &size);
    if (result == ERROR_FILE_NOT_FOUND) {
        wprintf(L"[信息] 当前没有待处理的文件操作。\n");
        return 0;
    }
    if (result != ERROR_SUCCESS) {
        WPRINTF_RED(L"[错误] 读取注册表失败，错误码：%lu\n", result);
        return -1;
    }

    result = ParseMultiSzData(data, size, &ops, &count);
    free(data);

    if (result != ERROR_SUCCESS) {
        WPRINTF_RED0(L"[错误] 解析数据失败。\n");
        return -1;
    }

    if (count == 0) {
        wprintf(L"[信息] 当前没有待处理的文件操作。\n");
        free(ops);
        return 0;
    }

    wprintf(L"\n================================================================\n");
    wprintf(L"  PendingFileRenameOperations 列表（共 %d 项）\n", count);
    wprintf(L"================================================================\n\n");

    for (i = 0; i < count; i++) {
        wprintf(L"  [%d] ", i + 1);

        switch (ops[i].type) {
        case OP_DELETE:
            wprintf(L"类型：删除（重启后删除）\n");
            wprintf(L"      源：%s\n", ops[i].sourcePath);
            break;

        case OP_MOVE:
            wprintf(L"类型：移动（重启后重命名）\n");
            wprintf(L"      源：%s\n", ops[i].sourcePath);
            wprintf(L"      目标：%s\n", ops[i].targetPath);
            break;

        case OP_SKIPPED:
            wprintf(L"类型：已跳过（标记为 ?? 前缀）\n");
            wprintf(L"      源：%s\n", ops[i].sourcePath);
            break;
        }
        wprintf(L"\n");
    }

    wprintf(L"================================================================\n\n");

    free(ops);
    return count;
}

/**
 * 去除路径首尾的引号和空白字符（用户可能用引号包裹路径）
 * @param str 输入字符串
 * @return 指向有效内容的指针（在原字符串上修改）
 */
static wchar_t* TrimPath(wchar_t* str)
{
    wchar_t* start;
    wchar_t* end;

    if (str == NULL) return NULL;

    /* 跳过首部空白和引号 */
    start = str;
    while (*start == L' ' || *start == L'\t' || *start == L'"') {
        start++;
    }

    /* 找到尾部有效字符 */
    end = start + wcslen(start) - 1;
    while (end > start && (*end == L' ' || *end == L'\t' || *end == L'"')) {
        *end = L'\0';
        end--;
    }

    /* 如果首部有引号，需要移动整个字符串 */
    if (start != str) {
        wcscpy_s(str, wcslen(str) + 1, start);
    }

    return str;
}

/**
 * 将文件添加到重启删除列表
 * @param filePath 文件路径
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG AddToDeleteList(const wchar_t* filePath)
{
    LONG result;
    wchar_t* cleanPath;

    if (filePath == NULL || *filePath == L'\0') {
        WPRINTF_RED0(L"[错误] 文件路径不能为空。\n");
        return ERROR_INVALID_PARAMETER;
    }

    /* 去除首尾引号和空白，复制到可变缓冲区 */
    cleanPath = (wchar_t*)malloc((wcslen(filePath) + 1) * sizeof(wchar_t));
    if (cleanPath == NULL) {
        return ERROR_OUTOFMEMORY;
    }
    wcscpy_s(cleanPath, wcslen(filePath) + 1, filePath);
    TrimPath(cleanPath);

    /* 使用官方 API */
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

/**
 * 从文本文件读取路径列表（每行一个路径）
 * 支持注释行（# 开头）和空行跳过，自动去除首尾空白和引号
 * @param filePath  文本文件路径
 * @param paths     输出参数，路径指针数组（调用者负责 free 每个元素和数组本身）
 * @param count     输出参数，路径数量
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG ReadPathsFromFile(const wchar_t* filePath, wchar_t*** paths, int* count)
{
    FILE* fp = NULL;
    errno_t err;
    wchar_t line[MAX_LONG_PATH_LEN];
    wchar_t** arr = NULL;
    int arrCount = 0;
    int capacity = 16;
    size_t i;

    *paths = NULL;
    *count = 0;

    err = _wfopen_s(&fp, filePath, L"r, ccs=UTF-8");
    if (err != 0 || fp == NULL) {
        return ERROR_FILE_NOT_FOUND;
    }

    arr = (wchar_t**)malloc(capacity * sizeof(wchar_t*));
    if (arr == NULL) {
        fclose(fp);
        return ERROR_OUTOFMEMORY;
    }

    while (fgetws(line, MAX_LONG_PATH_LEN, fp) != NULL) {
        wchar_t* trimmed;
        size_t len;

        /* 移除换行符 */
        len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) {
            line[--len] = L'\0';
        }

        /* 跳过空行 */
        if (len == 0) continue;

        /* 跳过注释行（# 开头） */
        if (line[0] == L'#') continue;

        /* 复制并去除首尾空白和引号 */
        trimmed = (wchar_t*)malloc((wcslen(line) + 1) * sizeof(wchar_t));
        if (trimmed == NULL) {
            for (i = 0; i < (size_t)arrCount; i++) free(arr[i]);
            free(arr);
            fclose(fp);
            return ERROR_OUTOFMEMORY;
        }
        wcscpy_s(trimmed, wcslen(line) + 1, line);
        TrimPath(trimmed);

        /* 跳过 trim 后为空的行 */
        if (trimmed[0] == L'\0') {
            free(trimmed);
            continue;
        }

        /* 扩容 */
        if (arrCount >= capacity) {
            wchar_t** newArr;
            capacity *= 2;
            newArr = (wchar_t**)realloc(arr, capacity * sizeof(wchar_t*));
            if (newArr == NULL) {
                free(trimmed);
                for (i = 0; i < (size_t)arrCount; i++) free(arr[i]);
                free(arr);
                fclose(fp);
                return ERROR_OUTOFMEMORY;
            }
            arr = newArr;
        }

        arr[arrCount++] = trimmed;
    }

    fclose(fp);
    *paths = arr;
    *count = arrCount;
    return ERROR_SUCCESS;
}

/**
 * 批量添加文件到重启删除列表
 * 根据微软官方文档，多次调用 MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)
 * 会按调用顺序依次追加到 PendingFileRenameOperations 注册表项，
 * 系统在重启时按相同顺序执行删除/移动操作。
 *
 * 注意（官方文档）：
 *   - 操作在重启时按调用顺序执行。若要删除含文件的目录，
 *     必须先在列表中删除目录内的文件，再删除目录本身。
 *   - 路径默认限制 MAX_PATH(260) 字符；超过时需加 \\?\ 前缀，
 *     可扩展至 32767 宽字符。
 *   - 远程共享路径不支持 MOVEFILE_DELAY_UNTIL_REBOOT。
 *
 * @param paths  路径指针数组
 * @param count  路径数量
 * @return 成功添加的数量
 */
static int AddMultipleToDeleteList(const wchar_t* const* paths, int count)
{
    int i;
    int success = 0;
    int failure = 0;

    if (paths == NULL || count <= 0) return 0;

    wprintf(L"\n--- 批量添加重启删除任务（共 %d 项）---\n\n", count);

    for (i = 0; i < count; i++) {
        wprintf(L"[%d/%d] ", i + 1, count);
        if (AddToDeleteList(paths[i]) == ERROR_SUCCESS) {
            success++;
        } else {
            failure++;
        }
    }

    wprintf(L"\n");
    wprintf(L"================================================================\n");
    wprintf(L"  批量添加完成：成功 %d 项，失败 %d 项\n", success, failure);
    wprintf(L"================================================================\n");

    return success;
}

/* ============================================================
 * 目录递归展开功能
 * ============================================================ */

/**
 * 检查路径是否为目录
 * @param path 文件路径
 * @return TRUE 如果是目录，FALSE 否则
 */
static BOOL IsDirectoryPath(const wchar_t* path)
{
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return FALSE;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
}

/**
 * 检查目录是否非空
 * @param dirPath 目录路径
 * @return TRUE 如果目录非空，FALSE 如果为空或无法访问
 */
static BOOL IsDirectoryNonEmpty(const wchar_t* dirPath)
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

/**
 * 向路径数组中添加一个路径（自动扩容）
 * @param paths     路径数组指针（会被更新）
 * @param count     路径数量指针（会被更新）
 * @param capacity  数组容量指针（会被更新）
 * @param path      要添加的路径（会被复制）
 * @return 成功返回 ERROR_SUCCESS
 */
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

/**
 * 递归收集目录下所有路径（后序遍历：子项在前，目录本身在后）
 * 确保重启删除时先删内容再删目录（符合微软官方文档要求）
 * 跳过重解析点（符号链接、挂载点等）以避免无限循环
 * @param dirPath    目录路径
 * @param paths      路径数组指针（会被更新）
 * @param count      路径数量指针（会被更新）
 * @param capacity   数组容量指针（会被更新）
 * @return 成功返回 ERROR_SUCCESS
 */
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
            /* 跳过 . 和 .. */
            if (wcscmp(findData.cFileName, L".") == 0 ||
                wcscmp(findData.cFileName, L"..") == 0) {
                continue;
            }

            /* 跳过重解析点（符号链接、挂载点），避免无限循环 */
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }

            swprintf_s(fullPath, MAX_LONG_PATH_LEN, L"%s\\%s",
                       dirPath, findData.cFileName);

            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                /* 递归处理子目录（后序：先收集子目录内容） */
                result = CollectDirectoryPaths(fullPath, paths, count, capacity);
                if (result != ERROR_SUCCESS) {
                    FindClose(hFind);
                    free(searchPath);
                    free(fullPath);
                    return result;
                }
            }

            /* 添加当前路径（文件或已处理完内容的子目录） */
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

    /* 最后添加目录本身（后序：所有内容已在前面） */
    return AddPathToArray(paths, count, capacity, dirPath);
}

/**
 * 展开路径列表中的非空目录
 * 将非空目录递归展开为其内部所有文件/子目录 + 目录本身（后序排列）
 * 交互模式下对非空目录逐个提示确认
 * @param inputPaths   用户输入的路径数组
 * @param inputCount   用户输入的路径数量
 * @param outPaths     输出参数，展开后的路径数组（调用者负责 free 每个元素和数组）
 * @param outCount     输出参数，展开后的路径数量
 * @param interactive  是否为交互模式（TRUE 时对非空目录提示确认）
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG ExpandDirectoryPaths(const wchar_t* const* inputPaths, int inputCount,
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

        /* 创建已修剪的路径副本用于目录检测 */
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

        /* 跳过空路径 */
        if (cleanPath[0] == L'\0') {
            free(cleanPath);
            continue;
        }

        /* 检查是否为非空目录 */
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

                /* 清除输入缓冲区中剩余的字符 */
                alen = wcslen(answer);
                if (alen > 0 && answer[alen - 1] != L'\n') {
                    wint_t ch;
                    while ((ch = fgetwc(stdin)) != L'\n' && ch != WEOF) { }
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

            /* 递归收集所有路径（后序排列） */
            result = CollectDirectoryPaths(cleanPath, &arr, &count, &capacity);
            free(cleanPath);

            if (result != ERROR_SUCCESS) {
                int j;
                for (j = 0; j < count; j++) free(arr[j]);
                free(arr);
                return result;
            }
        } else {
            /* 文件或空目录：直接添加 */
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

/**
 * 取消指定的文件操作
 * @param index 操作索引（从 1 开始）
 * @param mode  取消模式（跳过/抹除）
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG RemoveOperation(int index, RemoveMode mode)
{
    BYTE* data = NULL;
    DWORD size = 0;
    PendingOperation* ops = NULL;
    int count = 0;
    LONG result;
    int targetIdx;
    wchar_t* backupPath = NULL;

    if (index < 1) {
        WPRINTF_RED0(L"[错误] 索引必须大于等于 1。\n");
        return ERROR_INVALID_PARAMETER;
    }

    targetIdx = index - 1;

    /* 读取当前数据 */
    result = ReadRegistryData(&data, &size);
    if (result == ERROR_FILE_NOT_FOUND) {
        wprintf(L"[信息] 当前没有待处理的操作。\n");
        return ERROR_SUCCESS;
    }
    if (result != ERROR_SUCCESS) {
        WPRINTF_RED(L"[错误] 读取注册表失败，错误码：%lu\n", result);
        return result;
    }

    /* 解析数据 */
    result = ParseMultiSzData(data, size, &ops, &count);
    if (result != ERROR_SUCCESS) {
        free(data);
        WPRINTF_RED0(L"[错误] 解析数据失败。\n");
        return result;
    }

    /* 检查索引范围 */
    if (targetIdx >= count) {
        free(data);
        free(ops);
        WPRINTF_RED(L"[错误] 索引 %d 超出范围（共 %d 项）。\n", index, count);
        return ERROR_INVALID_PARAMETER;
    }

    /* 检查是否为跳过项 */
    if (ops[targetIdx].type == OP_SKIPPED) {
        if (mode == MODE_SKIP) {
            /* 跳过模式：已经是跳过状态，直接返回 */
            free(data);
            free(ops);
            wprintf(L"[警告] 第 %d 项已是跳过状态。\n", index);
            return ERROR_SUCCESS;
        }
        /* 抹除模式：继续执行删除操作 */
    }

    /* 抹除模式：先备份 */
    if (mode == MODE_ERASE) {
        wprintf(L"[安全] 正在备份数据...\n");
        result = BackupData(data, size, &backupPath);
        if (result == ERROR_SUCCESS) {
            wprintf(L"[安全] 备份已保存至：%s\n", backupPath);
        } else {
            WPRINTF_RED0(L"[警告] 备份失败，继续执行...\n");
        }
    }

    /* 执行操作 */
    if (mode == MODE_SKIP) {
        result = InjectSkipPrefix(&data, &size, targetIdx);
    } else {
        result = EraseEntry(&data, &size, targetIdx);
    }

    /* 清理 */
    if (data) free(data);
    free(ops);
    if (backupPath) free(backupPath);

    if (result == ERROR_SUCCESS) {
        wprintf(L"[成功] 操作已完成。\n");
    } else {
        WPRINTF_RED(L"[错误] 操作失败，错误码：%lu\n", result);
    }

    return result;
}

/* ============================================================
 * 第三层：用户交互层 - CLI 界面
 * ============================================================ */

/**
 * 显示主菜单
 */
static void ShowMenu(void)
{
    wprintf(L"\n");
    wprintf(L"╔════════════════════════════════════════════════════════════╗\n");
    wprintf(L"║       RebootWipe - Windows 重启文件操作管理器             ║\n");
    wprintf(L"╚════════════════════════════════════════════════════════════╝\n");
    wprintf(L"\n");
    wprintf(L"  [1] 查看待处理的文件操作列表\n");
    wprintf(L"  [2] 将文件标记为重启后删除\n");
    wprintf(L"  [3] 取消操作 - 跳过模式（注入 ?? 前缀）\n");
    wprintf(L"  [4] 取消操作 - 直接抹除模式\n");
    wprintf(L"  [5] 退出程序\n");
    wprintf(L"\n");
    wprintf(L"  请选择操作 (1-5): ");
}

/**
 * 处理查看操作
 */
static void HandleView(void)
{
    wprintf(L"\n--- 查看待处理文件操作 ---\n");
    ShowPendingList();
}

/**
 * 处理添加操作
 * 支持单路径、分号分隔的多路径、@文件路径导入、非空目录递归展开
 */
static void HandleAdd(void)
{
    wchar_t input[MAX_INPUT_LEN];
    size_t len;
    wchar_t** inputPaths = NULL;
    int inputCount = 0;
    int i;

    wprintf(L"\n--- 添加重启删除任务 ---\n\n");
    wprintf(L"  请输入文件路径：\n");
    wprintf(L"    · 单个路径直接输入\n");
    wprintf(L"    · 多个路径用分号 ; 分隔\n");
    wprintf(L"    · 从文件导入：@文件路径（每行一个路径，# 为注释）\n");
    wprintf(L"    · 支持目录：非空目录将递归展开并提示确认\n");
    wprintf(L"  : ");

    if (fgetws(input, MAX_INPUT_LEN, stdin) == NULL) {
        WPRINTF_RED0(L"[错误] 读取输入失败。\n");
        return;
    }

    /* 检查输入是否过长（未以换行符结尾说明缓冲区已满） */
    len = wcslen(input);
    if (len > 0 && input[len - 1] != L'\n') {
        wint_t ch;
        while ((ch = fgetwc(stdin)) != L'\n' && ch != WEOF) { }
        WPRINTF_RED0(L"[警告] 输入过长，已截断。\n");
    }

    /* 移除换行符 */
    while (len > 0 && (input[len - 1] == L'\n' || input[len - 1] == L'\r')) {
        input[--len] = L'\0';
    }

    if (len == 0) {
        WPRINTF_RED0(L"[错误] 输入不能为空。\n");
        return;
    }

    /* 解析输入路径 */
    if (input[0] == L'@') {
        /* 文件导入模式 */
        wchar_t* filePath = input + 1;
        LONG result;

        TrimPath(filePath);
        if (filePath[0] == L'\0') {
            WPRINTF_RED0(L"[错误] 文件路径不能为空。\n");
            return;
        }

        result = ReadPathsFromFile(filePath, &inputPaths, &inputCount);
        if (result != ERROR_SUCCESS) {
            WPRINTF_RED(L"[错误] 无法读取文件列表：%s\n", filePath);
            return;
        }
        if (inputCount == 0) {
            wprintf(L"[信息] 文件列表为空。\n");
            free(inputPaths);
            return;
        }
    } else {
        /* 分号分隔的多路径模式 */
        wchar_t* token;
        wchar_t* context;

        inputPaths = (wchar_t**)malloc(MAX_BATCH_PATHS * sizeof(wchar_t*));
        if (inputPaths == NULL) {
            WPRINTF_RED0(L"[错误] 内存不足。\n");
            return;
        }

        token = wcstok_s(input, L";", &context);
        while (token != NULL && inputCount < MAX_BATCH_PATHS) {
            if (*token != L'\0') {
                inputPaths[inputCount] = _wcsdup(token);
                if (inputPaths[inputCount] == NULL) {
                    for (i = 0; i < inputCount; i++) free(inputPaths[i]);
                    free(inputPaths);
                    WPRINTF_RED0(L"[错误] 内存不足。\n");
                    return;
                }
                inputCount++;
            }
            token = wcstok_s(NULL, L";", &context);
        }

        if (inputCount == 0) {
            WPRINTF_RED0(L"[错误] 未解析到有效路径。\n");
            free(inputPaths);
            return;
        }
    }

    /* 展开非空目录（交互模式，提示确认） */
    {
        wchar_t** expandedPaths = NULL;
        int expandedCount = 0;
        LONG result;

        result = ExpandDirectoryPaths((const wchar_t* const*)inputPaths, inputCount,
                                       &expandedPaths, &expandedCount, TRUE);

        /* 释放输入路径 */
        for (i = 0; i < inputCount; i++) free(inputPaths[i]);
        free(inputPaths);

        if (result != ERROR_SUCCESS) {
            WPRINTF_RED0(L"[错误] 展开目录失败。\n");
            return;
        }

        if (expandedCount == 0) {
            wprintf(L"[信息] 没有有效的路径需要添加。\n");
            free(expandedPaths);
            return;
        }

        /* 批量添加 */
        AddMultipleToDeleteList((const wchar_t* const*)expandedPaths, expandedCount);

        /* 释放展开后的路径 */
        for (i = 0; i < expandedCount; i++) free(expandedPaths[i]);
        free(expandedPaths);
    }
}

/**
 * 处理取消操作
 * @param mode 取消模式
 */
static void HandleRemove(RemoveMode mode)
{
    wchar_t input[32];
    int index;

    wprintf(L"\n--- %s ---\n\n",
            mode == MODE_SKIP ? L"跳过模式" : L"直接抹除模式");

    ShowPendingList();

    wprintf(L"  请输入要取消的操作序号 (0 返回): ");

    if (fgetws(input, 32, stdin) == NULL) {
        WPRINTF_RED0(L"[错误] 读取输入失败。\n");
        return;
    }

    index = _wtoi(input);
    if (index == 0) {
        wprintf(L"  已返回主菜单。\n");
        return;
    }
    if (index < 0) {
        WPRINTF_RED0(L"[错误] 无效的序号。\n");
        return;
    }

    RemoveOperation(index, mode);
}

/**
 * 显示命令行帮助
 */
static void ShowHelp(const wchar_t* progName)
{
    wprintf(L"RebootWipe - Windows 重启文件操作管理器\n\n");
    wprintf(L"用法：\n");
    wprintf(L"  %s                    交互式模式\n", progName);
    wprintf(L"  %s read               查看待处理列表\n", progName);
    wprintf(L"  %s add <路径> [路径...]  添加一个或多个文件/目录到重启删除列表\n", progName);
    wprintf(L"  %s add @<列表文件>       从文本文件批量导入（每行一个路径，# 注释）\n", progName);
    wprintf(L"  %s skip <n>           跳过第 n 项操作\n", progName);
    wprintf(L"  %s erase <n>          抹除第 n 项操作\n", progName);
    wprintf(L"  %s help               显示帮助\n", progName);
    wprintf(L"\n");
    wprintf(L"交互式 add 说明：\n");
    wprintf(L"  · 单个路径直接输入\n");
    wprintf(L"  · 多个路径用分号 ; 分隔\n");
    wprintf(L"  · 从文件导入：@文件路径\n");
    wprintf(L"  · 支持目录路径：非空目录将递归展开并提示确认\n");
    wprintf(L"\n");
    wprintf(L"注意：重启时按输入顺序执行删除。非空目录会自动递归展开，\n");
    wprintf(L"      先添加目录内文件/子目录，再添加目录本身。\n");
    wprintf(L"      路径超过 260 字符时需加 \\?\\ 前缀（最多 32767 字符）。\n");
}

/**
 * 解析并执行命令行参数
 * @return 返回值：0=进入交互模式，1=命令执行完成，-1=错误
 */
static int ParseCommand(int argc, wchar_t* argv[])
{
    if (argc < 2) {
        return 0;  /* 进入交互模式 */
    }

    if (_wcsicmp(argv[1], L"help") == 0 || 
        _wcsicmp(argv[1], L"-h") == 0 || 
        _wcsicmp(argv[1], L"/?") == 0) {
        ShowHelp(argv[0]);
        return 1;
    }

    if (_wcsicmp(argv[1], L"read") == 0) {
        return ShowPendingList() < 0 ? -1 : 0;
    }

    if (_wcsicmp(argv[1], L"add") == 0) {
        int i;
        wchar_t** inputPaths = NULL;
        int inputCount = 0;

        if (argc < 3) {
            WPRINTF_RED0(L"[错误] 缺少文件路径参数。\n");
            wprintf(L"用法：%s add <文件路径> [文件路径 ...]\n", argv[0]);
            wprintf(L"      %s add @<列表文件>\n", argv[0]);
            return -1;
        }

        /* 检查是否为文件导入模式（@ 前缀且为唯一路径参数） */
        if (argv[2][0] == L'@' && argc == 3) {
            LONG result = ReadPathsFromFile(argv[2] + 1, &inputPaths, &inputCount);
            if (result != ERROR_SUCCESS) {
                WPRINTF_RED(L"[错误] 无法读取文件列表：%s\n", argv[2] + 1);
                return -1;
            }
            if (inputCount == 0) {
                wprintf(L"[信息] 文件列表为空。\n");
                free(inputPaths);
                return 0;
            }
        } else {
            /* 多路径参数模式：argv[2] 到 argv[argc-1] */
            int total = argc - 2;
            inputPaths = (wchar_t**)malloc(total * sizeof(wchar_t*));
            if (inputPaths == NULL) {
                WPRINTF_RED0(L"[错误] 内存不足。\n");
                return -1;
            }
            for (i = 0; i < total; i++) {
                inputPaths[i] = _wcsdup(argv[2 + i]);
                if (inputPaths[i] == NULL) {
                    int j;
                    for (j = 0; j < i; j++) free(inputPaths[j]);
                    free(inputPaths);
                    WPRINTF_RED0(L"[错误] 内存不足。\n");
                    return -1;
                }
                inputCount++;
            }
        }

        /* 展开非空目录（命令行模式，不提示确认） */
        {
            wchar_t** expandedPaths = NULL;
            int expandedCount = 0;
            int success;
            LONG result;

            result = ExpandDirectoryPaths((const wchar_t* const*)inputPaths,
                                           inputCount, &expandedPaths,
                                           &expandedCount, FALSE);

            /* 释放输入路径 */
            for (i = 0; i < inputCount; i++) free(inputPaths[i]);
            free(inputPaths);

            if (result != ERROR_SUCCESS) {
                WPRINTF_RED0(L"[错误] 展开目录失败。\n");
                return -1;
            }

            if (expandedCount == 0) {
                wprintf(L"[信息] 没有有效的路径需要添加。\n");
                free(expandedPaths);
                return 0;
            }

            success = AddMultipleToDeleteList(
                (const wchar_t* const*)expandedPaths, expandedCount);

            for (i = 0; i < expandedCount; i++) free(expandedPaths[i]);
            free(expandedPaths);

            return (success == expandedCount) ? 0 : -1;
        }
    }

    if (_wcsicmp(argv[1], L"skip") == 0) {
        int index;
        if (argc < 3) {
            WPRINTF_RED0(L"[错误] 缺少序号参数。\n");
            wprintf(L"用法：%s skip <序号>\n", argv[0]);
            return -1;
        }
        index = _wtoi(argv[2]);
        if (index <= 0) {
            WPRINTF_RED0(L"[错误] 序号必须为正整数。\n");
            return -1;
        }
        if (RemoveOperation(index, MODE_SKIP) != ERROR_SUCCESS) {
            return -1;
        }
        return 0;
    }

    if (_wcsicmp(argv[1], L"erase") == 0) {
        int index;
        if (argc < 3) {
            WPRINTF_RED0(L"[错误] 缺少序号参数。\n");
            wprintf(L"用法：%s erase <序号>\n", argv[0]);
            return -1;
        }
        index = _wtoi(argv[2]);
        if (index <= 0) {
            WPRINTF_RED0(L"[错误] 序号必须为正整数。\n");
            return -1;
        }
        if (RemoveOperation(index, MODE_ERASE) != ERROR_SUCCESS) {
            return -1;
        }
        return 0;
    }

    WPRINTF_RED(L"[错误] 未知命令：%s\n", argv[1]);
    ShowHelp(argv[0]);
    return -1;
}

/* ============================================================
 * 自动提权功能
 * ============================================================ */

/**
 * 检查当前进程是否以管理员身份运行
 * @return TRUE 如果是管理员，FALSE 否则
 */
static BOOL IsAdministrator(void)
{
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = NULL;
    HANDLE tokenHandle = NULL;
    BOOL checkResult;

    /* 创建管理员组 SID */
    if (!AllocateAndInitializeSid(&ntAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup)) {
        return FALSE;
    }

    /* 打开当前进程的访问令牌 */
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenHandle)) {
        FreeSid(adminGroup);
        return FALSE;
    }

    /* 检查令牌是否包含管理员组 */
    checkResult = CheckTokenMembership(NULL, adminGroup, &isAdmin);

    CloseHandle(tokenHandle);
    FreeSid(adminGroup);

    return isAdmin && checkResult;
}

/**
 * 以管理员身份重新启动程序
 * @param argc 参数数量
 * @param argv 参数数组
 * @return 如果成功启动提权进程则返回 TRUE，FALSE 否则
 */
static BOOL RunAsAdmin(int argc, wchar_t* argv[])
{
    wchar_t exePath[MAX_PATH];
    wchar_t params[32768] = L"";
    SHELLEXECUTEINFOW sei;
    int i;
    DWORD result;

    /* 获取当前可执行文件路径 */
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        return FALSE;
    }

    /* 构建命令行参数 */
    for (i = 1; i < argc; i++) {
        /* 如果参数包含空格，用引号包裹 */
        if (wcschr(argv[i], L' ') != NULL) {
            wcscat_s(params, 32768, L"\"");
            wcscat_s(params, 32768, argv[i]);
            wcscat_s(params, 32768, L"\"");
        } else {
            wcscat_s(params, 32768, argv[i]);
        }
        if (i < argc - 1) {
            wcscat_s(params, 32768, L" ");
        }
    }

    /* 初始化 SHELLEXECUTEINFOW 结构 */
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    /* 执行提权启动 */
    result = ShellExecuteExW(&sei);

    if (!result) {
        return FALSE;
    }

    /* 释放进程句柄 */
    if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }

    return TRUE;
}

/**
 * 暂停并清屏
 * 等待用户按回车键后清空控制台，保持界面整洁
 */
static void PauseAndClear(void)
{
    wint_t ch;
    wprintf(L"\n按回车键继续...");
    /* 读取并丢弃所有字符直到换行符或 EOF */
    while ((ch = fgetwc(stdin)) != L'\n' && ch != WEOF) { }
    system("cls");
}

/* ============================================================
 * 程序入口
 * ============================================================ */

int wmain(int argc, wchar_t* argv[])
{
    wchar_t input[64];
    int choice;
    int result = 0;

    /* 设置控制台为 UTF-16 文本模式，确保 wprintf 正确输出中文 */
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    /* 保存原始控制台颜色，用于错误输出后恢复 */
    SaveConsoleColor();

    /* 自动提权：如果不是管理员，请求 UAC 提升 */
    if (!IsAdministrator()) {
        wprintf(L"[信息] 正在请求管理员权限...\n");
        if (RunAsAdmin(argc, argv)) {
            /* 提权成功，退出当前进程 */
            return 0;
        } else {
            WPRINTF_RED0(L"[错误] 无法获取管理员权限，程序将继续运行但部分功能可能受限。\n");
            wprintf(L"       建议：右键点击程序，选择「以管理员身份运行」。\n\n");
        }
    }

    /* 命令行模式 */
    if (argc > 1) {
        result = ParseCommand(argc, argv);
        return result < 0 ? 1 : 0;
    }
    while (1) {
        ShowMenu();

        if (fgetws(input, sizeof(input) / sizeof(input[0]), stdin) == NULL) {
            break;
        }

        /* 清除输入缓冲区中剩余的字符（处理超长输入） */
        {
            wint_t ch;
            size_t len = wcslen(input);
            /* 如果输入不以换行符结尾，说明缓冲区还有剩余数据 */
            if (len > 0 && input[len - 1] != L'\n') {
                /* 读取并丢弃剩余字符，直到换行符或EOF */
                while ((ch = fgetwc(stdin)) != L'\n' && ch != WEOF) {
                    /* 清空缓冲区 */
                }
                /* 清空 input 并设置错误提示 */
                input[0] = L'\0';
            }
        }

        /* 移除尾部换行符 */
        {
            size_t len = wcslen(input);
            while (len > 0 && (input[len - 1] == L'\n' || input[len - 1] == L'\r')) {
                input[--len] = L'\0';
            }
        }

        /* 检查是否为空输入 */
        if (input[0] == L'\0') {
            wprintf(L"[错误] 输入过长，请重新选择 1-5。\n");
            continue;
        }

        choice = _wtoi(input);

        switch (choice) {
        case 1:
            HandleView();
            PauseAndClear();
            break;
        case 2:
            HandleAdd();
            PauseAndClear();
            break;
        case 3:
            HandleRemove(MODE_SKIP);
            PauseAndClear();
            break;
        case 4:
            HandleRemove(MODE_ERASE);
            PauseAndClear();
            break;
        case 5:
            wprintf(L"\n再见！\n");
            return 0;
        default:
            wprintf(L"\n[错误] 无效选择，请输入 1-5。\n");
            break;
        }
    }

    return 0;
}