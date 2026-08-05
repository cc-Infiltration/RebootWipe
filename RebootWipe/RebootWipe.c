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

/* ============================================================
 * 常量定义
 * ============================================================ */

/* 注册表路径 */
#define REG_KEY_PATH       L"SYSTEM\\CurrentControlSet\\Control\\Session Manager"
#define REG_VALUE_NAME     L"PendingFileRenameOperations"

/* 路径长度限制 */
#define MAX_PATH_LEN       260

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
 * 第一层：底层 API 封装层
 * ============================================================ */

/**
 * 打开注册表键
 * @param access 访问权限（KEY_READ / KEY_WRITE）
 * @param hKey   输出参数，返回注册表句柄
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG RW_OpenRegKey(REGSAM access, HKEY* hKey)
{
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, REG_KEY_PATH, 0, access, hKey);
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
 * 写入 PendingFileRenameOperations 数据
 * @param buffer 数据缓冲区
 * @param size   数据大小（字节）
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG WriteRegistryData(const BYTE* buffer, DWORD size)
{
    HKEY hKey = NULL;
    LONG result;

    result = RW_OpenRegKey(KEY_WRITE, &hKey);
    if (result != ERROR_SUCCESS) {
        return result;
    }

    if (size == 0) {
        /* 数据为空，删除注册表值 */
        result = RegDeleteValueW(hKey, REG_VALUE_NAME);
    } else {
        result = RegSetValueExW(hKey, REG_VALUE_NAME, 0, REG_MULTI_SZ, buffer, size);
    }

    RegCloseKey(hKey);
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
            /* 移除 ?? 前缀 */
            wcsncpy_s(op->sourcePath, MAX_PATH_LEN, 
                      op->sourcePath + SKIP_PREFIX_LEN, srcLen - SKIP_PREFIX_LEN);
            op->sourcePath[srcLen - SKIP_PREFIX_LEN] = L'\0';
            op->type = OP_SKIPPED;
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

    /* 找到目标条目位置 */
    ptr = (const wchar_t*)oldBuffer;
    end = (const wchar_t*)(oldBuffer + oldSize);

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
    }

    if (currentIdx != index || ptr >= end) {
        return ERROR_INVALID_PARAMETER;
    }

    entryStart = ptr;
    offset = (size_t)((const BYTE*)entryStart - oldBuffer);

    /* 计算条目结束位置 */
    ptr = entryStart;

    /* 跳过源路径 */
    while (ptr < end && *ptr != L'\0') ptr++;
    if (ptr >= end) return ERROR_INVALID_PARAMETER;
    ptr++;

    /* 跳过目标路径 */
    if (ptr < end && *ptr != L'\0') {
        while (ptr < end && *ptr != L'\0') ptr++;
        if (ptr >= end) return ERROR_INVALID_PARAMETER;
        ptr++;
    }

    /* 检查是否需要包含额外的终止符 */
    if (ptr < end && *ptr == L'\0') {
        ptr++;  /* 包含终止符 */
    }

    entryEnd = ptr;
    bytesToRemove = (size_t)((const BYTE*)entryEnd - (const BYTE*)entryStart);

    /* 计算新大小 */
    newSize = oldSize - (DWORD)bytesToRemove;

    /* 边界处理 */
    if (newSize < 2 * sizeof(wchar_t)) {
        /* 数据太小，删除整个注册表值 */
        LONG result = WriteRegistryData(NULL, 0);
        if (result == ERROR_SUCCESS) {
            free(oldBuffer);
            *buffer = NULL;
            *size = 0;
        }
        return result;
    }

    /* 分配新缓冲区 */
    newBuffer = (BYTE*)malloc(newSize);
    if (newBuffer == NULL) {
        return ERROR_OUTOFMEMORY;
    }

    /* 复制前半部分 */
    memcpy(newBuffer, oldBuffer, offset);

    /* 使用 memmove 复制后半部分（严禁使用 memcpy） */
    memmove(newBuffer + offset, (const BYTE*)entryEnd, oldSize - (DWORD)offset - (DWORD)bytesToRemove);

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
        wprintf(L"[错误] 读取注册表失败，错误码：%lu\n", result);
        return -1;
    }

    result = ParseMultiSzData(data, size, &ops, &count);
    free(data);

    if (result != ERROR_SUCCESS) {
        wprintf(L"[错误] 解析数据失败。\n");
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
 * 将文件添加到重启删除列表
 * @param filePath 文件路径
 * @return 成功返回 ERROR_SUCCESS
 */
static LONG AddToDeleteList(const wchar_t* filePath)
{
    LONG result;

    if (filePath == NULL || *filePath == L'\0') {
        wprintf(L"[错误] 文件路径不能为空。\n");
        return ERROR_INVALID_PARAMETER;
    }

    /* 使用官方 API */
    result = MoveFileExW(filePath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (result == 0) {
        result = GetLastError();
        wprintf(L"[错误] 添加失败，错误码：%lu\n", result);
        if (result == ERROR_ACCESS_DENIED) {
            wprintf(L"       请确保以管理员身份运行本程序。\n");
        }
        return result;
    }

    wprintf(L"[成功] 已将文件添加到重启删除列表：%s\n", filePath);
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
        wprintf(L"[错误] 索引必须大于等于 1。\n");
        return ERROR_INVALID_PARAMETER;
    }

    targetIdx = index - 1;

    /* 读取当前数据 */
    result = ReadRegistryData(&data, &size);
    if (result != ERROR_SUCCESS) {
        wprintf(L"[错误] 读取注册表失败，错误码：%lu\n", result);
        return result;
    }

    /* 解析数据 */
    result = ParseMultiSzData(data, size, &ops, &count);
    if (result != ERROR_SUCCESS) {
        free(data);
        wprintf(L"[错误] 解析数据失败。\n");
        return result;
    }

    /* 检查索引范围 */
    if (targetIdx >= count) {
        free(data);
        free(ops);
        wprintf(L"[错误] 索引 %d 超出范围（共 %d 项）。\n", index, count);
        return ERROR_INVALID_PARAMETER;
    }

    /* 检查是否为跳过项 */
    if (ops[targetIdx].type == OP_SKIPPED) {
        free(data);
        free(ops);
        wprintf(L"[警告] 第 %d 项已是跳过状态。\n", index);
        return ERROR_SUCCESS;
    }

    /* 抹除模式：先备份 */
    if (mode == MODE_ERASE) {
        wprintf(L"[安全] 正在备份数据...\n");
        result = BackupData(data, size, &backupPath);
        if (result == ERROR_SUCCESS) {
            wprintf(L"[安全] 备份已保存至：%s\n", backupPath);
        } else {
            wprintf(L"[警告] 备份失败，继续执行...\n");
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
        wprintf(L"[错误] 操作失败，错误码：%lu\n", result);
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
 */
static void HandleAdd(void)
{
    wchar_t path[MAX_PATH_LEN];

    wprintf(L"\n--- 添加重启删除任务 ---\n\n");
    wprintf(L"  请输入文件路径: ");

    if (fgetws(path, MAX_PATH_LEN, stdin) == NULL) {
        wprintf(L"[错误] 读取输入失败。\n");
        return;
    }

    /* 移除换行符 */
    size_t len = wcslen(path);
    while (len > 0 && (path[len - 1] == L'\n' || path[len - 1] == L'\r')) {
        path[--len] = L'\0';
    }

    if (path[0] == L'\0') {
        wprintf(L"[错误] 路径不能为空。\n");
        return;
    }

    AddToDeleteList(path);
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
        wprintf(L"[错误] 读取输入失败。\n");
        return;
    }

    index = _wtoi(input);
    if (index == 0) {
        wprintf(L"  已返回主菜单。\n");
        return;
    }
    if (index < 0) {
        wprintf(L"[错误] 无效的序号。\n");
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
    wprintf(L"  %s              交互式模式\n", progName);
    wprintf(L"  %s read         查看待处理列表\n", progName);
    wprintf(L"  %s add <path>   添加文件到重启删除列表\n", progName);
    wprintf(L"  %s skip <n>     跳过第 n 项操作\n", progName);
    wprintf(L"  %s erase <n>    抹除第 n 项操作\n", progName);
    wprintf(L"  %s help         显示帮助\n", progName);
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
        ShowPendingList();
        return 1;
    }

    if (_wcsicmp(argv[1], L"add") == 0) {
        if (argc < 3) {
            wprintf(L"[错误] 缺少文件路径参数。\n");
            wprintf(L"用法：%s add <文件路径>\n", argv[0]);
            return -1;
        }
        AddToDeleteList(argv[2]);
        return 1;
    }

    if (_wcsicmp(argv[1], L"skip") == 0) {
        int index;
        if (argc < 3) {
            wprintf(L"[错误] 缺少序号参数。\n");
            wprintf(L"用法：%s skip <序号>\n", argv[0]);
            return -1;
        }
        index = _wtoi(argv[2]);
        if (index <= 0) {
            wprintf(L"[错误] 序号必须为正整数。\n");
            return -1;
        }
        RemoveOperation(index, MODE_SKIP);
        return 1;
    }

    if (_wcsicmp(argv[1], L"erase") == 0) {
        int index;
        if (argc < 3) {
            wprintf(L"[错误] 缺少序号参数。\n");
            wprintf(L"用法：%s erase <序号>\n", argv[0]);
            return -1;
        }
        index = _wtoi(argv[2]);
        if (index <= 0) {
            wprintf(L"[错误] 序号必须为正整数。\n");
            return -1;
        }
        RemoveOperation(index, MODE_ERASE);
        return 1;
    }

    wprintf(L"[错误] 未知命令：%s\n", argv[1]);
    ShowHelp(argv[0]);
    return -1;
}

/* ============================================================
 * 程序入口
 * ============================================================ */

int wmain(int argc, wchar_t* argv[])
{
    wchar_t input[16];
    int choice;
    int result = 0;

    /* 设置控制台为 UTF-16 文本模式，确保 wprintf 正确输出中文 */
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    /* 命令行模式 */
    if (argc > 1) {
        result = ParseCommand(argc, argv);
        return result < 0 ? 1 : 0;
    }

    /* 交互式模式 */
    while (1) {
        ShowMenu();

        if (fgetws(input, 16, stdin) == NULL) {
            break;
        }

        choice = _wtoi(input);

        switch (choice) {
        case 1:
            HandleView();
            break;
        case 2:
            HandleAdd();
            break;
        case 3:
            HandleRemove(MODE_SKIP);
            break;
        case 4:
            HandleRemove(MODE_ERASE);
            break;
        case 5:
            wprintf(L"\n再见！\n");
            return 0;
        default:
            if (input[0] != L'\n' && input[0] != L'\0') {
                wprintf(L"\n[错误] 无效选择，请输入 1-5。\n");
            }
            break;
        }
    }

    return 0;
}