<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [其他语言](../README.md#translations)

## 项目简介

Switchyard Wine 是 [Switchyard](https://github.com/jungwuk-ryu/Switchyard)
的 Wine runtime。Switchyard 是面向 macOS 的应用，提供 Windows 游戏和启动器的
托管容器运行环境。应用本身负责界面、容器状态和 runtime 选择；本仓库负责
Wine 代码、构建流水线与兼容性治理。

该分支的侧重点是“可持续兼容”：在既有 Switchyard 工作负载验证通过前，不会
仅因新上游版本发布就更换基础。下游改动以固定 WineHQ 基础版本为锚，
以可审阅的普通 Git 提交形式叠加。

## 分支存在的原因

- **针对 macOS 的运行时修复**：涵盖 Switchyard 实际故障路径，包括
  D3DMetal 回调与资源桥接、macOS MSync 同步、Chromium/CEF 渲染、图形提供者
  选择、媒体播放、多语言字体回退。
- **可验证的运行时身份**：外部输入会被固定并哈希化，构建在活动运行时外
  进行并通过校验后再切换；每个 runtime 记录源码版本、依赖哈希、架构及核心
  二进制哈希。
- **修复与回归测试并行**：仓库会对 D3DMetal、native callbacks、MSync、
  Steam overlay 热补丁、TLS、媒体、OpenGL 与运行时安全路径做回归验证。
- **有上下文的兼容性记录**：结果明确标注具体 runtime、macOS 主机、显卡路径、
  日期和已知限制，同一环境下的通过结果不等于“普遍支持”。

## MSIX 与打包桌面应用

Switchyard Wine 提供 `wineappx` 和 `appxsvc.dll` 支持已签名、未加密的
full-trust 桌面 MSIX/AppX。已实现的生命周期包含：验证检查、带校验提取、
按前缀安装/更新、卸载、恢复、垃圾回收，并可启动声明式 Win32 或 WinUI 3 应用，
带 package identity 与静态依赖。

该范围有意小于完整 Windows 语义。它不是 Microsoft Store 客户端，不是
UWP/AppContainer 实现，也不支持未签名 MSIX 包绕过签名校验，且不代表全部
Windows App SDK API 都可用。有关精确的包要求、命令、耐久性策略与当前限制，
请见
[MSIX 指南](../msix.md)。

## 运行截图

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard container library showing Windows games and launchers on macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced and Rockstar Games Launcher running in Switchyard" width="100%">
  <br>
  <sub>在同一受管 Wine 会话中运行 GTA V Enhanced 与 Rockstar Games Launcher。</sub>
</p>

## 获取与构建 Runtime

Switchyard 用户通常由应用直接管理容器和 runtime 选择。经过签名和公证的
Wine-only 归档可在
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases)
下载。Apple Game Porting Toolkit 为用户本地提供，不会包含在本仓库或发布包内。

在 Apple Silicon macOS 上从源码构建：

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

发布或替换 runtime 之前请先阅读
[构建指南](../building.md)，其中包含所需工具链、已验证依赖、staging 流程、
签名、公证以及回归测试清单。

## 文档

- [文档索引](../README.md)
- [架构与仓库边界](../architecture.md)
- [构建并发布 Runtime](../building.md)
- [MSIX 与打包桌面支持](../msix.md)
- [应用兼容性记录](../compatibility.md)
- [源码与依赖溯源（provenance）](../provenance.md)
- [Unity 游戏排障](../troubleshooting-unity-games.md)

一般的 Wine 使用请参考
[WineHQ 文档](https://gitlab.winehq.org/wine/wine/-/wikis/home)
和 [上游源码](https://gitlab.winehq.org/wine/wine)。本 README 仅覆盖
Switchyard Wine，不重复 upstream Wine 手册。

## 社区与许可

欢迎加入 [Switchyard Discord](https://discord.gg/USNfzUza7B)
参与运行时测试、兼容性报告和开发讨论。

Wine 与 Switchyard Wine 的更改使用 LGPL-2.1-or-later 许可，参见
`LICENSE` 与 `COPYING.LIB`。

Switchyard Wine 为独立项目，与 WineHQ、Apple、Microsoft 或上方展示的产品
无官方背书关系。
