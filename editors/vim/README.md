# Shaftls for Vim

Install the Vim runtime automatically with:

```sh
python3 install.py --vim
```

It writes only Shaft-owned files under `~/.vim/{ftdetect,syntax,plugin}` (or `~/vimfiles` on Windows). Alternatively, add `editors/vim` to `runtimepath` and configure the server path before opening a `.shaft` file:

```vim
let g:shaftls_cmd = '/absolute/path/to/Shaft-bootstrap/shaftls/build/shaftls'
```

The included syntax file highlights Shaft imports, macro calls, and `@config`, `@asm`, and `@end` metaprogramming directives. The LSP plugin uses Vim's built-in LSP API when available; older Vim versions retain syntax/filetype support.
