中文版本 (Chinese)
🚀 项目名称：GhostControl - GameSir T4 修正版
基于 StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher 修改
核心改进： 解决了原版驱动中摇杆轴向颠倒的痛点，完美适配 GameSir T4 (Start云手柄)。
✨ 功能亮点
摇杆修复：修正了原版驱动中摇杆方向颠倒的问题，操作手感丝滑顺畅。
全向控制：左摇杆、右摇杆及方向键（D-Pad）均已调试正常，满足各类游戏需求。
即插即用：基于 PS5 Payload 技术，通过 USB 连接即可让非官方手柄被 PS5 识别为虚拟 DualSense 手柄。
📌 使用须知
硬件要求：已获取内核权限（已越狱/已刷机）的 PS5 主机。
手柄模式：请确保 GameSir T4 处于正确的连接模式（通常为 Switch 模式或特定映射模式）。
免责声明：使用第三方驱动存在风险，请谨慎操作。
🙏 致谢
特别感谢 StonedModder 大神提供的底层核心代码。
感谢 OpenAI 提供的 AI 技术支持。
English Version
🚀 Project: GhostControl - GameSir T4 Patched Edition
Forked & Modified from StonedModder/Ghostcontrol-PS5-USB-Controller-Patcher
Key Fix: Resolved the inverted joystick axis issue present in the original version, providing perfect compatibility for the GameSir T4 (Start Cloud Controller).
✨ Features
Joystick Correction: Fixed the reversed analog stick directions, ensuring smooth and intuitive control.
Full Input Support: Both thumbsticks and the D-pad are fully functional and properly mapped.
Plug & Play: Utilizes PS5 Payload technology to inject USB controller input as a virtual DualSense device.
📌 Requirements
Hardware: A PS5 console with kernel exploit access (Jailbroken).
Controller Mode: Ensure your GameSir T4 is in the correct mode (e.g., Switch Pro mode).
Note: Use at your own risk.
🙏 Acknowledgements
Huge thanks to StonedModder for the original open-source project.
Thanks to OpenAI for AI support.
💡 技术背景补充 (供参考)
为了让你的介绍更具专业性，这里补充一些底层逻辑说明（基于参考文档）：
工作原理：该项目利用了 PS5 的 scePadVirtualDevice 接口。它通过 USB 读取第三方手柄（如 GameSir T4）的原始数据，经过方向修正后，将其“伪装”成一个官方的虚拟 DualSense 手柄发送给系统。
为何需要修正：很多第三方手柄在 HID 通信协议上与 Switch 或 XInput 标准存在细微差异（例如 X/Y 轴的正负极性定义相反），导致原版驱动直接读取时会出现“往上推摇杆角色往下走”的情况。你的修改主要是在 gc_main.c 或数据解析层对轴向数据进行了取反（value = -value）处理。
