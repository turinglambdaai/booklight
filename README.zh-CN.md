# Booklight

> 跨平台的书架与阅读器——PDF / EPUB 沉浸阅读，AI 在手边。

[English](README.md) · **中文**

[![release](https://img.shields.io/github/v/release/turinglambdaai/booklight)](https://github.com/turinglambdaai/booklight/releases/latest) ![platform](https://img.shields.io/badge/platform-macOS%2014%2B%20%7C%20Windows%2010%2B%20%7C%20Linux-lightgrey) ![built with](https://img.shields.io/badge/built%20with-Rivet-blue) [![CI](https://github.com/turinglambdaai/booklight/actions/workflows/ci.yml/badge.svg)](https://github.com/turinglambdaai/booklight/actions/workflows/ci.yml) [![License](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

苹果做出了最好的阅读体验，然后把它锁进了自己的设备。Booklight 把这份安静带给每个桌面：一个属于你的书架、不打扰的阅读、随叫随到的 AI——书是你的文件，模型是你的模型，花销是你的花销。任何东西都不会被复制进应用私有库，除了你主动发给自家服务商的请求，没有任何数据离开你的机器。

## 书架

- 启动即书架——封面、收藏集、搜索、阅读进度
- 书以**原位编目**：导入只引用你的文件，从不复制；文件被移走时书架标记「丢失」，等你重新链接
- 阅读进度自动汇入书架；合上书回到书架
- 书架文件（`library.json`）是单一、有文档、格式稳定的文件，放进你的同步盘即可同步——见 [DATA-FORMAT](shared/spec/DATA-FORMAT.md)

## 阅读

- **PDF**——连续滚动、双页书页视图、缩放/适宽、缩略图、目录、全文搜索、分屏、键盘导航
- **EPUB**——衬线重排版式、章节目录、全书搜索
- 批注——三色高亮、下划线、删除线、笔记与书签，存在书的旁边，天然适配云盘同步
- 朗读、打印、深色模式、随处续读

## AI 在手边

- 划词翻译、页面/全书总结、基于当前页/选区/全书的对话
- 全程流式输出
- 自带模型——智谱 GLM（含 GLM Coding 计划端点）、DeepSeek、月之暗面 Kimi、阿里通义千问、豆包（火山方舟）、硅基流动、Ollama，或任意 OpenAI 兼容端点；模型列表按需拉取
- Key 只存你的配置目录，请求直达你的服务商

## 轻编辑（刻意克制）

- 一级「工具」菜单承载页面手术：旋转、插空白页、删页、提取、合并——全程内存操作，源文件不动
- AcroForm 表单填写与导出

## 在线更新

Ed25519 签名校验的自更新（签名清单 + SHA-256 下载校验）；至多每天静默检查一次，点过确认才安装——三平台一致。

## 架构

一份 Racket 领域核心（`racket/booklight/` + `app/backend.rkt`）承载全部业务逻辑——书架、AI 流式、PDF 页面手术、批注存储、设置——经 [Rivet](https://github.com/turinglambdaai/rivet) 的类型化 RPC（RVT1）暴露；各平台第一方原生宿主只做渲染与交互：

| 平台宿主 | 技术栈 | 目录 |
|---|---|---|
| macOS | SwiftUI + 生成客户端 | `macos-host/` |
| Windows | WinUI 3（C++/WinRT）+ PDFium + 生成客户端 | `windows/` |
| Linux | GTK4 + PDFium + WebKitGTK + 生成客户端 | `linux/` |

PDF 页级操作由领域核心自研的最小读写器（`racket/booklight/pdfdoc.rkt` + `pdfedit.rkt`）完成——不依赖第三方 PDF 库。数据存哪、怎么同步：[DATA-FORMAT](shared/spec/DATA-FORMAT.md)。

## 从源码构建

前置：[Racket](https://racket-lang.org) CS 9.x（以 link 方式装好 [Rivet](https://github.com/turinglambdaai/rivet)）+ 你所在平台的 Swift / WinRT / GTK 工具链。Windows 与 Linux 宿主经 [PDFium](https://pdfium.googlesource.com/pdfium/)（BSD-3）渲染：构建时拉取固定版本并做 SHA-256 校验的预编译产物——仓库不提交任何二进制。

```bash
raco rivet doctor --json   # 工具链自检
raco rivet build           # 生成三端客户端 + 构建后端与宿主
raco rivet dev             # 开发循环（改动重建）
raco test racket/          # 领域核心测试
```

## 路线图

- [x] 书架——收藏集、封面、搜索、阅读进度、文件丢失处理（三平台齐）
- [x] 阅读——PDF/EPUB、批注、AI 工作流、轻编辑、朗读、打印、分屏、表单（三平台齐）
- [ ] 阅读主题——纸张/夜间页面底色、排版控制
- [ ] 移动端——iOS / iPadOS 宿主（Rivet portable Swift 层）与 Android 宿主（Rivet Kotlin 平台），对齐家族全平台标准

## 许可证

[AGPL-3.0](LICENSE)
