// GitHub Pages home only. Its text is translated here, not in web/i18n.js,
// because that file is built into the timer firmware and costs flash.
(() => {
  const zh = {
    eyebrow: '蓝牙控制 · Smart Timer V2',
    title: '用手机控制计时器',
    lead: '通过蓝牙开始、暂停和调整计时，无需 Wi-Fi 或账号。控制指令直接在浏览器和计时器之间传输。',
    open: '打开蓝牙应用',
    browser: '需要支持 Web Bluetooth 的浏览器，例如 Android 或桌面版 Chrome。',
    pairing: '先在计时器设置中开启蓝牙，然后用计时器的 Reset 按钮确认，完成一次配对。',
    install: '可添加到主屏幕，首次访问后可离线打开。',
    v1Title: '使用 V1 计时器？',
    v1: 'V1 不支持蓝牙。请在同一 Wi-Fi 网络中用浏览器打开它自己的页面。',
    safetyTitle: '使用前请阅读',
    safety: '此非官方的实验性固件控制一把可以束缚人的实体锁。无法解锁可能导致严重伤害或死亡。它按现状提供，不提供任何保证。请只用电池为计时器供电，切勿使用墙上插座。切勿只依赖软件或单一紧急方式脱离，切勿在独处时用于自我束缚。',
    safetyLink: '安全、保证与责任声明',
    source: '源代码',
    releases: '固件发布',
    hosted: '本网站也托管已签名的固件更新源。',
  };
  const nodes = [...document.querySelectorAll('[data-home]')];
  const english = new Map(nodes.map(node => [node, node.textContent]));
  const t = text => window.SmartTimerI18n?.text?.(text) ?? text;

  function applyLanguage() {
    const chinese = window.SmartTimerI18n?.language === 'zh-Hans';
    nodes.forEach(node => { node.textContent = chinese ? zh[node.dataset.home] : english.get(node); });
    applyTheme(document.documentElement.dataset.theme);
  }

  // Same session preference as the Bluetooth app.
  const THEME_STORAGE = 'smartTimerTheme';
  const themeButton = document.getElementById('themeButton');
  function applyTheme(theme) {
    const selected = theme === 'light' ? 'light' : 'dark';
    document.documentElement.dataset.theme = selected;
    themeButton.dataset.theme = selected;
    themeButton.setAttribute('aria-checked', String(selected === 'dark'));
    themeButton.setAttribute('aria-label', t(selected === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'));
    document.querySelector('meta[name="theme-color"]').setAttribute('content', selected === 'light' ? '#f7f3fa' : '#0b0710');
  }
  let theme = 'dark';
  try {
    const saved = sessionStorage.getItem(THEME_STORAGE);
    if (saved === 'light' || saved === 'dark') theme = saved;
    else if (window.matchMedia?.('(prefers-color-scheme: light)').matches) theme = 'light';
  } catch (_) { /* Default theme. */ }
  themeButton.addEventListener('click', () => {
    const selected = document.documentElement.dataset.theme === 'light' ? 'dark' : 'light';
    try { sessionStorage.setItem(THEME_STORAGE, selected); } catch (_) { /* This page only. */ }
    applyTheme(selected);
  });
  document.getElementById('languageButton').addEventListener('click', () => window.SmartTimerI18n?.toggleLanguage?.());
  window.addEventListener('smarttimer:languagechange', applyLanguage);
  applyTheme(theme);
  applyLanguage();
})();
