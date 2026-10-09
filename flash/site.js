/* SPDX-License-Identifier: Apache-2.0
 * 中文：官网界面预览与所选版本日志；不触发串口或固件刷写。
 * English: Screen previews and selected-release notes; no serial or flash operations.
 */
(() => {
  const screens = {
    home: {label: "首页", caption: "继续阅读、阅读记录和常用操作，打开就能找到。", asset: "home"},
    shelf: {label: "书架", styles: [
      {label: "亚克力书架", asset: "shelf-acrylic", caption: "真实封面搭配亚克力书架，收藏优先，用底部箭头翻页。"},
      {label: "深色书轨", asset: "shelf-dark", caption: "深色书轨托起封面，清楚呈现每一本书。"},
      {label: "列表书架", asset: "shelf-list", caption: "封面、书名、作者与阅读进度按行排好，每页四本。"}
    ]},
    reader: {label: "阅读", caption: "从《我与地坛》的一页开始，字体、书签和阅读设置就在工具栏里。", asset: "reader"},
    wifi: {label: "连接", caption: "清楚查看网络状态，连接后可以传书，也可以主动断开。", asset: "wifi"},
    lock: {label: "锁屏", styles: [
      {label: "阅读票根", asset: "lock-ticket", caption: "用阅读票根记下正在读的书、阅读进度和时长。"},
      {label: "图片壁纸", asset: "lock-wallpaper", caption: "导入喜欢的图片，等比例缩放裁切，铺满锁屏。"},
      {label: "书架拼贴", asset: "lock-collage", caption: "真实封面组成书架拼贴，搭配名字与藏书数量。"}
    ]}
  };
  const tabs = [...document.querySelectorAll("[data-screen]")];
  const screen = document.getElementById("screen-image");
  const styleGroup = document.getElementById("screen-styles");
  const dialog = document.getElementById("screen-dialog");
  const selectedStyle = {shelf: 0, lock: 0};
  if (screen) {
    function showPreview(item) {
      screen.src = `./assets/screens/${item.asset}.png`;
      screen.alt = `kiikoread ${item.label}界面预览`;
      document.getElementById("screen-caption").textContent = item.caption;
      document.getElementById("screen-open").setAttribute("aria-label", `放大${item.label}界面`);
    }
    function selectScreen(tab) {
      const key = tab.dataset.screen;
      const category = screens[key];
      for (const item of tabs) { item.setAttribute("aria-selected", String(item === tab)); item.tabIndex = item === tab ? 0 : -1; }
      styleGroup.replaceChildren();
      styleGroup.hidden = !category.styles;
      if (category.styles) {
        styleGroup.setAttribute("aria-label", `选择${category.label}样式`);
        category.styles.forEach((item, index) => {
          const button = Object.assign(document.createElement("button"), {type: "button", textContent: item.label});
          button.setAttribute("aria-pressed", String(index === selectedStyle[key]));
          button.addEventListener("click", () => {
            selectedStyle[key] = index;
            for (const choice of styleGroup.children) choice.setAttribute("aria-pressed", String(choice === button));
            showPreview(item);
          });
          styleGroup.append(button);
        });
        showPreview(category.styles[selectedStyle[key]]);
      } else showPreview(category);
      document.getElementById("screen-caption").setAttribute("aria-labelledby", tab.id);
      document.getElementById("screen-counter").textContent = `${String(tabs.indexOf(tab)+1).padStart(2,"0")} / ${String(tabs.length).padStart(2,"0")}`;
    }
    for (const tab of tabs) {
      tab.addEventListener("click", () => selectScreen(tab));
      tab.addEventListener("keydown", event => {
        const index = tabs.indexOf(tab);
        const next = {ArrowRight:(index+1)%tabs.length, ArrowLeft:(index+tabs.length-1)%tabs.length, Home:0, End:tabs.length-1}[event.key];
        if (next !== undefined) { event.preventDefault(); selectScreen(tabs[next]); tabs[next].focus(); }
      });
    }
    document.getElementById("screen-open").addEventListener("click", () => {
      document.getElementById("dialog-image").src = screen.src;
      document.getElementById("dialog-image").alt = screen.alt;
      document.getElementById("dialog-title").textContent = screen.alt;
      dialog.showModal();
    });
    document.getElementById("dialog-close").addEventListener("click", () => dialog.close());
    dialog.addEventListener("click", event => {
      const rect = dialog.getBoundingClientRect();
      if (event.target === dialog && (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom)) dialog.close();
    });
  }
  const archiveNotes = {
    "0.3.3-rc86": ["新版输入法，支持九宫格拼音与全键盘切换", "完善词库、联想与输入光标，支持长按连续删除", "优化按键反馈、候选词区域刷新及残影控制"],
    "0.3.3-rc85": ["优化阅读翻页显示效果", "优化蓝牙翻页器适配与按键识别", "隐藏没有广播名称的蓝牙设备"]
  };
  for (const [name, message] of [["loading", "正在读取所选版本的更新说明…"], ["unavailable", "所选版本暂不可用，请重选版本或刷新页面。"]]) {
    document.addEventListener(`kiikoread:release-${name}`, () => {
      document.getElementById("release-notes-title").textContent = "更新日志";
      document.getElementById("release-notes-list").replaceChildren(Object.assign(document.createElement("li"), {textContent:message}));
    });
  }
  document.addEventListener("kiikoread:release", async event => {
    const {manifest, latest} = event.detail;
    const list = document.getElementById("release-notes-list");
    document.getElementById("release-notes-title").textContent = `${manifest.version.split("-").pop()} · 更新日志`;
    let notes = archiveNotes[manifest.version] || ["历史版本说明请查看 GitHub Releases。"];
    if (latest) {
      list.replaceChildren(Object.assign(document.createElement("li"), {textContent:"正在读取最新更新说明…"}));
      try {
        const response = await fetch("./update.json", {cache:"no-store"});
        if (!response.ok) throw new Error("Unavailable");
        const feed = await response.json();
        if (feed.version !== manifest.version || typeof feed.notes !== "string") throw new Error("Version mismatch");
        notes = feed.notes.split(/\r?\n/).filter(Boolean).map(line => line.replace(/^\s*\d+[.、]\s*/, ""));
      } catch { notes = ["更新说明暂未加载，请刷新页面或查看 GitHub Releases。"]; }
    }
    if (document.getElementById("firmware-version").textContent !== manifest.version) return;
    notes = notes.filter(note => !/(微信读书|微读)/.test(note));
    list.replaceChildren(...notes.map(note => Object.assign(document.createElement("li"), {textContent:note})));
  });
})();
