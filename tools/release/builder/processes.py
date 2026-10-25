"""Console-free Windows children; callers keep piping output to their log."""
import subprocess
import sys


def hidden_options(*, group=False):
    if sys.platform != 'win32':
        return {}
    flags = subprocess.CREATE_NO_WINDOW
    if group:
        flags |= subprocess.CREATE_NEW_PROCESS_GROUP
    return {'creationflags': flags}
