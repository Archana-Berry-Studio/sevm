import argparse
import os

MODES = ("mochi", "node")


def write(path, text, force=False):
    if os.path.exists(path) and not force:
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return True


def scaffold(name, mode, force=False):
    root = ("mochi_modules" if mode == "mochi" else "node_modules")
    base = os.path.join(root, name)
    source_dir = "native" if mode == "mochi" else "src"
    guard = "MB_" + name.upper().replace("-", "_") + "_H"
    header = "#ifndef {0}\n#define {0}\n#include <stdint.h>\n\n#ifdef __cplusplus\nextern \"C\" {{\n#endif\n\nint {1}_init(void);\n\n#ifdef __cplusplus\n}}\n#endif\n\n#endif\n".format(guard, name)
    c_source = "#include \"../include/{0}_abi.h\"\n\nint {0}_init(void) {{\n    return 0;\n}}\n".format(name)
    cmake = "cmake_minimum_required(VERSION 3.16)\nproject({0} C)\n\nset(CMAKE_C_STANDARD 99)\nset(CMAKE_POSITION_INDEPENDENT_CODE ON)\n\nadd_library({0} SHARED {1}/{0}.c)\ntarget_include_directories({0} PUBLIC include)\nset_target_properties({0} PROPERTIES OUTPUT_NAME \"{0}_${{MOCHI_ARCH}}\" LIBRARY_OUTPUT_DIRECTORY \"${{CMAKE_SOURCE_DIR}}/lib\" RUNTIME_OUTPUT_DIRECTORY \"${{CMAKE_SOURCE_DIR}}/lib\")\n".format(name, source_dir)
    if mode == "node":
        manifest = "{{\n  \"name\": \"{0}\",\n  \"version\": \"0.1.0\",\n  \"type\": \"module\",\n  \"main\": \"script/index.js\",\n  \"license\": \"MIT\"\n}}\n".format(name)
        write(os.path.join(base, "package.json"), manifest, force)
    readme = "# {0}/{1}\n\nNode.js shim layer for `{1}`.\n\nMode: `{2}`.\n".format(root, name, mode)
    js = "import {{ cwd, resolveNativePath }} from '../../mochinode/index.js';\n\nexport const nativePath = resolveNativePath('{0}.dll', cwd());\n\nexport function init() {{\n    return nativePath;\n}}\n\nexport default {{ init, nativePath }};\n".format(name)
    test = "import {{ init }} from './index.js';\n\nif (typeof init !== 'function') throw new Error('missing init');\nprint('ok   {0} scaffold');\nprint('ALL_OK');\n".format(name)
    written = []
    os.makedirs(base, exist_ok=True)
    for directory in ("include", source_dir, "script", "lib"):
        os.makedirs(os.path.join(base, directory), exist_ok=True)
    targets = [
        (os.path.join(base, "README.md"), readme),
        (os.path.join(base, "CMakeLists.txt"), cmake),
        (os.path.join(base, "include", name + "_abi.h"), header),
        (os.path.join(base, source_dir, name + ".c"), c_source),
        (os.path.join(base, "script", "index.js"), js),
        (os.path.join(base, "script", "test.js"), test),
    ]
    for path, text in targets:
        if write(path, text, force):
            written.append(path)
    print("[OK] " + base + " (" + str(len(written)) + " new file)")
    for path in written:
        print("  + " + path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("mode", nargs="?", default="mochi", choices=MODES)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    scaffold(args.name, args.mode, args.force)


if __name__ == "__main__":
    main()
