# RebootWipe

**Language:** C · **Platform:** Windows · **License:** MIT

一个基于 C 语言的 Windows 重启文件操作管理器，用于管理 `PendingFileRenameOperations` 注册表项。

## 功能

- **查看（Read）**：解析并展示待处理的文件操作列表，支持删除操作、移动操作和跳过项的识别
- **加入（Add）**：使用 `MoveFileExW` API 将文件标记为重启后删除
- **取消（Remove）**：支持两种模式
  - **跳过模式**：在目标条目前注入 `??` 前缀，使其在重启时被系统忽略
  - **抹除模式**：物理删除注册表条目，同时自动备份原始数据

## 架构

项目采用三层架构设计：

| 层级 | 职责 | 关键模块 |
|------|------|----------|
| 底层 API 封装层 | 注册表读写、MoveFileEx 调用、内存管理 | `RW_OpenRegKey`, `ReadRegistryData`, `WriteRegistryData`, `BackupData` |
| 核心业务逻辑层 | REG_MULTI_SZ 解析、跳过算法、抹除算法 | `ParseMultiSzData`, `InjectSkipPrefix`, `EraseEntry` |
| 用户交互层 | CLI 界面、命令行解析 | `ShowMenu`, `ShowPendingList`, `HandleAdd`, `HandleRemove` |

## 编译

### 环境要求

- Windows 10/11
- Visual Studio 2022（MSVC v145）
- C 语言编译器（支持 C11 标准）

### 编译步骤

1. 使用 Visual Studio 打开 `RebootWipe.slnx`
2. 选择配置（Release | x64 或 Debug | x64）
3. 右键项目 → 生成

或使用命令行：

```bat
MSBuild RebootWipe.slnx /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```

## 使用

### 交互式模式

```
RebootWipe.exe
```

运行后显示主菜单：

```
╔════════════════════════════════════════════════════════════╗
║       RebootWipe - Windows 重启文件操作管理器             ║
╚════════════════════════════════════════════════════════════╝

  [1] 查看待处理的文件操作列表
  [2] 将文件标记为重启后删除
  [3] 取消操作 - 跳过模式（注入 ?? 前缀）
  [4] 取消操作 - 直接抹除模式
  [5] 退出程序

  请选择操作 (1-5):
```

### 命令行模式

```bash
RebootWipe.exe help              # 显示帮助
RebootWipe.exe read              # 查看待处理列表
RebootWipe.exe add <path>       # 添加文件到重启删除列表
RebootWipe.exe skip <n>          # 跳过第 n 项操作
RebootWipe.exe erase <n>         # 抹除第 n 项操作
```

## 核心原理

### PendingFileRenameOperations

Windows 注册表 `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager` 下的 `PendingFileRenameOperations` 值（`REG_MULTI_SZ` 类型）记录了系统重启后需要执行的文件操作：

- **删除操作**：`[源路径]\0\0`
- **移动操作**：`[源路径]\0[目标路径]\0`

### 跳过机制

通过在条目前注入 `??` 前缀，Windows Session Manager 在重启时会忽略以 `??` 开头的条目，从而实现"安全取消"。

### 安全机制

- 操作前自动备份原始注册表数据到临时文件
- 抹除操作使用 `memmove` 防止内存重叠
- 所有资源（注册表句柄、内存）均有释放验证

## 注意事项

- 程序需要以**管理员权限**运行（项目已配置 UAC 清单）
- 对 `PendingFileRenameOperations` 的操作直接影响系统重启行为，请谨慎操作
- 建议在执行抹除操作前确认备份文件已正确生成

## 项目结构

```
RebootWipe/
├── RebootWipe/
│   ├── RebootWipe.c              # 主源文件
│   ├── RebootWipe.vcxproj        # 项目文件
│   ├── RebootWipe.vcxproj.filters
│   └── RebootWipe.vcxproj.user
├── RebootWipe.slnx               # 解决方案文件
├── .gitignore
└── README.md
```

## 许可证

MIT License
