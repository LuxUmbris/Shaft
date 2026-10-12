'use strict';

const path = require('node:path');

function resolveShaftlsCommand(configuredPath, extensionPath) {
  if (configuredPath) return { executable: configuredPath, arguments: [] };
  return { executable: process.env.SHAFT_NODE || 'node', arguments: [path.join(extensionPath, 'server', 'shaft-lsp.cjs')] };
}

module.exports = { resolveShaftlsCommand };
