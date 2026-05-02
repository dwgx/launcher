# Launcher

Windows 桌面游戏启动器 / 订阅管理器。客户端 C++20 + Skia + Clay，后端 Rust + axum。

详细约定见 [CLAUDE.md](./CLAUDE.md)。

## 构建

```powershell
git clone https://github.com/dwgx1337/launcher.git
cd launcher
./scripts/download-fonts.ps1
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## 目录

| 路径 | 说明 |
|---|---|
| `src/` | 客户端 C++ 源码 |
| `SystemBackend/` | Rust 后端 + 签名 CLI |
| `assets/fonts/` | UD 字体（脚本下载） |
| `scripts/` | 部署、字体下载 |
| `docs/` | 详细架构 / 协议文档 |

## 状态

骨架阶段。Phase 1（加载卡片）开发中。
