# SoftSim 开发指南

SoftSim 是一个主要使用 C++23 编写的研究型硬件模拟器。
编辑与文档约定见 [AGENTS.md](../AGENTS.md)，内存模块说明见 [RamulatorWrapper](ramulator_wrapper.md)。

## 目录结构

| 路径             | 用途                                       |
| ---------------- | ------------------------------------------ |
| `CMakeLists.txt` | 项目级配置、第三方依赖和模块集成。         |
| `modules/`       | 各模块的源代码及各自的 `CMakeLists.txt`。  |
| `dram_configs/`  | 完整的 HBM2、HBM3 YAML 配置。              |
| `docs/`          | 架构、配置、使用和开发说明。               |
| `tasks/`         | Invoke 构建和运行入口。                    |
| `ext/`           | CMake 下载的第三方源码，不直接修改或提交。 |

## 所需工具

- CMake 3.25 或更新版本。
- 支持 C++23 的编译器和标准库，包括 `std::print` 和 C++23 ranges。
- Ninja。
- uv。
- Python 3.12 或更新版本。

Python 依赖声明在 [pyproject.toml](../pyproject.toml) 中，版本由 [uv.lock](../uv.lock) 锁定。
使用 `uv run`，不要直接调用项目虚拟环境中的 Python 或 Invoke 可执行文件。

## 构建和运行

日常构建和运行统一在项目根目录使用 `uv run invoke`。假设有一个名为 `xxxxx` 的目标。

```bash
# 列出可用任务
uv run invoke --list

# Release 构建
uv run invoke build.xxxxx

# Debug 构建
uv run invoke build.xxxxx --debug

# 清理后重新构建
uv run invoke build.xxxxx --clean

# 清理后进行 Debug 构建，并显示详细日志
uv run invoke build.xxxxx --clean --debug --verbose

# 运行已经构建的目标
uv run invoke run.xxxxx

# 删除所有 C++ 构建产物
uv run invoke build.clean
```

`--clean` 删除共享的 `output/cpp_build` 构建目录后重新配置，影响所有目标的构建缓存。
`build.clean` 删除整个 `output/`，包括稳定入口链接。运行任务不会自动构建目标。

### Invoke 任务组织与新增目标

- [tasks/build.py](../tasks/build.py)：配置 CMake 并构建指定目标。
- [tasks/run.py](../tasks/run.py)：运行已经构建的可执行文件。
- [tasks/\_utils.py](../tasks/_utils.py)：共享路径、消息输出和可执行文件链接创建逻辑。
- [tasks/**init**.py](../tasks/__init__.py)：注册 `build` 和 `run` 任务集合。

新增模块或可执行目标时：

1. 在 `modules/<module>/CMakeLists.txt` 中定义构建目标。
2. 在顶层 `CMakeLists.txt` 中通过 `add_subdirectory(modules/<module>)` 加入新模块。
3. 新增可执行目标时，在 `tasks/build.py`、`tasks/run.py` 中添加对应任务，复用 `build_cpp(...)` 和 `run_cpp(...)`。

Invoke 保持为 CMake 的轻量封装。编译器选择、编译选项、链接、输出目录和平台／工具链配置
应放在 CMake 中，而不是 `tasks/build.py` 中。

### 构建产物

```text
output/
├── cpp_build/          # CMake/Ninja 构建目录
│   ├── bin/            # CMake 输出的可执行文件
│   └── lib/            # CMake 输出的库文件
└── bin/                # Invoke 创建的稳定入口链接
```

对于可执行目标 `<target>`，CMake 输出为 `output/cpp_build/bin/<target>`，
Invoke 在构建成功后创建稳定入口 `output/bin/<target>`。

`output/`、`.venv/`、`.vscode-build/` 和 `ext/` 属于生成内容或第三方内容，不得提交到版本库。

### 直接调用 CMake

仅在需要排查 Invoke 下层的构建问题时使用，将 `xxxxx` 替换为实际目标名：

```bash
cmake -S . -B output/cpp_build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build output/cpp_build --target xxxxx
./output/cpp_build/bin/xxxxx
```

直接调用 CMake 不会创建 `output/bin/<target>` 稳定入口；该链接由 Invoke 构建任务创建。

## CMake 要求与约束

所有项目 C++ 目标使用 C++23，顶层配置必须保留：

```cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
```

Release 构建启用 CMake 过程间优化（LTO），项目目标使用 `-Wall -Wextra`。

项目目前不使用 C++ 命名模块，因此顶层配置还必须保留：

```cmake
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
```

缺少此设置时，较新的 CMake/Ninja 可能调用 Homebrew 的 `clang-scan-deps` 扫描第三方 C++23 目标。
此前在 macOS 上扫描 yaml-cpp 时曾出现 `fatal error: 'time.h' file not found`。
只有在明确引入命名模块，并完成模块扫描工具链的配置和验证后，才可移除此设置。

## VS Code 与 clangd

编辑器构建与 Invoke 构建相互独立：

- Invoke 使用 `output/cpp_build`。
- VS Code CMake Tools 使用 `.vscode-build`。
- `.vscode/settings.json` 为编辑器构建启用 `CMAKE_EXPORT_COMPILE_COMMANDS`。
- `.clangd` 读取 `.vscode-build/compile_commands.json`。

Invoke 构建不会刷新编辑器的编译数据库。
修改 CMake 目标或编译选项后，在 VS Code 中执行 `CMake: Configure`；
若诊断仍未更新，再重启 clangd。

## 验证规则

- 执行与改动对应的验证，除非用户明确要求不构建或不测试。仅修改文档时，检查路径和内容与代码的一致性即可。
- 如实报告实际执行的验证，不把测试命令或预期输出当作测试结果。
