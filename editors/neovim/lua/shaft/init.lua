-- Dependency-free Neovim builtin-LSP integration for Shaftls.
local M = {}

function M.setup(opts)
  opts = opts or {}
  vim.filetype.add({ extension = { shaft = 'shaft' } })
  vim.cmd('syntax enable')
  vim.diagnostic.config({
    underline = true,
    virtual_text = { spacing = 2, prefix = '●' },
    signs = true,
    severity_sort = true,
    float = { border = 'rounded', source = 'if_many' },
  })
  local group = vim.api.nvim_create_augroup('shaftls', { clear = true })
  vim.api.nvim_create_autocmd('FileType', {
    group = group,
    pattern = 'shaft',
    callback = function(args)
      vim.bo[args.buf].syntax = 'shaft'
      local source = debug.getinfo(1, 'S').source:sub(2)
      local bundled = vim.fn.fnamemodify(source, ':h') .. '/server/shaft-lsp.cjs'
      local command = opts.cmd
      if not command and vim.fn.filereadable(bundled) == 1 and vim.fn.exepath('node') ~= '' then
        command = { vim.fn.exepath('node'), bundled }
      end
      if not command then
        local shaftls = vim.fn.exepath('shaftls')
        if shaftls ~= '' then command = { shaftls } end
      end
      if not command or (type(command) == 'string' and command == '') then
        vim.notify('shaftls not found; pass cmd to require("shaft").setup.', vim.log.levels.WARN)
        return
      end
      if type(command) == 'string' then command = { command } end
      local compiler = opts.compilerPath or vim.fn.exepath('shaftc')
      local initializationOptions = {
        diagnostics = opts.diagnostics or 'onChange',
        ollamaEnabled = opts.ollamaEnabled ~= false,
        ollamaModel = opts.ollamaModel or 'spark-48k',
        ollamaUrl = opts.ollamaUrl or 'http://127.0.0.1:11434',
      }
      if compiler ~= '' then initializationOptions.compilerPath = compiler end
      if opts.stdlibPath then initializationOptions.stdlibPath = opts.stdlibPath end
      vim.lsp.start({
        name = 'shaftls',
        cmd = command,
        init_options = initializationOptions,
        root_dir = vim.fs.root(args.file, { 'Shaft.toml', '.git' }) or vim.fn.getcwd(),
        on_attach = function(client, buffer)
          vim.bo[buffer].omnifunc = 'v:lua.vim.lsp.omnifunc'
          if vim.lsp.completion and vim.lsp.completion.enable then
            vim.lsp.completion.enable(true, client.id, buffer, { autotrigger = true })
          end
          vim.keymap.set('i', '<C-Space>', '<C-x><C-o>', { buffer = buffer, desc = 'Shaft completion' })
          vim.keymap.set('n', 'K', function()
            local diagnostics = vim.diagnostic.get(buffer, { lnum = vim.api.nvim_win_get_cursor(0)[1] - 1 })
            if #diagnostics > 0 then
              vim.diagnostic.open_float(buffer, { scope = 'cursor', border = 'rounded', source = 'if_many' })
            elseif client.server_capabilities.hoverProvider then
              vim.lsp.buf.hover({ border = 'rounded' })
            end
          end, { buffer = buffer, desc = 'Show Shaft error or symbol documentation' })
        end,
      })
    end,
  })
end

return M
