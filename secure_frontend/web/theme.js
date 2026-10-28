// Strata built-in chat page: the theme before the first paint (upstream's index.html does this
// inline; this server's page policy allows no inline script). The saved choice, else the
// system's. Only "strata.theme" is ever stored here; the API key never is (app.js keeps it for
// this tab only).
(function () {
  "use strict";
  var SLOT = "strata.theme";
  function system() {
    return window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  function saved() {
    try {
      var value = window.localStorage.getItem(SLOT);
      return value === "dark" || value === "light" ? value : null;
    } catch (e) { return null; }
  }
  document.documentElement.setAttribute("data-theme", saved() || system());
  window.StrataTheme = {
    get: function () { return document.documentElement.getAttribute("data-theme") === "dark" ? "dark" : "light"; },
    set: function (theme) {
      var value = theme === "dark" ? "dark" : "light";
      document.documentElement.setAttribute("data-theme", value);
      try { window.localStorage.setItem(SLOT, value); } catch (e) { /* storage blocked: this page only */ }
    }
  };
}());
