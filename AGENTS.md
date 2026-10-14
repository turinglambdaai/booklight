# AGENTS.md

指引给 AI agent（及开发者）：如何理解、构建、运行、改动 PDFGist。

> main 是 PDFGist 的开发主线，纯 Rivet 架构（Rivet 线自 2026-10-08 接替）。
> v1 是已归档的 Tauri 2 + PDF.js 实现，仅存于 git 历史；
> v1 的行为/文案语义已由领域核心承接。

## 这是什么

PDFGist 以 Rivet（github.com/turinglambdaai/rivet）构建：一份 Racket 领域核心，
通过 Rivet 类型化 RPC（RVT1 协议）驱动各平台第一方原生 UI 薄壳。

| 平台宿主 | 技术栈 | 目录 | 状态 |
|---|---|---|---|
| macOS | SwiftUI 宿主 + 生成客户端 | `macos-host/` | 可跑（阅读/AI/批注/表单/页编辑/打印/TTS/分屏全量） |
| Windows | WinUI 3（C++/WinRT）宿主 + PDFium + 生成客户端 | `windows/` | P0 阅读层可跑（阅读/AI/EPUB/页编辑/更新器）；P2 见 README 路线图 |
| Linux | GTK4 宿主 + 生成客户端 | `linux/` | 宿主骨架 + 生成客户端 |
| agent CLI | — | — | 不适用 |

## 快速命令

```bash
# 前置：Racket CS 9.x，rivet 以 link 方式安装
# cd ../rivet && raco pkg install --auto --no-docs --name rivet --link file://$PWD

raco rivet doctor --json   # 工具链自检
raco rivet build           # 生成三端客户端 + 编译后端 bundle + 宿主构建
raco rivet dev             # 开发循环
raco test racket/          # 领域核心测试（275 项）
```

## 契约（不要破坏）

- **数据路径与格式与 v1 完全一致**（drop-in 迁移）：settings.json / annotations/<fnv1a64(path)>.json 或 sidecar <file>.pdfgist.json，目录 macOS ~/Library/Application Support/site.jrtx.pdfgist 等
- **i18n 单源**：`shared/i18n/{zh,en}.json`，平台副本必须逐字节一致；zh 为默认
- **RPC 面**：`app/backend.rkt` 的 define-rpc 是宿主唯一数据通道；改签名 = 各端宿主 + 生成客户端同步改
- **宿主只做渲染与交互**：业务一律走 RPC；定时器/调度/HTTP/存储/PDF 手术都在 Racket 后端
- **PDF 页级操作走领域核心**：`racket/pdfgist/pdfdoc.rkt` 是自研最小读写器，不引第三方 PDF 库；编辑全在内存，宿主负责落盘
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
└── scripts/            gen-swift-strings（i18n → Swift）、installer art
```
