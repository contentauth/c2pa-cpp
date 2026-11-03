(function () {
  "use strict";

  const script = document.currentScript;
  const versions = JSON.parse(script.dataset.versions);
  const assets = new URL(".", script.src);
  const root = new URL(script.dataset.prefix ? "../../" : "./", assets);
  const container = document.getElementById("projectalign");
  if (!container) {
    throw new Error("Cannot find the Doxygen project title for the version selector.");
  }

  const label = document.createElement("label");
  label.className = "docs-version-selector";
  label.textContent = "Version: ";
  const select = document.createElement("select");
  select.setAttribute("aria-label", "API documentation version");
  for (const tag of versions) {
    const option = document.createElement("option");
    option.value = new URL("versions/" + tag + "/index.html", root).href;
    option.textContent = tag;
    option.selected = tag === script.dataset.version;
    select.appendChild(option);
  }
  select.addEventListener("change", function () {
    window.location.assign(select.value);
  });
  label.appendChild(select);
  container.appendChild(label);
})();
