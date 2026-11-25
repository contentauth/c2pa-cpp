#!/usr/bin/env python3
"""Reconcile published releases using manifest-only optimistic API writes."""

import base64
import json
import os
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen

try:
    from .docs_versions import parse, published_stable, reconcile, version
except ImportError:
    from docs_versions import parse, published_stable, reconcile, version


MANIFEST = "api-docs/versions.json"
REPOSITORY = "contentauth/c2pa-cpp"


def api(method: str, path: str, token: str, data=None):
    request = Request(
        f"https://api.github.com/repos/{REPOSITORY}/{path}",
        data=json.dumps(data).encode() if data is not None else None,
        method=method,
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "Content-Type": "application/json",
        },
    )
    with urlopen(request, timeout=60) as response:
        return json.load(response)


def promote(token: str, release_tag: str = "", attempts: int = 6) -> bool:
    if not token:
        raise ValueError("A dedicated C2PA docs GitHub App installation token is required.")
    event_releases = []
    if release_tag:
        version(release_tag)
        release = api("GET", f"releases/tags/{release_tag}", token)
        if not published_stable(release) or release["tag_name"] != release_tag:
            raise ValueError(f"{release_tag} is not a published stable release.")
        event_releases.append(release)
    for attempt in range(attempts):
        current = api("GET", f"contents/{MANIFEST}?ref=main", token)
        config = parse(base64.b64decode(current["content"]).decode("utf-8"))
        releases = list(event_releases)
        page = 1
        while True:
            batch = api("GET", f"releases?per_page=100&page={page}", token)
            if not isinstance(batch, list):
                raise ValueError("GitHub releases response is not an array.")
            releases.extend(batch)
            if len(batch) < 100:
                break
            page += 1
        updated = reconcile(config, releases)
        if updated == config:
            print("Documentation versions are already current.")
            return False
        content = (json.dumps(updated, indent=2) + "\n").encode()
        try:
            # The only write endpoint is this exact manifest path on main.
            api("PUT", f"contents/{MANIFEST}", token, {
                "message": "docs: reconcile published API documentation versions",
                "branch": "main",
                "sha": current["sha"],
                "content": base64.b64encode(content).decode(),
            })
            print("Promoted API documentation versions on main.")
            return True
        except HTTPError as error:
            error.close()
            if error.code not in (409, 422) or attempt + 1 == attempts:
                raise
            time.sleep(attempt + 1)
    raise RuntimeError("Manifest promotion exhausted its retry budget.")


if __name__ == "__main__":
    promote(os.environ.get("DOCS_APP_TOKEN", ""), os.environ.get("RELEASE_TAG", ""))
