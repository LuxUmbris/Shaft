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

With the repository installer, the Node-based Shaft language server is installed alongside the plugin. It provides compiler diagnostics, underlined errors, diagnostic/symbol hover, definitions, and completion for language keywords, declared variables/functions/types, namespace members, and struct fields. The default Neovim setup uses `shaftc` from PATH for compiler diagnostics and Ollama at `http://127.0.0.1:11434` with model `spark-48k` for AI-assisted suggestions. Start Ollama and make that model available locally to use AI completion; regular language completion still works if Ollama is unavailable.

Diagnostics are shown as underlines, signs, and inline virtual text. Press `K` over an error to open its message (or over a symbol for documentation); press `<C-Space>` while typing for completion. Configure the model, compiler, stdlib, or command with `require('shaft').setup({ ollamaModel = 'spark-48k', compilerPath = '/path/to/shaftc' })`. No plugin manager, `nvim-lspconfig`, or completion plugin is required.
