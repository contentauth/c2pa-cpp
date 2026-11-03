const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const source = fs.readFileSync(
  path.join(__dirname, "../docs/version-selector.js"), "utf8"
);

for (const base of ["https://contentauth.github.io/c2pa-cpp/", "file:///tmp/site/"]) {
  for (const prefix of ["", "versions/v0.26.14", "versions/v0.26.13"]) {
    test("selector links and navigation: " + base + prefix, function () {
      const versions = ["v0.26.14", "v0.26.13"];
      const tag = prefix ? prefix.split("/").pop() : versions[0];
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
        assert.equal(option.value, base + "versions/" + versions[index] + "/index.html");
        assert.equal(option.selected, versions[index] === tag);
        assert.equal(option.textContent, versions[index]);
      }
      select.value = select.children[1].value;
      select.change();
      assert.equal(navigated, base + "versions/v0.26.13/index.html");
    });
  }
}
