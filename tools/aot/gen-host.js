/*
 * gen-host.js — generate the SEVM AOT host C file (bytecode-embedded bundle)
 * without linking it.  The link step is done separately by
 * tools/aot/link-aot.sh against the parser-free engine profile.
 */
import { build } from '../../node_modules/mochijsnative/index.js';

async function main() {
    const r = await build({
        mode: 'aot',
        bundle: true,
        projectDir: 'E:/sevm/examples/sevm-app',
        entry: 'src/server.js',
        name: 'sevm_aot2',
    });
    console.log(JSON.stringify({
        cFile: r.cFile,
        out: r.out,
        bundleFiles: r.bundle ? r.bundle.files : null,
        modules: r.bundle ? r.bundle.modules : null,
    }, null, 2));
}

main().catch((e) => { console.error('gen-host failed:', e && e.message); process.exit(1); });
