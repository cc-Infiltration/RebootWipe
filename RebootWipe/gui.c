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
 * 暂停并清屏
 * ============================================================ */

void PauseAndClear(void)
{
    wint_t ch;
    wprintf(L"\n按回车键继续...");
    while ((ch = fgetwc(stdin)) != L'\n' && ch != WEOF) { }
    system("cls");
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

    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        return FALSE;
    }

    for (i = 1; i < argc; i++) {
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
        WPRINTF_RED0(L"[错误] 索引必须大于等于 1。\n");
        return ERROR_INVALID_PARAMETER;
    }

    targetIdx = index - 1;

    result = ReadRegistryData(&data, &size);
    if (result == ERROR_FILE_NOT_FOUND) {
        wprintf(L"[信息] 当前没有待处理的操作。\n");
        return ERROR_SUCCESS;
    }
    if (result != ERROR_SUCCESS) {
        WPRINTF_RED(L"[错误] 读取注册表失败，错误码：%lu\n", result);
        return result;
    }

    result = ParseMultiSzData(data, size, &ops, &count);
    if (result != ERROR_SUCCESS) {
        free(data);
        WPRINTF_RED0(L"[错误] 解析数据失败。\n");
        return result;
    }

    if (targetIdx >= count) {
        free(data);
        free(ops);
        WPRINTF_RED(L"[错误] 索引 %d 超出范围（共 %d 项）。\n", index, count);
        return ERROR_INVALID_PARAMETER;
    }

    if (ops[targetIdx].type == OP_SKIPPED) {
        if (mode == MODE_SKIP) {
            free(data);
            free(ops);
            wprintf(L"[警告] 第 %d 项已是跳过状态。\n", index);
            return ERROR_SUCCESS;
        }
    }

    if (mode == MODE_ERASE) {
        wprintf(L"[安全] 正在备份数据...\n");
        result = BackupData(data, size, &backupPath);
        if (result == ERROR_SUCCESS) {
            wprintf(L"[安全] 备份已保存至：%s\n", backupPath);
        } else {
            WPRINTF_RED0(L"[警告] 备份失败，继续执行...\n");
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
        wprintf(L"[成功] 操作已完成。\n");
    } else {
        WPRINTF_RED(L"[错误] 操作失败，错误码：%lu\n", result);
    }

    return result;
}

/* ============================================================
 * CLI 界面
 * ============================================================ */

void ShowMenu(void)
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

void HandleView(void)
{
    wprintf(L"\n--- 查看待处理文件操作 ---\n");
    ShowPendingList();
}

void HandleAdd(void)
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

    len = wcslen(input);
    if (len > 0 && input[len - 1] != L'\n') {
        wint_t ch;
        while ((ch = fgetwc(stdin)) != L'\n' && ch != WEOF) { }
        WPRINTF_RED0(L"[警告] 输入过长，已截断。\n");
    }

    while (len > 0 && (input[len - 1] == L'\n' || input[len - 1] == L'\r')) {
        input[--len] = L'\0';
    }

    if (len == 0) {
        WPRINTF_RED0(L"[错误] 输入不能为空。\n");
        return;
    }

    if (input[0] == L'@') {
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

    {
        wchar_t** expandedPaths = NULL;
        int expandedCount = 0;
        LONG result;

        result = ExpandDirectoryPaths((const wchar_t* const*)inputPaths, inputCount,
                                      &expandedPaths, &expandedCount, TRUE);

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

        AddMultipleToDeleteList((const wchar_t* const*)expandedPaths, expandedCount);

        for (i = 0; i < expandedCount; i++) free(expandedPaths[i]);
        free(expandedPaths);
    }
}

void HandleRemove(RemoveMode mode)
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

void ShowHelp(const wchar_t* progName)
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
            WPRINTF_RED0(L"[错误] 缺少文件路径参数。\n");
            wprintf(L"用法：%s add <文件路径> [文件路径 ...]\n", argv[0]);
            wprintf(L"      %s add @<列表文件>\n", argv[0]);
            return -1;
        }

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
