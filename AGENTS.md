# AGENTS.md

## Authorization decision before implementation

Decision `2026-10-auth-1`: Offline perpetual entitlement plus a bounded update/feature window; no activation lease or device cap. Read the complete [local authorization decision](PRICING.md#authorization-decision-for-implementation) and its Chinese counterpart before commercial, trial, update-eligibility or credential work. Record this decision revision and the source/policy SHAs actually read in the PR/handoff. Refresh at each bounded slice; existing agent conversations do not receive this change automatically. Follow the existing product contract and implementation issue; do not activate a payment gate merely because a plan exists.

指引给 AI agent（及开发者）：如何理解、构建、运行、改动 Booklight。

> main 是 Booklight 的开发主线，纯 Rivet 架构（Rivet 线自 2026-10-08 接替）。
> v1 是已归档的 Tauri 2 + PDF.js 实现，仅存于 git 历史；
> v1 的行为/文案语义已由领域核心承接。

## 这是什么

Booklight 以 Rivet（github.com/turinglambdaai/rivet）构建：一份 Racket 领域核心，
通过 Rivet 类型化 RPC（RVT1 协议）驱动各平台第一方原生 UI 薄壳。

| 平台宿主 | 技术栈 | 目录 | 状态 |
|---|---|---|---|
| macOS | SwiftUI 宿主 + 生成客户端 | `macos-host/` | 可跑（阅读/AI/批注/表单/页编辑/打印/TTS/分屏全量） |
| Windows | WinUI 3（C++/WinRT）宿主 + PDFium + 生成客户端 | `windows/` | 可跑（阅读/AI/批注/表单/页编辑/打印/TTS/分屏全量） |
| Linux | GTK4 宿主 + PDFium + 生成客户端 | `linux/` | 可跑（阅读/AI/批注/表单/页编辑/打印/TTS/分屏全量） |
| agent CLI | — | — | 不适用 |

## 快速命令

```bash
# 前置：Racket CS 9.x，rivet 以 link 方式安装
# cd ../rivet && raco pkg install --auto --no-docs --name rivet --link file://$PWD

raco rivet doctor --json   # 工具链自检
raco rivet schema check rivet-schema.json --json # RPC 兼容性门禁
raco rivet build           # 生成三端客户端 + 编译后端 bundle + 宿主构建
raco rivet dev             # 开发循环
raco test racket/          # 领域核心测试（项目测试）
raco test racket/tests/backend-test.rkt # 真实 RVT1 wire test
raco rivet acceptance --json # 当前平台的打包产品验收
```

## 完成定义

- `raco rivet acceptance --json` 是当前平台的产品完成门禁：后端契约、真实
  RVT1、原生宿主构建、可分发包和打包后原生 UI 操作必须全部通过。
- GUI 工作不能只凭代码审查、单元测试、构建成功或截图宣称完成。默认验收会
  用打包后的应用打开 `racket/tests/fixtures/sample.epub`，并确认后端解析出的
  `The Sample Book` 与 `First Chapter` 出现在原生可访问性树。
- 修改公共交互必须同步 macOS、Windows、Linux，或明确记录真实平台边界；
  不得把业务逻辑复制到单个平台绕开 RVT1。
- 关键控件的 accessibility / automation 标识属于测试契约。修改布局或文案时
  不得无意删除；新增关键流程时同步扩展 `scripts/e2e/`。
- 不得用 `continue-on-error`、空测试、跳过 UI 或强杀进程作为成功证据。

## 契约（不要破坏）

- **数据路径与格式与 v1 完全一致**（drop-in 迁移）：settings.json / annotations/<fnv1a64(path)>.json 或 sidecar <file>.booklight.json，目录 macOS ~/Library/Application Support/site.jrtx.booklight 等
- **i18n 单源**：`shared/i18n/{zh,en}.json`，平台副本必须逐字节一致；zh 为默认
- **RPC 面**：`app/backend.rkt` 的 define-rpc 是宿主唯一数据通道；改签名 = 各端宿主 + 生成客户端同步改
- **宿主只做渲染与交互**：业务一律走 RPC；定时器/调度/HTTP/存储/PDF 手术都在 Racket 后端
- **PDF 页级操作走领域核心**：`racket/booklight/pdfdoc.rkt` 是自研最小读写器，不引第三方 PDF 库；编辑全在内存，宿主负责落盘
- **Rivet 改动走上游**：缺能力先提 issue/PR 到 turinglambdaai/rivet

## 项目结构

```
├── rivet.rktd          Rivet 应用清单（与 VERSION / app/version.rkt 对齐）
├── rivet-schema.json   RPC 面基线
├── app/backend.rkt     Rivet 后端入口（装配领域层）
├── racket/             Racket 领域核心（含 pdfdoc PDF 读写器）+ tests/
├── shared/i18n/        zh.json / en.json 单源
├── macos-host/         SwiftUI 宿主
├── windows/            WinUI3 宿主
├── linux/              GTK4 宿主
├── design/             图标与品牌源文件
├── docs/               产品主页（GitHub Pages）
└── scripts/            gen-swift-strings / gen-win-strings / gen-linux-strings（i18n → 各宿主）、installer art
```

## 组合定位（Portfolio placement）

- 当前决策（2026-10-v2）：免费阅读器 + 研究工作台 Pro 买断。具体价格、免费/付费边界和开卖门禁以本仓 [PRICING.md](PRICING.md) 为唯一权威；旧版收费候选与早鸟方案已被替代。
- 组合归属：[PORTFOLIO](https://github.com/turinglambdaai/turinglambdaai.github.io/blob/master/PORTFOLIO.md)。当前全部产品开发中、没有实际用户；目标定价不表示已经开卖，不因此立即给开发版加收费墙。
- 免费产品不接商业激活；付费层先实现可验证价值，再接 Commerce/Permit。优先完成本仓既有产品审查 issue；不修改现行源码 LICENSE。

## 产品交付与下一步（2026-10-23）

- 家族[交付标准](https://github.com/turinglambdaai/turinglambdaai.github.io/blob/master/docs/PRODUCT-STANDARD.md)与[组合策略](https://github.com/turinglambdaai/turinglambdaai.github.io/blob/master/PORTFOLIO.md)是共用政策；本仓具体行为以 `README.md` 为准，不复制五库依赖图。
- 当前执行入口：[产品审查 issue](https://github.com/turinglambdaai/booklight/issues/20)。先验证批注/页编辑的保存、重开、导出和坏文件恢复，以及 AI 发送范围/取消/密钥存储。数据可迁移与原件保护先于移动端扩张。
- 开工先检查干净工作树与远端，以 fast-forward-only 更新并记录 SHA；共享文件一次一个写入者，保留其他 checkout 的未提交工作。
- 每次只完成一个可验收切片，记录 owned files、依赖 pin、命令/平台/产物证据和回滚。已实现、已发布、实机验收分别陈述；不修改 requirements 或复用旧证据来制造通过。

## Documentation languages

Public default documentation is English; maintain complete Chinese user paths in `*.zh-CN.md`, with reciprocal links. Update both languages when prices, quotas, installation, recovery, privacy or supported platforms change. API/protocol identifiers and commands remain unchanged; do not maintain a second normative protocol or implementation. Follow the [family language policy](https://github.com/turinglambdaai/turinglambdaai.github.io/blob/master/docs/DOCUMENTATION-LANGUAGES.md). Existing internal notes are not a claim that every document is bilingual.

## Public product documentation

The paired `docs/USER-GUIDE.md` / `docs/USER-GUIDE.zh-CN.md` own the public installation, first-use, data/recovery, privacy and availability path. Keep the homepage and both README entry points aligned with them when behavior changes. Detailed technical contracts and the current implementation queue retain their existing owners. Do not publish target prices, a purchase link or a ready/secure/platform claim before its release evidence exists.
