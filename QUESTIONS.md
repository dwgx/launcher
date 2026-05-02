# 遗留问题清单

> 跨会话：每次开工先扫一遍，能就地解的别拖到下一轮。

## 待用户回答

### Q1 项目最终名字
代号叫 Launcher（或 Helix？）。CMake target、可执行名 `Launcher.exe`、后端代号 `helix-server`、订阅文件后缀 `.helix`。
是统一改成 Launcher，还是保留客户端=Launcher、协议=Helix 的双名？影响：
- 二进制文件名
- 注册表路径前缀
- GitHub 仓库名
- README 标题

### Q2 GitHub 仓库
私库名我暂用 `launcher`，远端 `https://github.com/dwgx1337/launcher`。
需要在 GitHub 上**手动**创建空私库（我没有你的 GH token）。建议：
```bash
gh repo create dwgx1337/launcher --private --description "Launcher launcher" --source=. --push
```
执行后我再接 `git remote add origin` 和首次推送。

### Q3 CDN 选型
后端 `config.cdn_base = "https://cdn.example.com"` 占位。订阅 `.helix` 文件要放哪里？
- Cloudflare R2（你之前提到）→ 需要 R2 账号和 access key
- Backblaze B2 → 便宜但延迟稍差
- 自建（直接用服务器 nginx 出 /opt/systembackend/blobs/）→ 0 配置但带宽吃自己的

倾向：先自建（nginx 静态目录），等用户量起来再上 R2。

### Q4 VMProtect 授权
最终发布要加壳。VMProtect 你有授权吗？还是用开源替代（OLLVM、obfuscator-llvm）？
影响发布流程和 CMake post-build 步骤。

### Q5 字体下载源
中文 Source Han Sans CN 完整版 130MB，SubsetOTF/CN 版 18MB。我默认走 SubsetOTF。
如果需要拼音/生僻字支持，要切到完整版。

### Q6 主窗口尺寸 vs 加载小窗
Phase 1 是 200×200 加载窗，加载完后切到 Login 应该：
- 同窗口放大到 1100×720（GLFW 原地 resize）
- 还是关掉 200×200 重新开 1100×720？

后者动画更可控（fade out + new window fade in），前者实现更省事。我倾向后者，但首次启动多一点延迟。

### Q7 1Panel 集成深度
服务器装了 1Panel。需要 SystemBackend 走 1Panel 反代（HTTPS+域名）还是直接裸跑 1337？
裸跑省事但要自己折腾 TLS；1Panel 反代天然有 ACME。

### Q8 hook 引擎接口
`IHookEngine` 留了空壳。是要接你已有的 hook 引擎（minhook / Detours / 自研），还是从零写？
原型阶段先空着无所谓，但 Phase 6 启动游戏时需要。

## 我的待办（不需要你回答）

- [ ] Phase 1：编译验证 200×200 窗口 + 旋转 spinner（需要先跑 fetch-skia + fetch-clay + vcpkg install）
- [ ] Phase 2：自定义无边框窗口 + WM_NCHITTEST 拖拽
- [ ] 服务端 cargo build + systemctl enable
- [ ] 客户端登录页 UI
- [ ] proto 生成 C++ 源
