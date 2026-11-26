#!/usr/bin/env python3
"""Populate per-module structure for each mochi_module (native-first layout).

For every mochi_modules/<mod>/ dir that has a flat sibling DLL and a source in
native/<mod>.c, this fills the canonical layout requested by the user:

    mochi_modules/<mod>/
      README.md          (already written by gen_manifests)
      manifest.json      (already written by gen_manifests)
      CMakeLists.txt     (this script)
      native/<mod>.c     (copy of E:/sevm/native/<mod>.c)
      lib/<mod>_x86_64.dll   (copy of the flat binary, arch-suffixed)
      include/<mod>.h    (minimal public ABI header)
      script/index.js    (thin wrapper: loadNative + re-export)

The engine loaders are updated separately to prefer the per-module lib/ binary.
"""
import os, shutil, sys

ROOT = r"E:/sevm"
NATIVE = os.path.join(ROOT, "native")
MOCHI = os.path.join(ROOT, "mochi_modules")
ARCH = "x86_64"

# export lists measured at runtime (ground truth) — used for the ABI header comment
EXPORTS = {
    "mochicrypto": ["hash", "hmac", "pbkdf2", "randomHex", "timingSafeEqual",
                    "webcrypto", "subtle", "createHmac", "randomBytes"],
    "mochifs": ["appendText", "copyFile", "cwd", "exists", "mkdir", "readText",
                "readdir", "remove", "rename", "stat", "writeText"],
    "mochizlib": ["deflateHex", "inflateHex", "deflateRawHex", "inflateRawHex",
                  "gzipHex", "gunzipHex", "crc32Hex"],
    "mochinode_net": ["httpServer", "accept", "read", "write", "close"],
    "mochinode_dynalib": ["load", "call", "free", "get"],
    "mochiws": ["connect", "httpGet", "WebSocket"],
    "mochiwasmy": ["ping", "load", "call"],
    "mochinode_sqlite": ["open", "exec", "query", "close"],
    "mochijsnative": ["eval", "evalFile", "register"],
}

HEADER_TMPL = """#ifndef MB_{UPPER}_H
#define MB_{UPPER}_H
/*
 * {name} — MochiBerry native-first module.
 *
 * ABI contract (see mochiberry/module-loader-contract.md):
 *   - exports the single entry point mbjs_init_module(MBJSContext*, const char*)
 *   - registers C functions via NewCModule / AddModuleExportList / SetModuleExportList
 *   - C functions that throw MUST return MBJS_EXCEPTION (never MBJS_UNDEFINED)
 *
 * Runtime symbols (MBJS_*) are resolved through mbjs_runtime_resolve() from
 * <root>/native/mbjs_runtime.h at load time; do NOT link against mbjs.exe.
 *
 * Exported symbols (measured): {exports}
 */
#include <stdint.h>

typedef struct MBJSContext MBJSContext;
typedef struct MBJSModuleDef MBJSModuleDef;

MBJSModuleDef *mbjs_init_module(MBJSContext *ctx, const char *name);

#endif /* MB_{UPPER}_H */
"""

CMAKE_TMPL = """cmake_minimum_required(VERSION 3.16)
project(mb_{name} C)

# {name} — native-first MochiBerry module.
# Builds lib/{name}_{arch}.dll (or .so / .dylib) from native/{name}.c.
# The plugin resolves all MBJS_* runtime symbols from the host engine at load
# time, so it must NOT link against mbjs.exe (importing it fails with error 126
# on Windows). Add -IE:/bcf/mochiberry (or the equivalent engine include dir)
# so mbjs_runtime.h / MBJS headers are visible.

set(NAME {name})
set(ARCH {arch})
set(INC_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}/include")
# point this at the engine headers:
set(MBJS_INC "$ENV{{MBJS_INC}}")
if(NOT MBJS_INC)
  set(MBJS_INC "E:/bcf/mochiberry")
endif()

add_library(mb_${{NAME}} SHARED native/${{NAME}}.c)
target_include_directories(mb_${{NAME}} PRIVATE ${{MBJS_INC}} ${{INC_DIR}})

set_target_properties(mb_${{NAME}} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${{NAME}}_${{ARCH}}"
  LIBRARY_OUTPUT_DIRECTORY "${{CMAKE_CURRENT_SOURCE_DIR}}/lib"
  RUNTIME_OUTPUT_DIRECTORY "${{CMAKE_CURRENT_SOURCE_DIR}}/lib"
  ARCHIVE_OUTPUT_DIRECTORY "${{CMAKE_CURRENT_SOURCE_DIR}}/lib")

if(MSVC)
  target_compile_definitions(mb_${{NAME}} PRIVATE MBJS_BUILD)
  target_link_libraries(mb_${{NAME}} PRIVATE bcrypt.lib winhttp.lib)
endif()
if(WIN32 AND NOT MSVC)
  target_link_libraries(mb_${{NAME}} PRIVATE bcrypt winhttp)
endif()
"""

WRAPPER_TMPL = """/*
 * {name} — thin JS binding over the native {arch} binary.
 * Loads lib/{name}_{arch}.dll via the engine's native loader and re-exports
 * the C ABI. Prefer native-first: keep heavy work in C, keep this file a
 * 1:1 binding (no object-wrapping layer).
 */
import {{ loadNative }} from '../../mochinode/index.js';

let _m = null;
async function ensure() {{
  if (!_m) _m = await loadNative('{name}.dll');
  return _m;
}}

export async function init() {{ return ensure(); }}
export function isNativeReady() {{ return _m != null; }}

export function __exports() {{
  return _m ? Object.keys(_m) : [];
}}

export default {{
  load: () => ensure(),
  exports: () => (_m ? Object.keys(_m) : []),
}};
"""


def pop_one(mod):
    d = os.path.join(MOCHI, mod)
    if not os.path.isdir(d):
        print(f"SKIP {mod}: no per-module dir"); return False
    src = os.path.join(NATIVE, mod + ".c")
    dll = os.path.join(MOCHI, mod + ".dll")
    made = []
    # native/
    nd = os.path.join(d, "native"); os.makedirs(nd, exist_ok=True)
    if os.path.isfile(src):
        shutil.copy2(src, os.path.join(nd, mod + ".c")); made.append("native/" + mod + ".c")
    # lib/
    ld = os.path.join(d, "lib"); os.makedirs(ld, exist_ok=True)
    if os.path.isfile(dll):
        shutil.copy2(dll, os.path.join(ld, f"{mod}_{ARCH}.dll")); made.append(f"lib/{mod}_{ARCH}.dll")
    # include/
    idir = os.path.join(d, "include"); os.makedirs(idir, exist_ok=True)
    hp = os.path.join(idir, mod + ".h")
    if not os.path.isfile(hp):
        ex = ", ".join(EXPORTS.get(mod, []))
        open(hp, "w", encoding="utf-8", newline="\n").write(
            HEADER_TMPL.format(UPPER=mod.upper(), name=mod, exports=ex))
        made.append("include/" + mod + ".h")
    # CMakeLists.txt
    cp = os.path.join(d, "CMakeLists.txt")
    if not os.path.isfile(cp):
        open(cp, "w", encoding="utf-8", newline="\n").write(
            CMAKE_TMPL.format(name=mod, arch=ARCH)); made.append("CMakeLists.txt")
    # script/
    sd = os.path.join(d, "script"); os.makedirs(sd, exist_ok=True)
    sp = os.path.join(sd, "index.js")
    if not os.path.isfile(sp):
        open(sp, "w", encoding="utf-8", newline="\n").write(
            WRAPPER_TMPL.format(name=mod, arch=ARCH)); made.append("script/index.js")
    print(f"[OK] {mod}: " + ", ".join(made))
    return True


def main():
    mods = sorted([m for m in os.listdir(MOCHI) if os.path.isdir(os.path.join(MOCHI, m))])
    n = 0
    for m in mods:
        if pop_one(m):
            n += 1
    print(f"DONE populated {n} modules")


if __name__ == "__main__":
    main()
