"""Validated API documentation selection and monotonic release reconciliation."""

import json
import re
from pathlib import Path


NUMBER = r"(?:0|[1-9][0-9]*)"
TAG = re.compile(rf"v({NUMBER})\.({NUMBER})\.({NUMBER})")
LINE = re.compile(rf"({NUMBER})\.({NUMBER})")
ALIAS = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


def version(tag: str) -> tuple[int, int, int]:
    if not isinstance(tag, str) or not (match := TAG.fullmatch(tag)):
        raise ValueError(f"Expected an exact stable vX.Y.Z tag: {tag!r}")
    return tuple(map(int, match.groups()))


def line(tag: str) -> str:
    major, minor, _ = version(tag)
    return f"{major}.{minor}"


def validate(config: dict) -> dict:
    if not isinstance(config, dict) or set(config) != {"versions", "aliases", "retiredLines"}:
        raise ValueError("Manifest requires exactly versions, aliases and retiredLines.")
    versions, aliases, retired = (config[key] for key in ("versions", "aliases", "retiredLines"))
    if not isinstance(versions, list) or not versions:
        raise ValueError("Documentation versions must be a non-empty list.")
    ordered = [version(tag) for tag in versions]
    lines = [line(tag) for tag in versions]
    if len(set(lines)) != len(lines) or ordered != sorted(ordered, reverse=True):
        raise ValueError("Versions require unique major/minor lines in descending semver order.")
    if not isinstance(retired, list) or any(
        not isinstance(value, str) or not LINE.fullmatch(value) for value in retired
    ):
        raise ValueError("retiredLines must be an array of unprefixed X.Y lines.")
    if len(set(retired)) != len(retired) or set(lines) & set(retired):
        raise ValueError("Retired lines must be unique and not active.")
    if not isinstance(aliases, dict):
        raise ValueError("aliases must be an object.")
    for alias, target in aliases.items():
        if (
            not isinstance(alias, str) or not ALIAS.fullmatch(alias)
            or alias in (".", "..") or LINE.fullmatch(alias)
            or not isinstance(target, str) or target not in lines
            or (TAG.fullmatch(alias) and line(alias) != target)
        ):
            raise ValueError(f"Invalid documentation alias: {alias!r} -> {target!r}")
    if any(aliases.get(tag) != line(tag) for tag in versions):
        raise ValueError("Every selected exact tag must alias its canonical line.")
    return config


def parse(text: str) -> dict:
    def unique_keys(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate manifest key: {key}")
            result[key] = value
        return result
    return validate(json.loads(text, object_pairs_hook=unique_keys))


def load(path: Path) -> dict:
    return parse(path.read_text(encoding="utf-8"))


def published_stable(release: dict) -> bool:
    return (
        isinstance(release, dict)
        and isinstance(release.get("tag_name"), str)
        and TAG.fullmatch(release["tag_name"]) is not None
        and release.get("draft") is False
        and release.get("prerelease") is False
        and isinstance(release.get("published_at"), str)
        and bool(release["published_at"])
    )


def reconcile(config: dict, releases: list[dict]) -> dict:
    validate(config)
    selected = {line(tag): tag for tag in config["versions"]}
    retired = set(config["retiredLines"])
    floor = max(tuple(map(int, value.split("."))) for value in set(selected) | retired)
    aliases = dict(config["aliases"])
    for release in releases:
        if not published_stable(release):
            continue
        tag = release["tag_name"]
        key = line(tag)
        if key in retired or (key not in selected and version(tag)[:2] <= floor):
            continue
        previous = selected.get(key)
        if previous is None or version(tag) > version(previous):
            selected[key] = tag
    for key, tag in selected.items():
        aliases[tag] = key
    return validate({
        "versions": sorted(selected.values(), key=version, reverse=True),
        "aliases": dict(sorted(aliases.items())),
        "retiredLines": config["retiredLines"],
    })
