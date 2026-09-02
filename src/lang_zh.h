/* ============================================================================
 *  HDDHealth Monitor - Simplified Chinese Language Pack
 *  ---------------------------------------------------------------------------
 *  100% Free and Open Source Software (FOSS).
 *  License : MIT
 * ============================================================================
 */

#pragma once
#ifndef LANG_ZH_H
#define LANG_ZH_H

#include "lang.h"

/* UI 字符串表 (中文) */
static const char* const g_Strings_ZH[STR_COUNT] = {
    /* Main Window & Menus */
    /* STR_APP_TITLE          */ "HDDHealth Monitor 1.1",
    /* STR_MENU_FILE           */ "文件(&F)",
    /* STR_MENU_VIEW           */ "视图(&V)",
    /* STR_MENU_LANG           */ "语言(&L)",
    /* STR_MENU_HELP           */ "帮助(&H)",
    /* STR_MENU_SCREENSHOT     */ "保存截图(&S)\tCtrl+S",
    /* STR_MENU_SAVETEXT       */ "保存为文本文件(&T)...\tCtrl+T",
    /* STR_MENU_EXIT           */ "退出(&X)",
    /* STR_MENU_HISTORY        */ "历史趋势图(&H)",
    /* STR_MENU_DONATE         */ "赞助支持(&D)...",
    /* STR_MENU_ABOUT          */ "关于 HDDHealth(&A)...",
    /* STR_MENU_SHOW_WINDOW    */ "显示主窗口(&S)",
    /* STR_LANG_NAME_ZH        */ "简体中文",
    /* STR_LANG_NAME_EN        */ "English",

    /* Main Window Labels */
    /* STR_LBL_DRIVES          */ "硬盘列表",
    /* STR_LBL_PERF            */ "性能状态",
    /* STR_LBL_HEALTH          */ "健康状态",
    /* STR_LBL_MODEL           */ "型号",
    /* STR_LBL_SERIAL          */ "序列号",
    /* STR_LBL_FIRMWARE        */ "固件版本",
    /* STR_LBL_CAPACITY        */ "容量",
    /* STR_LBL_TEMPERATURE     */ "温度",
    /* STR_LBL_SMART           */ "S.M.A.R.T.",
    /* STR_LBL_READ_SPEED      */ "连续速度",
    /* STR_BTN_HISTORY_GRAPH   */ "历史趋势图",
    /* STR_NO_DRIVES_FOUND     */ "未检测到硬盘",

    /* List View Columns */
    /* STR_COL_ID              */ "ID",
    /* STR_COL_ATTRIBUTE       */ "属性名称",
    /* STR_COL_VALUE           */ "当前值 / 详情",
    /* STR_COL_STATUS          */ "状态",

    /* Status Badges */
    /* STR_STATUS_OK           */ "OK",
    /* STR_STATUS_WARNING      */ "警告",
    /* STR_STATUS_FAILED       */ "异常",
    /* STR_STATUS_NA           */ "--",

    /* Drive Details & S.M.A.R.T. Texts */
    /* STR_TYPE_PREFIX         */ "类型: ",
    /* STR_HEALTH_FMT          */ "健康度: %d%%",
    /* STR_HEALTH_NA           */ "健康度: 未知",
    /* STR_PERF_FMT            */ "性能: %d%%",
    /* STR_PERF_NA             */ "性能: 未知",
    /* STR_DRIVE_FMT           */ "磁盘 %d",
    /* STR_TRAY_HEALTH_FMT     */ "磁盘 %d: 健康度 %d%%\n%s",
    /* STR_TRAY_HEALTH_NA_FMT  */ "磁盘 %d: 健康度 未知\n%s",
    /* STR_TRAY_SCANNING       */ "HDDHealth Monitor - 正在扫描...",
    /* STR_NVME_HEALTH_LOG_FMT */ "NVMe 健康日志   备用空间: %d%%   已用寿命: %d%%",
    /* STR_NVME_HEALTH_LOG_ZH_FMT */ "已用寿命 %d%%",
    /* STR_NVME_NOT_READABLE   */ "NVMe - 无法读取健康日志 (请以管理员身份运行)",
    /* STR_USB_NOT_SUPPORTED   */ "不可用 (不支持该 USB 桥接芯片)",
    /* STR_USB_SAT_PASSTHROUGH_FMT */ "USB SAT 直通模式 - %s",
    /* STR_USB_LIMITED_SCSI    */ "USB - 受限模式 (仅 SCSI 日志探测)",
    /* STR_SUPPORTED_FMT       */ "支持   %s",
    /* STR_NOT_SUPPORTED       */ "不支持",
    /* STR_ENABLED             */ "已开启",
    /* STR_DISABLED            */ "已禁用",
    /* STR_DETECTED            */ "已检测到",
    /* STR_NOT_AVAILABLE       */ "不可用",
    /* STR_UNAVAILABLE         */ "不可用",

    /* NVMe Critical Warning Flags */
    /* STR_NVME_CRIT_HEADER    */ "严重警告:",
    /* STR_NVME_CRIT_SPARE     */ " [备用空间不足]",
    /* STR_NVME_CRIT_TEMP      */ " [温度过高]",
    /* STR_NVME_CRIT_RELIABILITY */ " [可靠性下降!]",
    /* STR_NVME_CRIT_READ_ONLY */ " [只读模式!]",
    /* STR_NVME_CRIT_VOLATILE_MEM */ " [易失性内存备份失败]",

    /* Alert Titles & Messages */
    /* STR_ALERT_OVERHEAT_TITLE  */ "!! 严重警告: 硬盘过热 !!",
    /* STR_ALERT_OVERHEAT_MSG    */ "%s\n当前温度: %d °C (临界阈值: %d °C)\n请立即加强散热或关机以保护数据！",
    /* STR_ALERT_TEMP_WARN_TITLE */ "警告: 硬盘温度偏高",
    /* STR_ALERT_TEMP_WARN_MSG   */ "%s\n当前温度: %d °C (警告阈值: %d °C)\n请检查设备散热状况。",
    /* STR_ALERT_HEALTH_CRIT_TITLE */ "!! 严重警告: 硬盘健康状况极差 !!",
    /* STR_ALERT_HEALTH_CRIT_MSG */ "%s\n健康度: %d%% — 硬盘可能即将损坏，请立即备份所有重要数据！",
    /* STR_ALERT_HEALTH_WARN_TITLE */ "警告: 硬盘健康状况有所下降",
    /* STR_ALERT_HEALTH_WARN_MSG */ "%s\n健康度: %d%% — 请密切关注并及时备份数据。",
    /* STR_ALERT_FAILURE_PRED_TITLE */ "!! 预测硬盘即将发生故障 !!",
    /* STR_ALERT_FAILURE_PRED_MSG */ "%s\n硬盘固件 S.M.A.R.T. 预测即将发生故障。\n请立即备份所有数据！",
    /* STR_ALERT_NVME_CRIT_TITLE */ "NVMe 严重警告",
    /* STR_ALERT_REALLOC_TITLE   */ "警告: 检测到重映射扇区 (坏道修复)",
    /* STR_ALERT_REALLOC_MSG     */ "%s\n重映射扇区数: %lu\n这表明硬盘盘片表面存在物理坏道损伤。",
    /* STR_ALERT_UNCORRECT_TITLE */ "!! 检测到不可校正的扇区错误 !!",
    /* STR_ALERT_UNCORRECT_MSG   */ "%s\n不可校正错误数: %lu\n数据完整性受到威胁 — 请立即备份！",

    /* Screenshot Dialogs */
    /* STR_SS_SUCCESS_MSG      */ "截图保存成功！\n\n%s\n\n是否立即在资源管理器中打开？",
    /* STR_SS_SUCCESS_TITLE    */ "HDDHealth Monitor - 截图已保存",
    /* STR_SS_FAIL_MSG         */ "截图保存失败：\n%s",
    /* STR_SS_FAIL_TITLE       */ "HDDHealth Monitor - 截图错误",

    /* Report Export Dialogs */
    /* STR_REPORT_TITLE        */ "HDDHealth Monitor - 硬盘健康与 S.M.A.R.T. 检测报告",
    /* STR_REPORT_SAVED_MSG    */ "硬盘健康与 S.M.A.R.T. 检测报告已成功保存！\n\n文件路径：\n%s\n\n是否立即打开查看？",
    /* STR_REPORT_SAVED_TITLE  */ "HDDHealth Monitor - 报告保存成功",
    /* STR_REPORT_FAIL_MSG     */ "保存检测报告失败：\n%s",
    /* STR_REPORT_FAIL_TITLE   */ "HDDHealth Monitor - 保存错误",

    /* About Dialog */
    /* STR_ABOUT_TITLE         */ "关于 HDDHealth Monitor",
    /* STR_ABOUT_DESC          */ "底层硬盘健康监测与 S.M.A.R.T. 数据分析工具。",
    /* STR_ABOUT_FOSS          */ "100% 自由与开源软件 (FOSS)",
    /* STR_ABOUT_SPONSOR_LINK  */ "在 GitHub 上赞助支持作者",
    /* STR_ABOUT_COPYRIGHT     */ "Copyright (C) 2026 ARImetic Inc. All Rights Reserved.",
    /* STR_ABOUT_AUTHOR        */ "作者: Ari Sohandri Putra - 开源 / MIT License",
    /* STR_BTN_OK              */ "确定",
    /* STR_BTN_DONATE          */ "赞助支持",
    /* STR_BTN_CLOSE           */ "关闭",

    /* History Graph Window */
    /* STR_HIST_TITLE          */ "HDDHealth Monitor - 历史趋势图",
    /* STR_HIST_TAB_HEALTH     */ "健康度 %",
    /* STR_HIST_TAB_ATTRS      */ "S.M.A.R.T. 属性",
    /* STR_HIST_LBL_DRIVE      */ "目标硬盘:",
    /* STR_HIST_LBL_ATTR       */ "监测属性:",
    /* STR_HIST_BTN_CLEAR      */ "清除历史记录",
    /* STR_HIST_NO_DATA        */ "当前硬盘暂无历史记录数据。",

    /* History Graph Dropdown Tracked Attributes */
    /* STR_HATTR_TEMP          */ "温度 (°C)",
    /* STR_HATTR_REALLOC       */ "重映射扇区数",
    /* STR_HATTR_PENDING       */ "待映射扇区数",
    /* STR_HATTR_UNCORRECT     */ "不可校正扇区数",
    /* STR_HATTR_POH           */ "通电累计时间 (小时)",
    /* STR_HATTR_POWER_CYCLES  */ "通电开关次数",
    /* STR_HATTR_READ_ERR_RATE */ "底层读取错误率",
    /* STR_HATTR_CRC_ERR       */ "CRC 传输错误数",

    /* Donate Dialog */
    /* STR_DONATE_TITLE        */ "HDDHealth Monitor - 赞助支持",
    /* STR_DONATE_HEADER       */ "支持 HDDHealth Monitor 持续开发",
    /* STR_DONATE_EXPLAIN      */ "HDDHealth Monitor 是 100% 自由且完全开源的软件。\n"
                                  "没有收费授权，没有试用限制，无需激活。\n"
                                  "所有功能均永久免费向所有用户开放。\n\n"
                                  "如果您觉得本工具有所帮助并希望支持后续维护，\n"
                                  "欢迎考虑成为赞助支持者！",
    /* STR_DONATE_AUTHOR_LINE  */ "作者 : %s (%s)",
    /* STR_DONATE_FAIL_MSG     */ "无法在默认浏览器中打开赞助页面。\n请手动访问以下网址：\n\n%s",
    /* STR_DONATE_FAIL_TITLE   */ "打开浏览器失败",

    /* S.M.A.R.T. Units & Suffixes */
    /* STR_UNIT_WRITTEN        */ "已写入",
    /* STR_UNIT_LIFE_REMAINING */ "SSD 剩余寿命",
    /* STR_UNIT_WEAR_CYCLES    */ "次磨损均衡循环",
    /* STR_UNIT_FAILURES       */ "次失败",
    /* STR_UNIT_RESERVED_SPACE */ "可用预留空间",
    /* STR_UNIT_FLYING_HOURS   */ "磁头飞行小时数",
    /* STR_UNIT_HOURS          */ "小时",
    /* STR_UNIT_MINUTES        */ "分钟",
    /* STR_UNIT_UNITS          */ "单位",

    /* Fallback / Special USB rows */
    /* STR_ROW_USB_NO_SMART    */ "该 USB/移动硬盘当前无法读取 SMART 数据",
    /* STR_ROW_NVME_RUN_ADMIN  */ "NVMe SMART: 请以管理员权限运行并点击刷新",
    /* STR_ROW_NVME_WIN10_VER  */ "部分 NVMe 控制器需要 Windows 10 1903+ 或更新的系统驱动",
    /* STR_ROW_SCSI_PREDICT    */ "预测性故障指示 (SCSI 信息异常)",
    /* STR_ROW_SCSI_TEMP       */ "硬盘温度 (SCSI 日志页)",
    /* STR_ROW_SCSI_NOTE       */ "说明: 当前桥接芯片未暴露完整的 ATA SMART 属性表",
    /* STR_SCSI_PRED_FAIL      */ "已预测到故障",
    /* STR_SCSI_NO_PRED_FAIL   */ "未预测到故障"
};

/* 硬盘类型中文翻译 */
static const char* const g_DriveTypeNames_ZH[] = {
    "未知设备",           /* DRIVE_TYPE_UNKNOWN */
    "机械硬盘 (HDD)",     /* DRIVE_TYPE_HDD */
    "固态硬盘 (SATA SSD)",/* DRIVE_TYPE_SSD_SATA */
    "NVMe 固态硬盘",      /* DRIVE_TYPE_NVME */
    "移动硬盘/USB",       /* DRIVE_TYPE_USB */
    "M.2 SATA 固态硬盘",  /* DRIVE_TYPE_M2_SATA */
    "eMMC 存储",          /* DRIVE_TYPE_EMMC */
    "SD 存储卡",          /* DRIVE_TYPE_SD */
    "SCSI/SAS 企业级硬盘" /* DRIVE_TYPE_SCSI */
};

/* 健康状态中文翻译 */
static const char* const g_HealthStatusNames_ZH[] = {
    "未知",  /* HEALTH_STATUS_UNKNOWN */
    "良好",  /* HEALTH_STATUS_GOOD */
    "注意",  /* HEALTH_STATUS_CAUTION */
    "不良",  /* HEALTH_STATUS_BAD */
    "警告"   /* HEALTH_STATUS_WARNING */
};

typedef struct {
    BYTE        bID;
    const char* szName;
} ATTR_NAME_I18N;

/* S.M.A.R.T. 属性中文名称字典 */
static const ATTR_NAME_I18N g_SmartAttrNames_ZH[] = {
    /* 常用标准 ATA 属性 */
    { 0x01, "底层读取错误率 (Raw Read Error Rate)" },
    { 0x02, "吞吐量性能 (Throughput Performance)" },
    { 0x03, "主轴电机起旋时间 (Spin-Up Time)" },
    { 0x04, "启停计数 (Start/Stop Count)" },
    { 0x05, "重映射扇区计数 (Reallocated Sectors Count)" },
    { 0x06, "读取通道边限 (Read Channel Margin)" },
    { 0x07, "寻道错误率 (Seek Error Rate)" },
    { 0x08, "寻道时间性能 (Seek Time Performance)" },
    { 0x09, "通电累计时间 (Power-On Hours)" },
    { 0x0A, "起旋重试次数 (Spin Retry Count)" },
    { 0x0B, "校准重试次数 (Calibration Retry Count)" },
    { 0x0C, "通电次数 (Power Cycle Count)" },
    { 0x0D, "软件读取错误率 (Soft Read Error Rate)" },
    { 0x0E, "重力感应错误率 (G-Sense Error Rate Alt)" },
    { 0x0F, "磁头装载/卸载重试次数 (Load/Unload Retry Count)" },
    { 0x10, "磁头飞行时间 (Head Flying Hours)" },
    { 0x11, "校准重试次数备用 (Calibration Retry Count Alt)" },
    { 0x16, "当前氦气水平 (Current Helium Level)" },
    { 0x17, "氦气状态下限 (Helium Condition Lower)" },
    { 0x18, "氦气状态上限 (Helium Condition Upper)" },
    { 0x19, "氦气状态计数 (Helium Condition Count)" },
    { 0x1A, "额定剩余寿命 (Remaining Rated Life)" },
    { 0x1B, "剩余耐久度 (Endurance Remaining)" },
    { 0x1C, "可用预留空间 (Available Reserved Space)" },

    /* SSD 属性 */
    { 0xA0, "不安全关机计数 (Unsafe Shutdown Count)" },
    { 0xA1, "已用预留块总数 (Used Reserved Block Count Total)" },
    { 0xA2, "最差已用预留块数 (Used Reserved Block Count Worst)" },
    { 0xA3, "出厂坏块总数 (Initial Bad Block Count)" },
    { 0xA4, "总擦除次数 (Total Erase Count)" },
    { 0xA5, "最大擦除次数 (Max Erase Count)" },
    { 0xA6, "最小擦除次数 (Min Erase Count)" },
    { 0xA7, "平均擦除次数 (Average Erase Count)" },
    { 0xA8, "规格最大擦除次数 (Max Erase Count of Spec)" },
    { 0xA9, "剩余寿命百分比 (Remaining Life Percentage)" },
    { 0xAA, "可用预留空间 (Available Reserved Space)" },
    { 0xAB, "编程失败计数 (Program Fail Count)" },
    { 0xAC, "擦除失败计数 (Erase Fail Count)" },
    { 0xAD, "磨损均衡计数 (Wear Leveling Count)" },
    { 0xAE, "意外掉电计数 (Unexpected Power Loss)" },
    { 0xAF, "掉电保护失效 (Power Loss Protection Fail)" },
    { 0xB0, "芯片擦除失败计数 (Erase Fail Count Chip)" },
    { 0xB1, "磨损范围差值 (Wear Range Delta)" },
    { 0xB2, "已用预留块计数 (Used Reserved Block Count)" },
    { 0xB3, "已用预留块总数备用 (Used Reserved Block Count Total Alt)" },
    { 0xB4, "未用预留块总数 (Unused Reserved Block Count Total)" },
    { 0xB5, "总编程失败计数 (Program Fail Count Total)" },
    { 0xB6, "总擦除失败计数 (Erase Fail Count Total)" },
    { 0xB7, "SATA 降速错误计数 (SATA Downshift Error Count)" },
    { 0xB8, "端到端校验错误 (End-to-End Error)" },
    { 0xB9, "磁头稳定性 (Head Stability)" },
    { 0xBA, "感应运行振动检测 (Induced Op-Vibration Detection)" },
    { 0xBB, "脱机无法校正错误数 (Uncorrectable ECC Error Count)" },
    { 0xBC, "命令超时 (Command Timeout)" },
    { 0xBD, "高飞写入 (High Fly Writes)" },
    { 0xBE, "气流温度 (Airflow Temperature)" },
    { 0xBF, "重力感应冲击错误率 (G-Sense Error Rate)" },
    { 0xC0, "断电磁头缩回计数 (Power-Off Retract Count)" },
    { 0xC1, "磁头加载/卸载周期计数 (Load/Unload Cycle Count)" },
    { 0xC2, "硬盘温度 (Temperature)" },
    { 0xC3, "硬件 ECC 恢复计数 (Hardware ECC Recovered)" },
    { 0xC4, "重映射事件计数 (Reallocation Event Count)" },
    { 0xC5, "当前待映射扇区数 (Current Pending Sectors)" },
    { 0xC6, "脱机无法校正的扇区数 (Uncorrectable Sectors)" },
    { 0xC7, "UltraDMA CRC 传输错误计数 (UltraDMA CRC Error Count)" },
    { 0xC8, "多区域写入错误率 (Write Error Rate)" },
    { 0xC9, "软件读取错误率备用 (Soft Read Error Rate Alt)" },
    { 0xCA, "数据地址标记错误 (Data Address Mark Errors)" },
    { 0xCB, "偏离行程取消 (Run Out Cancel)" },
    { 0xCC, "软 ECC 校正 (Soft ECC Correction)" },
    { 0xCD, "热粗糙率 (Thermal Asperity Rate)" },
    { 0xCE, "飞行高度 (Flying Height)" },
    { 0xCF, "起旋高电流 (Spin High Current)" },
    { 0xD0, "起旋杂音 (Spin Buzz)" },
    { 0xD1, "脱机寻道性能 (Offline Seek Performance)" },
    { 0xD2, "写入期间振动 (Vibration During Write)" },
    { 0xD3, "写入期间振动备用 (Vibration During Write Alt)" },
    { 0xD4, "写入期间冲击 (Shock During Write)" },
    { 0xD5, "自由落体保护 (Free Fall Protection)" },
    { 0xD6, "自由落体事件计数 (Free Fall Event Count)" },
    { 0xDC, "盘片偏移 (Disk Shift)" },
    { 0xDD, "重力感应错误率备用 (G-Sense Error Rate Alt)" },
    { 0xDE, "加载运行时间 (Loaded Hours)" },
    { 0xDF, "加载/卸载重试次数 (Load/Unload Retry Count)" },
    { 0xE0, "加载摩擦力 (Load Friction)" },
    { 0xE1, "加载/卸载循环计数备用 (Load/Unload Cycle Count Alt)" },
    { 0xE2, "加载进入时间 (Load In-Time)" },
    { 0xE3, "扭矩放大计数 (Torque Amplification Count)" },
    { 0xE4, "断电磁头缩回周期 (Power-Off Retract Cycle)" },
    { 0xE5, "GMR 磁头振幅 (GMR Head Amplitude)" },
    { 0xE6, "GMR 磁头振幅备用 (GMR Head Amplitude Alt)" },
    { 0xE7, "SSD 剩余寿命/温度 (SSD Life Left / Temperature)" },
    { 0xE8, "可用预留空间 (Available Reserved Space)" },
    { 0xE9, "NAND 写入量 (GB) / 介质损耗 (Media Wearout)" },
    { 0xEA, "平均擦除次数 / 总写入 (Average Erase Count)" },
    { 0xEB, "良好块计数 / NAND 耐久度 (Good Block Count)" },
    { 0xEC, "写入错误计数 (Write Error Count)" },
    { 0xED, "循环冗余校验计数 (CRC Count)" },
    { 0xEE, "PMR 磁头稳定性 (PMR Head Stability)" },
    { 0xEF, "SATA PHY 物理层错误计数 (SATA PHY Error Count)" },
    { 0xF0, "磁头飞行时间备用 (Head Flying Hours Alt)" },
    { 0xF1, "总写入 LBA 扇区数 (Total LBAs Written)" },
    { 0xF2, "总读取 LBA 扇区数 (Total LBAs Read)" },
    { 0xF3, "总写入 LBA 扩展 (Total LBAs Written Expanded)" },
    { 0xF4, "总读取 LBA 扩展 (Total LBAs Read Expanded)" },
    { 0xF5, "最小擦除次数 (Min Erase Count)" },
    { 0xF6, "最大擦除次数 (Max Erase Count)" },
    { 0xF7, "平均擦除次数 (Average Erase Count)" },
    { 0xF8, "磨损均衡计数 (Wear Leveling Count)" },
    { 0xF9, "NAND 闪存写入量 (GiB) (NAND Writes GiB)" },
    { 0xFA, "读取错误重试率 (Read Error Retry Rate)" },
    { 0xFB, "最小剩余备用块 (Minimum Spares Remaining)" },
    { 0xFC, "新增闪存坏块数 (Newly Added Bad Flash Block)" },
    { 0xFD, "盘间表面缺陷计数 (Inter-Surface Defect Count)" },
    { 0xFE, "自由落体保护 (Free Fall Protection)" },
    { 0xFF, "厂商特定属性 (Vendor-Specific)" },
    { 0x00, NULL }
};

#endif /* LANG_ZH_H */
