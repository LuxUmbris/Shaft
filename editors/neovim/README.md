# Shaftls for Neovim

Install and automatically enable the built-in-LSP integration with:

```sh
python3 install.py --neovim
```

This writes only `lua/shaft/init.lua` and `plugin/shaft.lua` below the Neovim configuration directory (`~/.config/nvim` by default). Alternatively, add the repository `editors/neovim` directory to `runtimepath`, then configure built-in LSP:

```lua
require('shaft').setup({
  cmd = '/absolute/path/to/Shaft-bootstrap/shaftls/build/shaftls',
})
```

With an installed `shaftls` on PATH, omit `cmd`. The integration uses Neovim's built-in `vim.lsp.start`; no plugin manager or `nvim-lspconfig` dependency is required.
