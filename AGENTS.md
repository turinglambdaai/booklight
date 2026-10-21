# AGENTS.md

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
raco test racket/          # 领域核心测试（341 项）
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
├── rivet.rktd          Rivet 应用清单（0.1.0，与 VERSION / app/version.rkt 对齐）
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
