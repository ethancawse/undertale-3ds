"""Small, best-effort UI preferences in the builder's work folder only."""
import json
import os
import tempfile


def load(work):
    try:
        with open(os.path.join(work, "ui-preferences.json"), encoding="utf-8") as source:
            value = json.load(source)
        result = {"boring": value.get("boring") is True} if isinstance(value, dict) else {"boring": False}
        if isinstance(value, dict) and isinstance(value.get('console'), str):
            try:
                from ftp_delivery import endpoint
                host, port = endpoint(value['console'])
                result['console'] = '%s:%d' % (host, port)
            except OSError:
                pass
        return result
    except (OSError, ValueError, UnicodeError, RecursionError):
        return {"boring": False}


def save(work, boring, console=None):
    temporary = None
    try:
        os.makedirs(work, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=work,
                                         prefix=".ui-preferences-", delete=False) as target:
            temporary = target.name
            value = load(work)
            value['boring'] = bool(boring)
            if console is not None:
                from ftp_delivery import endpoint
                host, port = endpoint(console)
                value['console'] = '%s:%d' % (host, port)
            json.dump(value, target)
            target.write("\n")
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, os.path.join(work, "ui-preferences.json"))
        return True
    except OSError:
        return False
    finally:
        if temporary:
            try:
                os.unlink(temporary)
            except OSError:
                pass
