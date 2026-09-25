# cool-helper

Windows x64 的本地 AI 截图问答与 DWM Overlay 实验项目。

```text
CoolHelperHub（管理员中台）
  截图 → OpenAI-compatible 视觉模型 → 流式答案
                         ├→ 中台 ImGui 窗口
                         └→ 共享内存 IPC → DwmCfgOverlayLab → DWM Overlay
```

## 项目结构

- `CoolHelperHub/`：Win32 + Dear ImGui + D3D11 中台程序。负责截图、AI 请求、托盘/全局快捷键、DLL 安装与安全卸载，以及向 Overlay 发布答案。
- `DwmCfgOverlayLab/`：注入 `dwm.exe` 的 x64 DLL。负责按已验证的 DWM 呈现路径渲染 ImGui Overlay、接收 IPC 答案流，并在卸载前恢复 Hook。

## 构建

```powershell
cd CoolHelperHub
.\build.cmd                 # 默认 Release x64

cd ..\DwmCfgOverlayLab
.\build.cmd                 # 默认 Release x64，并复制 DLL 到 Hub 输出目录
```

详细配置、兼容性和生命周期说明见各子项目的 `README.md`。
