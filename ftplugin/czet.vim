" SPDX-License-Identifier: GPL-3.0-or-later
" ftplugin/czet.vim — sensible defaults for CZet (.ct/.czet, and plain .c
" built with -std=czet).
if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

" Undo everything this file sets, as :help ftplugin demands.
let b:undo_ftplugin = 'setlocal commentstring< comments< cindent< shiftwidth< tabstop< expandtab< suffixesadd< makeprg< errorformat<'

setlocal commentstring=/*\ %s\ */
setlocal comments=s1:/*,mb:*,ex:*/,://
setlocal cindent shiftwidth=4 tabstop=4 expandtab
setlocal suffixesadd=.ct,.czet,.c,.h
" Build the current buffer with the driver (adjust -libc= for glibc):
if executable('czet')
  setlocal makeprg=czet\ -static\ %\ -o\ %:r
  setlocal errorformat=%f:%l:%c:\ %m,%f:%l:\ %m
endif
" LSP: with lsp/czet-lsp (from this repo) compiled, register it for `czet`.
" Neovim 0.11+ (built-in client):
"   vim.lsp.start({ name='czet-lsp', cmd={'/path/to/czet-lsp'},
"     \ root_dir=vim.fs.root(0, {'.git'}) })
" vim-lsp:
"   au User lsp_setup call lsp#register_server({
"     \ 'name': 'czet-lsp', 'cmd': ['/path/to/czet-lsp'],
"     \ 'allowlist': ['czet'] })
