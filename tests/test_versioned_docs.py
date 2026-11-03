import io
import json
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

from scripts import generate_versioned_api_docs as docs


class VersionedDocsTests(unittest.TestCase):
    def test_version_list_preserves_default_and_order(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "versions.json"
            config.write_text('{"versions": ["v0.26.14", "v0.26.13"]}')
            self.assertEqual(docs.load_versions(config), ["v0.26.14", "v0.26.13"])

    def test_invalid_version_lists_are_rejected(self):
        invalid = [
            {}, [], {"versions": "v0.26.14"},
            *({"versions": versions} for versions in [
                [], ["v0.26.14", "v0.26.14"], ["../../main"], ["--help"],
                ["main"], [26], [None], ["v0.26.14\n"],
            ]),
        ]
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "versions.json"
            for value in invalid:
                with self.subTest(value=value):
                    config.write_text(json.dumps(value), encoding="utf-8")
                    with self.assertRaises(ValueError):
                        docs.load_versions(config)

    @patch.object(docs.subprocess, "run")
    def test_published_release_is_verified(self, run):
        run.return_value = Mock(
            stdout=json.dumps({"tag_name": "v0.26.14", "draft": False})
        )
        docs.verify_release("v0.26.14")
        run.assert_called_once_with(
            ["gh", "api", "repos/contentauth/c2pa-cpp/releases/tags/v0.26.14"],
            check=True, stdout=subprocess.PIPE, text=True,
        )

    @patch.object(docs.subprocess, "run")
    def test_unpublished_or_mismatched_release_fails(self, run):
        for release in [
            {"tag_name": "v0.26.14", "draft": True},
            {"tag_name": "v0.26.13", "draft": False},
            {"tag_name": "v0.26.14"}, [],
        ]:
            with self.subTest(release=release):
                run.return_value = Mock(stdout=json.dumps(release))
                with self.assertRaises(ValueError):
                    docs.verify_release("v0.26.14")
        run.side_effect = subprocess.CalledProcessError(1, ["gh", "api"])
        with self.assertRaises(subprocess.CalledProcessError):
            docs.verify_release("v0.26.14")

    def make_archive(self, filename="include/c2pa.hpp", mode=stat.S_IFREG):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("include/", b"")
            archive.writestr("src/", b"")
            archive.writestr("README.md", b"# Release documentation\n")
            archive.writestr("src/CMakeLists.txt", b"do not execute")
            entry = zipfile.ZipInfo(filename)
            entry.external_attr = mode << 16
            archive.writestr(entry, b"// Tagged API source\n")
        return buffer.getvalue()

    @patch.object(docs.subprocess, "run")
    def test_export_uses_exact_tag_and_only_documentation_inputs(self, run):
        run.side_effect = [
            Mock(stdout="a" * 40 + "\n"),
            Mock(stdout=self.make_archive()),
        ]
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory)
            docs.export_sources(docs.ROOT, "v0.26.14", destination)
            self.assertEqual(
                (destination / "include/c2pa.hpp").read_text(),
                "// Tagged API source\n",
            )
            self.assertTrue((destination / "README.md").is_file())
            self.assertFalse((destination / "src/CMakeLists.txt").exists())
        self.assertEqual(
            run.call_args_list[0].args[0],
            ["git", "rev-parse", "--verify", "refs/tags/v0.26.14^{commit}"],
        )
        self.assertEqual(
            run.call_args_list[1].args[0],
            ["git", "archive", "--format=zip", "a" * 40, "include", "src", "README.md"],
        )

    @patch.object(docs.subprocess, "run")
    def test_unsafe_or_missing_headers_fail(self, run):
        for filename, mode in [
            ("include/../../escape.h", stat.S_IFREG),
            ("/include/api.h", stat.S_IFREG),
            ("other/api.h", stat.S_IFREG),
            ("include/..\\escape.h", stat.S_IFREG),
            ("include/C:/escape.h", stat.S_IFREG),
            ("include/api.h", stat.S_IFLNK),
            ("include/README.md", stat.S_IFREG),
        ]:
            with self.subTest(filename=filename), tempfile.TemporaryDirectory() as directory:
                run.side_effect = [
                    Mock(stdout="a" * 40),
                    Mock(stdout=self.make_archive(filename, mode)),
                ]
                with self.assertRaises(ValueError):
                    docs.export_sources(docs.ROOT, "v0.26.14", Path(directory))

    @patch.object(docs.subprocess, "run")
    def test_doxygen_header_and_configuration(self, run):
        def doxygen(command, cwd, **kwargs):
            if command[1] == "-w":
                (cwd / "header.html").write_text("<head>\n</head>", encoding="utf-8")
            else:
                (cwd / "output/html").mkdir(parents=True)
                (cwd / "output/html/index.html").touch()

        run.side_effect = doxygen
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "Doxyfile").write_text("INPUT = include src README.md\n")
            docs.build_html(stage, stage / "output", "v0.26.14", ["v0.26.14"], "")
            header = (stage / "header.html").read_text()
            self.assertIn('src="$relpath^version-selector.js"', header)
            self.assertIn('data-version="v0.26.14"', header)
            self.assertIn('data-prefix=""', header)
            self.assertIn("PROJECT_NUMBER = \"v0.26.14\"", run.call_args.kwargs["input"])
            self.assertIn("HTML_EXTRA_FILES", run.call_args.kwargs["input"])

    @patch.object(docs.subprocess, "run")
    def test_missing_generated_index_is_an_error(self, run):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "Doxyfile").write_text("")
            (stage / "header.html").write_text("<head></head>")
            with self.assertRaisesRegex(RuntimeError, "did not generate"):
                docs.build_html(stage, stage / "output", "v0.26.14", ["v0.26.14"], "")

    @patch.object(docs, "verify_release")
    @patch.object(docs, "export_sources")
    @patch.object(docs, "build_html")
    def test_site_build_and_failure_preserve_existing_output(self, build_html, export, verify):
        def build(stage, output, tag, versions, prefix):
            (output / "html").mkdir(parents=True)
            (output / "html/index.html").write_text(f"{tag}:{prefix}")

        build_html.side_effect = build
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "api-docs").mkdir()
            (root / "api-docs/versions.json").write_text(
                '{"versions": ["v0.26.14", "v0.26.13"]}'
            )
            (root / "Doxyfile").touch()
            (root / "docs").mkdir()
            for asset in ("doxygen-extra.css", "version-selector.js"):
                (root / "docs" / asset).touch()
            with patch.object(docs, "ROOT", root):
                site = docs.build_site()
                self.assertEqual((site / "index.html").read_text(), "v0.26.14:")
                for tag in ("v0.26.14", "v0.26.13"):
                    self.assertEqual(
                        (site / "versions" / tag / "index.html").read_text(),
                        f"{tag}:versions/{tag}",
                    )
                self.assertTrue((site / ".nojekyll").is_file())
                self.assertEqual(list((root / "api-docs/_build").iterdir()), [site])
                self.assertEqual(verify.call_count, 2)
                self.assertEqual(export.call_count, 2)
                self.assertEqual(build_html.call_count, 3)
                build_html.side_effect = RuntimeError("Doxygen build failed")
                with self.assertRaisesRegex(RuntimeError, "Doxygen build failed"):
                    docs.build_site()
                self.assertEqual((site / "index.html").read_text(), "v0.26.14:")
                self.assertEqual(list((root / "api-docs/_build").iterdir()), [site])


if __name__ == "__main__":
    unittest.main()
