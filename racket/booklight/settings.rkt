#lang racket/base

;; Settings store, same file and shape as v1 (Tauri settings.rs).
;; JSON keys, same defaults (drop-in migration for v1 installs):
;;   <config-dir>/settings.json
;;     provider: {name, base_url, api_key, model}
;;     target_language: display name string (中文 by default)
;;     recent_files: [{path, title, page, scroll_ratio, last_read}] (<= 12)
;;     view_mode: "single" | "double"
;;     annotation_sidecar: bool
;;
;; The config dir is a parameter so tests can point at a temp directory.
;;
;; API key storage seam: the key stays inside settings.json for byte-compat
;; with v1 installs, but every read/write goes through `api-key-of` /
;; `set-api-key!`, so a secure-store backend (keychain, DPAPI) can replace
;; them without touching the rest of the stack.

(require json
         racket/file
         racket/path
         racket/port
         racket/string
         "i18n.rkt"
         "jsonw.rkt"
         "providers.rkt"
         "recents.rkt")

(provide current-config-dir
         config-dir-path
         settings-file-path
         migrate-legacy-config!
         pdfgist-settings
         pdfgist-settings?
         settings-provider
         settings-target-language
         settings-recent-files
         settings-view-mode
         settings-annotation-sidecar
         provider-config
         provider-config?
         provider-config-name
         provider-config-base-url
         provider-config-api-key
         provider-config-model
         default-settings
         load-settings
         save-settings!
         update-settings!
         api-key-of
         set-api-key!
         settings->storage
         storage->settings
         settings-file-exists?)

;; ---- config dir convention (Tauri app_config_dir parity) ----

(define app-identifier "site.jrtx.booklight")

(define (default-config-dir)
  (case (system-type)
    [(macosx)
     (build-path (find-system-path 'home-dir)
                 "Library" "Application Support" app-identifier)]
    [(windows)
     (define appdata (getenv "APPDATA"))
     (if (and appdata (non-empty-string? appdata))
         (build-path appdata app-identifier)
         (build-path (find-system-path 'home-dir)
                     "AppData" "Roaming" app-identifier))]
    [else
     (build-path (find-system-path 'home-dir) ".config" app-identifier)]))

(define current-config-dir (make-parameter (default-config-dir)))

;; The pre-booklight config dir (site.jrtx.pdfgist) holds settings,
;; annotations and recents for the same user. When the new dir does not
;; exist yet, copy the whole legacy dir over — one-time, non-destructive
;; (the old dir stays as a backup; the shelf re-imports books separately).
(define legacy-app-identifier "site.jrtx.pdfgist")

(define (migrate-legacy-config!)
  (define new-dir (current-config-dir))
  (if (directory-exists? new-dir)
      (void)
      (let ((parent (path-only new-dir)))
        (when parent
          (define old-dir (build-path parent legacy-app-identifier))
          (when (directory-exists? old-dir)
            (copy-directory/files old-dir new-dir))))))

(define (config-dir-path)
  (current-config-dir))

(define (settings-file-path)
  (build-path (current-config-dir) "settings.json"))

;; ---- model ----

(struct provider-config (name base-url api-key model) #:transparent)
(struct pdfgist-settings (provider target-language recent-files view-mode annotation-sidecar)
  #:transparent)

;; Short accessor aliases used across the stack and by the backend.
(define (settings-provider s) (pdfgist-settings-provider s))
(define (settings-target-language s) (pdfgist-settings-target-language s))
(define (settings-recent-files s) (pdfgist-settings-recent-files s))
(define (settings-view-mode s) (pdfgist-settings-view-mode s))
(define (settings-annotation-sidecar s) (pdfgist-settings-annotation-sidecar s))

(define (default-settings)
  (pdfgist-settings (provider-config default-provider-name
                                     default-provider-base-url
                                     ""
                                     default-provider-model)
                    default-language
                    '()
                    "single"
                    #f))

;; ---- secure-store seam ----

;; Reads the key out of a loaded settings value. The default keeps the key in
;; settings.json (v1 compatibility).
(define (api-key-of settings)
  (provider-config-api-key (settings-provider settings)))

;; Returns a settings value with the key replaced. Implementations of a
;; secure store would persist to the keychain here and return a value whose
;; settings.json field holds a sentinel (e.g. ""), keeping the file portable.
(define (set-api-key! settings key)
  (unless (string? key)
    (raise-argument-error 'set-api-key! "string?" key))
  (struct-copy pdfgist-settings
               settings
               [provider (struct-copy provider-config
                                      (settings-provider settings)
                                      [api-key key])]))

;; ---- storage <-> model ----

(define (string-or default value)
  (if (string? value) value default))

(define (bool-or default value)
  (if (boolean? value) value default))

(define (integer-or default value)
  (if (exact-integer? value) value default))

(define (float-or default value)
  (cond
    [(real? value) (exact->inexact value)]
    [else default]))

;; settings value -> ordered storage value (serde field order).
(define (settings->storage settings)
  (define provider (settings-provider settings))
  (jobj
   (list
    (cons "provider"
          (jobj (list (cons "name" (provider-config-name provider))
                      (cons "base_url" (provider-config-base-url provider))
                      (cons "api_key" (provider-config-api-key provider))
                      (cons "model" (provider-config-model provider)))))
    (cons "target_language" (settings-target-language settings))
    (cons "recent_files"
          (for/list ([recent (in-list (settings-recent-files settings))])
            (jobj (list (cons "path" (recent-file-path recent))
                        (cons "title" (recent-file-title recent))
                        (cons "page" (recent-file-page recent))
                        (cons "scroll_ratio" (recent-file-scroll-ratio recent))
                        (cons "last_read" (recent-file-last-read recent))))))
    (cons "view_mode" (settings-view-mode settings))
    (cons "annotation_sidecar" (settings-annotation-sidecar settings)))))

;; parsed jsexpr -> settings; missing fields fall back to defaults
;; (serde #[serde(default)] semantics). Type mismatches are tolerated per
;; field instead of failing the whole read; only a non-object document is a
;; hard parse error.
(define (storage->settings value)
  (unless (hash? value)
    (raise (exn:fail (tf "backend.error.parse-settings" "not a JSON object")
                     (current-continuation-marks))))
  (define fallback (default-settings))
  (define defaults-provider (settings-provider fallback))
  (define provider-hash (hash-ref value 'provider (hasheq)))
  (unless (hash? provider-hash)
    (set! provider-hash (hasheq)))
  (define recent-raw (hash-ref value 'recent_files '()))
  (pdfgist-settings
   (provider-config
    (string-or (provider-config-name defaults-provider)
               (hash-ref provider-hash 'name #f))
    (string-or (provider-config-base-url defaults-provider)
               (hash-ref provider-hash 'base_url #f))
    (string-or (provider-config-api-key defaults-provider)
               (hash-ref provider-hash 'api_key #f))
    (string-or (provider-config-model defaults-provider)
               (hash-ref provider-hash 'model #f)))
   (string-or (settings-target-language fallback)
              (hash-ref value 'target_language #f))
   (if (list? recent-raw)
       (filter recent-file?
               (for/list ([item (in-list recent-raw)])
                 (jsexpr->recent-file item)))
       '())
   (string-or (settings-view-mode fallback)
              (hash-ref value 'view_mode #f))
   (bool-or (settings-annotation-sidecar fallback)
            (hash-ref value 'annotation_sidecar #f))))

(define (jsexpr->recent-file item)
  (and (hash? item)
       (string? (hash-ref item 'path #f))
       (recent-file (hash-ref item 'path)
                    (string-or "" (hash-ref item 'title #f))
                    (max 1 (integer-or 1 (hash-ref item 'page #f)))
                    (float-or 0.0 (hash-ref item 'scroll_ratio #f))
                    (integer-or 0 (hash-ref item 'last_read #f)))))

;; ---- file IO ----

(define (settings-file-exists?)
  (file-exists? (settings-file-path)))

;; Missing file -> defaults (parity with settings.rs get_settings).
(define (load-settings)
  (define path (settings-file-path))
  (if (not (file-exists? path))
      (default-settings)
      (with-handlers
          ([exn:fail:filesystem?
            (lambda (e)
              (raise (exn:fail (tf "backend.error.read-settings" (exn-message e))
                               (current-continuation-marks))))])
        (define text
          (with-handlers
              ([exn:fail?
                (lambda (e)
                  (raise (exn:fail (tf "backend.error.read-settings" (exn-message e))
                                   (current-continuation-marks))))])
            (call-with-input-file path port->string)))
        (define value
          (with-handlers
              ([exn:fail?
                (lambda (e)
                  (raise (exn:fail (tf "backend.error.parse-settings" (exn-message e))
                                   (current-continuation-marks))))])
            (read-json (open-input-string text))))
        (storage->settings value))))

;; Pretty-writes in serde_json field order so the file stays diffable
;; against v1 installs.
(define (save-settings! settings)
  (unless (pdfgist-settings? settings)
    (raise-argument-error 'save-settings! "pdfgist-settings?" settings))
  (define path (settings-file-path))
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (raise (exn:fail (tf "backend.error.write-settings" (exn-message e))
                           (current-continuation-marks))))])
    (make-directory* (current-config-dir))
    (define data (json-value->bytes (settings->storage settings)))
    (call-with-output-file path
      (lambda (out) (write-bytes data out))
      #:exists 'replace))
  (void))

;; Read-modify-write under an in-process lock; callers sharing this module
;; (recents updates, key updates, saves) never interleave.
(define settings-lock (make-semaphore 1))

(define (with-settings-lock thunk)
  (call-with-semaphore settings-lock thunk))

(define (update-settings! updater)
  (unless (procedure? updater)
    (raise-argument-error 'update-settings! "procedure?" updater))
  (with-settings-lock
   (lambda ()
     (define updated (updater (load-settings)))
     (unless (pdfgist-settings? updated)
       (raise-argument-error 'update-settings! "procedure returning settings" updater))
     (save-settings! updated)
     updated)))
