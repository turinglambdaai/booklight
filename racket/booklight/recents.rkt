#lang racket/base

;; Recently-opened files, ported from the RecentFile struct in
;; v1 (Tauri) settings.rs recents shape + the saveRecent upsert:
;; newest entry first, deduplicated by path, trimmed to 12.
;;
;; Storage units (v1 file compatibility): `page` is 1-based, `scroll-ratio`
;; is a float in [0, 1], `last-read` is unix seconds. The RPC layer scales
;; the ratio and converts the timestamp at the boundary; this module stays
;; in storage units.

(require racket/list)

(provide recent-file
         recent-file?
         recent-file-path
         recent-file-title
         recent-file-page
         recent-file-scroll-ratio
         recent-file-last-read
         max-recents
         upsert-recent
         trim-recents)

(struct recent-file (path title page scroll-ratio last-read) #:transparent)

(define max-recents 12)

;; (upsert-recent entries new-entry) -> upserted list, newest first,
;; trimmed to `max-recents`. `title`/`page`/`scroll-ratio` of the existing
;; entry are replaced wholesale, matching main.ts saveRecent.
(define (upsert-recent entries entry)
  (unless (recent-file? entry)
    (raise-argument-error 'upsert-recent "recent-file?" entry))
  (trim-recents
   (cons entry
         (filter (lambda (existing)
                   (not (string=? (recent-file-path existing)
                                  (recent-file-path entry))))
                 entries))))

(define (trim-recents entries [limit max-recents])
  (unless (list? entries)
    (raise-argument-error 'trim-recents "list?" entries))
  (if (> (length entries) limit)
      (take entries limit)
      entries))
