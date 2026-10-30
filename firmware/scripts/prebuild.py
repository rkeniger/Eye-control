"""PlatformIO pre-build step: generates headers in the build directory.

- version_info.h: `git describe` and the build time, shown on the page and in /api/status.
- web_ui.h: web/index.html gzipped into a byte array, so the page ships inside the firmware.

Files are only rewritten when their content changes, so unchanged builds stay incremental.
"""

import gzip
import subprocess
from datetime import datetime
from pathlib import Path

Import("env")  # noqa: F821 (provided by PlatformIO)

project = Path(env.subst("$PROJECT_DIR"))  # noqa: F821
out = Path(env.subst("$BUILD_DIR")) / "generated"  # noqa: F821
out.mkdir(parents=True, exist_ok=True)


def write_if_changed(path: Path, text: str):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def git_version() -> str:
    try:
        return subprocess.check_output(
            ["git", "describe", "--always", "--dirty", "--tags"], cwd=project, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


built = datetime.now().astimezone().strftime("%Y-%m-%d %H:%M:%S %z")
write_if_changed(
    out / "version_info.h",
    f'#pragma once\n#define FIRMWARE_VERSION "{git_version()}"\n#define FIRMWARE_BUILT "{built}"\n',
)

html = gzip.compress((project / "web" / "index.html").read_bytes(), compresslevel=9, mtime=0)
rows = [", ".join(f"0x{b:02x}" for b in html[i : i + 20]) for i in range(0, len(html), 20)]
write_if_changed(
    out / "web_ui.h",
    "#pragma once\n#include <Arduino.h>\n// Generated from web/index.html by scripts/prebuild.py.\n"
    f"const uint8_t INDEX_HTML_GZ[] PROGMEM = {{\n{chr(10).join('    ' + r + ',' for r in rows)}\n}};\n",
)

env.Append(CPPPATH=[str(out)])  # noqa: F821
