from invoke import task

from ._utils import BIN_DIR, PROJECT_ROOT, error_message, task_title_message


def run_cpp(c, name, target_name):
    """Run a CMake-built executable target by name."""
    task_title_message(f"[TASK] Running {name} Executable...")

    binary_path = BIN_DIR / target_name
    if not binary_path.exists():
        error_message(f"[ERROR] Binary not found: {binary_path}")
        error_message(f"Please build the '{target_name}' target first.")
        return

    with c.cd(PROJECT_ROOT):
        c.run(f'"{binary_path}"', pty=True)


@task
def hello(c):
    """Run the hello executable."""
    run_cpp(c, "Hello", "hello")
