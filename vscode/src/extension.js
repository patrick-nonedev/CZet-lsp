// SPDX-License-Identifier: GPL-3.0-or-later
// src/extension.js — cliente mínimo VSCode -> czet-lsp (plain JS, sin build).
// Requiere `vscode-languageclient` (ver package.json). Si czet-lsp no está,
// la extensión sigue dando gramática + build task; el LSP solo avisa.

const vscode = require('vscode');
let client = null;

function getCfg() {
  const c = vscode.workspace.getConfiguration('czet');
  return {
    path: c.get('lsp.path', 'czet-lsp'),
    args: c.get('lsp.args', []),
  };
}

async function startClient(ctx) {
  let LC;
  try {
    LC = require('vscode-languageclient/node');
  } catch (e) {
    vscode.window.showWarningMessage(
      'CZet: falta vscode-languageclient (npm i). Gramática activa sin LSP.'
    );
    return;
  }
  const { path, args } = getCfg();
  const serverOptions = { command: path, args: args || [], options: {} };
  const clientOptions = {
    documentSelector: [{ scheme: 'file', language: 'czet' }],
    synchronize: { fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{ct,czet}') },
  };
  client = new LC.LanguageClient('czet-lsp', 'CZet LSP', serverOptions, clientOptions);
  try {
    await client.start();
  } catch (e) {
    vscode.window.showErrorMessage(
      `CZet: no se pudo lanzar "${path}". Compila lsp/czet-lsp.c o ajusta czet.lsp.path. (${e.message})`
    );
    client = null;
  }
}

function activate(ctx) {
  startClient(ctx);

  ctx.subscriptions.push(
    vscode.commands.registerCommand('czet.restartLsp', async () => {
      if (client) {
        await client.stop();
        client = null;
      }
      await startClient(ctx);
    })
  );

  // Task: compilar fichero actual con el driver czet.
  ctx.subscriptions.push(
    vscode.tasks.registerTaskProvider('czet', {
      provideTasks() {
        const cfg = vscode.workspace.getConfiguration('czet');
        const libc = cfg.get('build.libc', 'musl');
        const ed = vscode.window.activeTextEditor;
        if (!ed) return [];
        const file = ed.document.fileName;
        const out = file.replace(/\.(ct|czet|c)$/, '');
        const t = new vscode.Task(
          { type: 'czet', file },
          vscode.TaskScope.WorkspaceFolder,
          `czet ${file}`,
          'czet',
          new vscode.ShellExecution(`czet -libc=${libc} -static "${file}" -o "${out}"`)
        );
        t.group = vscode.TaskGroup.Build;
        t.problemMatchers = ['$gcc'];
        return [t];
      },
      resolveTask(t) { return t; },
    })
  );
}

async function deactivate() {
  if (client) {
    await client.stop();
    client = null;
  }
}

module.exports = { activate, deactivate };
