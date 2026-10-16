;; Minimal EPUB reader for the domain core — the v1 epub.ts contract:
;; container → OPF (title, manifest, spine) → TOC (EPUB3 nav, NCX fallback) →
;; sanitized chapter HTML (scripts/styles stripped, in-book images inlined as
;; data: URLs, dead images removed) → plain chapter text for the AI workflow
;; and whole-book search. Rendering belongs to the hosts (WKWebView).
#lang racket/base

(require file/unzip
         net/base64
         racket/bytes
         racket/dict
         racket/format
         racket/list
         racket/match
         racket/port
         racket/string
         (only-in xml read-xml document-element element? element-name element-attributes element-content
                  attribute? attribute-name attribute-value pcdata? pcdata-string
                  cdata? cdata-string entity? entity-text))

(provide (struct-out exn:fail:epub)
         (struct-out epub-book)
         (struct-out epub-toc-item)
         open-epub
         epub-chapter-html
         epub-chapter-text
         epub-doc-text)

(struct exn:fail:epub exn:fail ())
(struct epub-book (title spine toc entries) #:transparent) ; spine: paths, toc: items, entries: name->bytes hash
(struct epub-toc-item (title chapter level) #:transparent)

(define (epub-error msg) (raise (exn:fail:epub msg (current-continuation-marks))))

;; ------------------------------------------------------------ zip access

(define (read-entries bytes)
  (define entries (make-hash)) ; string name -> bytes
  (define in (open-input-bytes bytes))
  (unzip in
         (lambda (name flags port (extra #f))
           (hash-set! entries (string-trim (bytes->string/utf-8 name))
                      (port->bytes port))))
  entries)

(define (entry-bytes book path)
  (or (hash-ref (epub-book-entries book) path #f)
      ;; some epubs ship unencoded hrefs; fall back to a suffix match (v1)
      (let ((suffix (last (string-split path "/"))))
        (for/first (((k v) (in-hash (epub-book-entries book)))
                    #:when (string-suffix? k suffix))
          v))
      (epub-error (string-append "EPUB 缺少文件：" path))))

;; ------------------------------------------------------------ xml helpers

;; Elements as sxml: (name (@ (k "v") ...) child ...). Match on the local
;; name (after any namespace prefix), like v1's localName comparisons.
(define (el-local sym)
  (let ((s (symbol->string sym)))
    (string->symbol (last (string-split s ":")))))

;; read-xml document structures — no shape guessing
(define (el? v) (element? v))
(define (el-local? v local)
  (and (element? v) (eq? (el-local (element-name v)) local)))

(define (xml-kids el)
  (filter element? (element-content el)))

(define (xml-attr el name)
  (for/first ((a (in-list (element-attributes el)))
              #:when (eq? (el-local (attribute-name a)) name))
    (attribute-value a)))

(define (xml-find el local)
  (let loop ((stack (list el)))
    (cond
      ((null? stack) #f)
      ((el-local? (car stack) local) (car stack))
      (else (loop (append (xml-kids (car stack)) (cdr stack)))))))

(define (xml-find-all el local)
  (append
   (if (el-local? el local) (list el) '())
   (append* (map (λ (c) (xml-find-all c local)) (xml-kids el)))))

(define (xml-text el)
  (string-append*
   (map (λ (c)
          (cond
            ((pcdata? c) (pcdata-string c))
            ((cdata? c) (cdata-string c))
            ((entity? c) (or (entity-text c) ""))
            ((element? c) (xml-text c))
            (else "")))
        (element-content el))))

(define (parse-xml-string s what)
  (with-handlers ((exn:fail? (λ (e) (epub-error (string-append "EPUB 解析失败：" what)))))
    (document-element (read-xml (open-input-string s)))))

;; ------------------------------------------------------------ path helpers

(define (dir-of path)
  (define parts (string-split path "/"))
  (if (<= (length parts) 1)
      ""
      (string-join (drop-right parts 1) "/")))

(define (base-name path)
  (last (string-split path "/")))

(define (strip-anchor href)
  (car (string-split href "#")))

;; zip-relative join of opf-dir + href (handles ./ and ../, v1 joinZip)
(define (zip-join dir href)
  (define combined
    (if (string=? dir "")
        href
        (string-append dir "/" href)))
  (define parts
    (for/list ((p (in-list (string-split combined "/"))) #:unless (member p (list "" ".")))
      p))
  (let loop ((acc '()) (rest parts))
    (cond
      ((null? rest) (string-join (reverse acc) "/"))
      ((string=? (car rest) "..")
       (loop (if (null? acc) '() (cdr acc)) (cdr rest)))
      (else (loop (cons (car rest) acc) (cdr rest))))))

;; ------------------------------------------------------------ open

(define (open-epub data (fallback-title "EPUB"))
  (unless (and (bytes? data) (>= (bytes-length data) 4)
               (equal? (subbytes data 0 4) #"PK\x03\x04"))
    (epub-error "不是有效的 EPUB 文件"))
  (define entries (read-entries data))
  (define book* (epub-book fallback-title '() '() entries))
  (define container
    (entry-bytes book* "META-INF/container.xml"))
  (define container-sxml (parse-xml-string (bytes->string/utf-8 container) "container.xml"))
  (define rootfile (xml-find container-sxml 'rootfile))
  (define opf-path
    (or (and rootfile (xml-attr rootfile 'full-path)) "content.opf"))
  (define opf-dir (dir-of opf-path))
  (define opf-sxml
    (parse-xml-string (bytes->string/utf-8 (entry-bytes book* opf-path)) "content.opf"))

  (define title-el (xml-find opf-sxml 'title))
  (define title
    (let ((t (if title-el (string-trim (xml-text title-el)) "")))
      (if (string=? t "") fallback-title t)))

  ;; manifest: id -> (href media-type properties)
  (define manifest (make-hash))
  (for ((item (in-list (xml-find-all opf-sxml 'item))))
    (hash-set! manifest (or (xml-attr item 'id) "")
               (list (or (xml-attr item 'href) "")
                     (or (xml-attr item 'media-type) "")
                     (or (xml-attr item 'properties) ""))))

  ;; spine: idref order, html-ish media types only (v1)
  (define spine
    (for/list ((ref (in-list (xml-find-all opf-sxml 'itemref))))
      (define item (hash-ref manifest (or (xml-attr ref 'idref) "") #f))
      (and item
           (let ((mt (list-ref item 1)))
             (and (or (string=? mt "application/xhtml+xml") (string=? mt "text/html"))
                  (zip-join opf-dir (list-ref item 0))))))
    )
  (define spine-paths (filter string? spine))

  (define book (epub-book title spine-paths '() entries))

  ;; TOC: EPUB3 nav first, NCX fallback (v1 order)
  (define nav-item
    (for/first (((k v) (in-hash manifest)) #:when (string-contains? (list-ref v 2) "nav"))
      v))
  (define toc
    (or (and nav-item
             (parse-nav book (zip-join opf-dir (list-ref nav-item 0))))
        (let ((ncx-item
               (for/first (((k v) (in-hash manifest))
                           #:when (string=? (list-ref v 1) "application/x-dtbncx+xml"))
                 v)))
          (and ncx-item
               (parse-ncx book (zip-join opf-dir (list-ref ncx-item 0)))))
        '()))

  (struct-copy epub-book book (toc toc)))

(define (spine-index-of book zip-path)
  (define name (base-name (strip-anchor zip-path)))
  (for/first ((p (in-list (epub-book-spine book))) (i (in-naturals 1))
              #:when (string-suffix? p name))
    i))

(define (parse-nav book nav-path)
  (define sxml (parse-xml-string (bytes->string/utf-8 (entry-bytes book nav-path)) "nav.xhtml"))
  (define nav (xml-find sxml 'nav))
  (and nav
       (let loop ((el nav) (level 0))
         (append* (map (λ (c)
                         (cond
                           ((el-local? c 'ol) (loop c level))
                           ((el-local? c 'li)
                            (define a (for/first ((k (in-list (xml-kids c)))
                                                  #:when (el-local? k 'a))
                                        k))
                            (define nested (for/first ((k (in-list (xml-kids c)))
                                                       #:when (el-local? k 'ol))
                                             k))
                            (append
                             (if a
                                 (let ((idx (spine-index-of book
                                                            (zip-join (dir-of nav-path)
                                                                      (or (xml-attr a 'href) "")))))
                                   (if idx
                                       (list (epub-toc-item (string-trim (xml-text a)) idx level))
                                       '()))
                                 '())
                             (if nested (loop nested (add1 level)) '())))
                           (else '())))
                       (xml-kids el))))))

(define (parse-ncx book ncx-path)
  (define sxml (parse-xml-string (bytes->string/utf-8 (entry-bytes book ncx-path)) "toc.ncx"))
  (define navmap (xml-find sxml 'navMap))
  (if (not navmap)
      '()
      (let walk ((el navmap) (level 0))
        (append* (map (λ (point)
                        (define label (for/first ((c (in-list (xml-kids point)))
                                                  #:when (el-local? c 'navLabel))
                                         c))
                        (define content (for/first ((c (in-list (xml-kids point)))
                                                    #:when (el-local? c 'content))
                                          c))
                        (define title
                          (if label (string-trim (xml-text label)) ""))
                        (define src
                          (if content (or (xml-attr content 'src) "") ""))
                        (define idx (spine-index-of book (zip-join (dir-of ncx-path) src)))
                        (append
                         (if (and (non-empty-string? title) idx)
                             (list (epub-toc-item title idx level))
                             '())
                         (walk point (add1 level)))
                        (filter (λ (c) (el-local? c 'navPoint))
                                (xml-kids point))))))))

;; ------------------------------------------------------------ sanitize

(define image-mimes
  '((".png" . "image/png") (".jpg" . "image/jpeg") (".jpeg" . "image/jpeg")
    (".gif" . "image/gif") (".svg" . "image/svg+xml") (".webp" . "image/webp")))

(define (mime-for path)
  (for/first (((ext mime) (in-dict image-mimes))
              #:when (string-suffix? (string-downcase path) ext))
    mime))

(define (base64-encode-bs bs)
  (bytes->string/latin-1 (base64-encode bs #"")))

(define (inline-images! book chapter-path html)
  ;; replace relative img src with data: URLs; drop <img> whose file is
  ;; missing (v1 parseChapter)
  (define dir (dir-of chapter-path))
  (regexp-replace* #px"<img\\b[^>]*>" html
                   (lambda (tag)
                     (define m (or (regexp-match #rx"src=\"([^\"]+)\"" tag)
                                   (regexp-match #rx"src='([^']+)'" tag)))
                     (define src (and m (cadr m)))
                     (cond
                       ((not src) "")
                       ((regexp-match? #px"^(https?:|data:|blob:)" src) tag)
                       (else
                        (define img-path (zip-join dir (strip-anchor src)))
                        (define bs (with-handlers ((exn:fail:epub? (λ (_) #f)))
                                     (entry-bytes book img-path)))
                        (cond
                          ((not bs) "")
                          (else
                           (define mime (or (mime-for img-path) "application/octet-stream"))
                           (regexp-replace (regexp-quote (car m))
                                           tag
                                           (string-append "src=\"data:" mime ";base64,"
                                                          (base64-encode-bs bs) "\"")))))))))

(define (sanitize-chapter book chapter-path raw)
  ;; strip scripts, stylesheets and style blocks (v1 parseChapter), then
  ;; inline images; keep the body content only
  (define s (bytes->string/utf-8 raw))
  (define body (or (regexp-match #rx"(?s:<body\\b[^>]*>(.*)</body>)" s)
                   (regexp-match #rx"(?s:<body[^>]*>(.*)$)" s)))
  (define inner0
    (if body
        (cadr body)
        s))
  ;; fallback captures past </body></html> — strip the tail
  (define inner
    (regexp-replace #px"(?is:</body>\\s*</html>\\s*$)" inner0 ""))
  (define no-scripts
    (regexp-replace* #px"(?is:<script\\b.*?</script>)" inner "")
    )
  (define no-styles
    (regexp-replace* #px"(?is:<style\\b.*?</style>)" no-scripts ""))
  (define no-links
    (regexp-replace* #px"(?is:<link\\b[^>]*>)" no-styles ""))
  (inline-images! book chapter-path no-links))

(define (chapter-html doc)
  (string-append
   "<!doctype html><html><head><meta charset=\"utf-8\"></head><body>"
   doc
   "</body></html>"))

;; chapter HTML body content, sanitized (host wraps with its reading CSS)
(define (epub-chapter-html book index)
  (unless (and (>= index 1) (<= index (length (epub-book-spine book))))
    (epub-error "章节序号超出范围"))
  (define path (list-ref (epub-book-spine book) (sub1 index)))
  (string->bytes/utf-8
   (sanitize-chapter book path (entry-bytes book path))))

;; ------------------------------------------------------------ text

(define (decode-entities s)
  (define table
    (list (cons "&amp;" "&") (cons "&lt;" "<") (cons "&gt;" ">")
          (cons "&quot;" "\"") (cons "&apos;" "'") (cons "&#39;" "'")
          (cons "&nbsp;" " ") (cons "&mdash;" "—") (cons "&ndash;" "–")
          (cons "&hellip;" "…") (cons "&ldquo;" "“") (cons "&rdquo;" "”")
          (cons "&lsquo;" "‘") (cons "&rsquo;" "’")))
  (for/fold ((acc s)) ((pair (in-list table)))
    (string-replace acc (car pair) (cdr pair))))

(define (strip-tags s)
  (decode-entities
   (regexp-replace* #px"<[^>]*>" s " ")))

(define (chapter-text book index)
  (define path (list-ref (epub-book-spine book) (sub1 index)))
  (define raw (bytes->string/utf-8 (entry-bytes book path)))
  (define body (or (regexp-match #rx"(?s:<body\\b[^>]*>(.*)</body>)" raw)
                   (regexp-match #rx"(?s:<body[^>]*>(.*)$)" raw)))
  (define inner (if body (cadr body) raw))
  (define inner-clean
    (regexp-replace #px"(?is:</body>\\s*</html>\\s*$)" inner ""))
  (define no-scripts (regexp-replace* #px"(?is:<script\\b.*?</script>)" inner-clean ""))
  (define no-styles (regexp-replace* #px"(?is:<style\\b.*?</style>)" no-scripts ""))
  (define no-blocks (regexp-replace* #px"(?is:</(p|div|h[1-6]|li|br)\\s*>)" no-styles "\n"))
  (define stripped (strip-tags no-blocks))
  ;; v1: collapse [ \t]+ runs, trim ends; keep line structure
  (string-trim
   (string-join
    (for/list ((line (in-list (string-split stripped "\n"))))
      (string-replace line #rx"[ \t]+" " "))
    "\n")))

(define (epub-chapter-text book index)
  (unless (and (>= index 1) (<= index (length (epub-book-spine book))))
    (epub-error "章节序号超出范围"))
  (chapter-text book index))

;; v1 getDocText: first N chapters, each capped, "--- 第 i 章 ---" separators
(define (epub-doc-text book max-chapters cap-chars)
  (define n (min (length (epub-book-spine book)) (max 1 max-chapters)))
  (define parts
    (let loop ((i 1) (used 0) (acc '()))
      (if (> i n)
          (reverse acc)
          (let ((text (chapter-text book i)))
            (if (non-empty-string? text)
                (let* ((slice (if (> (string-length text) 3000)
                                  (string-append (substring text 0 3000) "…")
                                  text))
                       (used2 (+ used (string-length slice))))
                  (if (>= used cap-chars)
                      (reverse acc)
                      (loop (add1 i) used2
                            (cons (format "--- 第 ~a 章 ---\n~a" i slice) acc))))
                (loop (add1 i) used acc))))))
  (string-join parts "\n\n"))
