# flutter_turbo_fetch Documentation (`docs/`)

This directory contains public-facing technical references, architectural rationales, and benchmark evaluation reports for `flutter_turbo_fetch`.

For quick start, installation, and usage instructions, please refer to the project root [`README.md`](../README.md).  
For internal specifications and agent instructions (maintained in Japanese), refer to [`../specs/`](../specs/README.md) and [`../AGENTS.md`](../AGENTS.md).

---

## Technical Documents

- **[Design Rationale (`design-rationale.md`)](design-rationale.md)**:
  Detailed analysis of the three fundamental bottlenecks in mobile networking, evaluation of existing `pub.dev` packages, and technical justification of the four core design principles.
- **[Benchmark Results & Analysis (`benchmark-results.md`)](benchmark-results.md)**:
  Cross-platform executive summary and common evaluation methodology, accompanied by platform-specific empirical benchmark reports under `docs/benchmarks/` (Android, Windows, iOS, etc.).
- **[TurboTransformer Reference (`turbo-transformer.md`)](turbo-transformer.md)**:
  Architecture and implementation breakdown of zero-copy Isolate transformations, fused decoders, and semaphore concurrency governance.
