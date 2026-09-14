" Optional Vim 9 builtin-LSP setup. Set g:shaftls_cmd to an absolute server path.
if exists('g:loaded_shaft_lsp')
  finish
endif
let g:loaded_shaft_lsp = 1

function! s:shaftls_command() abort
  if exists('g:shaftls_cmd')
    return g:shaftls_cmd
  endif
  return exepath('shaftls')
endfunction

if exists('*LspAddServer')
  augroup shaft_lsp
    autocmd!
    autocmd FileType shaft call s:StartShaftLsp()
  augroup END
  function! s:StartShaftLsp() abort
    let l:command = s:shaftls_command()
    if empty(l:command)
      echohl WarningMsg | echom 'shaftls not found; set g:shaftls_cmd.' | echohl None
      return
    endif
    call LspAddServer([{'name': 'shaftls', 'filetype': ['shaft'], 'path': l:command, 'args': []}])
  endfunction
endif
