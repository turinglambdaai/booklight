#lang racket/base

;; Provider presets and target languages, verbatim from the v1 types.ts
;; (PRESETS + LANGUAGES). `label-zh` is the old Chinese-only display name and
;; doubles as the fallback when shared/i18n does not carry a "provider.<id>"
;; key for the active locale.

(require racket/list
         racket/string)

(provide provider-preset
         provider-preset?
         provider-preset-id
         provider-preset-label-zh
         provider-preset-base-url
         provider-preset-models
         provider-preset-default-model
         provider-preset-needs-key?
         all-provider-presets
         find-provider-preset
         languages
         default-language
         language-code->name
         language-name->code
         default-provider-name
         default-provider-base-url
         default-provider-model)

(struct provider-preset (id label-zh base-url models needs-key?) #:transparent)

;; PRESETS mirror the v1 types.ts table: same order, same ids, same model lists.
;; `needs-key?` is new-stack policy: Ollama runs locally and the custom slot
;; may legitimately target a key-less gateway; every hosted provider needs one.
(define all-provider-presets
  (list
   (provider-preset "glm-coding" "GLM Coding 套餐（智谱）"
                    "https://open.bigmodel.cn/api/coding/paas/v4"
                    (list "glm-5" "glm-4.6" "glm-4.5-air" "glm-4.5-flash") #t)
   (provider-preset "zhipu" "智谱 GLM"
                    "https://open.bigmodel.cn/api/paas/v4"
                    (list "glm-5" "glm-4.6" "glm-4.5-flash") #t)
   (provider-preset "deepseek" "DeepSeek"
                    "https://api.deepseek.com/v1"
                    (list "deepseek-chat" "deepseek-reasoner") #t)
   (provider-preset "qwen" "通义千问 Qwen"
                    "https://dashscope.aliyuncs.com/compatible-mode/v1"
                    (list "qwen3-max" "qwen-plus" "qwen-flash") #t)
   (provider-preset "doubao" "豆包（火山方舟）"
                    "https://ark.cn-beijing.volces.com/api/v3"
                    (list "doubao-seed-2-1-pro" "doubao-seed-2-0-lite") #t)
   (provider-preset "doubao-coding" "豆包 Coding 套餐（火山方舟）"
                    "https://ark.cn-beijing.volces.com/api/coding/v3"
                    (list "doubao-seed-2-1-pro" "doubao-seed-code") #t)
   (provider-preset "moonshot" "Moonshot Kimi"
                    "https://api.moonshot.cn/v1"
                    (list "kimi-k2-0905-preview" "kimi-latest" "moonshot-v1-8k") #t)
   (provider-preset "kimi-coding" "Kimi Code 套餐（Moonshot）"
                    "https://api.kimi.com/coding/v1"
                    (list "kimi-k3" "kimi-k2.5" "kimi-k2-turbo-preview") #t)
   (provider-preset "siliconflow" "SiliconFlow 硅基流动"
                    "https://api.siliconflow.cn/v1"
                    (list "Qwen/Qwen3-72B-Instruct" "deepseek-ai/DeepSeek-V3") #t)
   (provider-preset "openai" "OpenAI"
                    "https://api.openai.com/v1"
                    (list "gpt-4o-mini" "gpt-4o") #t)
   (provider-preset "ollama" "Ollama（本地）"
                    "http://localhost:11434/v1"
                    (list "qwen3" "llama3.1") #f)
   (provider-preset "custom" "自定义" "" '() #f)))

(define (find-provider-preset id)
  (for/first ([preset (in-list all-provider-presets)]
              #:when (string=? (provider-preset-id preset) id))
    preset))

;; The preset UIs pick the first model as the initial default.
(define (provider-preset-default-model preset)
  (define models (provider-preset-models preset))
  (if (null? models) "" (car models)))

;; LANGUAGES mirror the v1 types.ts order. settings.json stores these
;; display names (settings.rs defaults target_language to 中文).
(define languages
  (list "中文" "繁體中文" "English" "日本語" "한국어" "Français" "Deutsch" "Español"))

(define default-language "中文")

;; Wire codes for the Rivet TargetLanguage enum, in enum declaration order.
(define language-codes-by-name
  (list (cons "中文" "zh")
        (cons "繁體中文" "zh-hant")
        (cons "English" "en")
        (cons "日本語" "ja")
        (cons "한국어" "ko")
        (cons "Français" "fr")
        (cons "Deutsch" "de")
        (cons "Español" "es")))

(define (language-name->code name)
  (cond
    [(assoc name language-codes-by-name) => cdr]
    ;; v1 files can hold any free-text language; fold unknowns onto the
    ;; default rather than failing the whole settings read.
    [else "zh"]))

(define (language-code->name code)
  (or (for/or ([pair (in-list language-codes-by-name)])
        (and (string=? (cdr pair) code) (car pair)))
      default-language))

;; Defaults from the v1 (Tauri) ProviderConfig::default.
(define default-provider-name "deepseek")
(define default-provider-base-url "https://api.deepseek.com/v1")
(define default-provider-model "deepseek-chat")
