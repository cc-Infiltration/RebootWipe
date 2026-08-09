/**
 * gui.h - GUI 模块
 * CLI 界面、UAC 自动提权、暂停清屏
 */

#ifndef GUI_H
#define GUI_H

#include <windows.h>
#include "RebootWipe.h"

/* 待处理列表展示 */
int ShowPendingList(void);

/* 取消操作 */
LONG RemoveOperation(int index, RemoveMode mode);

/* 菜单与处理函数 */
void ShowMenu(void);
void HandleView(void);
void HandleAdd(void);
void HandleRemove(RemoveMode mode);

/* 命令行帮助与解析 */
void ShowHelp(const wchar_t* progName);
int ParseCommand(int argc, wchar_t* argv[]);

/* 暂停并清屏 */
void PauseAndClear(void);

/* UAC 提权 */
BOOL IsAdministrator(void);
BOOL RunAsAdmin(int argc, wchar_t* argv[]);

#endif /* GUI_H */
