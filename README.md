<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# CZet Editor Support

Editor integration for the CZet programming language: a minimal
Language Server Protocol (LSP) implementation, a Vim plugin, and a
Visual Studio Code extension. All components cover the same language
surface — `constexpr` functions, monomorphized generics
(`T f<T>(...)`, `struct box<T, U>`), `defer` blocks, `_Reflect`
reflection with `reflect_t`, `_Macro` token-level macros, `_Operator`
overloading, and the `<utils/...>` standard library — as specified in
the CZet `docs/` guides and exercised by `tests/*.czet`.

The recommended source extension is `.ct`; `.czet` remains accepted.
Plain `.c` files are also compiled with `-std=czet` by default
(see `czet --help`), but the editor plugins intentionally do not claim
the `.c` extension in order to preserve standard C highlighting.

```text
CZet-lsp/
  ftdetect/   Vim filetype detection for *.ct and *.czet
  syntax/     Vim syntax highlighting layered on stock C/GNU syntax
  ftplugin/   Vim buffer defaults, including czet driver build integration
  lsp/        Minimal LSP server in C (single file, stdio, no dependencies)
  vscode/     TextMate grammar, language configuration, and LSP client
```

## 1. Vim Plugin

This repository is a standard Vim plugin: `ftdetect/`, `syntax/`, and
`ftplugin/` sit at the repository root, so any plugin manager picks it
up without additional configuration.

With vim-plug:

```vim
Plug '/path/to/CZet-lsp'
```

Manual installation (the `rtp` entry must precede filetype and syntax
initialization):

```vim
set rtp^=/path/to/CZet-lsp
syntax on
filetype plugin on
```

Alternatively, copy `ftdetect/czet.vim`, `syntax/czet.vim`, and
`ftplugin/czet.vim` into the corresponding directories of `~/.vim`.

To verify the installation on any CZet buffer:

```vim
:set filetype?          " expected: filetype=czet
:echo b:current_syntax  " expected: czet
```

If `filetype` comes back empty, Vim has not sourced
`ftdetect/czet.vim`: confirm the path appears in `:set rtp?` and
`:scriptnames`. When the `rtp` entry is added after startup,
`ftdetect` scripts are not retroactively sourced — restart Vim, or run
`:runtime! ftdetect/czet.vim` and reopen the file.

## 2. Language Server (`lsp/`)

`lsp/czet-lsp.c` is a self-contained C11 language server speaking
JSON-RPC 2.0 over standard input and output with `Content-Length`
framing, as expected by Neovim, VSCode, and vim-lsp clients. It requires
only the C standard library.

Supported protocol methods:

| Method | Description |
|---|---|
| `initialize` | Declares `textDocumentSync: 1` (full), completion, hover, definition, and document symbol capabilities |
| `textDocument/didOpen`, `didChange`, `didClose` | Maintains open buffers in memory and republishes diagnostics on change |
| `textDocument/completion` | Prefix-filtered completion of CZet keywords, macro fragments, `<utils/...>` headers, and standard library APIs |
| `textDocument/hover` | Concise Markdown documentation for the keyword under the cursor |
| `textDocument/definition` | First declaration of the identifier within the same file |
| `textDocument/documentSymbol` | Functions (including generic instantiations), `struct`/`union`/`enum` declarations, `typedef`s, `_Macro` definitions, and `_Operator` overloads |
| `textDocument/publishDiagnostics` | Bracket balance analysis, CZet syntactic rules (`_Reflect(`, `_Operator(`, `defer` forms, `_Macro` termination), and, when the `czet` driver is available on `PATH`, compiler diagnostics via `czet -fsyntax-only` |

To build and inspect the server:

```sh
cc -O2 -std=c11 -Wall -o czet-lsp lsp/czet-lsp.c
./czet-lsp --help
```

Further details are provided in `lsp/README.md`. A registration
example for both the built-in Neovim client and vim-lsp is included as
comments in `ftplugin/czet.vim`.

## 3. Visual Studio Code (`vscode/`)

The extension contributes the `czet` language identifier for `.ct` and
`.czet` files, a TextMate grammar (`source.czet`), language
configuration (comments, brackets, auto-closing pairs), an LSP client
that launches the server from Section 2, and a `czet build` task backed
by the `czet` driver with the standard `$gcc` problem matcher. Refer to
`vscode/README.md` for complete instructions.

```sh
cd vscode && npm install
code --extensionDevelopmentPath=$PWD .
```

The server binary path defaults to `czet-lsp` resolved via `PATH`; it
may be overridden with the `czet.lsp.path` setting.
