# 图片管线（异步解码 + 下载池 + 后端缩略图，跨端）

本页把 2026-07 的**图片管线优化**三波（Wave 1/2/3）在一页里讲清。三波是一条端到端链路：
客户端把 WIC 解码搬离 paint 线程（Wave 1）、把网络下载搬进有界后台池（Wave 2），后端在**上传冷路径**
一次性生成多档缩略图 + BlurHash 占位串、读取热路径按需返回变体（Wave 3）。所有客户端代码在
**出货客户端** `tools/preview-d2d/`，后端在 `SystemBackend/crates/api`。

!!! info "背景：慢从哪来"
    诊断结论是「打开频道时同步解码全历史图片」——每条含图消息在 measure/paint 时都在 paint 线程同步走一遍
    WIC 解码，历史越长卡顿越久。三波分别拆掉「解码占用 UI 线程」「下载占用 UI 线程」「传输/解码的原图体积」。

!!! note "已部署 / 待办口径"
    Wave 3 后端（缩略图 / BlurHash / 变体服务 / ETag+304）**已部署到生产**（迁移 0019 已登记）。仍待办：
    客户端 BlurHash **真实解码**（`image_cache.h:193` 的 `decodeBlurhash` 现为返回 `nullptr` 的 stub）、
    旧图 **backfill**（迁移 0019 只加列，回填需单独后台任务，见 §3.4）。

---

## Wave 1：客户端异步解码（把 WIC 搬离 paint 线程）

### 1.1 DecodeService —— 进程级后台解码单例

`tools/preview-d2d/render/decode_worker.{h,cpp}` 提供 `DecodeService`（进程级单例，`decode_worker.h:102`），
持 1~2 个 `std::thread`（`decode_worker.h:89`）。每个 worker 自己 `CoInitializeEx(MTA)` + 持**独立的**
`IWICImagingFactory`，全部解码链路 off-thread（`decode_worker.h:1-13` 头注）：

```
CreateDecoderFromFilename → GetFrame(0) → 读 intrinsic GetSize()
  → IWICBitmapScaler->Initialize(frame, scaledW, scaledH, Fant)   （targetPx 长边，绝不放大）
  → IWICFormatConverter->Initialize(scaler, PBGRA)
  → CreateBitmapFromSource(conv, WICBitmapCacheOnLoad) => ComPtr<IWICBitmap> CPU 位图
```

完成的 `DecodeResult{kind,key,ok,iw,ih,frames}` 推入 `done_` 队列（mutex 保护，`decode_worker.h:94-95`），
随后 `PostMessageW(kMsgDecodeReady)`（`= WM_APP + 66`，`decode_worker.h:36`）通知 UI 线程消息泵。

!!! warning "线程约束：GPU 上传必须留在 UI 线程"
    `ID2D1DeviceContext` 是单线程的——`CreateBitmapFromWicBitmap`（GPU 上传）**不能**在 worker 里做，只能在
    UI 线程 `drainCompleted()` 里做（`decode_worker.h:11-13`）。WIC 内存位图是 agile 的，`WICBitmapCacheOnLoad`
    已把像素完全物化，因此跨线程交接 `IWICBitmap` 安全。

缓存键 `DecodeKey{path, targetPx}`（`decode_worker.h:41-47`）——同一文件的**头像尺寸**与**全览尺寸**是不同
key，可并存。`targetPx=0` 表示原生尺寸不缩放。

### 1.2 3 态非阻塞缓存 —— ImageCache / GifCache

`render/image_cache.h` 与 `render/gif_cache.h` 是平级、同套机制的缓存，Entry 有三态
`Pending / Ready / Failed`（`image_cache.h:174`）：

- `fromFile(path, targetPx, blurhash, out_opacity)`（`image_cache.h:64`）**不再在 paint 线程同步解码**：
  - 命中 `Ready` → 返回 `ID2D1Bitmap*` + touch LRU；
  - `Pending` / `Failed` → 返回 `nullptr`（本帧画占位）；
  - miss → 插 `Pending` 占位 + `decodeService().enqueue(...)` + 返回 `nullptr`（下帧淡入）。
- `drainCompleted()`（`image_cache.h:98`）是**唯一**创建 `ID2D1Bitmap` 的地方（UI 线程收到 `kMsgDecodeReady`
  后调，做 GPU 上传并翻 `Ready`），并回填 `intrinsic_[path]`（原始尺寸，跨逐出保留）。
- 图片缓存字节预算 `kImageCacheBudget = 192MB`（`image_cache.h:34`），GIF 帧预算独立
  `kGifCacheBudget = 96MB`（`gif_cache.h:35`）。

### 1.3 字节预算 LRU

`render/lru.h` 是 ImageCache / GifCache 共用的**字节预算 LRU**模板：`std::list<Node>` 维护 MRU→LRU 顺序 +
`unordered_map<Key, iterator>` 做 O(1) 命中/移动（`lru.h:120-123`）。每个节点带 `bytes`（缩放后 PBGRA =
`w*h*4`）。

!!! danger "逐出只在 paint 之前，避免悬垂位图"
    `evictToBudget()`（`lru.h:72`）**只**在 `drainCompleted()` 末尾（UI 线程、paint 之前）调用。若在 paint
    期间逐出，会把本帧正被 `DrawBitmap` 引用的 `ID2D1Bitmap` 释放成悬垂指针（`lru.h:5-7`、`image_cache.h:131`）。
    另外至少保留 1 个节点，避免刚插入项被自己逐出（`lru.h:73`）。

### 1.4 intrinsicSize —— 免解码测量，杀掉「开频道同步解全史」

关键杀手锏：`intrinsicSize(path)`（`image_cache.h:90`）返回已知的原始尺寸而**绝不触发解码**。
measure pass（`measureBubbleHeight`，`chat_paint.cpp:408,420-421`）与 paint 布局（`chat_paint.cpp:632-635`）
都用它拿尺寸定气泡高度——**不再为测量而解码全历史**。未知尺寸时用默认框，图解码完成后
`drainCompleted()` 填 `intrinsic_`，下帧自然校正。

### 1.5 淡入、精准逐出、BlurHash 缝

- **120ms 淡入**：Entry 记 `ready_ms`，`fromFile` 回填 `out_opacity`，`Ready` 后 ~120ms 线性斜坡
  （`image_cache.h:182-188`）。
- **`evict(path)` 取代 `invalidate()` 全清**：头像云同步刷新只逐出**单个路径**的全部 targetPx 变体
  （`image_cache.h:53-57`，`lru.h:eraseIf`），连 intrinsic 一并丢以便同名覆盖后重读。`invalidate()` 全清只
  留给 device-lost / logout（`image_cache.h:46-50`）。
- **BlurHash 缝（Wave 3 seam）**：Entry 带 `blurhash` 字符串 + `blur_bmp` + `decodeBlurhash()` stub
  （`image_cache.h:178-179,193-195`）——现返回 `nullptr`，调用点已预留，Wave 3 填 BlurHash→RGB→ID2D1Bitmap
  后无需重新布线。

---

## Wave 2：客户端下载池（把网络搬离 UI 线程）

### 2.1 DownloadPool —— 固定线程数有界池

`tools/preview-d2d/download_pool.{h,cpp}`：`DownloadPool` 单例（`download_pool.h:24`），默认 **4 个 worker**
（`start(int n = 4)`，`download_pool.h:27`），消费一个作业队列。核心是**以 `local_path` 去重合并**
（`download_pool.h:2-5`）：同一 `local_path` 的重复 `enqueue` 不再起第二次下载，只把新的 `(notify,msg)`
追加为等待方挂到在途作业上，下载完成后一次性 `PostMessage` 通知所有等待方（`download_pool.h:32-36`）。

worker 复用 `fetch::downloadMediaToPath`（自带磁盘缓存短路，见 `fetch.cpp:905`，`download_pool.h:2-3`）——
本地已有缓存直接短路，不发网络请求。

### 2.2 net.h 连接复用（keep-alive）

`tools/preview-d2d/net.h` 走 WinHTTP，做两级复用：`sharedSession()` 提高每服务器最大并发连接数让 keep-alive
池不过早串行化（`net.h:363-371`），并按 `host:port` 用 `thread_local` 缓存 `WinHttpConnect` 句柄
（`net.h:387-388`），避免每请求新建/关闭连接破坏 keep-alive。连接断了（send/recv 失败）丢弃缓存句柄重连
再试一次（`net.h:435`），请求结束只关请求句柄、连接句柄留缓存（`net.h:481`）。

### 2.3 头像磁盘缓存

头像走磁盘缓存 + `evict(path)` 精准刷新（见 §1.5）：云同步头像更新时只逐出该路径的内存变体并重下，不清空
整个图片缓存。

---

## Wave 3：后端缩略图 + BlurHash + 变体服务

### 3.1 生成（上传冷路径，`media_thumb.rs`）

`SystemBackend/crates/api/src/media_thumb.rs`：上传是冷路径、读取是热路径——在上传时一次性生成多档缩略图 +
BlurHash 占位串（`media_thumb.rs:1-15` 头注）。`generate()`（`media_thumb.rs:77`）整块放进
`tokio::task::spawn_blocking`（`media_thumb.rs:83`），避免 CPU 密集的解码/缩放/重编码占用 async worker。

- **档位**：媒体 `MEDIA_SLOTS = 64/128/256/400/512`，头像 `AVATAR_SLOTS = 64/128`（`media_thumb.rs:35-37`）。
- **downscale-only**：档位 ≥ 原图最长边则跳过，绝不放大（`media_thumb.rs:139-141`）。缩放用
  `FilterType::Lanczos3`（`media_thumb.rs:143`）。
- **编码**：带 alpha 的图 → PNG，照片 → JPEG **q≈82**（`encode_variant`，`media_thumb.rs:158-172`）。
  WIC 恒能解 JPEG/PNG，避免客户端 codec 依赖。
- **兄弟文件命名**从 sha 派生：`<stem>_s<slot>.<ext>`（`variant_filename`，`media_thumb.rs:51-53`），与原图同目录。
- **BlurHash**：4×3 分量（`BLURHASH_X/Y`，`media_thumb.rs:28-29`），先缩到最长边 64px 再逐像素编码
  （`compute_blurhash`，`media_thumb.rs:174-186`）；失败不致命，返回空串降级。

!!! danger "解压炸弹防护（三重）"
    `decode_guarded`（`media_thumb.rs:90-108`）解码前用 `image::Limits` 限最大边 **12000px**（`MAX_EDGE`）+
    分配上限 **256MB**（`MAX_ALLOC_BYTES`），解码后再兜底校验像素总数 **≤64MP**（`MAX_PIXELS`，
    `media_thumb.rs:22-26,104`）。任一超限或解码失败——**不 panic**，返回 `None`，调用方只存原图
    （`has_thumbs=false`）。

### 3.2 变体服务（读取热路径，`media.rs`）

`GET /api/media/:sha/:name?s=<px>` 解析变体档位：`resolve_slot(requested)`（`media_thumb.rs:65-67`）把请求尺寸
对齐到白名单里 ≥ 请求值的最小档；超出最大档或档文件缺失（旧上传）**回退原图**（`media.rs:236-254`）。
白名单拒绝任意尺寸，防 resize-DoS。

### 3.3 ETag / 304 / immutable 缓存

内容寻址 ⇒ 不可变。强 ETag = `"<core>"`，变体带后缀 `<sha>_s<slot>` 避免不同档共用缓存
（`media.rs:233-257`）。命中 `If-None-Match` 直接返回 **304 Not Modified**（`media.rs:258-273`），响应头
`Cache-Control: public, max-age=31536000, immutable`（`media.rs:266-267,287`）。

### 3.4 迁移 0019 与 backfill

`SystemBackend/migrations/0019_media_thumbs.sql` 对 `media_files` 加两列（全 additive、`IF NOT EXISTS`、
对旧行无破坏）：

- `blurhash TEXT` —— 上传时算出的 BlurHash 串；
- `has_thumbs BOOLEAN NOT NULL DEFAULT FALSE` —— 是否已生成兄弟缩略图文件。

上传落库时写这两列（`media.rs:138-173`）。旧行 `blurhash=NULL`、`has_thumbs=false`，下载走原图回退直到
backfill 补齐。**迁移不自动执行 backfill**（`0019:13-24` 注释）——需单独后台任务遍历
`mime LIKE 'image/%' AND has_thumbs=false` 的行，读原图 → `media_thumb::generate` → `UPDATE`。数据模型侧见
[数据模型 §3](../data/data-model.md)。

---

## 关键文件索引

| 波 | 端 | 文件 |
|---|---|---|
| Wave 1 | 客户端 | `render/decode_worker.{h,cpp}`、`render/lru.h`、`render/image_cache.h`、`render/gif_cache.h`、`chat_paint.cpp`（measure/paint 用 `intrinsicSize`） |
| Wave 2 | 客户端 | `download_pool.{h,cpp}`、`net.h`、`fetch.cpp`（磁盘缓存短路） |
| Wave 3 | 后端 | `crates/api/src/media_thumb.rs`、`crates/api/src/media.rs`、`migrations/0019_media_thumbs.sql` |

**未验证 / 待办**：客户端 BlurHash 真实解码（`decodeBlurhash` 仍是 stub）；旧图 backfill 任务尚未实现；
GIF 缩放解码在 worker 内逐帧完成、较重（`gif_cache.h:4-5`）。
