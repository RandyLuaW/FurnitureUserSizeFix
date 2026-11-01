<div align="center">

# Furniture User Size Fix

**一个 SKSE 插件：为非标准缩放体型的玩家与 NPC 角色适配其使用家具时的身体位置与姿态，并提供上肢 IK 调整功能以适配各工作台类型家具。**

[![License](https://img.shields.io/badge/license-source--available%20%7C%20GPL--3.0-lightgrey.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Platform](https://img.shields.io/badge/platform-Skyrim%20SE%201.5.97%20%7C%20AE%201.6.629%2B-lightgrey.svg)](#前置要求)
[![Binaries](https://img.shields.io/badge/binaries-Nexus%20Mods-informational.svg)](#下载)

[English](README.md) ｜ 简体中文 ｜ **下载：Nexus Mods**

> 本仓库**只托管源码**，不发布编译好的安装包——见[下载](#下载)。

</div>

---

## 这是什么

游戏在制作家具时，是按 1.0 体型的角色来安排位置与高度的。当角色的实际体型偏离 1.0 时，结果肉眼可见：陷入座椅、相对家具表面偏前或偏后、双手低于工作台面导致工具与材料穿过手臂。

本插件在运行时对**当前正在使用该家具的角色**进行修正：

- **位置修正** —— 前后与垂直两个分量独立，可按模型、按模型加坐点标记、或按单件家具分别设定。
- **上肢 IK** —— 通过旋转上臂把手部抬升至台面高度，而非整体抬升角色，因此双脚保持贴地。

本 MOD 仅包含插件本体：无 ESP、无脚本、不修改任何游戏记录，也不向存档写入任何数据。

## 有效体型如何计算

```
有效体型 = actor->GetScale() × "NPC" 节点缩放 × "NPC Root [Root]" 节点缩放
```

角色变矮可能有三种来源，本插件全部覆盖：种族默认身高、角色自身缩放、以及自定义骨架 NIF 中写入的节点缩放。体型为 1.0 时两个修正系数均精确为零，因此标准体型角色完全不受影响。

## 前置要求

| | |
|---|---|
| 游戏 | Skyrim SE 1.5.97，或 Skyrim AE 1.6.629 及以上 |
| SKSE | SE 需 0.2.0.20+，AE 需 2.2.3+ |
| Address Library | 与你的运行时版本匹配的文件 |
| 身体 MOD | 需提供 `DOBJ SAT1` 置空（例如 CBBE 3BA 的 `3BBB.esp`） |
| 可选 | SKSE Menu Framework，用于游戏内调参菜单 |

## 下载

编译好的安装包发布在 **Nexus Mods**，不在本仓库。该页面提供两个文件：

| 文件 | 用途 |
|---|---|
| `FurnitureUserSizeFix-<版本>-FOMOD.zip` | 主包。安装向导会询问游戏运行时与菜单语言。 |
| `FurnitureUserSizeFix-<版本>-CHS.zip` | 可选。仅简体中文菜单翻译。 |

若要从源码自行构建，见 [docs/BUILDING.md](docs/BUILDING.md)。

## 快速开始

1. 用 MOD 管理器安装下载到的安装包。向导会询问你的游戏运行时与菜单语言。
2. 启用本 MOD。它不添加任何插件文件，因此不影响加载顺序。
3. 坐到椅子上或站到工作台前。若需微调，打开 SKSE 菜单拖动滑块（立即生效），再按保存按钮。

配置文件位于 `SKSE/Plugins/FurnitureUserSizeFix.ini`，每一项均有注释说明。界面默认英文，其他语言以独立的可选语言包提供。

## 构建

见 [docs/BUILDING.md](docs/BUILDING.md)。简述：Visual Studio 2022 + CMake + CommonLibSSE-NG；仅当需要菜单时，才需要自行获取 SKSE Menu Framework 头文件（见 [include/README.md](include/README.md)）。

## 兼容性提示

- **与 Furniture Height Size Fix（FHSF）不兼容。** 两者在使用家具期间施加同一类补偿，同时启用会造成双重修正、产生相反的症状。请只启用其中之一。
- **与 FurnitureHandIK 不兼容。** 其功能已并入本插件，同时启用会使手臂旋转角度翻倍。
- **脚部 IK 类 MOD** 会与工作台类家具上的整体抬升相互抵触；若使用此类 MOD，建议优先调整角度而非残余抬升。
- **SexLab 等场景框架驱动的家具** 通过 `[Forbid]` 段默认排除，因为场景框架会把角色直接传送到家具上。

## 已知限制

- 磨刀石按骑跨式坐具处理，而非站立式工作台；其小体型下的腿部穿模源于腿间距与家具宽度的不匹配，本项目有意不做处理。
- 体型大于等于 1.0 的角色默认不做修正；菜单中提供了一个可选开关，该路径从未在游戏内验证。
- 所有数值以 0.65 体型为标定基准，运行时按角色实际体型换算。

## 安全性

本插件不向存档写入任何数据：不保存任何 co-save 数据、不注册脚本、不添加法术/技能/物品、不修改任何记录。所有位置修正均在使用家具期间逐帧施加，角色起身后立即还原，因此卸载后不会留下残留。

## 免责与许可声明

### 免责声明

本软件按「原样」提供，不附带任何担保，使用风险自负。与任何会在运行时改变角色位置的 MOD 一样，请自行备份存档。

本项目与 Bethesda Softworks、ZeniMax Media、SKSE 团队及任何 MOD 托管站点均无隶属、背书或赞助关系。

### 免费、源码公开、禁止转售

本项目为**源码公开**（source-available），而非 OSI 开源软件。你可以免费使用、研究、修改与再分发（含修改版本），前提是：署名原作者、附上本仓库链接、声明所做修改、保留许可与声明文件，并且**不得以任何形式收费**，包括打包进付费产品或付费整合包。

完整条款见 [LICENSE](LICENSE)；随包组件清单见 [THIRD-PARTY-LICENSES.txt](THIRD-PARTY-LICENSES.txt)。

### 关于编译产物

插件为可选菜单链接了 SKSE Menu Framework 的接口代码，该头文件为 GPL-3.0，因此含该菜单的二进制属结合作品，必须按 GPL-3.0 分发。为使这件事合法、同时让其余情形保留源码公开条款，本项目采用**双许可**：[LICENSE](LICENSE) 中的条款为默认，另提供 **GPL-3.0** 作为替代方案（见 LICENSE 第 11 节与 [LICENSE-GPL-3.0](LICENSE-GPL-3.0)）。含菜单的二进制按 GPL-3.0 选项分发；不含菜单头的构建产物不含 GPL-3.0 代码，适用默认条款。

编译好的安装包发布在 Nexus Mods，本仓库只托管源码。
