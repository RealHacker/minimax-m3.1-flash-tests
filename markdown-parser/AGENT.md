# Markdown parser project rules

## Project overview

This repository becomes a small CommonMark-flavored Markdown parser written in C11. It will parse documents into an arena-owned AST, expose a linkable library, and provide an `md` command-line program that emits AST JSON, HTML, and plaintext. Later stages add extensions, streaming/depth safety, and a table of contents. Implement only the current prompt, but preserve earlier output contracts.

## Technical boundaries

- Compile with `cc -std=c11 -O2`; use the C11 standard library only. Do not add third-party libraries or network access. JSON serialization is handwritten.
- Keep public declarations in `src/md.h` and the requested module boundaries in `src/`. The build must produce an `md` executable and, when requested, `libmd.a`.
- Never return pointers to temporary stack data. Arena ownership means one destroy operation releases a parsed document; no per-node free is needed.
- CLI output must be deterministic, UTF-8-preserving, and free of diagnostics on stdout. Use stderr for errors. Check all allocation and I/O failures.
- Do not modify evaluator fixtures. Run `make` and relevant sanitizer checks before reporting a checkpoint complete.