"""Small, best-effort UI preferences in the builder's work folder only."""
import json
import os
import tempfile


def load(work):
    try:
        with open(os.path.join(work, "ui-preferences.json"), encoding="utf-8") as source:
            value = json.load(source)
        return {"boring": value.get("boring") is True} if isinstance(value, dict) else {"boring": False}
    except (OSError, ValueError, UnicodeError, RecursionError):
        return {"boring": False}


def save(work, boring):
    temporary = None
    try:
        os.makedirs(work, exist_ok=True)
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=work,
                                         prefix=".ui-preferences-", delete=False) as target:
            temporary = target.name
            json.dump({"boring": bool(boring)}, target)
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
