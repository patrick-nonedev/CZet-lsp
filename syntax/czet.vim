" SPDX-License-Identifier: GPL-3.0-or-later
" syntax/czet.vim — highlighting for CZet (GNU C23 plus compiler extensions).
"
" Layered on top of stock syntax/c.vim, so the full GNU C23 base (C23
" keywords, __attribute__, __extension__, __asm__, Tenex/decimal floats,
" deja-vu comments/strings/numbers/preprocessor) comes from Vim itself.
" This file only ADDS: a C23 safety net for older runtimes, GNU builtins,
" and the CZet extensions (_Macro, _Reflect, _Operator, defer, constexpr,
" reflect_t, generics, <utils/...>). See the CZet docs/*.md.
"
" CZet is a strict superset of C: every definition below is additive and
" never clears what c.vim installed (no `syntax clear` here — that was an
" old bug that wiped the C base).
if exists('b:current_syntax')
  finish
endif

" Full C + GNU base from Vim itself. Afterwards b:current_syntax == 'c',
" which we release so this file can claim 'czet' at the end.
runtime! syntax/c.vim
if exists('b:current_syntax')
  unlet b:current_syntax
endif

syn case match
syn sync minlines=50

" ---- C23 safety net (stock Vim 9 c.vim already knows these; this keeps
" ---- older runtimes working. Later definitions win, links stay sane.) ----
syn keyword czetC23Type typeof typeof_unqual alignas _BitInt bool nullptr nullptr_t
syn keyword czetC23Kw constexpr static_assert thread_local alignof _Alignof
syn keyword czetBoolean true false

" ---- GNU keywords and builtins (stock c.vim gates its own GNU block on
" ---- g:c_gnu, so CZet defines them unconditionally — zero side effects
" ---- on plain C buffers). ----
syn keyword czetGnuKw __attribute__ __extension__ __asm__ __asm __volatile__ __volatile __signed__ __inline__ __const__ __restrict__ __noreturn__ __auto_type __typeof__
syn match czetGnuFunc '__builtin_[A-Za-z_][A-Za-z0-9_]*'

" ---- CZet extensions ----
" _Macro / _Reflect / _Operator are contextual in the compiler (a bare
" `int _Reflect = 42;` still parses), but highlighting them always as
" keywords is what an editor user expects.
syn keyword czetMacroKw _Macro
syn keyword czetReflectKw _Reflect _Operator
syn keyword czetDefer defer
syn keyword czetConstexpr constexpr
" reflect_t / reflect_field_t are primitives (valid with or without `struct`).
syn keyword czetReflectType reflect_t reflect_field_t

" #include <utils/...> — highlight the header as a type, on top of c.vim's
" own include handling (ours is defined later, so it wins for includes).
syn match czetInclude '#\s*include\>' nextgroup=czetIncFile skipwhite skipempty
syn match czetIncFile '"[^"]*"' contained
syn match czetIncFile '<[^>]*>' contained contains=czetUtilsHeader
syn match czetUtilsHeader 'utils/[a-z_]\+\.h' contained

" Macro parameters `$name`, repetition `$(` and `*`/`+`/`?`, fragments.
" Plain (non-contained) matches: they must be visible without a parent group.
syn match czetMacroParam '\$[A-Za-z_][A-Za-z0-9_]*'
syn match czetMacroRep '\$('
syn match czetMacroRep '[*+?]'
syn keyword czetMacroFrag ident expr type stmt block tokens number string literal

" Generic definition/call `name<...>` — highlight only the name when it is
" directly followed by `<` (lookahead, no offset hacks).
syn match czetGenericCall '\<[A-Za-z_][A-Za-z0-9_]*\ze\s*<[^=]'

" reflect_t members: .kind .size .align .name .count .elem .fields .offset .type
syn match czetReflectField '\.\(kind\|size\|align\|name\|count\|elem\|fields\|offset\|type\)\>'

" Most used <utils/...> APIs (visual anchors, not an exhaustive list).
syn keyword czetUtilsFunc arena_init arena_alloc arena_calloc arena_used arena_reset arena_free
syn keyword czetUtilsFunc str_init str_push_cstr str_push_char str_push_fmt str_cstr str_clear str_free
syn keyword czetUtilsFunc slice_make slice_len slice_at slice_sub
syn keyword czetUtilsFunc hmap_init hmap_put hmap_get hmap_del hmap_count hmap_free
syn keyword czetUtilsFunc btree_new btree_set btree_get btree_delete btree_min btree_max btree_count btree_free
syn keyword czetUtilsFunc critbit0_insert critbit0_contains critbit0_delete critbit0_clear critbit0_allprefixed
syn keyword czetUtilsFunc sm_save sm_load sm_save_r sm_load_r
syn keyword czetUtilsFunc dbuf_init dbuf_write dbuf_read tbuf_init tbuf_write tbuf_read
syn keyword czetUtilsFunc pool_create pool_submit pool_try_submit pool_wait pool_destroy
syn keyword czetUtilsFunc co_sched_init co_spawn co_run co_sched_free co_yield
syn keyword czetUtilsFunc tcp_listen tcp_connect tcp_accept tcp_send_all tcp_recv_all tcp_port tcp_close
syn keyword czetUtilsFunc ipc_server_open ipc_server_close ipc_client_open ipc_client_close ipc_send_all ipc_recv_all

hi def link czetC23Type Type
hi def link czetC23Kw Keyword
hi def link czetBoolean Boolean
hi def link czetGnuKw Keyword
hi def link czetGnuFunc Function
hi def link czetInclude Include
hi def link czetIncFile String
hi def link czetUtilsHeader Type
hi def link czetMacroKw Define
hi def link czetReflectKw Keyword
hi def link czetDefer Keyword
hi def link czetConstexpr Keyword
hi def link czetReflectType Type
hi def link czetMacroParam Identifier
hi def link czetMacroFrag Special
hi def link czetMacroRep Special
hi def link czetGenericCall Function
hi def link czetReflectField Special
hi def link czetUtilsFunc Function

let b:current_syntax = 'czet'
