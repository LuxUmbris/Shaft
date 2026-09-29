# Shaftls for Neovim

Install and automatically enable the built-in-LSP integration with:

```sh
python3 install.py --neovim
```

This writes `lua/shaft/init.lua`, `plugin/shaft.lua`, and `syntax/shaft.vim` below the Neovim configuration directory (`~/.config/nvim` by default). Alternatively, add the repository `editors/neovim` directory to `runtimepath`, then configure built-in LSP:

```lua
require('shaft').setup({
  cmd = '/absolute/path/to/Shaft-bootstrap/shaftls/build/shaftls',
})
```

With an installed `shaftls` on PATH, omit `cmd`. The integration enables the installed full Shaft syntax runtime for every Shaft buffer, then uses Neovim's built-in `vim.lsp.start` for semantic highlighting and live diagnostics; no plugin manager or `nvim-lspconfig` dependency is required.
