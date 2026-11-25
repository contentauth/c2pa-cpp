import base64
import copy
import json
import unittest
from unittest.mock import patch
from urllib.error import HTTPError

from scripts import docs_versions as versions
from scripts import promote_docs_versions as promotion


def manifest():
    return {
        "versions": ["v0.27.0", "v0.26.19"],
        "aliases": {"v0.27.0": "0.27", "v0.26.19": "0.26"},
        "retiredLines": [],
    }


def release(tag, **changes):
    return dict(tag_name=tag, draft=False, prerelease=False,
                published_at="2026-11-19T00:00:00Z", **changes)


def content(config, sha="before"):
    return {"sha": sha, "content": base64.b64encode(json.dumps(config).encode()).decode()}


class DocsManifestTests(unittest.TestCase):
    def test_invalid_contracts_fail(self):
        mutations = [
            {"versions": []},
            {"versions": ["v0.26.19", "v0.27.0"]},
            {"versions": ["v0.27.0", "v0.27.1"]},
            {"versions": ["v0.27.0-rc.1"]},
            {"versions": ["v00.27.0"]},
            {"versions": [None]},
            {"versions": "v0.27.0"},
            {"retiredLines": ["0.26"]},
            {"retiredLines": ["v0.25"]},
            {"retiredLines": ["0.25", "0.25"]},
            {"retiredLines": {}},
            {"aliases": []},
            {"aliases": {"../escape": "0.26"}},
            {"aliases": {"0.26": "0.26"}},
            {"aliases": dict(manifest()["aliases"], **{"0.28": "0.26"})},
            {"aliases": {"v0.26.19": "0.27"}},
            {"aliases": {"v0.25.0": "0.25"}},
            {"aliases": {}},
            {"unexpected": True},
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                versions.validate(dict(manifest(), **mutation))
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            versions.parse('{"versions": [], "aliases": {"old": "0.26", "old": "0.27"}}')

    def test_patch_updates_are_monotonic_and_idempotent(self):
        original = manifest()
        releases = [release("v0.26.20"), release("v0.26.18"), release("v0.27.0")]
        updated = versions.reconcile(original, releases)
        self.assertEqual(updated["versions"], ["v0.27.0", "v0.26.20"])
        self.assertEqual(updated["aliases"]["v0.26.19"], "0.26")
        self.assertEqual(updated["aliases"]["v0.26.20"], "0.26")
        self.assertEqual(versions.reconcile(updated, releases), updated)
        self.assertEqual(original, manifest())

    def test_numeric_order_burst_releases_and_no_historical_backfill(self):
        updated = versions.reconcile(manifest(), [
            release("v0.29.2"), release("v0.28.0"), release("v0.29.10"),
            release("v0.25.99"), release("v0.26.100"), release("v1.0.0"),
        ])
        self.assertEqual(updated["versions"], [
            "v1.0.0", "v0.29.10", "v0.28.0", "v0.27.0", "v0.26.100",
        ])
        self.assertNotIn("v0.25.99", updated["aliases"])

    def test_numeric_minor_order_does_not_use_lexical_sort(self):
        config = {
            "versions": ["v1.9.9"], "aliases": {"v1.9.9": "1.9"}, "retiredLines": [],
        }
        updated = versions.reconcile(config, [release("v1.10.0"), release("v1.9.10")])
        self.assertEqual(updated["versions"], ["v1.10.0", "v1.9.10"])

    def test_retirement_cannot_be_undone_and_new_high_line_can_advance(self):
        config = manifest()
        config["versions"] = ["v0.26.19"]
        config["aliases"] = {"v0.26.19": "0.26"}
        config["retiredLines"] = ["0.27"]
        updated = versions.reconcile(config, [
            release("v0.27.99"), release("v0.28.0"), release("v0.25.99"),
        ])
        self.assertEqual(updated["versions"], ["v0.28.0", "v0.26.19"])
        self.assertEqual(updated["retiredLines"], ["0.27"])

    def test_unpublished_prerelease_and_invalid_metadata_are_ignored(self):
        candidates = [
            dict(release("v9.0.0"), draft=True),
            dict(release("v8.0.0"), prerelease=True),
            dict(release("v7.0.0"), published_at=None),
            release("v6.0.0-rc.1"), release("v05.0.0"), {},
        ]
        self.assertEqual(versions.reconcile(manifest(), candidates), manifest())


class DocsPromotionTests(unittest.TestCase):
    @patch.object(promotion, "api")
    def test_idempotent_rebuild_does_not_write(self, api):
        api.side_effect = [content(manifest()), []]
        self.assertFalse(promotion.promote("installation-token"))
        self.assertTrue(all(call.args[0] == "GET" for call in api.call_args_list))

    @patch.object(promotion.time, "sleep")
    @patch.object(promotion, "api")
    def test_conflict_reloads_current_manifest_and_respects_concurrent_retirement(self, api, sleep):
        pruned = copy.deepcopy(manifest())
        pruned["versions"] = ["v0.27.0"]
        pruned["aliases"] = {"v0.27.0": "0.27"}
        pruned["retiredLines"] = ["0.26"]
        api.side_effect = [
            content(manifest()), [release("v0.26.20")],
            HTTPError("", 409, "conflict", {}, None),
            content(pruned, "after"), [release("v0.26.20")],
        ]
        self.assertFalse(promotion.promote("installation-token"))
        self.assertEqual(api.call_count, 5)
        sleep.assert_called_once_with(1)

    @patch.object(promotion.time, "sleep")
    @patch.object(promotion, "api")
    def test_conflict_merges_burst_updates_against_latest_sha(self, api, sleep):
        concurrent = versions.reconcile(manifest(), [release("v0.28.0")])
        api.side_effect = [
            content(manifest()), [release("v0.29.0")],
            HTTPError("", 409, "conflict", {}, None),
            content(concurrent, "latest"), [release("v0.29.0")], {},
        ]
        self.assertTrue(promotion.promote("token"))
        body = api.call_args.args[3]
        self.assertEqual(body["sha"], "latest")
        self.assertEqual(json.loads(base64.b64decode(body["content"]))["versions"],
                         ["v0.29.0", "v0.28.0", "v0.27.0", "v0.26.19"])

    @patch.object(promotion, "api")
    def test_event_metadata_verified_and_manifest_only_write(self, api):
        # A just-published release need not yet appear in the releases list.
        api.side_effect = [release("v0.28.0"), content(manifest()), [], {}]
        self.assertTrue(promotion.promote("installation-token", "v0.28.0"))
        method, path, token, body = api.call_args.args
        self.assertEqual((method, path, token), (
            "PUT", "contents/api-docs/versions.json", "installation-token",
        ))
        self.assertEqual(body["branch"], "main")
        self.assertEqual(body["sha"], "before")
        updated = json.loads(base64.b64decode(body["content"]))
        self.assertEqual(updated["versions"][0], "v0.28.0")

    @patch.object(promotion, "api")
    def test_missing_credentials_and_unpublished_event_fail_without_writes(self, api):
        with self.assertRaisesRegex(ValueError, "installation token"):
            promotion.promote("")
        api.assert_not_called()
        api.return_value = dict(release("v0.28.0"), prerelease=True)
        with self.assertRaisesRegex(ValueError, "published stable"):
            promotion.promote("token", "v0.28.0")
        self.assertEqual(api.call_count, 1)

    @patch.object(promotion.time, "sleep")
    @patch.object(promotion, "api")
    def test_bounded_conflict_retry_and_permissions_fail_closed(self, api, sleep):
        for code, count in [(409, 6), (422, 6), (403, 1)]:
            with self.subTest(code=code):
                api.reset_mock()
                api.side_effect = [
                    item
                    for _ in range(count)
                    for item in (content(manifest()), [release("v0.28.0")],
                                 HTTPError("", code, "failure", {}, None))
                ]
                with self.assertRaises(HTTPError):
                    promotion.promote("token")
                self.assertEqual(api.call_count, count * 3)

    @patch.object(promotion, "api")
    def test_release_pagination_is_exhausted(self, api):
        api.side_effect = [content(manifest()), [release("v0.25.0")] * 100,
                           [release("v0.28.0")], {}]
        self.assertTrue(promotion.promote("token"))
        self.assertEqual(api.call_args_list[2].args[1], "releases?per_page=100&page=2")


if __name__ == "__main__":
    unittest.main()
