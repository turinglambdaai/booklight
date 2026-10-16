#lang racket/base

;; URL normalization parity: the v1 (Tauri) llm.rs unit tests, ported
;; verbatim, plus the historical provider bug cases (GLM /api/paas/v4 and
;; Doubao /api/v3 must NOT get /v1 appended).

(require rackunit
         "../booklight/urlnorm.rkt")

;; llm.rs keeps_provider_version_segments
(check-equal? (normalize-base-url "https://api.deepseek.com/v1")
              "https://api.deepseek.com/v1")
(check-equal? (normalize-base-url "https://open.bigmodel.cn/api/coding/paas/v4")
              "https://open.bigmodel.cn/api/coding/paas/v4")
(check-equal? (normalize-base-url "https://open.bigmodel.cn/api/paas/v4/")
              "https://open.bigmodel.cn/api/paas/v4")
(check-equal? (normalize-base-url "https://ark.cn-beijing.volces.com/api/v3")
              "https://ark.cn-beijing.volces.com/api/v3")

;; Historical bug cases: versioned provider paths that don't end in /vN.
(check-equal? (normalize-base-url "https://open.bigmodel.cn/api/paas/v4")
              "https://open.bigmodel.cn/api/paas/v4")
(check-equal? (normalize-base-url "https://ark.cn-beijing.volces.com/api/v3/")
              "https://ark.cn-beijing.volces.com/api/v3")
(check-equal? (normalize-base-url "https://api.kimi.com/coding/v1")
              "https://api.kimi.com/coding/v1")
(check-equal? (normalize-base-url
               "https://dashscope.aliyuncs.com/compatible-mode/v1")
              "https://dashscope.aliyuncs.com/compatible-mode/v1")
(check-equal? (normalize-base-url "http://localhost:11434/v1")
              "http://localhost:11434/v1")

;; Multi-digit and uppercase version segments (Rust accepts v/V + digits).
(check-equal? (normalize-base-url "https://api.example.com/v10")
              "https://api.example.com/v10")
(check-equal? (normalize-base-url "https://api.example.com/V2")
              "https://api.example.com/V2")
(check-equal? (normalize-base-url "https://api.example.com/gateway/v2")
              "https://api.example.com/gateway/v2")

;; llm.rs appends_v1_when_missing
(check-equal? (normalize-base-url "https://api.example.com")
              "https://api.example.com/v1")
(check-equal? (normalize-base-url "https://api.example.com/gateway/")
              "https://api.example.com/gateway/v1")

;; Non-version trailing segments still get /v1.
(check-equal? (normalize-base-url "https://api.example.com/preview")
              "https://api.example.com/preview/v1")
(check-equal? (normalize-base-url "https://api.example.com/v")
              "https://api.example.com/v/v1")
(check-equal? (normalize-base-url "https://api.example.com/v2beta")
              "https://api.example.com/v2beta/v1")

;; llm.rs strips_full_chat_completions_path
(check-equal? (normalize-base-url "https://api.example.com/v1/chat/completions")
              "https://api.example.com/v1")
(check-equal? (normalize-base-url "https://api.example.com/v1/chat/completions/")
              "https://api.example.com/v1")
(check-equal? (normalize-base-url "https://api.example.com/chat/completions")
              "https://api.example.com/v1")

;; Whitespace is trimmed like Rust `input.trim()`.
(check-equal? (normalize-base-url "  https://api.example.com/v1  ")
              "https://api.example.com/v1")
(check-equal? (normalize-base-url " https://api.example.com ")
              "https://api.example.com/v1")

;; truncate_chars (Unicode scalar values, ellipsis appended).
(check-equal? (truncate-chars "short" 10) "short")
(check-equal? (truncate-chars "0123456789" 10) "0123456789")
(check-equal? (truncate-chars "0123456789x" 10) "0123456789…")
(check-equal? (truncate-chars "中文内容很长需要截断" 4) "中文内容…")
(check-equal? (truncate-chars "" 400) "")
