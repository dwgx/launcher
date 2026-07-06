# 客户端概览

客户端是 C++20 原生 Windows 程序，面向 VMProtect 加固，运行在**不可信环境**。本章分页覆盖：

- **[App 与 UI 层](app-ui.md)** —— 进程入口、`AppPhase` 状态机、`EventLoop`、Skia 渲染 + 动画管线、主题、视图/组件。
- **[网络 / 存储 / 核心](net-storage-core.md)** —— libcurl HTTP 客户端、Registry+DPAPI 隐写存储、SQLite 缓存、EventBus、订阅协议契约。
- **[加密与原生模块](crypto-native.md)** —— 编译期字符串混淆、DPAPI 封存、14 源 HWID 指纹、动态导入隐藏。
- **[图片管线（跨端）](image-pipeline.md)** —— 异步解码 + 下载池 + 后端缩略图三波优化（D2D 客户端 + 后端）。
- **[桌面通知](notifications.md)** —— 顶部动态岛 Toast（`tools/preview-d2d/toast.*`）。

!!! important "两套客户端代码：`src/` 骨架 vs `tools/preview-d2d/` 出货客户端"
    **实际出货、真正联通后端的客户端是 `tools/preview-d2d/`（D2D，Direct2D）**——图片管线、动态岛通知、聊天/市场
    等联网功能都在这里（见 `docs/PHASE_2_D2D_MIGRATION.md`）。而 `src/` 是**长期骨架**：构件（net/storage/crypto/
    native）已实现但主入口 `main.cpp` 尚未接线（下文「目录结构」与各子系统页描述的是 `src/`）。读者切勿把 `src/`
    当作活跃客户端。跨端图片链路见 [图片管线](image-pipeline.md)，通知见 [桌面通知](notifications.md)，
    截图回归见 [视觉冒烟测试](../dev/testing-visual-smoke.md)。

## 目录结构与职责

| 目录 | 职责 | 实现状态 |
|---|---|---|
| `src/app` | 进程入口、窗口、事件循环（唯一 `main`） | 已落地（Phase 1，仅驱动 LoadingView） |
| `src/ui/render` | `SkiaRenderer` + `FontManager` | 已落地（文本 shaping 简化，见对应页） |
| `src/ui/anim` | 补间动画基础设施 | 已落地（仅 LoadingView 之外未被使用） |
| `src/ui/theme` | `ThemeManager` 单例调色板 | 已落地 |
| `src/ui/views` | 具体视图 | **仅 `loading_view.cpp` 有实现**，其余为头文件声明 |
| `src/ui/components` | 可复用组件 | 仅 `loading_card` / `spinner` 有实现 |
| `src/net` | libcurl HTTP 封装 | 构件已实现，未被 app 层接线 |
| `src/storage` | Registry+DPAPI / SQLite | 构件已实现，未被 app 层接线 |
| `src/core` | EventBus | 已实现，无业务事件类型 |
| `src/crypto` | crypt_str / dpapi_seal | 已实现，被 storage 消费 |
| `src/native` | hwid / dyn_api | 已实现，无 net/app 调用者 |
| `src/proto` | subscription.proto（契约） | schema 就位，客户端消费侧未实现 |

!!! warning "客户端整体处于「构件就绪、未接线」阶段"
    `net`/`storage`/`core` 三层是功能完整但未接线的构件；`main.cpp` 仍是 Phase 1 占位，不实例化其中任何一个，
    项目里也**没有 `Launcher` 类**。凡涉及跨模块编排的流程，均在各页明确标注 `unverified（未落地）`。
    端到端视角见 [端到端数据流](../architecture/data-flow.md)。

## 关键约定

- **错误码而非异常**：`src/app/common.h` 定义 `Status` / `Result<T>`（`code==0`/`error_code==0` 为成功），全客户端禁用异常 + RTTI（对齐 CMake `/GR- /EHs-c-`，见 [构建与部署](../dev/build-deploy.md)），错误一律经 `Result` 传递。
- **禁止直接碰 SkCanvas**：业务层只通过 `SkiaRenderer` 绘制（`src/ui/render/skia_renderer.h:4-5`）。
- **敏感/非敏感分层存储**：敏感数据走 `Registry`（DPAPI + 隐写），非敏感走 `SqliteStore`（`src/storage/sqlite_store.h:3-5`）。
