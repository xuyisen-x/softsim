# AGENTS.md

## Project overview

SoftSim is a research hardware simulator written primarily in C++.

- Use C++23 for all project C++ targets.
- The top-level `CMakeLists.txt` owns project-wide configuration, third-party dependencies, and module integration.
- Each directory under `modules/` owns its targets in a local `CMakeLists.txt`.
- Add new modules to the top-level build with `add_subdirectory(modules/<module>)`.
- Third-party sources populated by CMake live under `ext/`; do not edit them for project fixes.

## Required tools

- CMake 3.20 or newer
- A compiler with C++23 support
- Ninja
- uv
- Python 3.12 or newer

Python dependencies are declared in `pyproject.toml` and locked in `uv.lock`. Use `uv run` instead of invoking the project environment's Python or Invoke executable directly.

## Canonical build and run commands

Use the Invoke tasks as the normal development interface:

```bash
# List available tasks
uv run invoke --list

# Release build
uv run invoke build.hello

# Debug build
uv run invoke build.hello --debug

# Clean build
uv run invoke build.hello --clean

# Verbose clean Debug build
uv run invoke build.hello --clean --debug --verbose

# Run an already-built target
uv run invoke run.hello

# Remove all C++ build artifacts
uv run invoke build.clean
```

When changing build behavior, keep Invoke as a thin wrapper around CMake. Compiler selection, compile options, linking, output directories, and platform/toolchain configuration belong in CMake rather than `tasks/build.py`.

## Invoke task organization

- `tasks/build.py` configures CMake and builds named targets.
- `tasks/run.py` runs previously built executables.
- `tasks/_utils.py` contains shared paths, messages, and binary-link creation.
- `tasks/__init__.py` registers the `build` and `run` task collections.

For a new executable target, reuse `build_cpp(...)` and `run_cpp(...)` and add matching tasks to `tasks/build.py` and `tasks/run.py`.

## Build outputs

Invoke uses this layout:

```text
output/
├── cpp_build/          # CMake/Ninja build tree
│   ├── bin/            # CMake executable outputs
│   └── lib/            # CMake library outputs
└── bin/                # Stable links created by Invoke
```

For `hello`:

- CMake output: `output/cpp_build/bin/hello`
- Stable Invoke link: `output/bin/hello`

The `output/`, `.venv/`, `.vscode-build/`, and `ext/` directories are generated or external content and must not be committed.

## Direct CMake equivalents

Use these only when diagnosing below the Invoke layer:

```bash
cmake -S . -B output/cpp_build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build output/cpp_build --target hello
./output/cpp_build/bin/hello
```

A direct CMake build does not create the stable `output/bin/<target>` link; `tasks/build.py` does that after a successful build.

## CMake requirements and constraints

The top-level configuration enforces:

```cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

Release builds enable CMake interprocedural optimization (LTO). Project targets use `-Wall -Wextra`.

The project currently does not use C++ named modules, so the top-level CMake configuration must retain:

```cmake
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
```

Without it, newer CMake/Ninja versions may run Homebrew `clang-scan-deps` for third-party C++23 targets. On macOS this previously failed while scanning yaml-cpp with `fatal error: 'time.h' file not found`. Remove this setting only when named modules are intentionally introduced and the module-scanning toolchain is configured and tested.

## VS Code and clangd

The editor build is separate from the Invoke build:

- Invoke builds in `output/cpp_build`.
- VS Code CMake Tools builds in `.vscode-build`.
- `.vscode/settings.json` enables `CMAKE_EXPORT_COMPILE_COMMANDS` for the editor build.
- `.clangd` reads `.vscode-build/compile_commands.json`.

`uv run invoke build.hello` does not refresh the editor compilation database. After changing CMake targets or compile settings, run `CMake: Configure` in VS Code and restart clangd if its diagnostics remain stale.

## Verification expectations

After changing CMake or build tasks, verify with:

```bash
uv run invoke build.hello --clean
uv run invoke run.hello
```

The expected program output is:

```text
Hello World
```

If a clean full build is unnecessarily expensive for a narrow source-only change, an incremental `uv run invoke build.hello` is acceptable, but report which validation was performed.
