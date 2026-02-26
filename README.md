# zclaw

<img
  src="docs/images/lobster_xiao_cropped_left.png"
  alt="Lobster soldering a Seeed Studio XIAO ESP32-C3"
  height="200"
  align="right"
/>

ESP32向けの超小型AIパーソナルアシスタント

zclawはC言語で記述され、ESP32ボード上で動作します。デフォルトビルドでは**888 KiB以下**という厳しいファームウェアサイズ制約を目標に開発されています。スケジュールタスク、GPIO制御、永続メモリ、自然言語によるカスタムツール構成をサポートしています。

**888 KiB**の制限はアプリケーションコードだけでなく、ファームウェア全体のサイズです。
`zclaw`のロジックに加え、ESP-IDF/FreeRTOSランタイム、Wi-Fi/ネットワーク、TLS/暗号化、証明書バンドルのオーバーヘッドを含みます。

使うのも楽しい、ハックするのも楽しい。
<br clear="right" />

## 完全なドキュメント

完全なガイドとリファレンスはドキュメントサイトを参照してください。

- [完全なドキュメント](https://zclaw.dev)
- [ユースケース: 実用的 + 楽しい](https://zclaw.dev/use-cases.html)
- [変更履歴 (web)](https://zclaw.dev/changelog.html)
- [完全なREADME (原文)](https://zclaw.dev/reference/README_COMPLETE.md)
- [再実装アーキテクチャ設計書（Git差分ベース）](docs/reimplementation-architecture-ja.md)
- [信頼性設計書（Discord + LLM、日本語）](docs/discord-llm-reliability-design-ja.md)


## クイックスタート

ワンラインブートストラップ (macOS/Linux):

```bash
bash <(curl -fsSL https://raw.githubusercontent.com/tnm/zclaw/main/scripts/bootstrap.sh)
```

既にクローン済みの場合:

```bash
./install.sh
```

非対話型インストール:

```bash
./install.sh -y
```

<details>
<summary>セットアップ注意事項</summary>

- `bootstrap.sh`はリポジトリをクローン/更新してから`./install.sh`を実行します。ブートストラップフローを事前に検査/検証できます（`ZCLAW_BOOTSTRAP_SHA256`整合性チェックを含む）。詳細は[スタートガイド](https://zclaw.dev/getting-started.html)を参照してください。
- フラッシュ内の認証情報を暗号化するには、セキュアモードを使用します（インストールフローで`--flash-mode secure`、または直接`./scripts/flash-secure.sh`）。
- フラッシュ書き込み後、`./scripts/provision.sh`でWiFi + LLM認証情報をプロビジョニングします。
- 実行時認証情報（WiFi SSID/パスワード、LLMバックエンド/モデル/APIキー（またはOllama API URL）、Discordボットトークン/チャンネルID）を更新するには、いつでも`./scripts/provision.sh`または`./scripts/provision-dev.sh`を再実行できます（再書き込み不要）。
- デフォルトのLLMレート制限は`100/時間`と`1000/日`です。`main/config.h`（`RATELIMIT_*`）でコンパイル時制限を変更できます。
- クイック検証パス: `./scripts/web-relay.sh`を実行し、テストメッセージを送信してデバイスが応答できることを確認します。
- シリアルポートがビジーの場合は、`./scripts/release-port.sh`を実行してから再試行してください。
- シークレットを再入力せずにローカルで再プロビジョニングを繰り返すには、ローカルプロファイルファイル付きで`./scripts/provision-dev.sh`を使用します（`provision-dev.sh`は`provision.sh --yes`をラップしています）。

</details>

## ハイライト

- Discordまたはホスト型Webリレー経由でチャット
- タイムゾーン対応スケジュール（`daily`、`periodic`、ワンショット`once`）
- 組み込み + ユーザー定義ツール
- ガードレール付きGPIO読み書き制御（一括`gpio_read_all`を含む）
- 再起動間で永続化されるメモリ
- ペルソナオプション: `neutral`、`friendly`、`technical`、`witty`
- Anthropic、OpenAI、OpenRouter、Ollama（カスタムエンドポイント）のプロバイダーサポート

## ハードウェア

動作確認済みターゲット: **ESP32-C3**、**ESP32-S3**、**ESP32-C6**。
他のESP32 variantsも基本的に動作します（一部はESP-IDFターゲットの手動設定が必要な場合あり）。
テスト報告をぜひお寄せください！

推奨スターターボード: [Seeed XIAO ESP32-C3](https://www.seeedstudio.com/Seeed-XIAO-ESP32C3-p-5431.html)

## ローカル開発とハッキング

典型的な高速開発ループ:

```bash
./scripts/test.sh host
./scripts/build.sh
./scripts/flash.sh --kill-monitor /dev/cu.usbmodem1101
./scripts/provision-dev.sh --port /dev/cu.usbmodem1101
./scripts/monitor.sh /dev/cu.usbmodem1101
```

プロファイルを一度セットアップしてから再利用:

```bash
./scripts/provision-dev.sh --write-template
# ~/.config/zclaw/dev.env を編集
./scripts/provision-dev.sh --show-config
./scripts/provision-dev.sh
```

詳細は[ローカル開発 & ハッキングガイド](https://zclaw.dev/local-dev.html)を参照してください。

### その他の便利なスクリプト

<details>
<summary>スクリプトを表示</summary>

- `./scripts/flash-secure.sh` - 暗号化付きでフラッシュ書き込み
- `./scripts/provision.sh` - NVSに認証情報をプロビジョニング
- `./scripts/provision-dev.sh` - 繰り返しプロビジョニング用ローカルプロファイルラッパー
- `./scripts/erase.sh` - NVSのみ（`--nvs`）またはフルフラッシュ（`--all`）をガードレール付きで消去
- `./scripts/monitor.sh` - シリアルモニター
- `./scripts/emulate.sh` - QEMUプロファイルを実行
- `./scripts/web-relay.sh` - ホスト型リレー + モバイルチャットUI
- `./scripts/benchmark.sh` - リレー/シリアルレイテンシのベンチマーク
- `./scripts/test.sh` - ホスト/デバイステストフローを実行
- `./scripts/test-api.sh` - ライブプロバイダーAPIチェックを実行（手動/ローカル）

</details>

## サイズ内訳

現在のデフォルト`esp32s3`の内訳（`idf.py -B build size-components`からのグループ化可能なイメージバイト数；行の合計は総イメージサイズ）:

| セグメント | バイト数 | サイズ | シェア |
| --- | ---: | ---: | ---: |
| zclawアプリロジック (`libmain.a`) | `35742` | ~34.9 KiB | ~4.1% |
| Wi-Fi + ネットワークスタック | `397356` | ~388.0 KiB | ~45.7% |
| TLS/暗号化スタック | `112922` | ~110.3 KiB | ~13.0% |
| 証明書バンドル + アプリメタデータ | `99722` | ~97.4 KiB | ~11.5% |
| その他ESP-IDF/ランタイム/ドライバ/libc | `224096` | ~218.8 KiB | ~25.8% |

このビルドの総イメージサイズは`869838`バイト；パディングされた`zclaw.bin`は`869952`バイト（~849.6 KiB）で、依然として制限未満です。

## レイテンシベンチマーク

リレーパスベンチマーク（Webリレー処理 + デバイス往復を含む）:

```bash
./scripts/benchmark.sh --mode relay --count 20 --message "ping"
```

ダイレクトシリアルベンチマーク（ホスト往復 + 最初の応答時間）。ファームウェアが`METRIC request ...`行をログ出力する場合、レポートにはデバイス側のタイミングも含まれます:

```bash
./scripts/benchmark.sh --mode serial --serial-port /dev/cu.usbmodem1101 --count 20 --message "ping"
```

## ライセンス

MIT
