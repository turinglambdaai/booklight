#lang racket/base

;; Hardcoded zh prompt templates, verbatim from the v1 (Tauri) sidebar. The
;; prompts stay Chinese-only on purpose (parity with the old UI); the target
;; language is interpolated into each template. Messages are (cons role
;; content) pairs, the core representation consumed by llm.rkt.

(require racket/string)

(provide translate-messages
         page-translate-user-text
         truncate-page-text
         page-truncate-chars
         summary-page-messages
         summary-selection-messages
         summary-doc-messages
         summary-doc-user-text
         assemble-doc-text
         doc-max-pages
         doc-per-page-chars
         doc-total-chars
         chat-system-prompt
         build-chat-messages
         chat-history-limit
         split-paragraphs
         bilingual-max-paragraphs)

;; ---- translation (sidebar.ts translatePrompt) ----

(define (translate-system-prompt lang)
  (string-append
   "你是专业的翻译引擎。将用户提交的内容翻译成" lang
   "：术语准确，保持段落结构，代码、数学公式与引用标记原样保留。只输出译文，不要任何解释。"))

(define (translate-messages text lang)
  (list (cons "system" (translate-system-prompt lang))
        (cons "user" text)))

;; sidebar.ts translatePage wraps the extracted page text before translating.
(define (page-translate-user-text page text)
  (format "以下是 PDF 第 ~a 页提取的文本：\n\n~a" page text))

;; ---- truncation ----

(define page-truncate-chars 8000)

;; sidebar.ts translatePage/summarizePage: cut at 8000 chars and mark it.
(define (truncate-page-text text)
  (if (> (string-length text) page-truncate-chars)
      (string-append (substring text 0 page-truncate-chars) "…（内容过长，已截断）")
      text))

;; ---- summary (sidebar.ts summarizePage / summarizeSelection / summarizeDoc) ----

(define (summary-page-messages text lang)
  (list (cons "system"
              (string-append
               "你是文档阅读助手。用" lang
               "总结用户提交的 PDF 页面：先用一段话概括核心内容，再用要点列出关键信息。使用 Markdown 输出。"))
        (cons "user" text)))

(define (summary-selection-messages text lang)
  (list (cons "system"
              (string-append
               "你是文档阅读助手。用" lang
               "总结用户提交的文本：先用一句话概括，再列出要点。使用 Markdown 输出。"))
        (cons "user" text)))

(define (summary-doc-messages text lang)
  (list (cons "system"
              (string-append
               "你是文档阅读助手。用" lang
               "总结用户提交的整份 PDF：主题与背景、核心观点或结论、结构与各部分要点、"
               "值得注意的数据或方法。使用 Markdown 输出。"))
        (cons "user" text)))

(define (summary-doc-user-text pages text)
  (format "以下是整份 PDF（前 ~a 页）的分页文本：\n\n~a" pages text))

;; ---- whole-document context (viewer.ts getDocText(12, 24000)) ----

(define doc-max-pages 12)
(define doc-per-page-chars 3000)
(define doc-total-chars 24000)

;; Hosts assemble page texts; this helper applies the exact old-stack shape:
;; trim each page, cap it at `doc-per-page-chars` chars (ellipsis-marked),
;; prefix a `--- 第 N 页 ---` marker, stop after `max-pages` pages or a total
;; of `total-chars` chars, and join with blank lines.
(define (assemble-doc-text page-texts
                           #:max-pages [max-pages doc-max-pages]
                           #:per-page [per-page doc-per-page-chars]
                           #:total [total-chars doc-total-chars])
  (let loop ([pages page-texts] [page-number 1] [used 0] [parts '()])
    (cond
      [(or (null? pages) (> page-number max-pages))
       (string-join (reverse parts) "\n\n")]
      [else
       (define text (string-trim (car pages)))
       (cond
         [(string=? text "")
          (loop (cdr pages) (add1 page-number) used parts)]
         [else
          (define slice
            (if (> (string-length text) per-page)
                (string-append (substring text 0 per-page) "…")
                text))
          (define part (format "--- 第 ~a 页 ---\n~a" page-number slice))
          (define new-used (+ used (string-length slice)))
          (if (>= new-used total-chars)
              (string-join (reverse (cons part parts)) "\n\n")
              (loop (cdr pages) (add1 page-number) new-used (cons part parts)))])])))

;; ---- chat (sidebar.ts sendChat) ----

(define chat-history-limit 12)

;; sidebar.ts: system prompt carries the document context with a label, and
;; only the last `chat-history-limit` history messages are kept.
(define (chat-system-prompt context label lang)
  (string-append
   "你是 PDF 阅读助手，用" lang "回答。仅依据提供的文档内容回答，"
   "内容不足以回答时明确说明。输出使用 Markdown。\n\n"
   "=== 文档内容（" label "）===\n"
   context))

;; history: list of (cons role content), oldest first. The reply request is
;; [system, ...last-12-history, user].
(define (build-chat-messages context label lang history user-message)
  (define trimmed-history
    (let ([count (length history)])
      (if (> count chat-history-limit)
          (list-tail history (- count chat-history-limit))
          history)))
  (append (list (cons "system" (chat-system-prompt context label lang)))
          trimmed-history
          (list (cons "user" user-message))))

;; ---- bilingual page translation (sidebar.ts splitParagraphs) ----

(define bilingual-max-paragraphs 12)

(define latin-tail-regexp #px"[A-Za-z0-9.,;:!?)]$")
(define latin-head-regexp #px"^[A-Za-z0-9(`\"']")

;; Exact port of splitParagraphs: trim lines, drop empties, accumulate into
;; paragraph-sized chunks; join with a single space exactly when the current
;; chunk ends and the next line starts with latin text characters. The
;; max-12-paragraph slice is the caller's policy (bilingual page view).
(define (split-paragraphs text [target-chars 400])
  (unless (string? text)
    (raise-argument-error 'split-paragraphs "string?" text))
  (define lines
    (filter (lambda (line) (> (string-length line) 0))
            (map string-trim (string-split text "\n"))))
  (let loop ([lines lines] [current ""] [acc '()])
    (cond
      [(null? lines)
       (reverse (if (> (string-length current) 0) (cons current acc) acc))]
      [else
       (define line (car lines))
       (define joiner
         (if (and (regexp-match? latin-tail-regexp current)
                  (regexp-match? latin-head-regexp line))
             " "
             ""))
       (define next (string-append current joiner line))
       (if (>= (string-length next) target-chars)
           (loop (cdr lines) "" (cons next acc))
           (loop (cdr lines) next acc))])))
