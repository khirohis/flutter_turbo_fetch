# HTTP/2 Benchmark Mock Server

`example/` アプリの並列 HTTP/2 ベンチマーク検証用ローカルサーバーです（外部 npm 依存なし、Node.js 標準の `node:http2` で動作します）。

## 1. TLS 証明書（`cert.pem` / `key.pem`）の準備

Android (`OkHttp`) や iOS (`URLSession`) で標準的な HTTP/2 (ALPN `h2`) をネゴシエーションするため、ローカル信頼済みの証明書（[`mkcert`](https://github.com/FiloSottile/mkcert) 推奨）をこのディレクトリに配置してください。

```bash
cd example/server
mkcert -key-file key.pem -cert-file cert.pem localhost 127.0.0.1 10.0.2.2
```

## 2. サーバーの起動

```bash
node example/server/http2_mock_server.mjs
```

## 3. エンドポイント仕様

- **`GET /json?sizeKb=100&delayMs=0`**
  - `sizeKb`: 生成する JSON ペイロードのサイズ（KB 単位、デフォルト `100`）
  - `delayMs`: 人工的なサーバー応答遅延（ミリ秒、デフォルト `0`）
  - レスポンスヘッダ `x-http-version` にネゴシエーションされたプロトコル（`2.0` または `1.1`）を返します。
- **Android エミュレータからのアクセス URL**:
  - `https://10.0.2.2:8443/json?sizeKb=100`
- **iOS シミュレータからのアクセス URL**:
  - `https://localhost:8443/json?sizeKb=100`
