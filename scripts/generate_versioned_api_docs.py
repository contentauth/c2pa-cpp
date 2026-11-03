#!/usr/bin/env python3
"""Build Doxygen documentation from explicitly selected published releases."""

import html
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[1]
TAG_PATTERN = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9][A-Za-z0-9.-]*)?")


def load_versions(path: Path) -> list[str]:
    config = json.loads(path.read_text(encoding="utf-8"))
    versions = config.get("versions") if isinstance(config, dict) else None
    if not isinstance(versions, list) or not versions:
        raise ValueError("Documentation versions must be a non-empty list.")
    for tag in versions:
        if not isinstance(tag, str) or not TAG_PATTERN.fullmatch(tag):
            raise ValueError("Documentation versions must be release tags such as v0.26.14.")
    if len(set(versions)) != len(versions):
        raise ValueError("Documentation versions must not contain duplicate tags.")
    return versions


def verify_release(tag: str) -> None:
    result = subprocess.run(
        ["gh", "api", f"repos/contentauth/c2pa-cpp/releases/tags/{tag}"],
        check=True, stdout=subprocess.PIPE, text=True,
    )
    release = json.loads(result.stdout)
    if (
        not isinstance(release, dict)
        or release.get("tag_name") != tag
        or release.get("draft") is not False
    ):
        raise ValueError(f"{tag} is not a published GitHub release.")


def export_sources(root: Path, tag: str, destination: Path) -> None:
    # Only export documentation inputs; never execute code from a release.
    commit = subprocess.run(
        ["git", "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}"],
        cwd=root, check=True, capture_output=True, text=True,
    ).stdout.strip()
    archive = subprocess.run(
        ["git", "archive", "--format=zip", commit, "include", "src", "README.md"],
        cwd=root, check=True, capture_output=True,
    ).stdout
    with zipfile.ZipFile(io.BytesIO(archive)) as sources:
        headers = 0
        for entry in sources.infolist():
            path = PurePosixPath(entry.filename)
            if (
                path.is_absolute()
                or ".." in path.parts
                or any("\\" in part or ":" in part for part in path.parts)
                or not path.parts
                or path.parts[0] not in ("include", "src", "README.md")
                or (path.parts[0] == "README.md" and len(path.parts) != 1)
                or stat.S_ISLNK(entry.external_attr >> 16)
            ):
                raise ValueError(f"Unsafe source archive in release {tag}.")
            if entry.is_dir() or (
                path != PurePosixPath("README.md")
                and path.suffix not in (".h", ".hpp", ".c")
            ):
                continue
            target = destination.joinpath(*path.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(sources.read(entry))
            if path.parts[0] == "include" and path.suffix in (".h", ".hpp"):
                headers += 1
        if not headers or not (destination / "README.md").is_file():
            raise ValueError(f"Release {tag} is missing API headers or README.md.")


def build_html(stage: Path, output: Path, tag: str, versions: list[str], prefix: str) -> None:
    header = stage / "header.html"
    subprocess.run(
        ["doxygen", "-w", "html", str(header), str(stage / "footer.html"),
         str(stage / "doxygen.css"), "Doxyfile"],
        cwd=stage, check=True,
    )
    template = header.read_text(encoding="utf-8")
    if "</head>" not in template:
        raise ValueError("Doxygen's default HTML header has no closing head element.")
    script = (
        '<script defer src="$relpath^version-selector.js"'
        f' data-version="{html.escape(tag, quote=True)}"'
        f' data-versions="{html.escape(json.dumps(versions), quote=True)}"'
        f' data-prefix="{html.escape(prefix, quote=True)}"></script>\n'
    )
    header.write_text(template.replace("</head>", script + "</head>"), encoding="utf-8")
    config = (stage / "Doxyfile").read_text(encoding="utf-8")
    config += (
        f'\nOUTPUT_DIRECTORY = "{output.as_posix()}"\n'
        f'PROJECT_NUMBER = "{tag}"\n'
        'HTML_HEADER = "header.html"\n'
        'HTML_EXTRA_FILES = "docs/version-selector.js"\n'
        'GENERATE_XML = NO\n'
    )
    subprocess.run(["doxygen", "-"], input=config, text=True, cwd=stage, check=True)
    if not (output / "html/index.html").is_file():
        raise RuntimeError(f"Doxygen did not generate an index page for {tag}.")


def build_site() -> Path:
    versions = load_versions(ROOT / "api-docs/versions.json")
    for tag in versions:
        verify_release(tag)

    build = ROOT / "api-docs/_build"
    build.mkdir(parents=True, exist_ok=True)
    output = build / "versioned"
    with tempfile.TemporaryDirectory(prefix="versioned-docs-", dir=build) as temporary:
        workspace = Path(temporary)
        site = workspace / "site"
        for tag in versions:
            stage = workspace / tag
            stage.mkdir()
            export_sources(ROOT, tag, stage)
            shutil.copy2(ROOT / "Doxyfile", stage)
            (stage / "docs").mkdir()
            for asset in ("doxygen-extra.css", "version-selector.js"):
                shutil.copy2(ROOT / "docs" / asset, stage / "docs")
            destination = stage / "output"
            build_html(stage, destination, tag, versions, f"versions/{tag}")
            shutil.copytree(destination / "html", site / "versions" / tag)

        # Rebuild the default so selector links also work at the existing root URL.
        default = workspace / versions[0]
        destination = default / "default-output"
        build_html(default, destination, versions[0], versions, "")
        shutil.copytree(destination / "html", site, dirs_exist_ok=True)
        (site / ".nojekyll").touch()
        if output.exists():
            shutil.rmtree(output)
        shutil.move(str(site), str(output))
    return output


if __name__ == "__main__":
    print(f"Versioned API docs generated at: {build_site()}")
