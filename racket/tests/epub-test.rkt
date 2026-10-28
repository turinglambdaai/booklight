;; Tests for the EPUB domain reader (racket/booklight/epub.rkt), run against
;; the committed fixture racket/tests/fixtures/sample.epub.
#lang racket/base

(require file/zip
         rackunit
         racket/file
         racket/runtime-path
         racket/string
         "../booklight/epub.rkt")

(define-runtime-path fixture-path "fixtures/sample.epub")
(define data (file->bytes fixture-path))

(define (make-zip-bytes entries)
  (define out (open-output-bytes))
  (zip->output
   (for/list ((entry (in-list entries)))
     (define name (car entry))
     (define payload (cdr entry))
     (zip-entry 'file name payload (bytes-length payload) #hash() 9))
   out
   #:timestamp 315532800)
  (get-output-bytes out))

(define (check-resource-limit thunk code limit minimum-actual)
  (check-exn
   (lambda (e)
     (and (exn:fail:epub:resource? e)
          (eq? (exn:fail:epub:resource-code e) code)
          (= (exn:fail:epub:resource-limit e) limit)
          (>= (exn:fail:epub:resource-actual e) minimum-actual)
          (string-prefix? (exn-message e)
                          (format "EPUB_RESOURCE_LIMIT[~a]:" code))))
   thunk))

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

(test-case "default resource budget leaves the normal fixture supported"
  (check-equal? (epub-resource-budget-archive-bytes default-epub-resource-budget)
                (* 256 1024 1024))
  (check-equal? (epub-resource-budget-entry-count default-epub-resource-budget)
                10000)
  (check-equal? (epub-resource-budget-entry-bytes default-epub-resource-budget)
                (* 64 1024 1024))
  (check-equal? (epub-resource-budget-expanded-bytes default-epub-resource-budget)
                (* 512 1024 1024))
  (check-equal? (epub-book-title (open-epub data)) "The Sample Book"))

(test-case "resource budget rejects an oversized compressed archive before unzip"
  (define limit (sub1 (bytes-length data)))
  (check-resource-limit
   (lambda ()
     (open-epub data #:budget (epub-resource-budget limit 100 1024 4096)))
   'archive-bytes limit (bytes-length data)))

(test-case "resource budget rejects a ZIP entry storm"
  (define archive
    (make-zip-bytes (list (cons "one" #"1")
                          (cons "two" #"2"))))
  (check-resource-limit
   (lambda ()
     (open-epub archive #:budget
                (epub-resource-budget (bytes-length archive) 1 1024 4096)))
   'entry-count 1 2))

(test-case "resource budget rejects one highly expanded entry"
  ;; Repeated bytes compress to a tiny archive, exercising the expanded-entry
  ;; path rather than the compressed archive limit.
  (define payload (make-bytes 4096 65))
  (define archive (make-zip-bytes (list (cons "bomb.bin" payload))))
  (check-true (< (bytes-length archive) (bytes-length payload)))
  (check-resource-limit
   (lambda ()
     (open-epub archive #:budget
                (epub-resource-budget (bytes-length archive) 10 1024 8192)))
   'entry-bytes 1024 1025))

(test-case "resource budget rejects aggregate expansion across entries"
  (define archive
    (make-zip-bytes (list (cons "one.bin" (make-bytes 700 65))
                          (cons "two.bin" (make-bytes 700 66)))))
  (check-resource-limit
   (lambda ()
     (open-epub archive #:budget
                (epub-resource-budget (bytes-length archive) 10 1024 1000)))
   'expanded-bytes 1000 1001))

(test-case "malformed ZIPs use a stable domain error"
  (check-exn
   (lambda (e)
     (and (exn:fail:epub? e)
          (not (exn:fail:epub:resource? e))
          (string=? (exn-message e)
                    "EPUB_INVALID_ARCHIVE: ZIP data is malformed or unsupported")))
   (lambda () (open-epub #"PK\x03\x04truncated"))))

(test-case "metadata: dc:creator becomes shelf author"
  (check-equal? (epub-author data) "Tester"))

(test-case "cover: EPUB 3 cover-image manifest item"
  (define cover (epub-cover-bytes data))
  (check-true (> (bytes-length cover) 4))
  (check-equal? (subbytes cover 1 4) #"PNG"))

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
