import shutil
from invoke import task

from ._utils import CPP_BUILD_DIR, PROJECT_ROOT, create_cpp_bin_link, task_title_message


def build_cpp(c, clean, debug, name, target_name, verbose):
    """Build a CMake executable target by name."""
    task_title_message(f"[TASK] Building {name} Executable...")

    if clean and CPP_BUILD_DIR.exists():
        print(f"Cleaning build directory: {CPP_BUILD_DIR}")
        shutil.rmtree(CPP_BUILD_DIR)

    CPP_BUILD_DIR.mkdir(parents=True, exist_ok=True)
    build_type = "Debug" if debug else "Release"
    print(f"Build Type: {build_type}")

    cmd_config = (
        f'cmake -S "{PROJECT_ROOT}" -B "{CPP_BUILD_DIR}" -G Ninja '
        f'-DCMAKE_BUILD_TYPE="{build_type}" '
    )
    c.run(cmd_config, pty=True)

    verbose_flag = "--verbose" if verbose else ""
    c.run(
        f'cmake --build "{CPP_BUILD_DIR}" --target {target_name} {verbose_flag}',
        pty=True,
    )
    create_cpp_bin_link(target_name)


@task
def hello(c, clean=False, debug=False, verbose=False):
    """Build the hello executable."""
    build_cpp(c, clean, debug, "Hello", "hello", verbose)


@task
def clean(c):
    """Clean all C++ build artifacts."""
    task_title_message("[TASK] Cleaning C++ build artifacts...")
    output_dir = PROJECT_ROOT / "output"
    if output_dir.exists():
        shutil.rmtree(output_dir)
