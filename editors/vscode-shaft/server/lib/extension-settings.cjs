'use strict';

function explicitValue(inspected = {}) {
  return inspected.workspaceFolderValue ?? inspected.workspaceValue ?? inspected.globalValue ?? '';
}

module.exports = { explicitValue };
