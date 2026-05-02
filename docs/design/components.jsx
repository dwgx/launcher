/* global React */
const { useState, useEffect, useRef } = React;

const Icon = ({ name, ...rest }) => {
  const common = { width: 18, height: 18, viewBox: "0 0 24 24", fill: "none",
    stroke: "currentColor", strokeWidth: 1.7, strokeLinecap: "round", strokeLinejoin: "round", ...rest };
  switch (name) {
    case "home": return (<svg {...common}><path d="M3 11.5 12 4l9 7.5"/><path d="M5 10.5V20h14v-9.5"/><path d="M10 20v-5h4v5"/></svg>);
    case "library": return (<svg {...common}><path d="M6 4v16"/><path d="M10 4v16"/><rect x="14" y="4" width="6" height="16" rx="1.5"/><path d="M16 9h2"/></svg>);
    case "cloud": return (<svg {...common}><path d="M7 18h10a4 4 0 0 0 .6-7.96 6 6 0 0 0-11.7 1.7A4 4 0 0 0 7 18Z"/></svg>);
    case "chat": return (<svg {...common}><path d="M21 12a8 8 0 0 1-11.6 7.1L4 21l1.9-5.4A8 8 0 1 1 21 12Z"/></svg>);
    case "settings": return (<svg {...common}><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .34 1.87l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.7 1.7 0 0 0-1.87-.34 1.7 1.7 0 0 0-1.04 1.56V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.55 1.7 1.7 0 0 0-1.87.34l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.7 1.7 0 0 0 .34-1.87 1.7 1.7 0 0 0-1.56-1.04H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.55-1.1 1.7 1.7 0 0 0-.34-1.87l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.7 1.7 0 0 0 1.87.34H9a1.7 1.7 0 0 0 1-1.56V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1.04 1.56 1.7 1.7 0 0 0 1.87-.34l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.7 1.7 0 0 0-.34 1.87V9a1.7 1.7 0 0 0 1.56 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1Z"/></svg>);
    case "logout": return (<svg {...common}><path d="M9 4H5a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h4"/><path d="M16 17l5-5-5-5"/><path d="M21 12H9"/></svg>);
    case "logo": return (<svg {...common} viewBox="0 0 24 24"><circle cx="12" cy="12" r="9" stroke="currentColor" strokeWidth="2"/><circle cx="12" cy="12" r="4" fill="currentColor"/></svg>);
    case "eye": return (<svg {...common}><path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z"/><circle cx="12" cy="12" r="3"/></svg>);
    case "eye-off": return (<svg {...common}><path d="m3 3 18 18"/><path d="M10.6 10.6a3 3 0 0 0 4.2 4.2"/><path d="M9.9 5.1A11 11 0 0 1 12 5c6.5 0 10 7 10 7a18 18 0 0 1-3.2 4.1"/><path d="M6.6 6.6A18 18 0 0 0 2 12s3.5 7 10 7c1.6 0 3-.3 4.3-.8"/></svg>);
    case "x": return (<svg {...common}><path d="m6 6 12 12"/><path d="M18 6 6 18"/></svg>);
    case "history": return (<svg {...common}><path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/><path d="M12 7v5l3 2"/></svg>);
    case "cloud-empty": return (<svg {...common} width="56" height="56"><path d="M7 18h10a4 4 0 0 0 .6-7.96 6 6 0 0 0-11.7 1.7A4 4 0 0 0 7 18Z"/></svg>);
    case "search": return (<svg {...common}><circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/></svg>);
    case "send": return (<svg {...common} viewBox="0 0 24 24"><path d="M3.5 11.5 21 4l-7.5 17.5-2-7-8-3Z"/><path d="m11.5 12.5 9.5-8.5"/></svg>);
    case "phone": return (<svg {...common}><path d="M22 16.9v3a2 2 0 0 1-2.2 2 19.8 19.8 0 0 1-8.6-3.1 19.5 19.5 0 0 1-6-6A19.8 19.8 0 0 1 2.1 4.2 2 2 0 0 1 4.1 2h3a2 2 0 0 1 2 1.7c.1.9.3 1.7.6 2.5a2 2 0 0 1-.5 2.1L8 9.6a16 16 0 0 0 6 6l1.3-1.3a2 2 0 0 1 2.1-.5c.8.3 1.6.5 2.5.6a2 2 0 0 1 1.7 2Z"/></svg>);
    case "video": return (<svg {...common}><rect x="2" y="6" width="14" height="12" rx="2"/><path d="m22 8-6 4 6 4V8Z"/></svg>);
    case "more": return (<svg {...common}><circle cx="12" cy="5" r="1.4"/><circle cx="12" cy="12" r="1.4"/><circle cx="12" cy="19" r="1.4"/></svg>);
    case "smile": return (<svg {...common}><circle cx="12" cy="12" r="9"/><path d="M8 14s1.5 2 4 2 4-2 4-2"/><path d="M9 9h.01M15 9h.01"/></svg>);
    case "paperclip": return (<svg {...common}><path d="m21 12-9.5 9.5a5 5 0 0 1-7-7L13 5.5a3.5 3.5 0 0 1 5 5l-8.5 8.5a2 2 0 0 1-3-3l7.5-7.5"/></svg>);
    case "check": return (<svg {...common}><path d="m4 12 5 5L20 6"/></svg>);
    case "check2": return (<svg {...common} viewBox="0 0 24 16"><path d="m1 8 4 4 9-9"/><path d="m9 12 4 4 10-10"/></svg>);
    case "user": return (<svg {...common}><circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/></svg>);
    case "moon": return (<svg {...common}><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8Z"/></svg>);
    case "shield": return (<svg {...common}><path d="M12 3 4 6v6c0 5 3.5 8.5 8 9 4.5-.5 8-4 8-9V6l-8-3Z"/></svg>);
    case "bell": return (<svg {...common}><path d="M6 8a6 6 0 1 1 12 0c0 7 3 8 3 8H3s3-1 3-8"/><path d="M10 21a2 2 0 0 0 4 0"/></svg>);
    default: return null;
  }
};

const STATUS = {
  online:  { dot:"online",  label:"在线",  i18n:{en:"Online",  "zh-CN":"在线",  "ja-JP":"オンライン"} },
  busy:    { dot:"busy",    label:"繁忙",  i18n:{en:"Busy",    "zh-CN":"繁忙",  "ja-JP":"取り込み中"} },
  away:    { dot:"away",    label:"离开",  i18n:{en:"Away",    "zh-CN":"离开",  "ja-JP":"離席中"} },
  sleep:   { dot:"sleep",   label:"睡眠",  i18n:{en:"Sleeping","zh-CN":"睡眠",  "ja-JP":"おやすみ"} },
  offline: { dot:"offline", label:"离线",  i18n:{en:"Offline", "zh-CN":"离线",  "ja-JP":"オフライン"} },
};

const Avatar = ({ name = "User", size = "medium", status = null }) => {
  const initial = (name || "?").trim().charAt(0).toUpperCase();
  return (
    <div className={`avatar ${size}`}>
      {initial}
      {status && <span className={`dot ${STATUS[status]?.dot || ""}`} />}
    </div>
  );
};

const StatusLabel = ({ status, lang }) => {
  const s = STATUS[status] || STATUS.offline;
  const text = (s.i18n && s.i18n[lang]) || s.label;
  return <span><span className={`status-dot ${s.dot}`} style={{display:"inline-block", marginRight:6, verticalAlign:"middle"}}/>{text}</span>;
};

// User popover dropdown
const UserMenu = ({ user, status, setStatus, lang, t, onLogout, onShowHistory, onNavSettings }) => {
  const [open, setOpen] = useState(false);
  const ref = useRef(null);
  useEffect(() => {
    if (!open) return;
    const onDoc = (e) => { if (ref.current && !ref.current.contains(e.target)) setOpen(false); };
    const onKey = (e) => { if (e.key === "Escape") setOpen(false); };
    document.addEventListener("mousedown", onDoc);
    document.addEventListener("keydown", onKey);
    return () => { document.removeEventListener("mousedown", onDoc); document.removeEventListener("keydown", onKey); };
  }, [open]);
  const statusKeys = ["online","busy","away","sleep","offline"];
  return (
    <div className="popover-anchor" ref={ref}>
      <div className="user-trigger" onClick={() => setOpen(o => !o)}>
        <span style={{fontSize:13, color:"var(--text-muted)"}}>{user.name}</span>
        <Avatar name={user.name} size="medium" status={status} />
      </div>
      {open && (
        <div className="popover">
          <div className="header">
            <Avatar name={user.name} size="medium" status={status} />
            <div>
              <div className="name">{user.name}</div>
              <div className="email">{user.email}</div>
            </div>
          </div>
          <div className="group">
            {statusKeys.map(k => (
              <div key={k} className="item" onClick={() => { setStatus(k); setOpen(false); }}>
                <span className={`status-dot ${STATUS[k].dot}`} />
                <span>{(STATUS[k].i18n && STATUS[k].i18n[lang]) || STATUS[k].label}</span>
                {status === k && <span className="check"><Icon name="check" width="14" height="14"/></span>}
              </div>
            ))}
          </div>
          <div className="group">
            <div className="item" onClick={() => { onShowHistory(); setOpen(false); }}>
              <span className="glyph"><Icon name="history" width="16" height="16"/></span>
              <span>{t("history.label")}</span>
            </div>
            <div className="item" onClick={() => { onNavSettings(); setOpen(false); }}>
              <span className="glyph"><Icon name="settings" width="16" height="16"/></span>
              <span>{t("menu.settings")}</span>
            </div>
            <div className="item">
              <span className="glyph"><Icon name="bell" width="16" height="16"/></span>
              <span>{t("menu.notifications") || (lang==="en"?"Notifications":lang==="ja-JP"?"通知":"通知")}</span>
            </div>
          </div>
          <div className="group">
            <div className="item danger" onClick={() => { onLogout(); setOpen(false); }}>
              <span className="glyph"><Icon name="logout" width="16" height="16"/></span>
              <span>{t("menu.logout")}</span>
            </div>
          </div>
        </div>
      )}
    </div>
  );
};

const Topbar = ({ title, user, status, setStatus, lang, t, onLogout, onShowHistory, onNavSettings }) => (
  <div className="topbar">
    <span className="title">{title}</span>
    <span className="grow" />
    <UserMenu user={user} status={status} setStatus={setStatus} lang={lang} t={t}
              onLogout={onLogout} onShowHistory={onShowHistory} onNavSettings={onNavSettings} />
  </div>
);

const Sidebar = ({ active, onNav, t }) => {
  const items = [
    { id: "home",     glyph: "home",     label: t("menu.home") },
    { id: "library",  glyph: "library",  label: t("menu.library") },
    { id: "chat",     glyph: "chat",     label: t("menu.chat", "Chat") },
    { id: "cloud",    glyph: "cloud",    label: t("menu.cloud") },
    { id: "settings", glyph: "settings", label: t("menu.settings") },
  ];
  return (
    <aside className="sidebar">
      <nav className="nav">
        {items.map(it => (
          <div key={it.id}
               className={`menu-item ${active === it.id ? "active" : ""}`}
               onClick={() => onNav(it.id)}>
            <span className="glyph"><Icon name={it.glyph} /></span>
            <span className="label">{it.label}</span>
          </div>
        ))}
      </nav>
      <div className="grow" />
      <div className="menu-item" onClick={() => onNav("logout")}>
        <span className="glyph"><Icon name="logout" /></span>
        <span className="label">{t("menu.logout")}</span>
      </div>
    </aside>
  );
};

Object.assign(window, { Icon, Avatar, Topbar, Sidebar, UserMenu, StatusLabel, STATUS });
