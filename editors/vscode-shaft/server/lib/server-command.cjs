'use strict';

function resolveShaftlsCommand(configuredPath) {
  return { executable: configuredPath || 'shaftls', arguments: [] };
}

module.exports = { resolveShaftlsCommand };
