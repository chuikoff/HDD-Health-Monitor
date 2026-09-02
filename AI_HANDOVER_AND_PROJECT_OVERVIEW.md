# HDDHealth Monitor - 项目架构与 AI 改进交接文档 (Project Handover)

> **文档用途**：供开发者与后续 AI 助手快速理解本项目的设计架构、核心特点以及历次 AI 重构与改进的详细记录。

---

## 1. 项目概览与核心特点 (Project Overview & Characteristics)

### 1.1 项目定位
**HDDHealth Monitor** 是一款面向 Windows 平台的高性能、轻量级、完全开源（FOSS）的磁盘健康与 S.M.A.R.T. 监控工具。

### 1.2 技术栈与设计特点
1. **纯原生 Win32 + GDI/GDI+ 架构**：
   - 零重型框架依赖（无 Qt、MFC、.NET 或 Electron）。
   - 启动极快（毫秒级），内存占用极低（通常 < 15MB）。
2. **零运行时依赖与单文件分发**：
   - 使用 MinGW-w64 编译，配置 `-static -static-libgcc -static-libstdc++` 静态链接。
   - 生成的 `bin/HDDHealthMonitor.exe` 独立无外部 DLL 依赖，可直接便携运行。
3. **多接口与多协议支持**：
   - 支持 SATA (ATA)、NVMe、USB 外接硬盘盒 / 桥接芯片（JMicron, ASMedia, Realtek, VIA 等）。
   - 支持 IOCTL 穿透查询（`IOCTL_STORAGE_QUERY_PROPERTY`, `IOCTL_ATA_PASS_THROUGH`, `SMART_RCV_DRIVE_DATA`, SCSI Pass-Through 等）。
4. **现代化 UI 与高分屏支持**：
   - 侧边栏多硬盘卡片切换、健康度/性能仪表条、自绘彩色状态徽标（Badges）。
   - 内置双语（简体中文 / English）即时无缝切换。
   - 历史健康度与 S.M.A.R.T. 属性趋势图表（GDI+ 双缓冲平滑绘制）。
   - 任务栏托盘实时温度常驻显示。

---

## 2. 源码模块架构说明 (Codebase Architecture)

```
HDD-Health-Monitor/
├── Makefile                     # MinGW-w64 构建脚本 (支持 make clean; make)
├── README.md                    # 官方说明文档
├── ask.txt                      # 改进需求规范文件
├── AI_HANDOVER_AND_PROJECT_OVERVIEW.md # 本交接文档
├── bin/
│   └── HDDHealthMonitor.exe     # 最终输出可执行文件
├── src/
│   ├── main.cpp                 # 应用程序入口、DPI 初始化、单实例 Mutex、主消息循环
│   ├── mainwnd.h / .cpp         # 主窗口、布局引擎、DPI 动态缩放、自绘控件、文本导出
│   ├── smart.h / .cpp           # 磁盘设备枚举、S.M.A.R.T. 协议交互、NVMe 规范解析、健康算法
│   ├── smart_history.h / .cpp   # 历史数据采样、持久化存储 (.dat) 与 GDI+ 趋势折线图
│   ├── lang.h / .cpp            # 双语翻译引擎、UTF-8 转 UTF-16 宽字符安全转换
│   ├── lang_zh.h / lang_en.h    # 中英文词条映射表
│   ├── donate.h / .cpp          # 赞助与开源信息弹窗（DPI 与宽字符适配）
│   ├── app.rc / resource.h      # 资源脚本（图标、版本信息）
│   └── app.manifest             # PerMonitorV2 高 DPI 声明与 UTF-8 代码页 Manifest
```

### 各模块职责分工

| 源文件 | 核心职责 |
| :--- | :--- |
| `src/smart.cpp` | 硬件底层通信：遍历 PhysicalDrive 设备，检测是否为 NVMe / USB，下发 SMART IOCTL，提取原始字节并计算温度、寿命与健康等级。 |
| `src/mainwnd.cpp` | UI 核心与交互：主界面排版、硬盘按钮绘制（`DriveBtnWndProc`）、属性表格自绘（`NM_CUSTOMDRAW`）、高分屏响应（`WM_DPICHANGED`）、报告导出（`ExportToTextFile`）。 |
| `src/smart_history.cpp` | 趋势图：按天/次采样记录健康数据，使用 GDI+ 渲染抗锯齿折线、网格坐标轴、悬浮 Tooltip 与 Tab 分页。 |
| `src/lang.cpp` | 国际化：管理 `LANG_ZH_CN` / `LANG_EN`，提供 `LStrW()`、`Lang_GetSmartAttrNameW()` 及环形缓冲区宽字符安全转换。 |

---

## 3. AI 实施的重大改进与重构记录 (Changelog of AI Improvements)

### 3.1 底层驱动检测与健壮性提升
- **硬盘检测与空列表退出问题修复**：
  - 增强了 `SetupAPI` 设备枚举与 `\\.\PhysicalDriveX` 直接扫描的双通道容错；
  - 完善了 NVMe 识别逻辑（支持 SCSI 与 StorageQuery 两种路径解析），避免在管理员或非管理员权限下由于探测失败导致程序提前退出；
  - 增加了安全边界检查，防止异常驱动器返回脏数据导致越界崩溃。

### 3.2 字符集与中文乱码全面修复 (Unicode Migration)
- **原因分析**：原工程混用了 UTF-8 字节串与 ANSI 版 Windows GDI API（如 `DrawTextA`, `SetDlgItemTextA`, `ListView_SetItemA`），且硬编码了单字节 `\xb0` 作为度数符号，在中文 Windows (GBK/CP936) 环境下出现大面积乱码。
- **重构方案**：
  - 在 `src/lang.cpp` 中引入高性能、线程安全的 `Utf8ToW` 宽字符转换层与 `LStrW()` 系列函数；
  - 将主界面、左侧硬盘卡片、右侧详情、列表视图、历史图表、弹窗全部迁移至 Unicode 宽字符 Win32 API（`*W` 函数）；
  - 统一温度符号为标准 UTF-16 `°C`；
  - 修复 `ListView` 列头初次创建由于宏展开调用 ANSI 版本导致的列名乱码。

### 3.3 全界面 2K / 4K 高分屏自适应 (High-DPI Scaling)
- **Manifest 声明**：在 `src/app.manifest` 中配置 `dpiAwareness = PerMonitorV2, PerMonitor`。
- **动态坐标与字体缩放体系**：
  - 引入全局 `g_nDpi` 与 `SCALE_DPI(v)` 宏，所有尺寸、外边距、列宽、按钮大小、进度条均基于实际 DPI 计算；
  - 字体统一采用 ClearType 优化的 `Microsoft YaHei UI`，字体高度通过 `MulDiv(-size, dpi, 96)` 动态生成；
  - 全面支持 `WM_DPICHANGED` 消息，在跨屏幕拖拽或调整系统缩放比时自动触发 `RelayoutControls`、重建 GDI 字体并无缝重绘；
  - 修复了“关于 (About)”与“赞助支持 (Donate)”子窗口固定像素导致文字挤压重叠的问题。

### 3.4 界面文案与展示细节优化
- **左侧硬盘卡片**：中文模式下由 `perf:100% Health:xx%` 规范显示为 `性能: 100%  健康: xx%`，温度显示 `xx °C`。
- **NVMe 状态简化**：右侧健康信息精简为 `已用寿命 xx%`，避免长文案换行溢出。
- **S.M.A.R.T. 状态规范**：正常状态在中文版中保持为国际通用的 **`OK`**（不再显示为“正常”）。
- **历史图表 Tab 标题**：重命名为 **`健康度 %`** 和 **`S.M.A.R.T. 属性`**，内部坐标轴及文字均已适配宽字符与高 DPI。

### 3.5 新增功能：导出到文本报告 (Save to Text File)
- **菜单与快捷键**：在“文件”菜单新增 **“保存到文本文件...”**（快捷键 `Ctrl + T`）。
- **导出内容**：
  - 自动添加 UTF-8 BOM（保证各类文本编辑器打开均无乱码）；
  - 输出系统基本信息与检测时间；
  - 逐盘导出详细规格（型号、序列号、固件版本、容量、接口类型、实时温度、健康度等级、连续读取速度）；
  - 完整格式化导出 NVMe 状态日志及 S.M.A.R.T. 原始属性对照表（含 ID、属性名、当前值/详情、状态判定）。

---

## 4. 后续 AI 接手与维护指南 (Developer / AI Guide)

### 4.1 编译与测试命令
本项目可在 Windows 11 / 10 下使用 MinGW-w64 编译：
```powershell
# 1. 确保 MinGW-w64 在环境变量中（或指定路径）
$env:PATH = "D:\Program Files\mingw64\bin;" + $env:PATH

# 2. 清理并全量编译
make clean
make

# 3. 输出路径
.\bin\HDDHealthMonitor.exe
```

### 4.2 开发注意事项
1. **字符集规范**：
   - 内部多语言词条源文件（`lang_zh.h`, `lang_en.h`）采用 UTF-8 编码保存；
   - 涉及到所有 Win32 控件文本显示、GDI 绘图、对话框调用时，**必须统一调用 `*W` 宽字符 API** 及 `LStrW()` / `Utf8ToW()`，切勿使用 `*A` 函数。
2. **高 DPI 规范**：
   - 任何新增的 UI 控件、坐标、宽高、间距，必须使用 `SCALE_DPI(value)` 或 `MulDiv(value, dpi, 96)` 进行换算；
   - 字体创建必须绑定对应 DPI 计算出的负高度值，并在窗口销毁时清理 GDI 句柄。
3. **硬件检测扩展**：
   - 如需新增特定 USB 硬盘盒主控或新 NVMe 厂商指令，在 `src/smart.cpp` 的 `IsNVMeDrive` 与 `GetDriveSmartInfo` 中添加穿透逻辑即可。
