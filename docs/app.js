(() => {
  const shots = {
    docking: {
      src: "assets/screenshots/obs-docking.png",
      alt: "OBS 工作区全景截图",
      caption: "OBS 工作区全景：B站直播面板、OBS 原生开始直播按钮与互动/榜单面板统一停靠在 OBS 内",
      title: "OBS 工作区全景 · 停靠布局与按钮联动",
      desc: "把「B站直播」和弹幕、榜单面板直接停靠在 OBS 四周，布局会随着 OBS 自动保存，下次打开自动还原；左下角直接点 OBS 原本的「开始直播」即可开播。",
      bullets: [
        "支持吸附在 OBS 任意边缘、合并成标签页，或者拖出来独立置顶悬浮",
        "外接副屏拔掉后，原本在副屏上的悬浮窗会自动回到当前屏幕内",
        "开播状态变化时，OBS 原生「开始 / 停止直播」按钮文字和可用状态同步联动"
      ]
    },
    room: {
      src: "assets/screenshots/room-dock.png",
      alt: "B站直播主控面板截图",
      caption: "B站直播主面板：扫码登录、改标题、联动搜索分区、预览 4:3 封面并打开三个互动窗口",
      title: "B站直播主面板 · 开播准备一步搞定",
      desc: "登录账号后自动读取直播间、当前标题、分区与封面审核状态。在这里就能完成改标题、选分区、换封面，以及打开三个实时互动窗口或解除鼠标穿透。",
      bullets: [
        "登录会话由当前 Windows 用户加密保护，本场推流密钥只留在内存里无需复制",
        "改完标题停手约 1 秒或移开焦点即自动同步，失败时也会保留你的草稿",
        "切换父分类只显示对应子分区，子分区支持直接打字搜索筛选"
      ]
    },
    crop: {
      src: "assets/screenshots/cover-crop.png",
      alt: "4:3 封面裁切弹窗截图",
      caption: "4:3 封面裁切弹窗：支持鼠标拖动构图、滑块和滚轮缩放，裁完直接上传",
      title: "内置 4:3 封面裁切器 · 选图直接上传",
      desc: "鼠标移到主面板封面上点击选图，就能在裁切窗口里拖动位置、滚轮或滑块缩放构图，点击「保存并上传」自动裁成标准 4:3（720 × 540）并更新封面。",
      bullets: [
        "自带九宫格辅助构图线，拖到边缘会自动对齐，不会留白边",
        "支持滚轮随鼠标位置缩放、方向键微调，点「重置」一键恢复默认",
        "点「取消」不会上传图片，放心试构图"
      ]
    },
    panels: {
      src: "assets/screenshots/realtime-panels.png",
      alt: "互动 / 观众 / 大航海三面板截图",
      caption: "三大实时面板：直播互动（弹幕/礼物/SC/进房）、房间观众榜与大航海榜单",
      title: "互动 / 观众 / 大航海 · 三个实用小窗口",
      desc: "这些面板共用一条弹幕连接，配色跟随 OBS 主题。礼物随弹幕一起显示，拖动分隔线可以调整弹幕和进场动态各自的高度。直播互动浮窗支持鼠标穿透。",
      bullets: [
        "直播互动：支持字号调节、鼠标悬停暂停滚动、拖动调整上下栏高度",
        "房间观众：支持在线榜（贡献值 / 进房时间）、亲密榜及多时间段切换",
        "大航海：支持周榜、月榜、陪伴榜切换与「加载更多」，并保护神秘用户隐私"
      ]
    },
    moderation: {
      src: "assets/screenshots/moderation-notice.png",
      alt: "超管违规整改提醒弹窗截图",
      caption: "超管违规整改提醒：即使互动面板隐藏或开启鼠标穿透，OBS 主窗口也会及时弹窗并响铃提醒",
      title: "超管警告与整改提醒 · 关键消息不漏看",
      desc: "当平台下发超管警告、内容整改提醒或切断通知时，OBS 主窗口会立刻弹出提示框并播放系统提示音，不会因为关掉了弹幕窗口或开了穿透而错过。",
      bullets: [
        "直接显示平台下发的原因说明与桌面端操作建议",
        "点击「查看详情」或文字里的链接即可打开浏览器查看官方违规记录",
        "确认关闭仅收起本地提示，不会影响正常的开停播操作"
      ]
    }
  };

  const tabs = document.querySelectorAll("#galleryTabs .gallery-tab");
  const img = document.getElementById("galleryImage");
  const title = document.getElementById("galleryTitle");
  const desc = document.getElementById("galleryDesc");
  const bullets = document.getElementById("galleryBullets");

  if (tabs.length && img && title && desc && bullets) {
    tabs.forEach((tab) => {
      tab.addEventListener("click", () => {
        const key = tab.getAttribute("data-shot");
        const data = shots[key];
        if (!data) return;

        tabs.forEach((t) => {
          t.classList.remove("active");
          t.setAttribute("aria-selected", "false");
        });
        tab.classList.add("active");
        tab.setAttribute("aria-selected", "true");

        img.src = data.src;
        img.alt = data.alt;
        img.setAttribute("data-caption", data.caption);
        title.textContent = data.title;
        desc.textContent = data.desc;
        bullets.innerHTML = "";
        data.bullets.forEach((text) => {
          const li = document.createElement("li");
          li.textContent = text;
          bullets.appendChild(li);
        });
      });
    });
  }

  // Lightbox
  const lightbox = document.getElementById("lightbox");
  const lightboxImg = document.getElementById("lightboxImg");
  const lightboxCaption = document.getElementById("lightboxCaption");
  const lightboxClose = document.getElementById("lightboxClose");

  if (lightbox && lightboxImg && lightboxCaption && lightboxClose) {
    const openLightbox = (sourceImg) => {
      lightboxImg.src = sourceImg.src;
      lightboxImg.alt = sourceImg.alt;
      lightboxCaption.textContent = sourceImg.getAttribute("data-caption") || sourceImg.alt;
      lightbox.classList.add("open");
      lightbox.setAttribute("aria-hidden", "false");
    };

    const closeLightbox = () => {
      lightbox.classList.remove("open");
      lightbox.setAttribute("aria-hidden", "true");
    };

    document.querySelectorAll("img[data-lightbox]").forEach((el) => {
      el.addEventListener("click", () => openLightbox(el));
    });

    lightboxClose.addEventListener("click", closeLightbox);
    lightbox.addEventListener("click", (e) => {
      if (e.target === lightbox) closeLightbox();
    });
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape" && lightbox.classList.contains("open")) closeLightbox();
    });
  }

  // Mobile nav toggle
  const navToggle = document.getElementById("navToggle");
  const navLinks = document.getElementById("navLinks");
  if (navToggle && navLinks) {
    navToggle.addEventListener("click", () => {
      navLinks.classList.toggle("open");
    });
    navLinks.querySelectorAll("a").forEach((a) => {
      a.addEventListener("click", () => navLinks.classList.remove("open"));
    });
  }

  // Copy code snippet
  document.querySelectorAll("[data-copy]").forEach((btn) => {
    btn.addEventListener("click", async () => {
      const target = document.getElementById(btn.getAttribute("data-copy"));
      if (!target) return;
      try {
        await navigator.clipboard.writeText(target.textContent);
        const old = btn.textContent;
        btn.textContent = "已复制";
        setTimeout(() => {
          btn.textContent = old;
        }, 1600);
      } catch {
        btn.textContent = "请手动复制";
      }
    });
  });
})();
