import os
import sys

ROOT_HOME = os.path.dirname(__file__)

# Entry points whose executable in ROOT/bin has a different name
_EXECUTABLE_NAMES = {"root": "root.exe"}


def _exec_tool(tool: str) -> None:
    # Find the executable from the current installation directory
    bindir = os.path.join(ROOT_HOME, "bin")
    exe = os.path.join(bindir, _EXECUTABLE_NAMES.get(tool, tool))
    if not os.path.exists(exe):
        raise FileNotFoundError(
            f"Could not find '{os.path.basename(exe)}' executable in directory '{bindir}'. "
            "Something is wrong in the ROOT installation."
        )
    # Replace the current process, so that exit codes and signals behave exactly as for the
    # underlying executable, and so that it sees its real location in argv[0] (root-config
    # derives ROOTSYS from it)
    os.execv(exe, [exe] + sys.argv[1:])


def main() -> None:
    _exec_tool("root")


def dispatch() -> None:
    # Generic entry point for the executables in ROOT/bin: the one to run is given by the name
    # of the entry point script that was invoked
    _exec_tool(os.path.basename(sys.argv[0]))


if __name__ == "__main__":
    main()
