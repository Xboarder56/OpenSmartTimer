// Browser-only appearance preferences. No timer/network/storage API calls.
// Keep relay/spa/appearance.js identical for the separately deployed relay.
(() => {
  const key = 'smartTimerPalette';
  const palettes = ['purple', 'slate', 'teal'];
  const root = document.documentElement;
  const backgrounds = {
    purple: {light: '#f7f3fa', dark: '#0b0710'},
    slate: {light: '#f3f5f8', dark: '#0c1017'},
    teal: {light: '#f1f8f7', dark: '#081513'},
  };
  const valid = value => palettes.includes(value) ? value : 'purple';
  function updateChrome() {
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.setAttribute('content', backgrounds[valid(root.dataset.palette)][root.dataset.theme === 'light' ? 'light' : 'dark']);
  }
  function apply(value) {
    const selected = valid(value);
    root.dataset.palette = selected;
    document.querySelectorAll('[data-palette-choice]').forEach(button => {
      button.setAttribute('aria-pressed', String(button.dataset.paletteChoice === selected));
    });
    updateChrome();
  }
  let saved = 'purple';
  try { saved = localStorage.getItem(key); } catch (_) { /* In-memory fallback. */ }
  apply(saved);
  document.querySelectorAll('[data-palette-choice]').forEach(button => {
    button.addEventListener('click', () => {
      const selected = valid(button.dataset.paletteChoice);
      apply(selected);
      button.focus({preventScroll: true});
      try { localStorage.setItem(key, selected); } catch (_) { /* Still works this session. */ }
    });
  });
  // Light/dark remains owned by each app's existing accessible toggle.
  new MutationObserver(updateChrome).observe(root, {attributes: true, attributeFilter: ['data-theme']});
  window.addEventListener('storage', event => {
    if (event.key === key || event.key === null) apply(event.newValue);
  });
  document.querySelectorAll('.appearance-menu').forEach(menu => {
    menu.addEventListener('keydown', event => {
      if (event.key === 'Escape') { menu.open = false; menu.querySelector('summary').focus(); }
      if (event.key === 'Tab') setTimeout(() => {
        if (!menu.contains(document.activeElement)) menu.open = false;
      }, 0);
    });
    document.addEventListener('click', event => { if (!menu.contains(event.target)) menu.open = false; });
    menu.addEventListener('focusout', event => {
      // Safari blurs the summary without focusing a clicked palette button.
      // A missing destination must not hide that button before its click.
      if (event.relatedTarget && !menu.contains(event.relatedTarget)) menu.open = false;
    });
  });
})();
