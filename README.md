# flutter_turbo_fetch

`flutter_turbo_fetch` is a high-performance, non-blocking networking plugin for Flutter (`package:dio`). Designed to overcome the performance bottlenecks of traditional Dart-based HTTP clients, it offloads heavy lifting to platform-native HTTP layers and background isolates, ensuring a silky-smooth UI thread.

---

## 🚀 Key Features

- **Blazing Fast Performance**: Bypasses traditional Dart HTTP bottlenecks by leveraging platform-native network stacks for batch response reception.
- **UI Thread Protection**: Offloads heavy payload processing and JSON parsing to background **Isolates** with minimal memory copy overhead.
- **Developer Friendly**: Clean, drop-in integration for `package:dio`.

---

## 📦 Architecture Concept

- **Platform Native HTTP Layer**: Handles concurrent HTTP/2 networking and payload assembly on native background threads.
- **Dart Adapter & Transformer Layer**: Bridges native responses to `Dio` and dispatches payload decoding without blocking the UI thread.
