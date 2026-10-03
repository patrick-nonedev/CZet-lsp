<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# VSCode (`vscode/`)

Gramática TextMate + `language-configuration` + cliente del mini-LSP
(`../lsp/czet-lsp.c`).

## Qué incluye

- `syntaxes/czet.tmGrammar.json` — `source.czet`: comentarios,
  `_Macro/_Reflect/_Operator/defer/constexpr`, `reflect_t`,
  fragmentos `ident/expr/type/…`, repetición `$(…)*|+|?`,
  campos `.kind/.size/…`, genéricos `f<T>`, `<utils/….h>` y sus APIs.
- `language-configuration.json` — comentarios, brackets, auto-close.
- `src/extension.js` — arranca `czet-lsp` vía `vscode-languageclient`;
  si no está, avisa pero la gramática sigue funcionando. Añade comando
  `CZet: Restart LSP` (`czet.restartLsp`) y task provider `czet build`
  (`czet -libc=… -static fichero -o salida`, con `$gcc` problemMatcher).
- `package.json` — `onLanguage:czet`, settings `czet.lsp.path`,
  `czet.lsp.args`, `czet.build.libc`.

## Instalar (dev)

```sh
# 1. LSP
cc -O2 -std=c11 -Wall -o /usr/local/bin/czet-lsp ../lsp/czet-lsp.c
# 2. Extensión
cd vscode
npm install   # instala vscode-languageclient
code --extensionDevelopmentPath=$PWD .
# o empaqueta: npx vsce package
```

Ajusta la ruta si no está en `PATH`:

```json
{ "czet.lsp.path": "/ruta/a/czet-lsp" }
```
