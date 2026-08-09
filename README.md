# 🧹 RebootWipe

**Windows 重启文件操作管理器** — 管理 `PendingFileRenameOperations` 注册表项，安全地安排文件在系统重启后删除。

[![Language](https://img.shields.io/badge/language-C-blue.svg)](https://learn.microsoft.com/en-us/cpp/c-language)
[![Platform](https://img.shields.io/badge/platform-Windows-lightgray.svg)](https://www.microsoft.com/windows)
[![License](https://img.shields.io/badge/license-GPL--3.0-orange.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Visual Studio](https://img.shields.io/badge/Visual%20Studio-2022-purple.svg)](https://visualstudio.microsoft.com/)

---

## ✨ 特性

- 📖 **查看** — 解析并展示待处理的文件操作列表（删除 / 移动 / 跳过）
- ➕ **加入** — 使用 `MoveFileExW` API 将文件标记为重启后删除
  - 支持单路径、分号分隔多路径、`@文件` 批量导入
  - 非空目录递归展开（后序遍历，自动处理父子依赖）
  - 交互式非空目录确认提示
  - 原生支持长路径（`\\?\` 前缀，最长 32,767 字符）
- 🚫 **取消** — 两种安全模式
  - **跳过模式**：注入 `??` 前缀，系统重启时自动忽略
  - **抹除模式**：物理删除注册表条目，自动备份原始数据
- 🔐 **UAC 自动提权** — 启动时自动请求管理员权限
- 🛡️ **安全机制** — 写后读验证、备份恢复、内存安全

---

## 🚀 快速开始

### 环境要求

| 项目 | 要求 |
|------|------|
| 操作系统 | Windows 10 / 11 (x64) |
| 编译器 | MSVC (Visual Studio 2022 v145) |
| 标准 | C11 |

### 编译

**方式一：Visual Studio**

1. 打开 `RebootWipe.slnx`
2. 选择配置（`Release | x64`）
3. 右键项目 → **生成**

**方式二：命令行**

```bat
MSBuild RebootWipe.slnx /p:Configuration=Release /p:Platform=x64 /t:Rebuild
```

### 使用

**交互式模式**

```
RebootWipe.exe
```

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

**命令行模式**

```bash
RebootWipe.exe help                 # 显示帮助
RebootWipe.exe read                 # 查看待处理列表
RebootWipe.exe add C:\temp\file.txt  # 添加单个文件
RebootWipe.exe add C:\a.txt;C:\b.txt;D:\dir  # 批量添加
RebootWipe.exe add @list.txt        # 从文件批量导入
RebootWipe.exe skip 3               # 跳过第 3 项操作
RebootWipe.exe erase 5              # 抹除第 5 项操作
```

---

## 🏗️ 架构

项目采用 **Core + GUI** 两模块分层设计：

```
RebootWipe.c (入口)
  ├── core                          # 核心模块
  │     ├── 注册表 I/O              # 读写 PendingFileRenameOperations
  │     ├── 核心算法                # 解析 / 跳过 / 抹除
  │     ├── 文件操作                # 添加 / 批量 / 目录展开
  │     └── 控制台基础设施          # 颜色输出
  └── gui                           # GUI 模块
        ├── CLI 界面                # 菜单 / 处理函数 / 命令解析
        ├── UAC 自动提权            # RunAsAdmin
        └── 暂停清屏                # PauseAndClear
```

### 模块文件

| 模块 | 文件 | 职责 |
|------|------|------|
| 公共定义 | `RebootWipe.h` | 类型、常量、跨平台宏 |
| 核心 | `core.h` / `core.c` | 注册表 I/O、核心算法、文件操作、控制台颜色 |
| GUI | `gui.h` / `gui.c` | CLI 菜单、命令处理、UAC、清屏 |
| 入口 | `RebootWipe.c` | `wmain` 启动流程编排 |

---

## 📁 项目结构

```
RebootWipe/
├── RebootWipe/
│   ├── RebootWipe.h              # 公共头文件
│   ├── RebootWipe.c              # 程序入口 (wmain)
│   ├── core.h                    # 核心模块声明
│   ├── core.c                    # 核心模块实现
│   ├── gui.h                     # GUI 模块声明
│   ├── gui.c                     # GUI 模块实现
│   ├── RebootWipe.vcxproj        # 项目文件
│   ├── RebootWipe.vcxproj.filters
│   └── RebootWipe.vcxproj.user
├── RebootWipe.slnx               # 解决方案文件
├── .gitignore
├── LICENSE
└── README.md
```

---

## 📖 核心原理

### PendingFileRenameOperations

Windows 注册表 `HKLM\SYSTEM\CurrentControlSet\Control\Session Manager` 下的 `PendingFileRenameOperations` 值（`REG_MULTI_SZ` 类型）记录了系统重启后需要执行的文件操作：

- **删除操作**：`[源路径]\0\0`
- **移动操作**：`[源路径]\0[目标路径]\0`

### 跳过机制

通过在条目前注入 `??` 前缀，Windows Session Manager 在重启时会忽略以 `??` 开头的条目，从而实现"安全取消"，而不破坏原始数据。

### 目录删除顺序

根据微软官方文档，`MoveFileEx` 只能删除空目录。对于非空目录，程序采用**后序遍历**递归展开：

1. 先收集目录内所有文件和子目录
2. 子目录继续递归展开
3. 最后添加目录本身

注册表中删除条目的顺序为：**文件 → 子目录 → 父目录**，确保重启时能正确删除。

> 官方文档：[MoveFileExW function](https://learn.microsoft.com/zh-cn/windows/win32/api/winbase/nf-winbase-movefileexw)

### 安全机制

- 💾 **自动备份**：操作前将原始注册表数据备份到临时文件
- 🔒 **内存安全**：抹除操作使用 `memmove` 防止内存重叠
- ✅ **写后验证**：写入后立即读回验证，防止杀软静默拦截
- 🧹 **资源释放**：所有注册表句柄和内存均有释放验证
- 🔄 **重解析点检测**：自动跳过重解析点（符号链接），避免无限循环

---

## ⚠️ 注意事项

- 程序需要以**管理员权限**运行（项目已配置 UAC 清单，启动时自动提权）
- 对 `PendingFileRenameOperations` 的操作**直接影响系统重启行为**，请谨慎操作
- 建议在执行抹除操作前确认备份文件已正确生成
- 远程共享路径不支持 `MOVEFILE_DELAY_UNTIL_REBOOT`

---

## 🤝 贡献

欢迎贡献代码！请遵循以下步骤：

1. Fork 本仓库
2. 创建功能分支 (`git checkout -b feature/amazing-feature`)
3. 提交更改 (`git commit -m 'Add some amazing feature'`)
4. 推送到分支 (`git push origin feature/amazing-feature`)
5. 提交 Pull Request

---

## 📄 许可证

本项目基于 [GPL-3.0](LICENSE) 许可证开源