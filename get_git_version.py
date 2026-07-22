# Liest die aktuelle Git-Version (letzter Tag + Commits seither + Kurzhash,
# z.B. "v1.2.1" auf einem sauberen Tag oder "v1.2.1-3-gabc1234-dirty" bei
# uncommitteten Aenderungen) und macht sie als GIT_VERSION-Makro im Code
# verfuegbar (siehe http_server.cpp) -- aktualisiert sich automatisch bei
# jedem Build, muss also nirgends manuell gepflegt werden.
Import("env")
import subprocess


def get_git_version():
    try:
        return (
            subprocess.check_output(
                ["git", "describe", "--tags", "--always", "--dirty"],
                stderr=subprocess.DEVNULL,
                cwd=env["PROJECT_DIR"],
            )
            .strip()
            .decode("utf-8")
        )
    except Exception:
        return "unknown"


env.Append(CPPDEFINES=[("GIT_VERSION", '\\"%s\\"' % get_git_version())])
