#lang racket/base

;; Annotations store: FNV path keys, sidecar paths, legacy bare-array files,
;; serde default application, and save validation.

(require json
         rackunit
         racket/file
         racket/string
         racket/path
         "../booklight/annotations.rkt"
         "../booklight/fnv.rkt"
         "../booklight/jsonw.rkt"
         "../booklight/settings.rkt")

(define temp-root (make-temporary-file "pdfgist-annotations-~a" 'directory))

(dynamic-wind
 void
 (lambda ()
   (parameterize ([current-config-dir temp-root])
     ;; ---- path computation ----
     ;; booklight appends ".booklight.json" to the full PDF path
     ;; ("paper.pdf" -> "paper.pdf.booklight.json"); the pdfgist name is
     ;; the legacy shape the loader still reads.
     (check-equal? (sidecar-storage-path "/docs/paper.pdf")
                   "/docs/paper.pdf.booklight.json")
     (check-equal? (legacy-sidecar-path "/docs/paper.pdf")
                   "/docs/paper.pdf.pdfgist.json")
     (define key-path (annotation-storage-path "/docs/paper.pdf"))
     (check-equal?
      (path->string key-path)
      (path->string
       (build-path temp-root
                   "annotations"
                   (string-append (fnv1a64-hex "/docs/paper.pdf") ".json"))))
     (check-equal? (path->string (annotations-dir-path))
                   (path->string (build-path temp-root "annotations")))

     ;; ---- missing file -> empty document ----
     (check-equal? (load-document-bytes "/docs/paper.pdf" #f)
                   (string->bytes/utf-8
                    "{\n  \"annotations\": [],\n  \"bookmarks\": []\n}"))

     ;; ---- legacy 0.8.x bare array is wrapped on load ----
     (make-directory* (annotations-dir-path))
     (define legacy-file (annotation-storage-path "/docs/legacy.pdf"))
     (call-with-output-file legacy-file
       (lambda (out)
         (display
          (string-append
           "[{\"id\":\"a1\",\"page\":2,\"rects\":[{\"x\":1.5,\"y\":2.5,"
           "\"width\":30,\"height\":10}],\"excerpt\":\"摘录\"}]")
          out))
       #:exists 'replace)
     (define legacy-loaded (load-document-bytes "/docs/legacy.pdf" #f))
     (define legacy-value (read-json (open-input-bytes legacy-loaded)))
     (check-true (hash? legacy-value))
     (check-equal? (length (hash-ref legacy-value 'annotations)) 1)
     (check-equal? (hash-ref legacy-value 'bookmarks) '())
     (define legacy-annotation (car (hash-ref legacy-value 'annotations)))
     ;; serde #[serde(default)] fields re-applied on load.
     (check-equal? (hash-ref legacy-annotation 'color) "yellow")
     (check-equal? (hash-ref legacy-annotation 'kind) "highlight")
     (check-equal? (hash-ref legacy-annotation 'note) "")
     (check-equal? (hash-ref legacy-annotation 'created) 0)
     (check-equal? (hash-ref legacy-annotation 'page) 2)
     ;; Rect floats survive normalization.
     (check-equal? (hash-ref (car (hash-ref legacy-annotation 'rects)) 'x) 1.5)

     ;; ---- annotation object defaults ----
     (define object-file (annotation-storage-path "/docs/defaults.pdf"))
     (call-with-output-file object-file
       (lambda (out) (display "{\"annotations\":[{}]}" out))
       #:exists 'replace)
     (define object-loaded (read-json (open-input-bytes (load-document-bytes "/docs/defaults.pdf" #f))))
     (define default-annotation (car (hash-ref object-loaded 'annotations)))
     (check-equal? (hash-ref default-annotation 'color) "yellow")
     (check-equal? (hash-ref default-annotation 'kind) "highlight")
     (check-equal? (hash-ref default-annotation 'page) 1)
     (check-equal? (hash-ref default-annotation 'id) "")
     (check-equal? (hash-ref default-annotation 'rects) '())
     (check-equal? (length (hash-ref object-loaded 'bookmarks)) 0)

     ;; Bookmark defaults.
     (call-with-output-file object-file
       (lambda (out)
         (display "{\"annotations\":[],\"bookmarks\":[{\"page\":4}]}" out))
       #:exists 'replace)
     (define with-bookmark (read-json (open-input-bytes (load-document-bytes "/docs/defaults.pdf" #f))))
     (define bookmark (car (hash-ref with-bookmark 'bookmarks)))
     (check-equal? (hash-ref bookmark 'page) 4)
     (check-equal? (hash-ref bookmark 'label) "")
     (check-equal? (hash-ref bookmark 'created) 0)

     ;; ---- store: validates and normalizes ----
     (store-document-bytes!
      "/docs/stored.pdf"
      #f
      (string->bytes/utf-8
       (string-append
        "{\"annotations\":[{\"id\":\"s1\",\"page\":1,\"rects\":[],"
        "\"excerpt\":\"文本\",\"color\":\"green\",\"kind\":\"underline\","
        "\"note\":\"笔记\",\"created\":99}],\"bookmarks\":[{\"page\":3}]}")))
     (define stored-file (annotation-storage-path "/docs/stored.pdf"))
     (check-true (file-exists? stored-file))
     (define stored (read-json (open-input-bytes (load-document-bytes "/docs/stored.pdf" #f))))
     (check-equal? (length (hash-ref stored 'annotations)) 1)
     (check-equal? (length (hash-ref stored 'bookmarks)) 1)

     ;; Missing bookmarks key is tolerated on save (serde default)...
     (store-document-bytes!
      "/docs/stored.pdf" #f (string->bytes/utf-8 "{\"annotations\":[]}"))
     (check-equal?
      (length (hash-ref (read-json (open-input-bytes (load-document-bytes "/docs/stored.pdf" #f))) 'bookmarks))
      0)

     ;; ...but a bare array (legacy load-only shape) is rejected on save.
     (check-exn
      (lambda (e) (regexp-match? #rx"批注数据格式不正确" (exn-message e)))
      (lambda ()
        (store-document-bytes!
         "/docs/stored.pdf" #f (string->bytes/utf-8 "[]"))))
     (check-exn
      (lambda (e) (regexp-match? #rx"批注文件解析失败" (exn-message e)))
      (lambda ()
        (store-document-bytes!
         "/docs/stored.pdf" #f (string->bytes/utf-8 "not json"))))
     (check-exn
      (lambda (e) (regexp-match? #rx"批注数据格式不正确" (exn-message e)))
      (lambda ()
        (store-document-bytes!
         "/docs/stored.pdf" #f (string->bytes/utf-8 "{\"annotations\":\"x\"}"))))

     ;; ---- sidecar mode ----
     (define pdf-path (build-path temp-root "sidecar-doc.pdf"))
     (define sidecar-pdf (path->string pdf-path))
     ;; a pre-booklight pdfgist sidecar is picked up transparently
     (define legacy-path (string->path (string-append sidecar-pdf ".pdfgist.json")))
     (call-with-output-file legacy-path
       (lambda (out)
         (display "{\"annotations\":[],\"bookmarks\":[]}" out))
       #:exists 'replace)
     (check-equal? (load-document-bytes sidecar-pdf #t)
                   (string->bytes/utf-8
                    "{\n  \"annotations\": [],\n  \"bookmarks\": []\n}"))
     ;; storing writes the booklight name…
     (store-document-bytes!
      sidecar-pdf #t (string->bytes/utf-8 "{\"annotations\":[],\"bookmarks\":[{\"page\":3}]}"))
     (check-true (file-exists? (path->string (string->path (string-append sidecar-pdf ".booklight.json")))))
     ;; …and from then on the new file wins
     (define reread (load-document-bytes sidecar-pdf #t))
     (check-true (string-contains? (bytes->string/utf-8 reread) "\"page\": 3"))
     (check-false (string-contains? (bytes->string/utf-8 reread) "\"id\":\"x\""))))
 (lambda ()
   (delete-directory/files temp-root)))
