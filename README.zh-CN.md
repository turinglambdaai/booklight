# PDFGist

> 本地优先的 AI PDF 阅读器——接入你自己的大模型。一份 Racket 领域核心，通过类型化 RPC 驱动各平台第一方原生宿主。

[English](README.md) · **中文**

[![release](https://img.shields.io/github/v/release/turinglambdaai/pdfgist)](https://github.com/turinglambdaai/pdfgist/releases/latest) ![platform](https://img.shields.io/badge/platform-macOS%2014%2B%20%7C%20Windows%2010%2B%20%7C%20Linux-lightgrey) ![built with](https://img.shields.io/badge/built%20with-Rivet-blue) [![CI](https://github.com/turinglambdaai/pdfgist/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/pdfgist/actions/workflows/ci.yml) [![License](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

大多数 AI PDF 应用把你锁进它们的订阅。PDFGist 反其道而行：应用本身只是一个轻快的阅读器，智能来自你自己指定的模型。你的 Key、你的模型、你的账单——Key 保存在本机，请求直达你选择的服务商，除此之外没有任何数据离开你的机器。

## 功能

- **PDF 阅读**——多标签、连续滚动、双页书视图、缩放/适宽、缩略图、目录、全文搜索、深色模式、打印、分屏、语音朗读、键盘导航
- **EPUB 阅读**——衬线流式排版、章节目录、书内全文搜索、字号调节、深色模式，同一套 AI 工作流作用于章节
- **页面编辑**——删除、旋转、插入空白页、提取所选页为新文件、合并文档、烘焙水印与文本框（全部在内存中进行，绝不改动你的源文件）
- **续读**——最近文件记住页码与滚动位置；文件始终留在你的文件夹和同步盘里，绝不复制进应用库
- **划词翻译**——选中任意文本，一键翻译，流式输出
- **段落对照**——当前页逐段翻译，原文/译文成对按阅读顺序展示，多段并行流式
- **页面与全文总结**——当前页、选区或整份文档的一键结构化总结
- **与文档对话**——基于当前页、选区或文档前若干页提问
- **表单填写**——填写文本/勾选/选择字段，导出填写后的 PDF
- **批注与书签**——三色高亮、笔记、书签，存放在 PDF 旁的 sidecar JSON 里，天然支持网盘同步
- **自带模型**——预置智谱 GLM（含 GLM Coding 套餐专属端点）、DeepSeek、Moonshot Kimi、通义千问、豆包（火山方舟）、硅基流动、Ollama，或任意 OpenAI 兼容 Base URL；模型列表按需拉取
- **在线更新**——签名校验的应用自更新（Ed25519 签名的发布清单 + SHA-256 校验下载）；每天最多静默检查一次，安装始终需你确认
- **本地优先**——设置与 API Key 保存在用户配置目录；无账号，无遥测

## 架构

一份 Racket 领域核心（`racket/` + `app/backend.rkt`）拥有全部业务逻辑——AI 流式、PDF 页级操作、批注存储、设置——通过 [Rivet](https://github.com/turinglambdaai/rivet) 类型化 RPC（RVT1）暴露；各平台第一方宿主只做渲染与交互：

| 宿主 | 技术栈 | 目录 |
|---|---|---|
| macOS | SwiftUI + 生成客户端 | `macos-host/` |
| Windows | WinUI 3（C++/WinRT）+ PDFium + 生成客户端 | `windows/` |
| Linux | GTK4 + PDFium + WebKitGTK + 生成客户端 | `linux/` |

macOS 宿主功能最全（批注、表单、打印、朗读、分屏）。Windows 宿主首发 P0 阅读层：PDFium 多标签阅读、划选翻译/总结/对话（流式）、WebView2 渲染 EPUB、页编辑、设置与签名自更新。Linux 宿主对齐同一 P0 层（GTK4）：PDFium 渲染（与 Windows 宿主同一 pin 的 chromium/8100 版本——bblanchon/pdfium-binaries 同一构建的各平台包，均做 SHA-256 校验）、WebKitGTK 渲染 EPUB、页编辑、AI 工作流、设置，以及适配三种 Linux 安装形态的签名自更新（deb/rpm 走包管理器、AppImage 原地换+重启、tar.gz 给解压指引）——批注高亮、打印、朗读、分屏、表单随后补齐（见路线图）。

PDF 页级操作由领域核心自研的最小 PDF 读写器（`racket/pdfgist/pdfdoc.rkt`）完成——不依赖任何第三方 PDF 库。

## 从源码构建

前置要求：[Racket](https://racket-lang.org) CS 9.x 并以 link 方式安装 [Rivet](https://github.com/turinglambdaai/rivet)，以及所在平台的 Swift / WinRT / GTK 工具链。Windows 宿主通过 [PDFium](https://pdfium.googlesource.com/pdfium/)（BSD-3）渲染 PDF：构建期从 bblanchon/pdfium-binaries 拉取 pin 死版本并做 SHA-256 校验的二进制——仓库里不提交任何二进制。

```bash
raco rivet doctor --json   # 工具链自检
raco rivet build           # 生成三端客户端 + 构建后端与宿主
raco rivet dev             # 开发循环（改动自动重建）
raco test racket/          # 领域核心测试
```

## 路线图

- [x] Windows 宿主——阅读 / AI / EPUB / 页编辑 / 更新（P0 层）
- [ ] Windows P2——批注高亮叠加、朗读、打印、分屏、表单填写
- [x] Linux 宿主——阅读 / AI / EPUB / 页编辑 / 更新（P0 层）
- [ ] Linux P2——批注高亮叠加、朗读、打印、分屏、表单填写
- [ ] 移动端 —— iOS / iPadOS 宿主（Rivet portable Swift 层）与 Android 宿主（Rivet Kotlin 平台），对齐家族全平台标准；watchOS 进度伴机尾随手机宿主

## 许可证

[AGPL-3.0](LICENSE)
