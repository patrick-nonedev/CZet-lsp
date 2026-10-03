" SPDX-License-Identifier: GPL-3.0-or-later
" ftdetect/czet.vim — detect *.ct (preferred) and *.czet as CZet.
"
" NOTE: use a forced `set filetype=` (not `setfiletype`) so this wins over
" any generic built-in rule regardless of autocmd ordering.
augroup czet_ftdetect
  autocmd!
  autocmd BufNewFile,BufRead *.ct,*.czet set filetype=czet
augroup END
" Plain .c files are intentionally NOT claimed (they still build with
" -std=czet by default, but their filetype stays `c`). To treat every
" .c file as CZet, uncomment the next line:
" autocmd BufNewFile,BufRead *.c set filetype=czet
