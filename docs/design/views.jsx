/* global React, Avatar, Icon, StatusLabel, STATUS */
const { useState, useEffect, useRef, useMemo } = React;

// ============== HomeView ==============
const HomeView = ({ user, status, t, lang, onShowHistory }) => (
  <div className="view active">
    <div className="stagger">
      <h1 className="greet">{t("home.greet").replace("{name}", user.name)}</h1>
      <div className="greet-sub">{t("home.subtitle")}</div>
      <div className="card profile-card">
        <Avatar name={user.name} size="large" status={status} />
        <div className="profile-id">
          <div className="name">{user.name}</div>
          <div className="email">{user.email}</div>
          <div className="online" style={{display:"inline-flex", alignItems:"center", gap:6}}>
            <StatusLabel status={status} lang={lang} />
          </div>
        </div>
        <div className="meta">
          <span className="key">{t("home.tier")}</span>
          <span className="val">{t(`tier.${user.tier_key}`)}</span>
          <span />
          <span className="key">{t("home.expires")}</span>
          <span className="val">{user.expires}</span>
          <span />
          <span className="key">{t("home.device_id")}</span>
          <span className="val mono">{user.device_id_short}</span>
          <span />
          <span className="key">{t("home.last_login")}</span>
          <span className="val mono">{user.last_login}</span>
          <span className="history-link" onClick={onShowHistory}>
            {t("history.label") || "历史"}
          </span>
        </div>
      </div>
    </div>
  </div>
);

// ============== LibraryView ==============
const LibraryView = ({ items, t }) => (
  <div className="view active">
    <h1 className="greet">{t("menu.library")}</h1>
    <div className="greet-sub">{items.length} {t("library.count") || "items"}</div>
    {items.length === 0 ? (
      <div className="empty">
        <div className="icon"><Icon name="library" width="56" height="56" strokeWidth="1.2"/></div>
        <div>{t("library.empty")}</div>
      </div>
    ) : (
      <div className="lib-grid stagger">
        {items.map(g => (
          <div className="game-card" key={g.id}>
            <div className="cover" style={{background: `radial-gradient(120% 80% at 30% 20%, ${g.tint} 0%, transparent 60%)`}}/>
            <div className="meta">
              <div className="name">{g.name}</div>
              <div className="ver">{g.version}{g.outdated && <span className="badge warn" style={{marginLeft:8}}>{t("library.outdated")}</span>}</div>
            </div>
          </div>
        ))}
      </div>
    )}
  </div>
);

// ============== CloudView ==============
const CloudView = ({ t }) => (
  <div className="view active">
    <h1 className="greet">{t("cloud.title")}</h1>
    <div className="greet-sub">{t("cloud.saves")} · {t("cloud.configs")}</div>
    <div className="empty">
      <div className="icon"><Icon name="cloud-empty" /></div>
      <div>{t("cloud.no_data")}</div>
    </div>
  </div>
);

// ============== SettingsView ==============
const SettingsView = ({ t, lang, setLang, theme, setTheme }) => {
  const langs = [
    { id: "en", label: "English" },
    { id: "zh-CN", label: "简体中文" },
    { id: "ja-JP", label: "日本語" },
  ];
  const themes = [
    { id: "system", label: t("settings.theme_system") },
    { id: "light",  label: t("settings.theme_light") },
    { id: "dark",   label: t("settings.theme_dark") },
  ];
  return (
    <div className="view active">
      <h1 className="greet">{t("menu.settings")}</h1>
      <div className="greet-sub">{t("settings.appearance")}</div>
      <div className="settings stagger">
        <h2>{t("settings.language")}</h2>
        <div className="seg">
          {langs.map(l => (
            <button key={l.id} className={lang===l.id?"on":""} onClick={()=>setLang(l.id)}>{l.label}</button>
          ))}
        </div>
        <h2>{t("settings.theme")}</h2>
        <div className="seg">
          {themes.map(th => (
            <button key={th.id} className={theme===th.id?"on":""} onClick={()=>setTheme(th.id)}>{th.label}</button>
          ))}
        </div>
        <h2>{t("settings.about")}</h2>
        <div className="card" style={{padding:"16px 18px", maxWidth:480, fontSize:13, color:"var(--text-muted)"}}>
          <div style={{color:"var(--text-primary)", fontWeight:600, marginBottom:4}}>Launcher</div>
          <div>v0.1.0 · Phase 1.5 · Skia + Clay + GLFW</div>
        </div>
      </div>
    </div>
  );
};

// ============== History modal (paginated) ==============
const ALL_SESSIONS = [
  { device: "Windows · ThinkBook", ip: "118.112.34.6",  location: "成都, 中国",     when: "05-02 10:32" },
  { device: "Windows · ThinkBook", ip: "118.112.34.6",  location: "成都, 中国",     when: "05-01 22:08" },
  { device: "Windows · Office",    ip: "203.45.67.89",  location: "北京, 中国",     when: "04-29 14:22" },
  { device: "Windows · ThinkBook", ip: "118.112.34.6",  location: "成都, 中国",     when: "04-28 09:15" },
  { device: "macOS · MBP",         ip: "192.168.1.87",  location: "本地",           when: "04-25 18:51" },
  { device: "Windows · ThinkBook", ip: "118.112.34.6",  location: "成都, 中国",     when: "04-23 11:04" },
  { device: "iPad · Safari",       ip: "118.112.34.10", location: "成都, 中国",     when: "04-21 20:30" },
  { device: "Windows · Office",    ip: "203.45.67.89",  location: "北京, 中国",     when: "04-19 09:48" },
  { device: "Windows · ThinkBook", ip: "118.112.34.6",  location: "成都, 中国",     when: "04-17 14:12" },
  { device: "Linux · WSL",         ip: "118.112.34.6",  location: "成都, 中国",     when: "04-15 22:01" },
  { device: "Windows · ThinkBook", ip: "118.112.34.6",  location: "成都, 中国",     when: "04-13 08:55" },
  { device: "Android · Pixel",     ip: "117.140.22.5",  location: "上海, 中国",     when: "04-10 16:39" },
  { device: "Windows · Office",    ip: "203.45.67.89",  location: "北京, 中国",     when: "04-08 10:01" },
];

const HistoryModal = ({ onClose, t }) => {
  const [page, setPage] = useState(0);
  const PER = 5;
  const pages = Math.ceil(ALL_SESSIONS.length / PER);
  const slice = ALL_SESSIONS.slice(page * PER, page * PER + PER);
  useEffect(() => {
    const onKey = (e) => { if (e.key === "Escape") onClose(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);
  return (
    <div className="scrim" onClick={(e)=>{ if(e.target===e.currentTarget) onClose(); }}>
      <div className="modal">
        <h3>{t("history.title") || "登录历史"}</h3>
        <div className="sub">{ALL_SESSIONS.length} {t("history.records", "条记录")}</div>
        {slice.map((s,i)=>(
          <div className="row-item" key={page*PER+i}>
            <div>
              <div style={{fontWeight:600}}>{s.device}</div>
              <div className="ip">{s.ip} · {s.location}</div>
            </div>
            <div className="when">{s.when}</div>
          </div>
        ))}
        <div className="pager">
          <button disabled={page===0} onClick={()=>setPage(p=>Math.max(0,p-1))}>‹</button>
          {Array.from({length: pages}).map((_,i)=>(
            <button key={i} className={i===page?"on":""} onClick={()=>setPage(i)}>{i+1}</button>
          ))}
          <button disabled={page>=pages-1} onClick={()=>setPage(p=>Math.min(pages-1,p+1))}>›</button>
        </div>
        <div className="actions">
          <button className="btn-ghost" onClick={onClose}>{t("common.close") || "关闭"}</button>
        </div>
      </div>
    </div>
  );
};

// ============== LoginView ==============
const LoginView = ({ t, onSubmit }) => {
  const [mode, setMode] = useState("login");
  const [u, setU] = useState("");
  const [p, setP] = useState("");
  const [showP, setShowP] = useState(false);
  const [remember, setRemember] = useState(true);
  const [err, setErr] = useState("");
  const btnRef = useRef(null);
  const submit = (e) => {
    e.preventDefault();
    if (!u || !p) { setErr(t("auth.error_invalid")); return; }
    const btn = btnRef.current;
    if (btn) {
      const rect = btn.getBoundingClientRect();
      const r = document.createElement("span");
      r.className = "ripple";
      const size = Math.max(rect.width, rect.height);
      r.style.width = r.style.height = size + "px";
      r.style.left = (rect.width/2 - size/2) + "px";
      r.style.top  = (rect.height/2 - size/2) + "px";
      btn.appendChild(r); setTimeout(()=>r.remove(), 600);
    }
    setErr("");
    onSubmit && onSubmit({ username: u, password: p, remember });
  };
  return (
    <div className="login-wrap view active">
      <div className="login-card">
        <h1>
          <span style={{display:"inline-grid", placeItems:"center", width:32, height:32, borderRadius:8, background:"var(--primary)", color:"#fff"}}>
            <Icon name="logo" width="20" height="20" />
          </span>
          {t("app.name")}
        </h1>
        <div className="tagline">{t("app.tagline")}</div>
        <div className="tab-switch" style={{marginTop:22}}>
          <div className="pill" style={{transform: `translateX(${mode==="login"?0:"100%"})`}}/>
          <button className={mode==="login"?"on":""} onClick={()=>setMode("login")}>{t("auth.sign_in")}</button>
          <button className={mode==="register"?"on":""} onClick={()=>setMode("register")}>{t("auth.register") || "注册"}</button>
        </div>
        <form onSubmit={submit}>
          <div className={`field ${u?"filled":""}`}>
            <input id="u" value={u} onChange={(e)=>setU(e.target.value)} autoComplete="username" />
            <label htmlFor="u">{t("auth.username")}</label>
            <span className="underline" />
          </div>
          <div className={`field ${p?"filled":""}`} style={{position:"relative"}}>
            <input id="p" type={showP?"text":"password"} value={p} onChange={(e)=>setP(e.target.value)} autoComplete="current-password" />
            <label htmlFor="p">{t("auth.password")}</label>
            <span className="underline" />
            <button type="button" onClick={()=>setShowP(s=>!s)}
              style={{position:"absolute", right:10, top:14, background:"transparent", border:0, color:"var(--text-muted)", cursor:"pointer", padding:6, borderRadius:6}}>
              <Icon name={showP?"eye-off":"eye"} />
            </button>
          </div>
          {err && <div className="error">{err}</div>}
          <div className="row">
            <label className="check">
              <input type="checkbox" checked={remember} onChange={(e)=>setRemember(e.target.checked)} />
              <span className="box"></span>
              <span>{t("auth.remember_me")}</span>
            </label>
            <a style={{fontSize:13, color:"var(--text-muted)", cursor:"pointer"}}>{t("auth.forgot") || "忘记密码？"}</a>
          </div>
          <button ref={btnRef} className="btn-primary" type="submit">
            {mode==="login" ? t("auth.submit") : (t("auth.register") || "注册")}
          </button>
        </form>
      </div>
    </div>
  );
};

// ============== Loading view ==============
const LoadingView = () => (
  <div className="loading-wrap"><div className="loading-card"><div className="spinner" /></div></div>
);

// ============== Chat view (Discord-style) ==============
const CHANNEL_GROUPS = [
  { id:"important", name:"IMPORTANT", channels:[
    { id:"announcements", name:"announcements", unread: 2 },
    { id:"rules", name:"rules" },
  ]},
  { id:"general", name:"GENERAL", channels:[
    { id:"general",   name:"general",   unread: 3 },
    { id:"random",    name:"random" },
    { id:"helpdesk",  name:"helpdesk",  unread: 1 },
  ]},
  { id:"games", name:"GAMES", channels:[
    { id:"cs2",       name:"cs2",        unread: 12 },
    { id:"touhou",    name:"touhou" },
    { id:"vrchat",    name:"vrchat" },
  ]},
  { id:"shop", name:"SHOP", channels:[
    { id:"market",    name:"market",     special:"market" },
    { id:"trades",    name:"trades" },
  ]},
];

const SAMPLE_MESSAGES = {
  general: [
    { kind:"day", text:"今天" },
    { from:"yuki",   author:"yuki",   text:"早！谁今晚有空开把 cs2 啊", time:"09:42", status:"online" },
    { from:"yuki",   author:"yuki",   text:"我打 premier 单挑掉分到一个怀疑人生", time:"09:42", status:"online" },
    { from:"reimu",  author:"博丽灵梦", text:"我！但是 8 点之后", time:"09:50", status:"away" },
    { from:"me",     text:"+1 等我下班", time:"10:01", read:true },
    { from:"yuki",   author:"yuki",   text:"晚上一起开 cs 吗？", time:"10:42", status:"online" },
    { from:"sakuya", author:"十六夜咲夜", text:"", sticker:"🎮", time:"10:43", status:"busy" },
    { from:"me",     text:"开开开 8 点 disc 见", time:"10:44", read:true },
    { kind:"typing", who:"yuki" },
  ],
  announcements: [
    { kind:"day", text:"今天" },
    { from:"system", kind:"system", text:"📢 Launcher v0.1.0 已发布" },
    { from:"reimu", author:"博丽灵梦", text:"周六晚 8 点联机，记得报名", time:"08:00", status:"away" },
  ],
  rules:     [{ kind:"day", text:"04-01" }, { from:"system", kind:"system", text:"请阅读社区规则" }],
  random:    [{ kind:"day", text:"今天" }, { from:"yuki", author:"yuki", text:"今天天气不错", time:"08:30", status:"online" }],
  helpdesk:  [{ kind:"day", text:"今天" }, { from:"flandre", author:"芙兰朵露", text:"我登录不上 ，help", time:"02:14", status:"offline" }],
  cs2:       [
    { kind:"day", text:"今天" },
    { from:"reimu", author:"博丽灵梦", text:"matchmaking 又寄了…", time:"09:18", status:"away" },
    { from:"yuki",  author:"yuki", gif:"中国男篮经典", time:"09:19", status:"online" },
    { from:"me",    text:"steam 又抽风", time:"09:20", read:true },
  ],
  touhou:    [{ kind:"day", text:"昨天" }, { from:"marisa", author:"雾雨魔理沙", text:"红魔馆通关了～", time:"22:30", status:"sleep" }],
  vrchat:    [{ kind:"day", text:"周三" }, { from:"sakuya", author:"十六夜咲夜", text:"周末来世界？", time:"15:00", status:"busy" }],
  trades:    [{ kind:"day", text:"04-25" }, { from:"flandre", author:"芙兰朵露", text:"出 awp ｜ 印花集", time:"03:14", status:"offline" }],
};

const MARKET_ITEMS = [
  { id:1,  title:"Vintage StatTrak™ AWP | Asiimov",         author:"sakuya",  price:"¥420", tag:"热卖", body:"九成新 · 印花完整 · 5 年老号出 · 价格可议" },
  { id:2,  title:"Touhou Project 全套同人本 (15 册)",       author:"reimu",   price:"¥220", tag:"包邮", body:"二手 · C103 ~ C104 · 含豪华装订 · 仅出整套" },
  { id:3,  title:"Razer DeathAdder V3 (无线版)",            author:"yuki",    price:"¥380", tag:"全新", body:"未拆封 · 自用备机 · 含原装电池 · 顺丰发出" },
  { id:4,  title:"Launcher 永久会员账号",                    author:"flandre", price:"¥99",  tag:"虚拟", body:"绑定主邮箱 · 含全部已有订阅 · 可改密码" },
  { id:5,  title:"东方 Dolls — Marisa 1/7 比例手办",        author:"marisa",  price:"¥680", tag:"绝版", body:"GSC 旧版 · 盒新内胆完整 · 仅展示无瑕疵" },
  { id:6,  title:"代练 CS2 Premier 上分 (Faceit 也接)",     author:"yuki",    price:"¥50/局", tag:"服务", body:"5 年 Faceit 3000+ · 可指定段位 · 不上号" },
  { id:7,  title:"日服 PSN 30000 円 充值卡",                author:"sakuya",  price:"¥1480",tag:"虚拟", body:"实时发码 · 7×24 · 错码包补 · 长期供应" },
  { id:8,  title:"自家烤的迷迭香曲奇 (12 块装)",            author:"reimu",   price:"¥38",  tag:"周边", body:"零添加 · 周三周五出炉 · 同城自取或快递" },
  { id:9,  title:"求收个 Logitech Pro X Superlight",        author:"flandre", price:"求购",  tag:"求购", body:"白色优先 · 八成新以上 · 长期收 · 价好" },
  { id:10, title:"夜雀食堂第二期补番记 (B 站会员)",         author:"marisa",  price:"¥15/月",tag:"服务", body:"代充 · 不限地区 · 直冲不掉 · 长期合作可议" },
];

const MarketView = ({ t }) => {
  const [q, setQ] = useState("");
  const [sort, setSort] = useState("hot");
  const [page, setPage] = useState(0);
  const PER = 5;
  const filtered = useMemo(() => {
    let r = MARKET_ITEMS.filter(it =>
      !q || it.title.toLowerCase().includes(q.toLowerCase())
        || it.author.toLowerCase().includes(q.toLowerCase()));
    if (sort === "new")   r = [...r].reverse();
    if (sort === "price") r = [...r].sort((a,b) => (parseInt(a.price.replace(/[^0-9]/g,""))||0) - (parseInt(b.price.replace(/[^0-9]/g,""))||0));
    return r;
  }, [q, sort]);
  const pages = Math.max(1, Math.ceil(filtered.length / PER));
  const slice = filtered.slice(page * PER, page * PER + PER);
  return (
    <div className="market">
      <div className="market-head">
        <div className="market-search">
          <span className="ico"><Icon name="search" width="16" height="16"/></span>
          <input placeholder={t("market.search") || "搜索商品 / 卖家"} value={q} onChange={(e)=>{ setQ(e.target.value); setPage(0); }} />
        </div>
        <select className="market-sort" value={sort} onChange={(e)=>setSort(e.target.value)}>
          <option value="hot">{t("market.sort_hot") || "热度"}</option>
          <option value="new">{t("market.sort_new") || "最新"}</option>
          <option value="price">{t("market.sort_price") || "价格"}</option>
        </select>
      </div>
      <div className="market-grid">
        {slice.map(it => (
          <div className="market-card" key={it.id}>
            <div className="title">
              {it.title}
              <span className="pill">{it.tag}</span>
            </div>
            <div className="body">{it.body}</div>
            <div className="foot">
              <span className="author">@{it.author}</span>
              <span>·</span>
              <span style={{color:"var(--text-primary)", fontWeight:700}}>{it.price}</span>
            </div>
          </div>
        ))}
        {slice.length === 0 && <div className="empty" style={{padding:"40px 0"}}>没有匹配的商品</div>}
      </div>
      <div className="pager">
        <button disabled={page===0} onClick={()=>setPage(p=>Math.max(0,p-1))}>‹</button>
        {Array.from({length: pages}).map((_,i)=>(
          <button key={i} className={i===page?"on":""} onClick={()=>setPage(i)}>{i+1}</button>
        ))}
        <button disabled={page>=pages-1} onClick={()=>setPage(p=>Math.min(pages-1,p+1))}>›</button>
      </div>
    </div>
  );
};

const EMOJI_SET = ["😀","😁","😂","🤣","😄","😅","😆","😉","😊","😋","😎","😍","🥰","😘","😗","🙃","🙂","🤩","🤔","🤨","😐","😑","😶","🙄","😏","😣","😥","😮","🤐","😯","😪","😫","🥱","😴","😌","😛","😜","🤪","😝","🤤","🥳","🥺","😢","😭","😱","😨","😰","😥","🤯","😳","🥶","😡","🤬","💀","👻","👽","🤖","👍","👎","👏","🙏","💪","🔥","💯","🎮","🍣","🍙","🌸","⭐","🚀","💖","🌈"];
const GIF_SET = ["杏仁糖蹦迪","柴犬狂笑","太君卷动","恭喜发财","姐姐看我","摔倒猫猫","王境泽真香","贴贴.gif","老板说的对","emo 大军"];

const Picker = ({ kind, onPick, onClose }) => {
  const [tab, setTab] = useState(kind || "emoji");
  const [q, setQ] = useState("");
  const ref = useRef(null);
  useEffect(() => {
    const onDoc = (e) => { if (ref.current && !ref.current.contains(e.target)) onClose(); };
    document.addEventListener("mousedown", onDoc);
    return () => document.removeEventListener("mousedown", onDoc);
  }, [onClose]);
  const filteredEmoji = q ? EMOJI_SET : EMOJI_SET;
  const filteredGif = GIF_SET.filter(g => !q || g.includes(q));
  return (
    <div className={`picker ${tab}`} ref={ref}>
      <div className="tabs">
        <div className={`tab ${tab==="emoji"?"on":""}`} onClick={()=>setTab("emoji")}>Emoji</div>
        <div className={`tab ${tab==="sticker"?"on":""}`} onClick={()=>setTab("sticker")}>表情包</div>
        <div className={`tab ${tab==="gif"?"on":""}`} onClick={()=>setTab("gif")}>GIF</div>
      </div>
      <div className="find"><input placeholder="搜索…" value={q} onChange={(e)=>setQ(e.target.value)} /></div>
      <div className="grid">
        {tab === "gif"
          ? filteredGif.map((g,i) => (
              <div key={i} className="gif-cell" onClick={()=>onPick({kind:"gif", value:g})}>{g}</div>
            ))
          : (tab === "sticker" ? ["🎮","🔥","💀","🎉","💯","✨","🚀","⭐","🌸","💖","👏","🙏"] : filteredEmoji).map((e,i) => (
              <div key={i} className="emoji-cell"
                   onClick={()=>onPick({kind: tab==="sticker"?"sticker":"emoji", value:e})}>{e}</div>
            ))
        }
      </div>
    </div>
  );
};

const HeaderMenu = ({ trigger, items, align="right" }) => {
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
  return (
    <div className="popover-anchor icon-btn-anchor" ref={ref}>
      <div className="icon-btn" onClick={()=>setOpen(o=>!o)}>{trigger}</div>
      {open && (
        <div className={`popover from-right`}>
          <div className="group">
            {items.map((it, i) => it.divider ? (
              <div key={i} style={{height:1, background:"var(--divider)", margin:"4px 0"}}/>
            ) : (
              <div key={i} className={`item ${it.danger?"danger":""}`} onClick={()=>{ it.onClick && it.onClick(); setOpen(false); }}>
                {it.icon && <span className="glyph"><Icon name={it.icon} width="16" height="16"/></span>}
                <span>{it.label}</span>
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  );
};

const Bubble = ({ msg, prevSameAuthor, isGroup }) => {
  if (msg.kind === "day")    return <div className="day-divider"><span>{msg.text}</span></div>;
  if (msg.kind === "system") return <div className="bubble system">{msg.text}</div>;
  if (msg.kind === "typing") {
    return (
      <div className="bubble-row">
        <div className="gutter"></div>
        <div className="typing"><span/><span/><span/></div>
      </div>
    );
  }
  const me = msg.from === "me";
  const isSticker = !!msg.sticker;
  const isGif = !!msg.gif;
  return (
    <div className={`bubble-row ${me?"me":""}`}>
      {!me && (
        <div className="gutter">
          {!prevSameAuthor && <Avatar name={msg.author || msg.from} size="small" status={msg.status} />}
        </div>
      )}
      <div className={`bubble ${me?"me":"them"} ${prevSameAuthor?"tail-stack":""} ${isSticker?"sticker":""} ${isGif?"gif":""}`}>
        {!me && isGroup && !prevSameAuthor && !isSticker && !isGif && <div className="author">{msg.author || msg.from}</div>}
        {isSticker ? msg.sticker
          : isGif ? (<><span className="gif-tag">GIF</span>{msg.gif}<span className="meta">{msg.time}</span></>)
          : (<><span>{msg.text}</span><span className="meta">{msg.time}{me && <Icon name="check2" width="14" height="10" style={{color: msg.read? "#7DD3FC" : "currentColor"}}/>}</span></>)
        }
      </div>
    </div>
  );
};

const ChatView = ({ t, status: myStatus, name: myName }) => {
  const [activeId, setActiveId] = useState("general");
  const [collapsed, setCollapsed] = useState({});
  const [drafts, setDrafts] = useState({});
  const [streams, setStreams] = useState(SAMPLE_MESSAGES);
  const [picker, setPicker] = useState(null); // null | "emoji" | "gif" | "sticker"
  const streamRef = useRef(null);
  const taRef = useRef(null);

  const allChannels = CHANNEL_GROUPS.flatMap(g => g.channels);
  const active = allChannels.find(c => c.id === activeId) || allChannels[0];
  const messages = streams[activeId] || [];

  useEffect(() => {
    const el = streamRef.current; if (!el) return;
    el.scrollTop = el.scrollHeight;
  }, [activeId, streams]);

  const draft = drafts[activeId] || "";
  const setDraft = (v) => setDrafts(d => ({...d, [activeId]: v}));

  const send = () => {
    const text = (draft || "").trim();
    if (!text) return;
    const time = new Date().toLocaleTimeString([], {hour:"2-digit", minute:"2-digit"});
    const next = { from:"me", text, time, read:false };
    setStreams(s => ({...s, [activeId]: [...(s[activeId]||[]), next]}));
    setDraft("");
    if (taRef.current) taRef.current.style.height = "auto";
  };
  const pick = (p) => {
    const time = new Date().toLocaleTimeString([], {hour:"2-digit", minute:"2-digit"});
    if (p.kind === "emoji") {
      setDraft(draft + p.value);
    } else if (p.kind === "sticker") {
      setStreams(s => ({...s, [activeId]: [...(s[activeId]||[]), { from:"me", sticker:p.value, time, read:false }]}));
    } else if (p.kind === "gif") {
      setStreams(s => ({...s, [activeId]: [...(s[activeId]||[]), { from:"me", gif:p.value, time, read:false }]}));
    }
    setPicker(null);
  };
  const onKey = (e) => {
    if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); send(); }
  };
  const autosize = (e) => {
    e.target.style.height = "auto";
    e.target.style.height = Math.min(120, e.target.scrollHeight) + "px";
    setDraft(e.target.value);
  };

  const toggleGroup = (id) => setCollapsed(c => ({...c, [id]: !c[id]}));

  return (
    <div className="chat-shell">
      <aside className="chat-list">
        <div className="chat-server-head">
          <span className="glyph"><Icon name="logo" width="14" height="14"/></span>
          <span>Launcher Server</span>
        </div>
        <div className="chat-rows">
          {CHANNEL_GROUPS.map(g => (
            <div key={g.id} className={`chan-group ${collapsed[g.id]?"collapsed":""}`}>
              <div className="chan-group-head" onClick={()=>toggleGroup(g.id)}>
                <span className="caret">▾</span>
                <span>{g.name}</span>
              </div>
              <div className="chan-list">
                {g.channels.map(c => (
                  <div key={c.id} className={`chan ${c.id===activeId?"active":""}`} onClick={()=>setActiveId(c.id)}>
                    <span className="hash">#</span>
                    <span style={{flex:1, overflow:"hidden", textOverflow:"ellipsis", whiteSpace:"nowrap"}}>{c.name}</span>
                    {c.unread > 0 && <span className="badge-mini">{c.unread}</span>}
                  </div>
                ))}
              </div>
            </div>
          ))}
        </div>
      </aside>
      <section className="chat-pane">
        <div className="chat-header">
          <span className="hash" style={{color:"var(--text-muted)", fontSize:20, fontWeight:300, marginRight:4}}>#</span>
          <div className="meta">
            <div className="name">{active.name}</div>
            <div className="sub">
              {active.special === "market" ? (t("market.sub") || "社区交易市场") : (t("chat.channel") || "频道")}
            </div>
          </div>
          <div className="actions">
            <HeaderMenu trigger={<Icon name="search"/>} items={[
              { label: (t("chat.search_in") || "在频道内搜索"), icon:"search" },
              { label: (t("chat.search_global") || "全局搜索"), icon:"search" },
            ]} />
            <HeaderMenu trigger={<Icon name="more"/>} items={[
              { label:(t("chat.pin") || "置顶频道"), icon:"shield" },
              { label:(t("chat.mute") || "静音"), icon:"bell" },
              { divider:true },
              { label:(t("chat.invite") || "邀请成员"), icon:"user" },
              { divider:true },
              { label:(t("chat.leave") || "离开频道"), icon:"logout", danger:true },
            ]} />
          </div>
        </div>
        {active.special === "market" ? (
          <MarketView t={t} />
        ) : (
          <>
            <div className="chat-stream" ref={streamRef}>
              {messages.map((m, i) => {
                const prev = messages[i-1];
                const prevSameAuthor = prev && !prev.kind && prev.from === m.from && prev.from !== "me" && !m.sticker && !m.gif;
                return <Bubble key={i} msg={m} prevSameAuthor={prevSameAuthor} isGroup={true} />;
              })}
            </div>
            <div className="chat-composer" style={{position:"relative"}}>
              {picker && <Picker kind={picker} onPick={pick} onClose={()=>setPicker(null)} />}
              <div className="icon-btn" title="emoji" onClick={()=>setPicker(p => p==="emoji"?null:"emoji")}><Icon name="smile" /></div>
              <div className="icon-btn" title="sticker" onClick={()=>setPicker(p => p==="sticker"?null:"sticker")}>
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round"><path d="M21 11V5a2 2 0 0 0-2-2H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h6"/><path d="M14 21v-4a3 3 0 0 1 3-3h4"/><path d="M21 14v3a4 4 0 0 1-4 4h-3"/></svg>
              </div>
              <div className="icon-btn" title="gif" onClick={()=>setPicker(p => p==="gif"?null:"gif")} style={{fontSize:11, fontWeight:800, letterSpacing:.5}}>GIF</div>
              <div className="icon-btn" title="attach"><Icon name="paperclip" /></div>
              <div className="composer-field">
                <textarea ref={taRef} rows="1" placeholder={t("chat.placeholder") || "写点什么…"}
                          value={draft} onChange={autosize} onKeyDown={onKey} />
              </div>
              <button className="send-btn" onClick={send} disabled={!draft.trim()}>
                <Icon name="send" width="18" height="18" style={{transform:"translateX(-1px)"}} />
              </button>
            </div>
          </>
        )}
      </section>
    </div>
  );
};

Object.assign(window, { HomeView, LibraryView, CloudView, SettingsView, HistoryModal, LoginView, LoadingView, ChatView });
