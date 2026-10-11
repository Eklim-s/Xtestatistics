"""Patch PlatformIO's bundled SCons to tolerate missing FortranCommon.

PlatformIO 6.2.0 ships SCons >= 4.5.x (via tool-scons packages) which
removed the FortranCommon module, but the ESP-IDF build helpers still
import it during the linking step.  This pre-build hook patches the
offending import to degrade gracefully.
"""

import sys
from pathlib import Path

Import("env")  # noqa: F821 -- provided by PlatformIO

# PlatformIO extracts tool-scons into the build cache.  The top-level
# scons.py lives next to the SCons package directory.
build_dir = Path(env.subst("$BUILD_DIR"))  # .pio/build/<env>
pio_dir = build_dir.parent.parent          # .pio

# Several scons versions may be in play; patch every one we can reach.
# Priority: the PlatformIO penv (which the outer SCons process uses),
# then any tool-scons extraction the build may reference.
for candidate in sorted(
    [Path.home() / ".platformio" / "penv" / "lib" / "python3" / d / "site-packages"
     for d in ("3.14", "3.13", "3.12", "3.11", "3.10")],
    reverse=True,
):
    link_common = candidate / "SCons" / "Tool" / "linkCommon" / "__init__.py"
    if not link_common.is_file():
        continue

    original = link_common.read_text()
    if "try:" in original and "ModuleNotFoundError" in original:
        continue  # already patched

    patched = original.replace(
        "    import SCons.Tool.FortranCommon\n\n    has_cplusplus = SCons.Tool.cxx.iscplusplus(source)\n    has_fortran = SCons.Tool.FortranCommon.isfortran(env, source)",
        "    try:\n        import SCons.Tool.FortranCommon as _fortran_common\n        has_fortran = _fortran_common.isfortran(env, source)\n    except (ImportError, ModuleNotFoundError):\n        has_fortran = False\n\n    has_cplusplus = SCons.Tool.cxx.iscplusplus(source)",
    )
    if patched == original:
        # Try a simpler regex-based approach
        import re
        patched = re.sub(
            r'(\s+)import SCons\.Tool\.FortranCommon\s*\n\s+has_cplusplus = SCons\.Tool\.cxx\.iscplusplus\(source\)\s*\n\s+has_fortran = SCons\.Tool\.FortranCommon\.isfortran\(env, source\)',
            r'\1try:\n\1    import SCons.Tool.FortranCommon as _fortran_common\n\1    has_fortran = _fortran_common.isfortran(env, source)\n\1except (ImportError, ModuleNotFoundError):\n\1    has_fortran = False\n\1\n\1has_cplusplus = SCons.Tool.cxx.iscplusplus(source)',
            original,
        )
    if patched != original:
        link_common.write_text(patched)
        print(f"PATCHED FortranCommon import in {link_common}", file=sys.stderr)