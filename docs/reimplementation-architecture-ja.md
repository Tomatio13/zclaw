# zclaw 再実装アーキテクチャ設計書（Git差分ベース）

本書は、現時点の `git diff` に含まれる変更を基準に、zclaw を別実装で再構築できる粒度まで分解した設計書です。  
対象は ESP-IDF + FreeRTOS 上で動く「Discord連携AIエージェント」です。

## 1. この差分で何が変わったか

差分の中心は「Telegram中心構成」から「Discord中心構成」への移行です。  
同時に、低メモリ機での信頼性対策が入っています。

主要変更:

- Telegram実装を削除
  - `main/telegram*.{c,h}` 群を削除
  - Telegram向けテスト/スクリプトを削除
- Discord実装を追加
  - `main/discord.{c,h}` を新規追加
  - 入出力メッセージ型を `discord_msg_t` に統一
- HTTP/TLS競合対策
  - `main/net_http_guard.{c,h}`（共有HTTPロック）を追加
  - `llm.c` と `discord.c` の通信を排他化
- UTF-8安全化
  - `main/utf8_utils.{c,h}` を追加
  - 履歴保存・応答分割・JSONパース連結で利用
- 空応答回復フロー追加
  - 空応答時の1回再試行
  - 再試行後も空なら履歴全クリア
- M5Stack Core向けビルドプリセット追加
  - `sdkconfig.m5stack-core.defaults`
  - `scripts/build.sh` / `flash.sh` の `--m5stack` 対応

## 2. 目標アーキテクチャ

### 2-1. レイヤ構成

1. `Platform Layer`
- ESP-IDF, FreeRTOS, NVS, WiFi, HTTP client, SNTP

2. `Transport Layer`
- `channel.c`（USB/UARTローカルI/O）
- `discord.c`（Discord REST polling + send）
- `net_http_guard.c`（LLM/Discord HTTP排他）

3. `Agent Layer`
- `agent.c`（会話履歴、LLM呼び出し、ツールループ、返信）
- `json_util.c`（LLM API request/response 変換）
- `llm.c`（プロバイダ通信）

4. `Domain Layer`
- `tools*.c`（GPIO, memory, cron, persona, system）
- `cron.c`（スケジューラ）
- `memory.c`（永続化）
- `ratelimit.c`（時間/日次制限）

5. `Provisioning / Ops Layer`
- `scripts/provision.sh`（NVSキー注入）
- `scripts/build.sh`, `scripts/flash.sh`, `scripts/test.sh`

## 3. ランタイム構成（タスクとキュー）

### 3-1. タスク

- `channel_read_task` / `channel_write_task`
  - ローカル入力（USB/UART）を受信
  - エージェント返信をローカル出力
- `discord_task`
  - Outbound queue の送信処理
  - 일정間隔で Discord polling
- `agent_task`
  - 入力キューを消費して会話処理
- `cron_task`
  - スケジュールを監視し、擬似ユーザー入力として投入

### 3-2. キュー契約

- `input_queue`: `channel_msg_t`
  - `text`, `source`, `chat_id`
  - source: `CHANNEL` / `DISCORD` / `CRON`
- `channel_output_queue`: `channel_output_msg_t`
  - ローカル表示用テキスト
- `discord_output_queue`: `discord_msg_t`
  - Discord送信用テキスト（最大2000）

## 4. 起動シーケンス

`app_main()` の実質フロー:

1. `memory_init()`（NVS初期化）
2. OTA状態確認
3. 工場リセットボタン判定
4. Boot loop保護判定
5. WiFi接続（credentialsはNVS優先）
6. `cron_init()`（NTP同期 + timezoneロード）
7. `llm_init()`（backend/model/keyロード）
8. `ratelimit_init()`
9. `net_http_guard_init()`
10. `discord_init()`（token/channel有無チェック）
11. `tools_init()`
12. `channel_init()`
13. キュー作成
14. `channel_start()` → `discord_start()` → `agent_start()` → `cron_start()`
15. Readyログ + Discord起動メッセージ

## 5. 入力から返信までのデータフロー

### 5-1. 通常メッセージ

1. Discordまたはローカルチャネルで受信
2. `input_queue` に `channel_msg_t` 投入
3. `agent_task` が取得
4. `history_add(user)` で履歴追加
5. `json_build_request()` でLLMリクエスト生成
6. `llm_request_with_retry()` でAPI呼び出し
7. `json_parse_response()` で
   - テキスト
   - tool call
   を抽出
8. テキストなら返信送信、tool callならツール実行→再度LLM

### 5-2. 空応答時のフォールバック

条件: `text_out == ""` かつ `tool_calls == none`

1. 同一ターン内で1回だけ再試行
2. 再試行は「最新ユーザ発話1件のみ」「tools無し」
3. それでも空なら `history_clear_all()` で履歴全削除
4. ユーザーに再入力を促すメッセージを返す

## 6. LLMアダプタ設計

### 6-1. バックエンド抽象

- `LLM_BACKEND_ANTHROPIC`
- `LLM_BACKEND_OPENAI`
- `LLM_BACKEND_OPENROUTER`
- `LLM_BACKEND_OLLAMA`

保持状態:

- backend
- model
- api key
- api_url override

### 6-2. request JSON生成

- `json_util.c` が provider差分を吸収
- OpenAI系:
  - `messages[]`
  - `tools[]`
  - `max_completion_tokens`（OpenAI）
  - GPT-5系には `reasoning_effort=minimal` を付与
- Anthropic系:
  - `messages[]` + `tools[]` + `max_tokens`

### 6-3. response JSON解析

優先抽出:

1. `message.content`（string/array両対応）
2. `tool_calls`
3. fallbackとして `output_text` / `output[]`

テキスト連結時は UTF-8 境界を壊さない。

## 7. Discordアダプタ設計

### 7-1. 送信

- REST: `POST /channels/{id}/messages`
- Bot tokenをAuthorization headerへ付与
- 4xx/5xx時はレスポンス本文もログ出力

### 7-2. 受信

- REST polling: `GET /channels/{id}/messages`
- `after=<last_message_id>` を使い差分取得
- 初回pollは backlog を処理せず、最新IDだけカーソル化
- botメッセージは無視

### 7-3. 長文分割

- Discord制限(2000)を超える場合はチャンク分割
- UTF-8安全境界で分割

## 8. 同期・排他設計

### 8-1. HTTP共有ロック

目的:
- 低RAM環境で Discord poll と LLM call の同時TLSを避ける

仕様:

- `net_http_guard_lock(timeout)`
- `net_http_guard_unlock()`
- LLM/Discord双方が通信前後に必ず利用

### 8-2. cronエントリ排他

- `cron.c` は `s_entries_mutex` でエントリ配列を保護

## 9. 永続化設計（NVS）

主要キー:

- WiFi: `wifi_ssid`, `wifi_pass`
- LLM: `llm_backend`, `api_key`, `llm_model`, `llm_api_url`
- Discord: `discord_token`, `discord_channel`
- その他: `timezone`, `persona`, `boot_count`
- レート制限: `rl_daily`, `rl_day`, `rl_year`

設計意図:

- 起動に必要な設定はすべてNVSから復元可能
- compile-time定義はフォールバック用途

## 10. ツールサブシステム

`tools.c` で静的レジストリを保持。

カテゴリ:

- GPIO: write/read/read_all/delay
- I2C: scan
- Memory: set/get/list/delete (`u_` key制約)
- Persona: set/get/reset
- Cron: set/list/delete/get_time/set_timezone/get_timezone
- System: version/health
- User tools: create/list/delete

ポリシー:

- ペルソナは文体のみを変える
- 安全判断やツール選択ロジックは変えない

## 11. 設定・運用スクリプト設計

### 11-1. build/flash

- `scripts/build.sh`
  - board preset対応: `esp32s3-box-3`, `m5stack-core`
- `scripts/flash.sh`
  - 同じboard preset対応
  - ポート占有検知と解放補助

### 11-2. provisioning

- `scripts/provision.sh`
  - WiFi/LLM/DiscordをNVSに注入
  - backend別API key要件を検証
- `scripts/provision-dev.sh`
  - 開発用 `.env` プロファイルラッパー

## 12. テスト設計（現状）

- `scripts/test.sh host`
  - `agent/json/tools/runtime` などをホストで実行
- Telegram関連テストは削除済み
- Discord/新キー体系に合わせたテストへ置換

注意:
- 環境に `cjson` が無いと host test コンパイルは失敗する

## 13. 再実装ガイド（最短順）

再実装は次の順で行うと安全です。

1. `messages` 契約を固定
- `channel_msg_t`, `channel_output_msg_t`, `discord_msg_t`

2. `agent` の最小ループ実装
- user input -> LLM -> text reply のみ

3. `json_util` と `llm` を接続
- OpenAI系1種でまず通す

4. `discord` transport を追加
- send + poll + backlog skip

5. `tools` を段階導入
- `get_time`, `memory_*` から開始

6. `cron` を導入
- once/periodic -> daily の順

7. 信頼性機能を導入
- HTTP lock
- UTF-8 safe copy/split
- 空応答リトライ + 履歴クリア

8. provisioning/build/flash/test のスクリプト整備

## 14. 再実装時の受け入れ基準

最低限、次を満たせば同等アーキテクチャとみなせます。

- Discord経由で user message を受信して応答できる
- tool call を1往復以上処理できる
- 空応答時に再試行し、最終的に履歴クリアできる
- 2000超メッセージでもDiscord 400を出さず送信できる
- WiFi/LLM/Discord設定をNVSで保持し、再起動後も復元できる
- cron once/periodic/daily が機能する

## 15. 既知のトレードオフ

- HTTP排他は競合を減らすが、pollスキップが増える
- 履歴全クリアは復旧に強いが、文脈継続性は失う
- 文字数制約は安定性向上と引き換えに表現量を削る

## 16. 主要ファイルマップ

中核:

- `main/main.c` 起動オーケストレーション
- `main/agent.c` 会話制御
- `main/llm.c` LLM通信
- `main/json_util.c` API JSON変換
- `main/discord.c` Discord transport

信頼性:

- `main/net_http_guard.c` 通信排他
- `main/utf8_utils.c` UTF-8安全コピー

ドメイン:

- `main/tools*.c` ツール群
- `main/cron.c` スケジューラ
- `main/memory.c` 永続化
- `main/ratelimit.c` 制限

運用:

- `scripts/build.sh`
- `scripts/flash.sh`
- `scripts/provision.sh`
- `scripts/provision-dev.sh`
- `scripts/test.sh`

