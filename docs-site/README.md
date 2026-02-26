# zclaw ドキュメントサイト

クラシックなC言語リファレンスにインスピレーションを得た、印刷本風のビジュアルスタイルを持つカスタム静的ドキュメントサイト。

## ローカルプレビュー

```bash
./scripts/docs-site.sh
# または:
./scripts/docs-site.sh --host 0.0.0.0 --port 8788 --open
```

## 構造

- `README.html` - WebフォーマットのREADMEランディングページ
- `index.html` - 概要とチャプターマップ
- `getting-started.html` - セットアップ、フラッシュ、プロビジョニングフロー
- `tools.html` - ツールリファレンスとスケジュール文法
- `architecture.html` - ランタイム/タスクモデル
- `security.html` - セキュリティと運用
- `build-your-own-tool.html` - カスタムツールの設計とメンテナンスワークフロー
- `local-dev.html` - ローカル開発、プロビジョニングプロファイル、ハッキングループ
- `use-cases.html` - オンデバイスアシスタントの実用的で遊び心のあるシナリオ
- `changelog.html` - リリース履歴とアップグレードノート
- `styles.css` - ビジュアルシステムとレスポンシブレイアウト
- `app.js` - サイドバー/ナビゲーション動作
