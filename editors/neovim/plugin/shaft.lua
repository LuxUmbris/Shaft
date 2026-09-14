-- Start Shaftls automatically for Shaft buffers after the runtime is installed.
if vim.g.loaded_shaftls_plugin then
  return
end
vim.g.loaded_shaftls_plugin = true
require('shaft').setup()
