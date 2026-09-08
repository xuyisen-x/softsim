from pathlib import Path

from termcolor import cprint


PROJECT_ROOT = Path(__file__).resolve().parent.parent
CPP_BUILD_DIR = PROJECT_ROOT / "output" / "cpp_build"
CPP_BIN_DIR = CPP_BUILD_DIR / "bin"
BIN_DIR = PROJECT_ROOT / "output" / "bin"


def task_title_message(message):
    cprint(message, "green", attrs=["bold", "reverse"])


def error_message(message):
    cprint(message, "red", attrs=["bold"])


def create_cpp_bin_link(target_name):
    """Create a stable link to a CMake-built executable."""
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    target_path = CPP_BIN_DIR / target_name
    link_path = BIN_DIR / target_name

    if not target_path.exists():
        error_message(f"[ERROR] Target binary not found: {target_path}")
        return

    if link_path.exists() or link_path.is_symlink():
        link_path.unlink()
    link_path.symlink_to(target_path)
