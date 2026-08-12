/**
 * RebootWipe - Windows 重启文件操作管理器
 *
 * 功能：管理 PendingFileRenameOperations 注册表项
 *   1. 查看 (Read)：解析并展示待处理文件操作列表
 *   2. 加入 (Add)：将文件标记为重启后删除
 *   3. 取消 (Remove)：跳过模式 / 直接抹除模式
 *
 * 架构：
 *   - 核心模块 (core)：注册表 I/O、核心算法、文件操作、控制台基础设施
 *   - GUI 模块 (gui)：CLI 界面、UAC 自动提权、暂停清屏
 *
 * 程序入口：wmain
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>

#include "RebootWipe.h"
#include "core.h"
#include "gui.h"

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

    /* 交互式模式 */
    while (1) {
        ShowMenu();

        if (fgetws(input, sizeof(input) / sizeof(input[0]), stdin) == NULL) {
            break;
        }

        /* 清除输入缓冲区中剩余的字符（处理超长输入） */
        {
            size_t len = wcslen(input);
            if (len > 0 && input[len - 1] != L'\n') {
                FlushStdin();
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
            wprintf(L"\n[错误] 输入过长，请重新选择 1-5。\n");
            PauseAndClear();
            continue;
        }

        /* 验证输入是否为有效数字 */
        {
            int validChoice = 0;
            if (!SafeParseInt(input, &validChoice) || validChoice < 1 || validChoice > 5) {
                wprintf(L"\n[错误] 无效选择，请输入 1-5。\n");
                PauseAndClear();
                continue;
            }
            choice = validChoice;
        }

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
            PauseAndClear();
            break;
        }
    }

    return 0;
}
