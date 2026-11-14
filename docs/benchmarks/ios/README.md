# iOS physical-device benchmarks

The physical-device Release study uses a dedicated entry point at
`example/lib/ios_benchmark_main.dart`. Simulator and Debug runs are functional
checks and are never substituted for physical-device Release measurements.

## Protocol

Compare `IOHttpClientAdapter + BackgroundTransformer`,
`Http2Adapter + BackgroundTransformer`, and `TurboHttpAdapter + TurboTransformer`.
Each scenario uses one warmup per adapter followed by ten measured trials per
adapter. Each trial performs the entire existing ten-round workload:

| Scenario | Requested sizes | Concurrency | Requests per trial |
| --- | --- | ---: | ---: |
| A | 5–45 KiB, steps of 5 | 3 | 90 |
| B | 10–100 KiB, steps of 10 | 5 | 100 |
| C | 20–200 KiB, steps of 20 | 10 | 100 |

Order rotates IO → HTTP/2 → Turbo, HTTP/2 → Turbo → IO, Turbo → IO → HTTP/2.
Ten trials do not divide evenly by three, so each position occurs three or four
times per adapter. A client is retained across warmup and measurement within
each adapter/scenario. Scenarios run A, B, C; scenario order is not randomized.

**This is a split-pipeline experiment.** Every adapter first returns the full
body as bytes through Dio's synchronous bytes transformer. Then its configured
JSON transformer decodes those bytes. Network time ends at full-body reception;
parse time excludes socket reads. Both include their associated Dart/native
overheads, rather than representing pure wire time or pure parser CPU time.
Total time includes request scheduling and JSON shape validation.

The ordinary example's adapter/transformer timing boundary is asymmetric:
streaming body reads occur inside its Parse interval, whereas TurboFetch
finishes receiving before its Net interval ends. Those historical phase values
are not interchangeable with this protocol. Splitting the pipeline also changes
buffering and memory behavior; results do not establish a drop-in speedup in an
arbitrary production Dio pipeline.

All adapters decode the same UTF-8 JSON without custom model mapping. The
server pre-generates all benchmark sizes. Requested sizes are lower bounds:
the generator adds complete records, so the actual body can be larger. Actual
byte lengths and the server-observed HTTP version are verified per request.
No compression or artificial server delay is enabled.

RSS is process-wide, sampled every 25 ms plus a final sample; short-lived peaks
can be missed. Baselines include retained clients, previous allocations, and
runtime caches. Results are written and flushed between trials, outside timed
sections, which can still influence subsequent thermal/GC state. Frame metrics
are **N/A** in this Release protocol; no zero-jank claim follows. No Profile
results are mixed into Release data.

## Reproduce

1. Use FVM Flutter 3.47.8, record `fvm flutter --version`, `xcodebuild -version`,
   `node --version`, the device model/OS, source commit, and resolved package
   versions. Pair and unlock a physical iPhone and enable Developer Mode.
2. Put the Mac and iPhone on the same LAN. Use the Mac's LAN address as
   `<LAN_IP>`; USB deployment does not forward `localhost` to the Mac.
3. Generate a local certificate **outside the repository** with a matching SAN:

   ```bash
   openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 7 \
     -keyout /tmp/ios-benchmark-key.pem -out /tmp/ios-benchmark-cert.pem \
     -subj /CN=local-benchmark \
     -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:<LAN_IP>'
   chmod 600 /tmp/ios-benchmark-key.pem
   ```

   Alternatively use a locally trusted CA and install/trust it on the test
   device. Do not commit certificates, private keys, device identifiers, or
   signing identities. `*.pem` is already ignored by this repository.
4. Start the server, binding only to that LAN address. Use a fresh server log
   for each session; the exporter rejects duplicate request identifiers.

   ```bash
   HOST=<LAN_IP> PORT=8444 REQUIRE_TLS=1 BENCHMARK_LOG=1 \
     CERT_PATH=/tmp/ios-benchmark-cert.pem KEY_PATH=/tmp/ios-benchmark-key.pem \
     node example/server/http2_mock_server.mjs > /tmp/ios-server.jsonl 2>&1
   ```

5. Verify TLS and HTTP/2 from the Mac:

   ```bash
   curl --http2 --cacert /tmp/ios-benchmark-cert.pem -v \
     'https://<LAN_IP>:8444/json?sizeKb=10'
   ```

   Check SAN validation, negotiated `h2`, status 200, and `x-http-version: 2.0`.
   This verifies the host path only. Device responses/server logs must also
   confirm the actual iPhone protocol.
6. In `example/`, build and install the dedicated app:

   ```bash
   fvm flutter build ios --release -t lib/ios_benchmark_main.dart \
     --dart-define=BENCHMARK_BASE_URL=https://<LAN_IP>:8444 \
     --dart-define=BENCHMARK_LOCAL_TLS=true \
     --dart-define=BENCHMARK_SOURCE_SHA=<MEASUREMENT_COMMIT_SHA>
   xcrun devicectl device install app --device <DEVICE_ID> build/ios/iphoneos/Runner.app
   xcrun devicectl device process launch --device <DEVICE_ID> \
     --terminate-existing io.github.khirohis.flutterTurboFetchExample
   ```

   `BENCHMARK_LOCAL_TLS=true` bypasses certificate validation equally for all
   three clients and is allowed only for a private IPv4 origin or localhost.
   This is a test-only condition, not production TLS guidance. Set it to false
   when using a trusted certificate. Allow the app's local network prompt.
   Keep it foregrounded and avoid other activity. Record power/thermal/network
   conditions; allow cooling before reruns. Do not infer thermal stability.
7. After the screen reports completion, retrieve the app's evidence:

   ```bash
   xcrun devicectl device copy from --device <DEVICE_ID> \
     --domain-type appDataContainer \
     --domain-identifier io.github.khirohis.flutterTurboFetchExample \
     --source tmp/ios-benchmark.jsonl --destination /tmp/ios-device.jsonl
   python3 example/tool/export_ios_benchmark.py \
     /tmp/ios-device.jsonl /tmp/ios-server.jsonl \
     docs/benchmarks/ios/<ACTUAL_DATE>
   ```

   The exporter requires a completed Release session, 90 measured trials,
   nine warmups, 8,700 successful measured requests, matching server records,
   expected protocols, server-observed `h2` ALPN for HTTP/2, and identical byte
   lengths for matching requested sizes. A public server CSV retains allowlisted
   protocol and response metadata without host/device addresses.
   It rejects partial/failed sessions and exports only allowlisted fields.
   CSV `N/A` means unavailable; warmup rows are explicitly marked and excluded
   from trial statistics. P50 uses the median and P95 uses nearest rank.
   Standard deviation uses the sample definition (`n-1`).

## Evidence status

Physical-device results are published only after the completion and consistency
checks above pass. Failed preparation runs are not performance measurements.

- [2026-11-14 Release results — 90 trials, 8,700 measured requests](2026-11-14-release.md)
