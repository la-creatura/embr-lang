// extension.js
// starts `embr lsp` for .embr files, so VS Code shows errors, go-to-definition, references, rename, hover, completion and the
// outline. the server is the embr program itself; this file only starts it and stops it.
//
// settings
//   embr.path   the embr program to run. default "embr" (it must be on your PATH), or give a full path
//
// run `npm install` in this folder once: it needs vscode-languageclient

const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

function start() {
    const cmd = vscode.workspace.getConfiguration("embr").get("path") || "embr";
    client = new LanguageClient(
        "embr",
        "embr",
        { command: cmd, args: ["lsp"] },
        { documentSelector: [{ scheme: "file", language: "embr" }, { scheme: "untitled", language: "embr" }] }
    );
    client.start().catch((e) => {
        vscode.window.showErrorMessage("embr: could not start `" + cmd + " lsp`: " + e.message + ". set embr.path to the embr program.");
    });
}

async function restart() {
    if (client) { try { await client.stop(); } catch (_) { /* it was not running */ } }
    start();
}

function activate(context) {
    start();
    context.subscriptions.push(
        vscode.commands.registerCommand("embr.restartServer", restart),
        vscode.workspace.onDidChangeConfiguration((e) => { if (e.affectsConfiguration("embr.path")) restart(); })
    );
}

function deactivate() {
    return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
