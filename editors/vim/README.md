# Shaftls for Vim

Install the Vim runtime automatically with:

```sh
python3 install.py --vim
```

It writes only Shaft-owned files under `~/.vim/{ftdetect,syntax,plugin}` (or `~/vimfiles` on Windows). Alternatively, add `editors/vim` to `runtimepath` and configure the server path before opening a `.shaft` file:

```vim
let g:shaftls_cmd = '/absolute/path/to/Shaft-bootstrap/shaftls/build/shaftls'
```

The included syntax file highlights ordinary Shaft source comprehensively: keywords, built-in and lowercase custom types in declarations/type positions, function declarations and calls, variables and properties, numbers, strings, character literals, comments, operators, delimiters, imports, macro calls, `@!config` conditions, red `@error` directives, and inline-assembly instructions, registers, operands, immediates, and comments. The LSP plugin uses Vim's built-in LSP API when available to publish live structural and unterminated-string diagnostics; older Vim versions retain syntax/filetype support.
