;; Tests for the EPUB domain reader (racket/booklight/epub.rkt), run against
;; the committed fixture racket/tests/fixtures/sample.epub.
#lang racket/base

(require rackunit
         racket/file
         racket/runtime-path
         racket/string
         "../pdfgist/epub.rkt")

(define-runtime-path fixture-path "fixtures/sample.epub")
(define data (file->bytes fixture-path))

(test-case "open: title, spine order, TOC with nesting"
  (define book (open-epub data))
  (check-equal? (epub-book-title book) "The Sample Book")
  (check-equal? (epub-book-spine book)
                (list "OEBPS/text/ch1.xhtml" "OEBPS/text/ch2.xhtml"))
  (define toc (epub-book-toc book))
  (check-equal? (length toc) 3) ; nav declares the nested li twice (v1 does too)
  (check-equal? (epub-toc-item-title (car toc)) "First Chapter")
  (check-equal? (epub-toc-item-chapter (car toc)) 1)
  (check-equal? (epub-toc-item-level (caddr toc)) 1))

(test-case "chapter html: sanitized, images inlined as data URLs"
  (define html (bytes->string/utf-8 (epub-chapter-html (open-epub data) 1)))
  (check-false (string-contains? html "<script"))
  (check-false (string-contains? html "alert"))
  (check-true (string-contains? html "data:image/png;base64,"))
  (check-false (string-suffix? html "</body></html>")))

(test-case "chapter text: tags stripped, entities decoded"
  (define text (epub-chapter-text (open-epub data) 2))
  (check-true (string-contains? text "The quick brown fox"))
  (check-true (string-contains? text "A & B <tag>")))

(test-case "doc text: v1 separators and per-chapter cap"
  (define text (epub-doc-text (open-epub data) 12 24000))
  (check-true (string-contains? text "--- 第 1 章 ---"))
  (check-true (string-contains? text "--- 第 2 章 ---"))
  ;; max-chapters caps the range
  (define one (epub-doc-text (open-epub data) 1 24000))
  (check-false (string-contains? one "--- 第 2 章 ---")))

(test-case "invalid input rejected with a domain error"
  (check-exn exn:fail:epub? (λ () (open-epub #"not an epub")))
  (check-exn exn:fail:epub? (λ () (epub-chapter-html (open-epub data) 99))))
