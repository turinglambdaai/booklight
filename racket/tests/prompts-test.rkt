#lang racket/base

;; Prompt templates and text helpers: snapshot parity with v1 sidebar.ts
;; plus behavior tests for paragraph splitting and doc assembly.

(require rackunit
         racket/list
         racket/string
         "../booklight/prompts.rkt")

;; ---- translation ----

(check-equal?
 (translate-messages "Hello" "English")
 (list
  (cons "system"
        (string-append
         "你是专业的翻译引擎。将用户提交的内容翻译成English：术语准确，保持段落结构，"
         "代码、数学公式与引用标记原样保留。只输出译文，不要任何解释。"))
  (cons "user" "Hello")))

(check-equal? (page-translate-user-text 3 "内容")
              "以下是 PDF 第 3 页提取的文本：\n\n内容")

;; ---- truncation ----

(check-equal? (truncate-page-text (make-string 8000 #\a)) (make-string 8000 #\a))
(check-equal? (truncate-page-text (make-string 8001 #\a))
              (string-append (make-string 8000 #\a) "…（内容过长，已截断）"))

;; ---- summary ----

(check-equal?
 (summary-page-messages "文本" "中文")
 (list
  (cons "system"
        (string-append
         "你是文档阅读助手。用中文总结用户提交的 PDF 页面：先用一段话概括核心内容，"
         "再用要点列出关键信息。使用 Markdown 输出。"))
  (cons "user" "文本")))

(check-equal?
 (summary-selection-messages "文本" "日本語")
 (list
  (cons "system"
        (string-append
         "你是文档阅读助手。用日本語总结用户提交的文本：先用一句话概括，再列出要点。"
         "使用 Markdown 输出。"))
  (cons "user" "文本")))

(check-equal?
 (summary-doc-messages "文本" "English")
 (list
  (cons "system"
        (string-append
         "你是文档阅读助手。用English总结用户提交的整份 PDF：主题与背景、核心观点或结论、"
         "结构与各部分要点、值得注意的数据或方法。使用 Markdown 输出。"))
  (cons "user" "文本")))

(check-equal? (summary-doc-user-text 12 "分页文本")
              "以下是整份 PDF（前 12 页）的分页文本：\n\n分页文本")

;; ---- doc assembly (viewer.ts getDocText(12, 24000)) ----

(check-equal? (assemble-doc-text '("第一页" "第二页"))
              "--- 第 1 页 ---\n第一页\n\n--- 第 2 页 ---\n第二页")

;; Empty pages are skipped but their page numbers are not (getDocText loops
;; over the original 1-based page indices).
(check-equal? (assemble-doc-text '("有字" "" "  " "最后"))
              "--- 第 1 页 ---\n有字\n\n--- 第 4 页 ---\n最后")

;; Per-page cap appends the ellipsis marker.
(check-equal? (assemble-doc-text (list (make-string 3001 #\a)))
              (string-append "--- 第 1 页 ---\n" (make-string 3000 #\a) "…"))

;; Total cap stops assembly at 24000 chars: 8 pages x 3000.
(define full-pages (build-list 20 (lambda (_) (make-string 3000 #\b))))
(check-equal? (length (regexp-match* #rx"--- 第" (assemble-doc-text full-pages))) 8)

;; Max-pages cap: 12 one-thousand-char pages.
(define small-pages (build-list 20 (lambda (_) (make-string 1000 #\c))))
(check-equal? (length (regexp-match* #rx"--- 第" (assemble-doc-text small-pages))) 12)

(check-equal? (assemble-doc-text '()) "")

;; ---- chat ----

(check-equal?
 (chat-system-prompt "上下文" "第 2 页" "中文")
 (string-append
  "你是 PDF 阅读助手，用中文回答。仅依据提供的文档内容回答，"
  "内容不足以回答时明确说明。输出使用 Markdown。\n\n"
  "=== 文档内容（第 2 页）===\n上下文"))

(define history
  (for/list ([i (in-range 15)])
    (cons "user" (format "旧消息 ~a" i))))
(define chat (build-chat-messages "ctx" "选区" "中文" history "问题"))
(check-equal? (length chat) 14) ; system + last 12 + user
(check-equal? (car chat) (cons "system" (chat-system-prompt "ctx" "选区" "中文")))
(check-equal? (last chat) (cons "user" "问题"))
;; Keeps the LAST 12: the first 3 old messages are dropped.
(check-equal? (cadr chat) (cons "user" "旧消息 3"))
(check-equal? (list-ref chat 12) (cons "user" "旧消息 14"))

;; ---- splitParagraphs ----

;; CJK lines merge without a space.
(check-equal? (split-paragraphs "第一行\n第二行") '("第一行第二行"))
;; Latin word boundaries merge with a single space.
(check-equal? (split-paragraphs "Hello\nworld") '("Hello world"))
;; The joiner only fires for latin tails AND latin heads.
(check-equal? (split-paragraphs "Hello。\nworld") '("Hello。world"))
(check-equal? (split-paragraphs "Hello\n世界") '("Hello世界"))
;; Lines are trimmed and empties dropped; latin lines still merge.
(check-equal? (split-paragraphs "  a  \n\n b \n") '("a b"))
(check-equal? (split-paragraphs "") '())
;; Target length pushes finished paragraphs out (at line boundaries only,
;; exactly like the TS implementation).
(define long-lines
  (string-append (make-string 200 #\x) "\n"
                 (make-string 200 #\x) "\n"
                 (make-string 200 #\x)))
(define paragraphs (split-paragraphs long-lines))
(check-equal? (length paragraphs) 2)
;; The latin->latin joiner inserts a single space between the two x-lines.
(check-equal? (car paragraphs)
              (string-append (make-string 200 #\x) " " (make-string 200 #\x)))
(check-equal? (cadr paragraphs) (make-string 200 #\x))
