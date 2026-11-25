const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const source = fs.readFileSync(
  path.join(__dirname, "../docs/version-selector.js"), "utf8"
);

for (const base of ["https://contentauth.github.io/c2pa-cpp/", "file:///tmp/site/"]) {
  for (const prefix of ["", "versions/0.27", "versions/0.26"]) {
    test("selector links and navigation: " + base + prefix, function () {
      const versions = ["v0.27.0", "v0.26.19"];
      const lines = ["0.27", "0.26"];
      const tag = prefix ? versions[lines.indexOf(prefix.split("/").pop())] : versions[0];
      function element() {
        return {
          children: [],
          appendChild(child) { this.children.push(child); },
          setAttribute() {},
          addEventListener(event, listener) { this[event] = listener; }
        };
      }
      const container = element();
      let navigated;
      vm.runInNewContext(source, {
        URL,
        document: {
          currentScript: {
            src: new URL((prefix ? prefix + "/" : "") + "version-selector.js", base).href,
            dataset: { versions: JSON.stringify(versions), version: tag, prefix }
          },
          getElementById(id) {
            assert.equal(id, "projectalign");
            return container;
          },
          createElement: element
        },
        window: { location: { assign(url) { navigated = url; } } }
      });
      const select = container.children[0].children[0];
      assert.equal(select.children.length, versions.length);
      for (const [index, option] of select.children.entries()) {
        assert.equal(option.value, base + "versions/" + lines[index] + "/index.html");
        assert.equal(option.selected, versions[index] === tag);
        assert.equal(option.textContent, lines[index]);
      }
      select.value = select.children[1].value;
      select.change();
      assert.equal(navigated, base + "versions/0.26/index.html");
    });
  }
}
