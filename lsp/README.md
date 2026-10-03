<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# `czet-lsp` — mini LSP para CZet

Single-file, C11, solo libc. Sin dependencias. Habla JSON-RPC 2.0 por
stdio con `Content-Length` (lo que esperan Neovim/VSCode/vim-lsp).

```sh
cc -O2 -std=c11 -Wall -o czet-lsp czet-lsp.c
# o con el propio CZet (es C válido):
czet -static czet-lsp.c -o czet-lsp
./czet-lsp --help
```

## Qué sabe de CZet

Leído de `docs/*.md` + `tests/*.czet`:

- `constexpr` (con funciones), genéricos `T f<T>(...)` /
  `struct box<T,U>` / `union cell<T>` instanciados `f<int>(...)`,
- `defer { ... };` y `defer;`,
- `_Reflect(T)` → `reflect_t` (`kind/size/align/name/count/elem/fields`,
  kinds 0..10),
- `_Macro NOMBRE<$p: frag> { (forma) { expansión } };` con fragmentos
  `ident expr type stmt block tokens number string literal` y repetición
  `$(...)*|+|?`,
- `_Operator(Tipo, "op")` con `a`/`b` como punteros (`+ - * / %`,
  comparaciones, `& | ^ << >>`, `+= … >>=`, `! ~`, `++ --`, `=`),
- stdlib `<utils/...>` sin flags (salvo `thread_pool.h` → `-pthread`):
  `arena str slice hashmap btree critbit save_memory double_buffering
  triple_buffering thread_pool aco server ipc`,
- extensiones `.ct` (preferida), `.czet` (aceptada), `.c` plano con
  `-std=czet` por defecto.

## Capacidades

| Método | Qué hace |
|---|---|
| `initialize` | `textDocumentSync:1`, completion, hover, definition, documentSymbol |
| `didOpen/didChange/didClose` | guarda buffer en memoria, republica diagnósticos |
| `completion` | filtra por prefijo: keywords CZet + fragmentos + `<utils/...>` + APIs |
| `hover` | markdown corta de `_Macro/_Reflect/_Operator/defer/constexpr/...` |
| `definition` | primera declaración de la palabra en el mismo fichero |
| `documentSymbol` | funcs, `struct/union/enum`, `typedef`, `_Macro`, `_Operator` |
| `publishDiagnostics` | balance `(){}[]` + reglas (`_Reflect(`, `_Operator(`, `defer {/;`, `_Macro … };`) + `czet -fsyntax-only` si el driver está en `PATH` |

Flags:

```text
czet-lsp [--log=/tmp/czet-lsp.log] [--no-czet] [--help]
```

- `--no-czet` desactiva la llamada al driver (solo chequeos internos,
  útil en tests o sin `czet` instalado).

## Probar a mano

```sh
printf 'Content-Length: 76\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  | ./czet-lsp --no-czet
```
