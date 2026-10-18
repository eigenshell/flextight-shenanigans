// SPDX-License-Identifier: GPL-2.0-or-later
// Run a WASI program (ftweb_cli.wasm) under node with one directory
// opened at its own absolute path:  node wasi_run.mjs prog.wasm dir args...
import { readFile } from 'node:fs/promises';
import { WASI } from 'node:wasi';

const [wasm, dir, ...args] = process.argv.slice(2);
const wasi = new WASI({
    version: 'preview1',
    args: ['ftweb_cli', ...args],
    env: {},
    preopens: { [dir]: dir },
    returnOnExit: true,
});
const mod = await WebAssembly.compile(await readFile(wasm));
const inst = await WebAssembly.instantiate(mod, wasi.getImportObject());
process.exitCode = wasi.start(inst);
