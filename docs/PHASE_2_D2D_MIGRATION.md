# Phase 2 Migration — GDI+ Preview → D2D + DComp 金标准 Pipeline

> **目标**：把 `tools/preview/loading_demo.cpp` 的全部 UI 渲染从 GDI+ 迁到
> Direct2D + DirectComposition + DXGI flip-model + waitable swap chain。
> 这条 pipeline 已在 `tools/preview-skia/skia_d3d.cpp` 验证完整跑通，用户原话
> "**太丝滑了 就用它**"。
>
> **重要**：**不是** Skia。aseprite/skia bundle 是 GL only，集成成本高且无收益
> （D2D 跟 Skia D3D 同档次硬件 16× analytic AA）。这条路线已经决定。

---

## 0. 给下一个 Agent 的强制读物（按顺序）

1. 这份文件 — 整体规范
2. `tools/preview-skia/skia_d3d.cpp` — 唯一权威参考实现，**跑过**、**用户认可**
3. `tools/preview/loading_demo.cpp` — 要被替换的源（保留为旧版参考，不要删）
4. `SESSION_HANDOFF.md` — 当前 commit / 已部署后端 / 已知问题
5. `CLAUDE.md` — 项目硬约束
6. `C:\Users\dwgx1\.claude\projects\D--Project-Launcher\memory\feedback_d2d_pipeline.md`
   — 我踩过的坑 + 选型理由

---

## 1. Pipeline 架构（不可改）

```
┌────────────────────────────────────────────────────────────────────┐
│ Win32 HWND  WS_EX_NOREDIRECTIONBITMAP                             │
│ ↓ 没 DWM redirection bitmap，DWM 直接从 swap chain 合成           │
│ ↓ 代价：GDI / PrintWindow 看不到内容（CopyFromScreen 才能截屏）   │
├────────────────────────────────────────────────────────────────────┤
│ D3D11 Device                                                       │
│ ↓ Flag: D3D11_CREATE_DEVICE_BGRA_SUPPORT (DComp + D2D 都要)       │
│ ↓ Feature levels: 11_1, 11_0                                       │
├────────────────────────────────────────────────────────────────────┤
│ DXGI Flip-model Swap Chain (CreateSwapChainForComposition)         │
│ ↓ Format: DXGI_FORMAT_B8G8R8A8_UNORM                              │
│ ↓ AlphaMode: PREMULTIPLIED  (DComp 强制要求)                      │
│ ↓ SwapEffect: FLIP_DISCARD                                         │
│ ↓ BufferCount: 3                                                   │
│ ↓ Flags: FRAME_LATENCY_WAITABLE_OBJECT [+ ALLOW_TEARING if support]│
│ ↓ swap2->SetMaximumFrameLatency(1)                                 │
│ ↓ frame_waitable = swap2->GetFrameLatencyWaitableObject()          │
├────────────────────────────────────────────────────────────────────┤
│ DComp 视觉树                                                        │
│ ↓ DCompositionCreateDevice(IDXGIDevice) → IDCompositionDevice     │
│ ↓ CreateTargetForHwnd(hwnd, /*topmost=*/TRUE) → IDCompositionTarget│
│ ↓ CreateVisual → IDCompositionVisual                               │
│ ↓ visual->SetContent(swapchain)                                    │
│ ↓ target->SetRoot(visual)                                          │
│ ↓ device->Commit()                                                 │
├────────────────────────────────────────────────────────────────────┤
│ D2D1 渲染 — 跟 D3D11 同 GPU resource，零拷贝                       │
│ ↓ D2D1CreateFactory(SINGLE_THREADED) → ID2D1Factory1              │
│ ↓ factory->CreateDevice(IDXGIDevice) → ID2D1Device                │
│ ↓ device->CreateDeviceContext(NONE) → ID2D1DeviceContext          │
├────────────────────────────────────────────────────────────────────┤
│ 帧循环（金标准节奏）                                                 │
│ for (;;) {                                                         │
│   WaitForSingleObjectEx(frame_waitable, 100, TRUE);                │
│   while (PeekMessage) Dispatch;                                    │
│   if (resize_pending) ResizeBuffers + SetTarget(nullptr);          │
│   bindD2DTarget();           // GetBuffer(0) → CreateBitmapFromDxgiSurface
│   d2d_ctx->BeginDraw();                                            │
│   d2d_ctx->Clear(...);                                             │
│   render_ui();                                                     │
│   d2d_ctx->EndDraw();                                              │
│   d2d_ctx->SetTarget(nullptr);  // **必须**释放 让 Present 能 flip │
│   swap->Present(0, ALLOW_TEARING);                                 │
│ }                                                                  │
└────────────────────────────────────────────────────────────────────┘
```

---

## 2. 不许碰的东西（已经对，别动）

| 模块 | 文件 | 状态 |
|---|---|---|
| 后端 (Rust + axum) | `SystemBackend/` | ✅ 部署在 154.40.36.22 production |
| WinHTTP 客户端 | `tools/preview/net.inl` | ✅ HTTP + WS 都跑通了 |
| 注册表持久化 | `loading_demo.cpp::persist::*` | ✅ 30 候选路径 + DPAPI |
| HWID 计算 | `loading_demo.cpp::hwidHex()` | ✅ ComputerName + UserName + VolSerial → SHA256 |
| 异步 fetch helpers | fetchMyStickers / fetchMyPacks / fetchUserTags / fetchHistory / fetchRemoteAvatar | ✅ 都接通后端 |
| 业务状态 (g_user / g_packs / g_user_tags / chatv::* streams / WsInbox) | 各 .inl | ✅ |
| 动画 Tween + tx::Slide/Fade/Scale | `transitions.inl` | ✅ |
| 后端协议 / API endpoint | `SystemBackend/crates/api/src/main.rs` | ✅ |

**只换渲染层**。GDI+ Graphics 调用全部替换成 D2D `ID2D1DeviceContext` 调用，
其他业务逻辑、状态机、网络、动画 tween 数学全保留。

---

## 3. GDI+ → D2D API 映射表

| GDI+ | D2D | 备注 |
|---|---|---|
| `Graphics(HDC)` | `ID2D1DeviceContext` (cached) | 一次创建，每帧复用 |
| `Graphics::FillRectangle(brush, rect)` | `ctx->FillRectangle(rect, brush)` | 参数序反 |
| `Graphics::DrawRectangle(pen, rect)` | `ctx->DrawRectangle(rect, brush, stroke_w, stroke_style)` | |
| `fillRR(g, x, y, w, h, r, color)` (我们的 helper) | `ctx->FillRoundedRectangle({rect, r, r}, brush)` | D2D 原生支持，不用 GraphicsPath |
| `strokeRR(...)` | `ctx->DrawRoundedRectangle({rect, r, r}, brush, w, stroke_style)` | |
| `Graphics::FillEllipse` | `ctx->FillEllipse({{cx,cy}, rx, ry}, brush)` | |
| `Graphics::DrawLine(pen, p1, p2)` | `ctx->DrawLine(p1, p2, brush, w, stroke_style)` | |
| `GraphicsPath::AddBezier` | `ID2D1PathGeometry` + `Open()` + `AddBezier`/`AddArc` | 用 PathGeometry，不要 GraphicsPath |
| `Graphics::FillPath` | `ctx->FillGeometry(geom, brush)` | |
| `Graphics::DrawString` | `ctx->DrawText(str, len, format, rect, brush)` | format = `IDWriteTextFormat`（**用 DirectWrite，不用 GDI 字体**）|
| `Graphics::MeasureString` | `IDWriteTextLayout::GetMetrics` | layout 一次创建，多次查 |
| `SolidBrush(color)` | `ID2D1SolidColorBrush` (cached per color) | |
| `Pen(color, width)` | `ID2D1SolidColorBrush` + `ID2D1StrokeStyle` | brush 跟 stroke_style 解耦 |
| `Pen::SetAlignment(Inset)` | `D2D1_STROKE_STYLE_PROPERTIES::dashCap` 等 | D2D stroke 默认就是 inset 行为 |
| `Color(a, r, g, b)` (0..255) | `D2D1::ColorF(r, g, b, a)` (0..1f) | **顺序差**：Color 是 ARGB，ColorF 是 RGBA，且范围 0..1 |
| `Image::FromFile(path)` | `IWICImagingFactory::CreateDecoderFromFilename` → `CreateBitmapFromWicBitmap` → `ID2D1Bitmap` | WIC 在 D2D 里是规范图片解码 |
| `Graphics::DrawImage(img, rect)` | `ctx->DrawBitmap(bitmap, rect)` | |
| `Graphics::SetClip(path)` | `ctx->PushLayer(layer_params, layer)` 或 `PushAxisAlignedClip(rect)` | 矩形裁剪用 axis-aligned 快 10x |
| `Graphics::ResetClip()` | `ctx->PopLayer()` 或 `PopAxisAlignedClip()` | |
| `Graphics::TranslateTransform(x, y)` | `ctx->SetTransform(D2D1::Matrix3x2F::Translation(x, y))` | 注意：Identity 不再需要"重置"，直接 SetTransform 覆盖 |
| `Graphics::ScaleTransform(sx, sy)` | `ctx->SetTransform(Matrix3x2F::Scale({sx, sy}))` | |
| `LinearGradientBrush` | `ID2D1LinearGradientBrush` + `ID2D1GradientStopCollection` | 多 stops 用 D2D1::GradientStop[] |
| `ImageAttributes(ColorMatrix)` | `D2D1_BITMAP_BRUSH_PROPERTIES` 或 `ID2D1Effect (ColorMatrix)` | D2D 1.1+ Effects 框架 |

---

## 4. 字体 / 文字渲染 — DirectWrite

**不要用 GDI Font（`HFONT` / `Graphics::DrawString`）**。D2D 用 DirectWrite，
**原生支持 Win11 彩色 emoji COLR/CPAL** — 我们之前 GDI+ 那边的 `Segoe UI Emoji`
只能渲单色就是因为 GDI+ 不走 DirectWrite。

```cpp
ComPtr<IDWriteFactory> dwrite;
DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                    (IUnknown**)dwrite.GetAddressOf());

// 一次性建 TextFormat 缓存（按 face + size + weight）
ComPtr<IDWriteTextFormat> fmt;
dwrite->CreateTextFormat(
    L"Microsoft YaHei UI", nullptr,
    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
    /*font_size_dip=*/12.0f, L"zh-cn", &fmt);

// 字号单位是 DIP (device independent pixel = 1/96 inch)，跟 GDI+ UnitPoint 不同：
//   DIP_size = pt * 96/72 = pt * 4/3
// e.g. 9pt → 12 DIP

// 渲染：
ctx->DrawText(L"你好 😀", 4, fmt.Get(), rect, brush.Get());
// 彩色 emoji 自动出来，不用我们费劲 fallback。
```

**DirectWrite 字体回退链**：通过 `IDWriteFontFallback`（DirectWrite 1.1+）
配置 SP/CJK/Emoji 优先级。Win11 系统默认 fallback 已经够用，先别折腾自定义
fallback。

**测量文字宽度**（替代 `Graphics::MeasureString`）：

```cpp
ComPtr<IDWriteTextLayout> layout;
dwrite->CreateTextLayout(text, len, fmt.Get(), max_w, max_h, &layout);
DWRITE_TEXT_METRICS m;
layout->GetMetrics(&m);
// m.width / m.height
// 渲染用 ctx->DrawTextLayout(origin, layout.Get(), brush)
```

---

## 5. Brush 缓存策略

D2D Brush 创建有成本，**不能每帧 new**。建 brushcache：

```cpp
// 按 (a, r, g, b) 量化作 key
struct BrushKey { uint32_t argb; bool operator==(...) const; };
struct BrushKeyHash { ... };
std::unordered_map<BrushKey, ComPtr<ID2D1SolidColorBrush>, BrushKeyHash> g_brushes;

ID2D1SolidColorBrush* getBrush(D2D1_COLOR_F c) {
    BrushKey k{ pack_argb(c) };
    auto it = g_brushes.find(k);
    if (it != g_brushes.end()) return it->second.Get();
    ComPtr<ID2D1SolidColorBrush> b;
    g_app.d2d_ctx->CreateSolidColorBrush(c, &b);
    auto [ins, _] = g_brushes.emplace(k, std::move(b));
    return ins->second.Get();
}
```

类似的，`IDWriteTextFormat` 按 (face, weight, size_q) 缓存。
`ID2D1StrokeStyle` 按 (cap, join, dash) 缓存（数量很少，可以全局 4-5 个）。

**ResizeBuffers 时不要 invalidate brush 缓存** — brush 跟 D3D device 绑定，
device 没换就一直能用。

---

## 6. 阴影渲染

GDI+ 我们用 `drawShadow` 一次大 path 假阴影。D2D 有真高斯模糊（GPU 加速）：

**方法 A — 简单**: 跟 GDI+ 一样，多层 RoundedRect 假阴影（参考 spinner demo
里的 6 层版本）。够用，跨平台一致。

**方法 B — 真高斯**: D2D 1.1 Effects 框架
```cpp
ComPtr<ID2D1Effect> shadow;
ctx->CreateEffect(CLSID_D2D1Shadow, &shadow);
shadow->SetInput(0, source_bitmap);
shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 4.0f);
shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.4f));
ctx->DrawImage(shadow.Get(), D2D1::Point2F(0, 4));
```

但需要先把 source 渲到 off-screen bitmap。第一波先用方法 A，后续优化再走 B。

---

## 7. 整体迁移步骤（按风险递增）

**Step 1：搭新主程序框架**（保留 GDI+ Preview 不动）
- 文件：`tools/preview-d2d/main.cpp`
- 把 `skia_d3d.cpp` 的 D3D + DComp + D2D + waitable 部分提取成
  `D2DApp` 类（`init` / `resize` / `beginFrame` / `endFrame` / `present`）
- 写 brushcache / textformatcache helpers
- WIC bitmap 加载 helper

**Step 2：移植入场动画**（最简单，没业务依赖）
- Stage::Dot → Loading → Auth → ShrinkSuccess → CheckSuccess → ExpandMain
- 全部用 D2D 重画
- 验证：跟 GDI+ 版本视觉对比，确认顺滑度提升明显

**Step 3：移植 Auth view**
- AuthForm + InputBox（自绘，已经在用）
- 浮动 label 动画
- 错误提示

**Step 4：移植 Topbar + Sidebar**
- Topbar：标题 + 头像 pill + 状态 dot
- Sidebar：6 个 menu item + active 指示条
- Account dropdown popover

**Step 5：移植 Home view**
- Hero 文字 + profile-card + 3 stat 卡 + chips

**Step 6：移植 Chat view**（最复杂，最后做）
- 频道列表 + 消息流 + 气泡 + composer + picker + emoji + sticker pack
- 头像缩略图（WIC 解码 → 缓存 ID2D1Bitmap）
- 图片 / GIF / video bubble — `ctx->DrawBitmap` 替代 `Graphics::DrawImage`

**Step 7：移植 Modals**
- CS2 / ChangePw / AddTag / CreatePack / RenamePack / Confirm / HistoryNew

**Step 8：移植 Lunching / Market / Cloud / Settings / Profile**

**Step 9：删 `tools/preview/`** （只在所有功能 parity 后）

---

## 8. 命名 / 文件组织

```
tools/preview-d2d/
├── main.cpp                  # wWinMain + frame loop
├── d2d_app.{h,cpp}           # D2DApp 类（init/resize/begin/end）
├── render/
│   ├── brush_cache.{h,cpp}   # ID2D1SolidColorBrush 缓存
│   ├── text_cache.{h,cpp}    # IDWriteTextFormat / TextLayout 缓存
│   ├── stroke_cache.{h,cpp}  # ID2D1StrokeStyle 缓存
│   ├── image_cache.{h,cpp}   # WIC + ID2D1Bitmap 缓存
│   └── primitives.{h,cpp}    # fillRR/strokeRR/drawText_/drawShadow 等 helper
├── views/
│   ├── auth.cpp
│   ├── home.cpp
│   ├── lunching.cpp
│   ├── chat/
│   │   ├── chat_view.cpp
│   │   ├── chat_list.cpp
│   │   ├── chat_pane.cpp
│   │   ├── chat_bubble.cpp
│   │   ├── chat_composer.cpp
│   │   └── chat_picker.cpp
│   ├── market.cpp
│   ├── cloud.cpp
│   ├── settings.cpp
│   └── profile.cpp
├── modals/
│   ├── modal_cs2.cpp
│   ├── modal_change_pw.cpp
│   ├── modal_add_tag.cpp
│   ├── modal_pack.cpp     # CreatePack + RenamePack 共用
│   └── modal_confirm.cpp
├── icons.cpp                 # SVG-style icons via ID2D1PathGeometry
├── transitions.h             # 复用现有 tx::Slide/Fade/Scale (与 GDI+ Preview 共享)
├── persist.{h,cpp}           # 复制自 GDI+ Preview，不变
├── net.{h,cpp}               # 复制自 GDI+ Preview，不变
├── hwid.{h,cpp}              # 复制
└── build_d2d.bat
```

---

## 9. 必须遵守的硬规则（坑）

1. **ClearRenderTargetView / ColorF 顺序**：D2D 全部是 RGBA float (0..1)，
   不是 GDI 的 ARGB。`D2D1::ColorF(r, g, b, a)`，**a 在最后**。
2. **Premultiplied alpha**：DComp 强制要求 premul。
   - SolidColorBrush 不需要手动 premul，D2D 内部处理
   - 但你创建 swap chain bitmap、传 `D2D1_ALPHA_MODE_PREMULTIPLIED` 时
     要保证 `Clear` / 内容也是 premul（透明度 a 时 R/G/B 要乘 a）
3. **不要跨帧重用 ID2D1Bitmap1**（FLIP_DISCARD swap chain back buffer）：
   每帧 GetBuffer(0) → CreateBitmapFromDxgiSurface → SetTarget。
   帧末必须 SetTarget(nullptr) 让 Present 能 flip。
4. **NOREDIRECT 窗口**：
   - 不能用 GDI 函数画到客户区
   - PrintWindow 截图全黑（这是预期）
   - 截屏要用 `Graphics::CopyFromScreen` 或 Windows.Graphics.Capture
   - GDI Mouse 命中测试 (WM_NCHITTEST) 照常工作
5. **ResizeBuffers 前**：必须 `d2d_ctx->SetTarget(nullptr)` + 释放所有引用
   back buffer 的 ID2D1Bitmap1，否则 ResizeBuffers 失败 (E_INVALIDARG)。
6. **Per-Monitor DPI**：
   - `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 必调
   - D2D RT 默认 96 DPI；要拿真 DPI 调
     `IDWriteFactory::CreateTextFormat(... font_size_dip=pt*4/3)`，
     再 `ctx->SetTransform(Matrix3x2F::Scale({dpi/96, dpi/96}))`
   - 鼠标坐标除 dpi_scale 转回逻辑（hits 注册都是逻辑坐标）
7. **不要 Sleep / DwmFlush**：waitable + Present(0, ALLOW_TEARING) 是节奏唯一
   来源。`timeBeginPeriod(1)` 没用了 — waitable 不靠 OS scheduler。
8. **WM_ERASEBKGND return 1**：跟 GDI+ Preview 一样跳擦背景。
9. **Tearing 检测**：`IDXGIFactory5::CheckFeatureSupport(ALLOW_TEARING)`
   失败时 swap chain 不要带 `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` flag，
   Present 也不要带 `DXGI_PRESENT_ALLOW_TEARING` — 否则报错。
10. **Thread**：D2D Factory 用 `D2D1_FACTORY_TYPE_SINGLE_THREADED` 即可，
    所有 D2D / DComp 调用在 UI 线程。

---

## 10. 测试 / 验证 checklist

每个 Step 完成后：
- [ ] cl.exe build 干净（无 error，warning < 5）
- [ ] 启动不崩，运行 60 秒以上无内存增长（Task Manager 看 Working Set）
- [ ] CopyFromScreen 截屏视觉跟 GDI+ Preview 同页面对比，无明显退化
- [ ] 鼠标 hover / 点击命中区域正确
- [ ] 动画顺滑（弹出来 modal / dropdown / picker 不卡）
- [ ] DPI 125% / 150% 显示器跑过一次（窗口尺寸 / 文字大小 / 鼠标命中都对）
- [ ] 调试 Layer 启用 (`D3D11_CREATE_DEVICE_DEBUG` + `D2D1_DEBUG_LEVEL_WARNING`)
      → 输出窗口无 D3D / D2D 警告

最终：
- [ ] 跟 GDI+ Preview 功能 1:1 parity
- [ ] 弹模态 / 切 view / 缩窗 / 转圈圈 全部肉眼丝滑
- [ ] 删除 `tools/preview/`（保留 git 历史，不留死代码）

---

## 11. 当前已知 / 待办（来自 GDI+ Preview 的累积）

- 头像 / sticker / pack cover 都已经下载到 `%LOCALAPPDATA%/Launcher/`，
  D2D 移植后用 WIC 加载这些已存在文件
- WS 实时接收 + history 拉 + status 同步已经接通后端，**只换渲染**
- 表情包分组 create/rename/share/delete 后端 + 客户端逻辑都全
- user_tags / 头像云同步全套都通

**Backend 不要碰**。已经部署在 154.40.36.22:1337 production，跑得好好的。

---

## 12. 用户偏好（从历次反馈累积）

- 不要回复贴截图（用户自己看自己用）
- 每轮收尾必须实际 build + 跑（cl.exe + cargo），失败修不要默默交付
- 改生产组件前必须问 + CHANGELOG 必记（AGENTS.md 守则）
- 关键字符串走 `CRYPT_STR("...")` 宏 + GetProcAddress 关键 WinAPI
- Google C++ Style + `m_` 前缀，无 RTTI / 无异常，错误用 `Result<T>`
- 注释只写 **why**，不写 what；不要"这里实现了优雅的 xxx"自夸
- 设计 token 严格对齐 `C:\Users\dwgx1\Downloads\Launcher\styles.css`（898 行）
- UI 弹动画 cubic-bezier(0.16, 1, 0.3, 1) 200ms，按钮 hover 150ms

---

## 13. 不许偷懒

- 不许"做一半 commit 一半"。每个 Step 是一个完整的 feature parity 单元，
  build 干净 + 视觉验证后才 commit。
- 不许 "TODO 留着下个 agent" — 要么做完，要么显式列在 SESSION_HANDOFF.md
  说明阻塞原因。
- 不许 "现在没时间所以用占位"。占位是 D2D ClearRenderTargetView，**全 UI 移植**
  必须真完成，不要临时降级到 GDI+。
- 不许跳过 brushcache / textcache 性能优化 — 一次性写好，后续每个 view 复用。
- 不许把字体回到 GDI/UnitPoint。**统一 DirectWrite + DIP**。

完成的标志：用户在 1100×720 Main view 里点击各种 modal、滚动 chat、切换
view、拖动窗口，**整个体验比当前 D2D PoC spinner 还顺**（因为 PoC 渲染量
很小）。
