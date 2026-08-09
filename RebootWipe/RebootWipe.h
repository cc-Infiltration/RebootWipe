/**
 * RebootWipe - Windows 重启文件操作管理器
 * 公共头文件：类型定义、常量、宏
 *
 * 功能：管理 PendingFileRenameOperations 注册表项
 *   1. 查看 (Read)：解析并展示待处理文件操作列表
 *   2. 加入 (Add)：将文件标记为重启后删除
 *   3. 取消 (Remove)：跳过模式 / 直接抹除模式
 */

#ifndef REBOOTWIPE_H
#define REBOOTWIPE_H

#include <windows.h>

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
#define CONSOLE_RED        FOREGROUND_RED | FOREGROUND_INTENSITY
#define CONSOLE_GREEN      FOREGROUND_GREEN | FOREGROUND_INTENSITY
#define CONSOLE_YELLOW     FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY
#define CONSOLE_CYAN       FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
#define CONSOLE_WHITE      FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
#define CONSOLE_GRAY       FOREGROUND_INTENSITY
#define CONSOLE_DARK       0
#define CONSOLE_NORMAL     FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
#define CONSOLE_BG_BLUE    BACKGROUND_BLUE | BACKGROUND_INTENSITY
#define CONSOLE_BG_RED     BACKGROUND_RED | BACKGROUND_INTENSITY
#define CONSOLE_BG_GREEN   BACKGROUND_GREEN | BACKGROUND_INTENSITY
#define CONSOLE_BG_YELLOW  BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_INTENSITY

/* 跳过前缀 */
#define SKIP_PREFIX        L"??"
#define SKIP_PREFIX_LEN    2

/* 自定义错误码 */
#ifndef ERROR_CANCELLED
#define ERROR_CANCELLED    1223L
#endif

/* ============================================================
 * 核心数据结构
 * ============================================================ */

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

/* 待处理操作结构体 */
typedef struct {
    OperationType type;                    /* 操作类型 */
    wchar_t sourcePath[MAX_PATH_LEN];      /* 源文件路径 */
    wchar_t targetPath[MAX_PATH_LEN];      /* 目标路径（仅移动操作有效） */
} PendingOperation;

#endif /* REBOOTWIPE_H */
