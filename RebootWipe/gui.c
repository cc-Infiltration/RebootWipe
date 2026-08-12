/**
 * gui.c - GUI 模块实现
 * CLI 界面、UAC 自动提权、暂停清屏
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <shellapi.h>

#include "RebootWipe.h"
#include "core.h"
#include "gui.h"

/* ============================================================
 * 工具函数
 * ============================================================ */

static void TruncatePath(const wchar_t* src, wchar_t* dst, int maxLen)
{
    int len = (int)wcslen(src);
    if (len <= maxLen) {
        wcscpy_s(dst, maxLen + 1, src);
        return;
    }
    wcsncpy_s(dst, maxLen + 1, src, maxLen);
    wcscpy_s(dst + maxLen - 3, 4, L"...");
}

/* ============================================================
 * 暂停并清屏
 * ============================================================ */

void PauseAndClear(void)
{
    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"\n  按回车键继续...");
    ResetConsoleColor();
    FlushStdin();
    ClearConsole();
}

/* ============================================================
 * UAC 自动提权
 * ============================================================ */

BOOL IsAdministrator(void)
{
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = NULL;
    HANDLE tokenHandle = NULL;
    BOOL checkResult;

    if (!AllocateAndInitializeSid(&ntAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup)) {
        return FALSE;
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenHandle)) {
        FreeSid(adminGroup);
        return FALSE;
    }

    checkResult = CheckTokenMembership(NULL, adminGroup, &isAdmin);

    CloseHandle(tokenHandle);
    FreeSid(adminGroup);

    return isAdmin && checkResult;
}

BOOL RunAsAdmin(int argc, wchar_t* argv[])
{
    wchar_t exePath[MAX_PATH];
    wchar_t params[32768] = L"";
    SHELLEXECUTEINFOW sei;
    int i;
    DWORD result;
    size_t totalLen = 0;
    size_t maxLen = 32768;  /* _countof(params) */

    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        return FALSE;
    }

    for (i = 1; i < argc; i++) {
        BOOL hasSpace;
        size_t argLen;
        size_t needed;

        argLen = wcslen(argv[i]);
        hasSpace = (wcschr(argv[i], L' ') != NULL);

        /* 所需字符数 = 已有长度 + 分隔空格 + 参数（可能加引号） */
        needed = totalLen + (totalLen > 0 ? 1 : 0) + argLen + (hasSpace ? 2 : 0);
        if (needed >= maxLen) {
            /* 超出缓冲区，忽略剩余参数 */
            break;
        }

        if (totalLen > 0) {
            params[totalLen++] = L' ';
        }

        if (hasSpace) {
            params[totalLen++] = L'\"';
        }

        wcscpy_s(params + totalLen, maxLen - totalLen, argv[i]);
        totalLen += argLen;

        if (hasSpace) {
            params[totalLen++] = L'\"';
        }
    }

    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    result = ShellExecuteExW(&sei);

    if (!result) {
        return FALSE;
    }

    if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }

    return TRUE;
}

/* ============================================================
 * 功能函数
 * ============================================================ */

int ShowPendingList(void)
{
    BYTE* data = NULL;
    DWORD size = 0;
    PendingOperation* ops = NULL;
    int count = 0;
    LONG result;
    int i;
    int delCount = 0, moveCount = 0, skipCount = 0;

    result = ReadRegistryData(&data, &size);
    if (result == ERROR_FILE_NOT_FOUND) {
        SetConsoleColor(CONSOLE_CYAN);
        wprintf(L"\n  [信息] 当前没有待处理的文件操作。\n");
        ResetConsoleColor();
        return 0;
    }
    if (result != ERROR_SUCCESS) {
        WPRINTF_RED(L"\n  [错误] 读取注册表失败，错误码：%lu\n", result);
        return -1;
    }

    result = ParseMultiSzData(data, size, &ops, &count);
    free(data);

    if (result != ERROR_SUCCESS) {
        WPRINTF_RED0(L"\n  [错误] 解析数据失败。\n");
        return -1;
    }

    if (count == 0) {
        SetConsoleColor(CONSOLE_CYAN);
        wprintf(L"\n  [信息] 当前没有待处理的文件操作。\n");
        ResetConsoleColor();
        free(ops);
        return 0;
    }

    for (i = 0; i < count; i++) {
        switch (ops[i].type) {
        case OP_DELETE:  delCount++; break;
        case OP_MOVE:   moveCount++; break;
        case OP_SKIPPED: skipCount++; break;
        }
    }

    wprintf(L"\n");
    SetConsoleColor(CONSOLE_WHITE);
    wprintf(L"  ╔══════════════════════════════════════════════════════════════╗\n");
    wprintf(L"  ║  PendingFileRenameOperations 列表                           ║\n");
    wprintf(L"  ╚══════════════════════════════════════════════════════════════╝\n");
    ResetConsoleColor();

    wprintf(L"  共 %d 项  |  ", count);
    SetConsoleColor(CONSOLE_RED);   wprintf(L"删除 %d", delCount);
    ResetConsoleColor();            wprintf(L"  ");
    SetConsoleColor(CONSOLE_YELLOW); wprintf(L"移动 %d", moveCount);
    ResetConsoleColor();            wprintf(L"  ");
    SetConsoleColor(CONSOLE_GRAY);  wprintf(L"已跳过 %d", skipCount);
    ResetConsoleColor();            wprintf(L"\n\n");

    for (i = 0; i < count; i++) {
        wchar_t shortPath[MAX_PATH_LEN];

        switch (ops[i].type) {
        case OP_DELETE:
            SetConsoleColor(CONSOLE_RED);
            wprintf(L"  ┌─ [%d] 删除\n", i + 1);
            TruncatePath(ops[i].sourcePath, shortPath, 70);
            wprintf(L"  │  源：%s\n", shortPath);
            ResetConsoleColor();
            break;

        case OP_MOVE:
            SetConsoleColor(CONSOLE_YELLOW);
            wprintf(L"  ┌─ [%d] 移动\n", i + 1);
            TruncatePath(ops[i].sourcePath, shortPath, 40);
            wprintf(L"  │  源：%s\n", shortPath);
            TruncatePath(ops[i].targetPath, shortPath, 40);
            wprintf(L"  │  目标：%s\n", shortPath);
            ResetConsoleColor();
            break;

        case OP_SKIPPED:
            SetConsoleColor(CONSOLE_GRAY);
            wprintf(L"  ┌─ [%d] 已跳过\n", i + 1);
            TruncatePath(ops[i].sourcePath, shortPath, 70);
            wprintf(L"  │  源：%s\n", shortPath);
            ResetConsoleColor();
            break;
        }
        wprintf(L"  └─────────────────────────────────────────────────────\n\n");
    }

    free(ops);
    return count;
}

LONG RemoveOperation(int index, RemoveMode mode)
{
    BYTE* data = NULL;
    DWORD size = 0;
    PendingOperation* ops = NULL;
    int count = 0;
    LONG result;
    int targetIdx;
    wchar_t* backupPath = NULL;

    if (index < 1) {
        WPRINTF_RED0(L"  [错误] 索引必须大于等于 1。\n");
        return ERROR_INVALID_PARAMETER;
    }

    targetIdx = index - 1;

    result = ReadRegistryData(&data, &size);
    if (result == ERROR_FILE_NOT_FOUND) {
        SetConsoleColor(CONSOLE_CYAN);
        wprintf(L"  [信息] 当前没有待处理的操作。\n");
        ResetConsoleColor();
        return ERROR_SUCCESS;
    }
    if (result != ERROR_SUCCESS) {
        WPRINTF_RED(L"  [错误] 读取注册表失败，错误码：%lu\n", result);
        return result;
    }

    result = ParseMultiSzData(data, size, &ops, &count);
    if (result != ERROR_SUCCESS) {
        free(data);
        WPRINTF_RED0(L"  [错误] 解析数据失败。\n");
        return result;
    }

    if (targetIdx >= count) {
        free(data);
        free(ops);
        WPRINTF_RED(L"  [错误] 索引 %d 超出范围（共 %d 项）。\n", index, count);
        return ERROR_INVALID_PARAMETER;
    }

    if (ops[targetIdx].type == OP_SKIPPED) {
        if (mode == MODE_SKIP) {
            free(data);
            free(ops);
            SetConsoleColor(CONSOLE_YELLOW);
            wprintf(L"  [警告] 第 %d 项已是跳过状态。\n", index);
            ResetConsoleColor();
            return ERROR_SUCCESS;
        }
    }

    if (mode == MODE_ERASE) {
        wchar_t input[8];
        wprintf(L"\n  [警告] 警告：抹除模式将物理删除注册表条目，");
        SetConsoleColor(CONSOLE_RED);
        wprintf(L"无法恢复！\n");
        ResetConsoleColor();
        wprintf(L"  确认抹除第 %d 项操作？(y/N): ", index);

        if (fgetws(input, 8, stdin) == NULL) {
            free(data);
            free(ops);
            return ERROR_CANCELLED;
        }

        {
            size_t len = wcslen(input);
            if (len > 0 && input[len - 1] != L'\n') {
                FlushStdin();
            }
            while (len > 0 && (input[len - 1] == L'\n' || input[len - 1] == L'\r')) {
                input[--len] = L'\0';
            }
        }

        if (_wcsicmp(input, L"y") != 0) {
            free(data);
            free(ops);
            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  [信息] 已取消操作。\n");
            ResetConsoleColor();
            return ERROR_CANCELLED;
        }

        SetConsoleColor(CONSOLE_CYAN);
        wprintf(L"  正在备份数据...\n");
        ResetConsoleColor();
        result = BackupData(data, size, &backupPath);
        if (result == ERROR_SUCCESS) {
            SetConsoleColor(CONSOLE_GREEN);
            wprintf(L"  [成功] 备份已保存至：%s\n", backupPath);
            ResetConsoleColor();
        } else {
            SetConsoleColor(CONSOLE_YELLOW);
            wprintf(L"  [警告] 备份失败，继续执行...\n");
            ResetConsoleColor();
        }
    }

    if (mode == MODE_SKIP) {
        result = InjectSkipPrefix(&data, &size, targetIdx);
    } else {
        result = EraseEntry(&data, &size, targetIdx);
    }

    if (data) free(data);
    free(ops);
    if (backupPath) free(backupPath);

    if (result == ERROR_SUCCESS) {
        if (mode == MODE_SKIP) {
            SetConsoleColor(CONSOLE_GREEN);
            wprintf(L"  [成功] 第 %d 项已标记为跳过（?? 前缀）\n", index);
        } else {
            SetConsoleColor(CONSOLE_GREEN);
            wprintf(L"  [成功] 第 %d 项已从注册表中抹除\n", index);
        }
        ResetConsoleColor();
    } else {
        WPRINTF_RED(L"  [错误] 操作失败，错误码：%lu\n", result);
    }

    return result;
}

/* ============================================================
 * CLI 界面
 * ============================================================ */

void ShowMenu(void)
{
    int pendingCount = 0;
    BYTE* data = NULL;
    DWORD dataSize = 0;

    ReadRegistryData(&data, &dataSize);
    if (data != NULL) {
        PendingOperation* ops = NULL;
        int count = 0;
        if (ParseMultiSzData(data, dataSize, &ops, &count) == ERROR_SUCCESS) {
            pendingCount = count;
            free(ops);
        }
        free(data);
    }

    wprintf(L"\n");
    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  ╔══════════════════════════════════════════════════════════════╗\n");
    wprintf(L"  ║                                                              ║\n");
    wprintf(L"  ║           RebootWipe - 重启文件操作管理器                   ║\n");
    wprintf(L"  ║                                                              ║\n");
    ResetConsoleColor();

    if (pendingCount > 0) {
        wprintf(L"  ║  ");
        SetConsoleColor(CONSOLE_YELLOW);
        wprintf(L"[警告] 有 %d 项待处理操作", pendingCount);
        ResetConsoleColor();
        wprintf(L"                              ║\n");
    } else {
        wprintf(L"  ║  ");
        SetConsoleColor(CONSOLE_GREEN);
        wprintf(L"[成功] 无待处理操作");
        ResetConsoleColor();
        wprintf(L"                                  ║\n");
    }

    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  ║                                                              ║\n");
    wprintf(L"  ╚══════════════════════════════════════════════════════════════╝\n");
    ResetConsoleColor();

    wprintf(L"\n");
    wprintf(L"  ");
    SetConsoleColor(CONSOLE_WHITE); wprintf(L"[1]"); ResetConsoleColor();
    wprintf(L" 查看待处理的文件操作列表\n");

    wprintf(L"  ");
    SetConsoleColor(CONSOLE_WHITE); wprintf(L"[2]"); ResetConsoleColor();
    wprintf(L" 将文件标记为重启后删除\n");

    wprintf(L"  ");
    SetConsoleColor(CONSOLE_WHITE); wprintf(L"[3]"); ResetConsoleColor();
    wprintf(L" 取消操作 - ");
    SetConsoleColor(CONSOLE_YELLOW); wprintf(L"跳过模式"); ResetConsoleColor();
    wprintf(L"（注入 ?? 前缀）\n");

    wprintf(L"  ");
    SetConsoleColor(CONSOLE_WHITE); wprintf(L"[4]"); ResetConsoleColor();
    wprintf(L" 取消操作 - ");
    SetConsoleColor(CONSOLE_RED); wprintf(L"直接抹除"); ResetConsoleColor();
    wprintf(L"模式\n");

    wprintf(L"  ");
    SetConsoleColor(CONSOLE_WHITE); wprintf(L"[5]"); ResetConsoleColor();
    wprintf(L" 退出程序\n");

    wprintf(L"\n");
    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  请选择操作 (1-5): ");
    ResetConsoleColor();
}

void HandleView(void)
{
    ShowPendingList();
}

void HandleAdd(void)
{
    wchar_t input[MAX_INPUT_LEN];
    size_t len;
    wchar_t** inputPaths = NULL;
    int inputCount = 0;
    int i;

    wprintf(L"\n  ╔══════════════════════════════════════════════════════════════╗\n");
    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  ║  添加重启删除任务                                          ║\n");
    ResetConsoleColor();
    wprintf(L"  ╚══════════════════════════════════════════════════════════════╝\n\n");

    SetConsoleColor(CONSOLE_GRAY);
    wprintf(L"  输入方式：\n");
    wprintf(L"    · 单路径 → 直接输入\n");
    wprintf(L"    · 多路径 → 用分号 ; 分隔\n");
    wprintf(L"    · 文件导入 → @文件路径（每行一个，# 为注释）\n");
    wprintf(L"    · 目录 → 自动递归展开并提示确认\n");
    ResetConsoleColor();

    wprintf(L"\n  ");
    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"请输入：");
    ResetConsoleColor();

    if (fgetws(input, MAX_INPUT_LEN, stdin) == NULL) {
        WPRINTF_RED0(L"  [错误] 读取输入失败。\n");
        return;
    }

    len = wcslen(input);
    if (len > 0 && input[len - 1] != L'\n') {
        FlushStdin();
        WPRINTF_YELLOW0(L"  [警告] 输入过长，已截断。\n");
    }

    while (len > 0 && (input[len - 1] == L'\n' || input[len - 1] == L'\r')) {
        input[--len] = L'\0';
    }

    if (len == 0) {
        WPRINTF_RED0(L"  [错误] 输入不能为空。\n");
        return;
    }

    if (input[0] == L'@') {
        wchar_t* filePath = input + 1;
        LONG result;

        TrimPath(filePath);
        if (filePath[0] == L'\0') {
            WPRINTF_RED0(L"  [错误] 文件路径不能为空。\n");
            return;
        }

        wprintf(L"\n  从文件导入：%s\n\n", filePath);
        result = ReadPathsFromFile(filePath, &inputPaths, &inputCount);
        if (result != ERROR_SUCCESS) {
            WPRINTF_RED(L"  [错误] 无法读取文件列表：%s\n", filePath);
            return;
        }
        if (inputCount == 0) {
            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  [信息] 文件列表为空。\n");
            ResetConsoleColor();
            free(inputPaths);
            return;
        }
        SetConsoleColor(CONSOLE_CYAN);
        wprintf(L"  [信息] 读取到 %d 条路径\n", inputCount);
        ResetConsoleColor();
    } else {
        wchar_t* token;
        wchar_t* context;

        inputPaths = (wchar_t**)malloc(MAX_BATCH_PATHS * sizeof(wchar_t*));
        if (inputPaths == NULL) {
            WPRINTF_RED0(L"  [错误] 内存不足。\n");
            return;
        }

        token = wcstok_s(input, L";", &context);
        while (token != NULL && inputCount < MAX_BATCH_PATHS) {
            if (*token != L'\0') {
                inputPaths[inputCount] = _wcsdup(token);
                if (inputPaths[inputCount] == NULL) {
                    for (i = 0; i < inputCount; i++) free(inputPaths[i]);
                    free(inputPaths);
                    WPRINTF_RED0(L"  [错误] 内存不足。\n");
                    return;
                }
                inputCount++;
            }
            token = wcstok_s(NULL, L";", &context);
        }

        if (inputCount == 0) {
            WPRINTF_RED0(L"  [错误] 未解析到有效路径。\n");
            free(inputPaths);
            return;
        }

        SetConsoleColor(CONSOLE_CYAN);
        wprintf(L"\n  [信息] 解析到 %d 条路径\n", inputCount);
        ResetConsoleColor();
    }

    {
        wchar_t** expandedPaths = NULL;
        int expandedCount = 0;
        LONG result;

        result = ExpandDirectoryPaths((const wchar_t* const*)inputPaths, inputCount,
                                      &expandedPaths, &expandedCount, TRUE);

        for (i = 0; i < inputCount; i++) free(inputPaths[i]);
        free(inputPaths);

        if (result != ERROR_SUCCESS) {
            WPRINTF_RED0(L"  [错误] 展开目录失败。\n");
            return;
        }

        if (expandedCount == 0) {
            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  [信息] 没有有效的路径需要添加。\n");
            ResetConsoleColor();
            free(expandedPaths);
            return;
        }

        SetConsoleColor(CONSOLE_CYAN);
        wprintf(L"  [信息] 展开后共 %d 条路径，开始添加...\n\n", expandedCount);
        ResetConsoleColor();

        AddMultipleToDeleteList((const wchar_t* const*)expandedPaths, expandedCount);

        for (i = 0; i < expandedCount; i++) free(expandedPaths[i]);
        free(expandedPaths);
    }
}

void HandleRemove(RemoveMode mode)
{
    wchar_t input[32];
    int index;

    wprintf(L"\n  ╔══════════════════════════════════════════════════════════════╗\n");
    if (mode == MODE_SKIP) {
        SetConsoleColor(CONSOLE_YELLOW);
        wprintf(L"  ║  取消操作 - 跳过模式                                       ║\n");
    } else {
        SetConsoleColor(CONSOLE_RED);
        wprintf(L"  ║  取消操作 - 抹除模式                                       ║\n");
    }
    ResetConsoleColor();
    wprintf(L"  ╚══════════════════════════════════════════════════════════════╝\n\n");

    ShowPendingList();

    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  请输入要取消的操作序号 (");
    SetConsoleColor(CONSOLE_GRAY);
    wprintf(L"0 返回");
    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"): ");
    ResetConsoleColor();

    if (fgetws(input, 32, stdin) == NULL) {
        WPRINTF_RED0(L"  [错误] 读取输入失败。\n");
        return;
    }

    /* 如果输入被截断（没有换行符），清空剩余缓冲区 */
    {
        size_t ilen = wcslen(input);
        if (ilen > 0 && input[ilen - 1] != L'\n') {
            FlushStdin();
        }
    }

    if (!SafeParseInt(input, &index)) {
        WPRINTF_RED0(L"  [错误] 无效的序号。\n");
        return;
    }
    if (index == 0) {
        SetConsoleColor(CONSOLE_CYAN);
        wprintf(L"  [信息] 已返回主菜单。\n");
        ResetConsoleColor();
        return;
    }
    if (index < 0) {
        WPRINTF_RED0(L"  [错误] 无效的序号。\n");
        return;
    }

    RemoveOperation(index, mode);
}

void ShowHelp(const wchar_t* progName)
{
    SetConsoleColor(CONSOLE_WHITE);
    wprintf(L"  RebootWipe - Windows 重启文件操作管理器\n");
    ResetConsoleColor();
    wprintf(L"\n");

    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  ╔══════════════════════════════════════════════════════════════╗\n");
    wprintf(L"  ║  用法                                                       ║\n");
    wprintf(L"  ╚══════════════════════════════════════════════════════════════╝\n\n");
    ResetConsoleColor();

    wprintf(L"    %-32s %s\n", progName, L"交互式模式");
    wprintf(L"    %-32s %s\n", L"", L"启动后显示菜单进行操作");
    wprintf(L"\n");

    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  命令行命令：\n\n");
    ResetConsoleColor();

    SetConsoleColor(CONSOLE_WHITE);
    wprintf(L"    %s read\n", progName);
    ResetConsoleColor();
    wprintf(L"      查看当前待处理的文件操作列表\n\n");

    SetConsoleColor(CONSOLE_WHITE);
    wprintf(L"    %s add <路径> [路径...]\n", progName);
    ResetConsoleColor();
    wprintf(L"      添加一个或多个文件/目录到重启删除列表\n\n");

    SetConsoleColor(CONSOLE_WHITE);
    wprintf(L"    %s add @<列表文件>\n", progName);
    ResetConsoleColor();
    wprintf(L"      从文本文件批量导入（每行一个路径，# 为注释）\n\n");

    SetConsoleColor(CONSOLE_WHITE);
    wprintf(L"    %s skip <n>\n", progName);
    ResetConsoleColor();
    wprintf(L"      跳过第 n 项操作（注入 ?? 前缀）\n\n");

    SetConsoleColor(CONSOLE_WHITE);
    wprintf(L"    %s erase <n>\n", progName);
    ResetConsoleColor();
    wprintf(L"      抹除第 n 项操作（物理删除注册表条目）\n\n");

    SetConsoleColor(CONSOLE_WHITE);
    wprintf(L"    %s help\n", progName);
    ResetConsoleColor();
    wprintf(L"      显示本帮助信息\n\n");

    SetConsoleColor(CONSOLE_CYAN);
    wprintf(L"  ╔══════════════════════════════════════════════════════════════╗\n");
    wprintf(L"  ║  交互式 add 说明                                             ║\n");
    wprintf(L"  ╚══════════════════════════════════════════════════════════════╝\n\n");
    ResetConsoleColor();

    SetConsoleColor(CONSOLE_GRAY);
    wprintf(L"    · 单个路径直接输入\n");
    wprintf(L"    · 多个路径用分号 ; 分隔\n");
    wprintf(L"    · 从文件导入：@文件路径\n");
    wprintf(L"    · 支持目录路径：非空目录将递归展开并提示确认\n");
    ResetConsoleColor();

    wprintf(L"\n");
    SetConsoleColor(CONSOLE_YELLOW);
    wprintf(L"  [警告] 注意：\n");
    ResetConsoleColor();
    wprintf(L"    · 重启时按输入顺序执行删除\n");
    wprintf(L"    · 非空目录会自动递归展开（先内容后目录）\n");
    wprintf(L"    · 路径超过 260 字符时需加 \\?\\ 前缀（最多 32767 字符）\n");
}

int ParseCommand(int argc, wchar_t* argv[])
{
    if (argc < 2) {
        return 0;
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
            WPRINTF_RED0(L"  [错误] 缺少文件路径参数。\n");
            wprintf(L"\n");
            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  用法：");
            ResetConsoleColor();
            wprintf(L"%s add <文件路径> [文件路径 ...]\n", argv[0]);
            wprintf(L"      %s add @<列表文件>\n", argv[0]);
            return -1;
        }

        if (argv[2][0] == L'@' && argc == 3) {
            LONG result = ReadPathsFromFile(argv[2] + 1, &inputPaths, &inputCount);
            if (result != ERROR_SUCCESS) {
                WPRINTF_RED(L"  [错误] 无法读取文件列表：%s\n", argv[2] + 1);
                return -1;
            }
            if (inputCount == 0) {
                SetConsoleColor(CONSOLE_CYAN);
                wprintf(L"  [信息] 文件列表为空。\n");
                ResetConsoleColor();
                free(inputPaths);
                return 0;
            }
            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  [信息] 从文件读取到 %d 条路径\n\n", inputCount);
            ResetConsoleColor();
        } else {
            int total = argc - 2;
            inputPaths = (wchar_t**)malloc(total * sizeof(wchar_t*));
            if (inputPaths == NULL) {
                WPRINTF_RED0(L"  [错误] 内存不足。\n");
                return -1;
            }
            for (i = 0; i < total; i++) {
                inputPaths[i] = _wcsdup(argv[2 + i]);
                if (inputPaths[i] == NULL) {
                    int j;
                    for (j = 0; j < i; j++) free(inputPaths[j]);
                    free(inputPaths);
                    WPRINTF_RED0(L"  [错误] 内存不足。\n");
                    return -1;
                }
                inputCount++;
            }
            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  [信息] 共 %d 条路径\n\n", inputCount);
            ResetConsoleColor();
        }

        {
            wchar_t** expandedPaths = NULL;
            int expandedCount = 0;
            int success;
            LONG result;

            result = ExpandDirectoryPaths((const wchar_t* const*)inputPaths,
                                          inputCount, &expandedPaths,
                                          &expandedCount, FALSE);

            for (i = 0; i < inputCount; i++) free(inputPaths[i]);
            free(inputPaths);

            if (result != ERROR_SUCCESS) {
                WPRINTF_RED0(L"  [错误] 展开目录失败。\n");
                return -1;
            }

            if (expandedCount == 0) {
                SetConsoleColor(CONSOLE_CYAN);
                wprintf(L"  [信息] 没有有效的路径需要添加。\n");
                ResetConsoleColor();
                free(expandedPaths);
                return 0;
            }

            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  [信息] 展开后共 %d 条路径，开始添加...\n\n", expandedCount);
            ResetConsoleColor();

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
            WPRINTF_RED0(L"  [错误] 缺少序号参数。\n");
            wprintf(L"\n");
            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  用法：");
            ResetConsoleColor();
            wprintf(L"%s skip <序号>\n", argv[0]);
            return -1;
        }
        if (!SafeParseInt(argv[2], &index) || index <= 0) {
            WPRINTF_RED0(L"  [错误] 序号必须为正整数。\n");
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
            WPRINTF_RED0(L"  [错误] 缺少序号参数。\n");
            wprintf(L"\n");
            SetConsoleColor(CONSOLE_CYAN);
            wprintf(L"  用法：");
            ResetConsoleColor();
            wprintf(L"%s erase <序号>\n", argv[0]);
            return -1;
        }
        if (!SafeParseInt(argv[2], &index) || index <= 0) {
            WPRINTF_RED0(L"  [错误] 序号必须为正整数。\n");
            return -1;
        }
        if (RemoveOperation(index, MODE_ERASE) != ERROR_SUCCESS) {
            return -1;
        }
        return 0;
    }

    WPRINTF_RED(L"  [错误] 未知命令：%s\n\n", argv[1]);
    ShowHelp(argv[0]);
    return -1;
}
