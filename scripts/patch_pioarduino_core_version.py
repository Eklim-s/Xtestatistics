"""Keep PEP 440 post releases compatible with platform Core requirements."""

from pathlib import Path

import platformio


def main():
    if ".post" not in platformio.__version__:
        return
    path = Path(platformio.__file__).parent / "platform" / "base.py"
    source = path.read_text(encoding="utf-8")
    original = "CORE_SEMVER = pepver_to_semver(__version__)"
    fixed = 'CORE_SEMVER = pepver_to_semver(__version__.replace(".post", "+post."))'
    if fixed in source:
        return
    if source.count(original) != 1:
        raise RuntimeError("Unsupported pioarduino Core version check")
    path.write_text(source.replace(original, fixed), encoding="utf-8")
    print("Fixed pioarduino Core post-release version comparison")


if __name__ == "__main__":
    main()
