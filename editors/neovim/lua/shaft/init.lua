-- Dependency-free Neovim builtin-LSP integration for Shaftls.
local M = {}

function M.setup(opts)
  opts = opts or {}
  vim.filetype.add({ extension = { shaft = 'shaft' } })
  local group = vim.api.nvim_create_augroup('shaftls', { clear = true })
  vim.api.nvim_create_autocmd('FileType', {
    group = group,
    pattern = 'shaft',
    callback = function(args)
      local command = opts.cmd or vim.fn.exepath('shaftls')
      if command == '' then
        vim.notify('shaftls not found; pass cmd to require("shaft").setup.', vim.log.levels.WARN)
        return
      end
      vim.lsp.start({
        name = 'shaftls',
        cmd = { command },
        root_dir = vim.fs.root(args.file, { 'Shaft.build', '.git' }) or vim.fn.getcwd(),
      })
    end,
  })
end

return M
