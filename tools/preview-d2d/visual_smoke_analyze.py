#!/usr/bin/env python3
"""视觉冒烟 - 截图分析器。
读取 visual_smoke hook 导出的 PNG,做区域级布局体检:分区内容占比、内容包围盒、
垂直重心偏移、空白带检测、以及可选的 golden 像素 diff 回归。

用法:
  python analyze.py <png_dir>                    # 体检 + 打印报告
  python analyze.py <png_dir> --json report.json # 附带机读报告
  python analyze.py <png_dir> --golden <gdir>    # 与基准图像素 diff

设计:D2D 导出的是 premultiplied BGRA(带透明圆角),先合成到已知深底
(18,17,16)再分析,消除圆角透明对占比统计的干扰。
"""
import sys, glob, os, json
from PIL import Image
import numpy as np

BASE = np.array([18, 17, 16])          # 客户端深色底
SIDEBAR_W = 64                          # 左侧导航栏宽
TOPBAR_H = 56                           # 顶栏高
INK_THRESH = 36                         # 与背景差 > 此值 = 有内容

def load_rgb(path):
    im = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", im.size, (*BASE, 255))
    return np.array(Image.alpha_composite(bg, im).convert("RGB")).astype(int)

def ink_mask(rgb):
    return np.abs(rgb - BASE).sum(2) > INK_THRESH

def analyze_one(path):
    rgb = load_rgb(path); ink = ink_mask(rgb); H, W = ink.shape
    name = os.path.basename(path)
    is_main = any(name.startswith(p) for p in ("03","04","05","06","07","08","09","10","17"))
    sb = 100 * ink[:, :SIDEBAR_W].mean()
    tb = 100 * ink[:TOPBAR_H, :].mean()
    ct = 100 * ink[TOPBAR_H:, SIDEBAR_W:].mean()
    rows = np.where(ink.any(1))[0]; cols = np.where(ink.any(0))[0]
    bbox = [int(cols.min()), int(rows.min()), int(cols.max()), int(rows.max())] if len(rows) else None
    yc = float(rows.mean()) if len(rows) else 0.0
    voff = round(yc - H / 2)
    flags = []
    if is_main and sb < 1.0: flags.append("侧栏疑似缺失")
    if ct < 0.5: flags.append("内容区近空(可能未加载数据/渲染失败)")
    if bbox:
        left_gap, right_gap = bbox[0], W - bbox[2]
        # 居中判定:左右留白接近 = 居中卡片(正常);差距大 = 真偏移
        asym = abs(left_gap - right_gap)
        if asym > 80 and (left_gap > 80 or right_gap > 80):
            side = "左偏" if left_gap > right_gap else "右偏"
            flags.append(f"内容{side}(左空{left_gap} 右空{right_gap})")
    # 空白横带:内容区中间出现连续空行(可能控件塌陷)
    content = ink[TOPBAR_H:, SIDEBAR_W:]
    row_has = content.any(1)
    if row_has.any():
        first, last = np.where(row_has)[0][[0, -1]]
        gap = (~row_has[first:last]).sum()
        if gap > (last - first) * 0.6: flags.append("内容区大片空白带")
    return {"file": name, "sidebar_pct": round(sb,1), "topbar_pct": round(tb,1),
            "content_pct": round(ct,1), "bbox": bbox, "vcenter_offset_px": voff,
            "flags": flags}

def golden_diff(path, gdir):
    g = os.path.join(gdir, os.path.basename(path))
    if not os.path.exists(g): return {"golden": "MISSING"}
    a = load_rgb(path); b = load_rgb(g)
    if a.shape != b.shape: return {"golden": "SIZE_MISMATCH", "cur": a.shape, "gold": b.shape}
    d = np.abs(a - b).sum(2)
    changed = 100 * (d > 24).mean()
    return {"golden_diff_pct": round(changed, 2),
            "verdict": "OK" if changed < 0.5 else ("MINOR" if changed < 3 else "REGRESSION")}

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    d = sys.argv[1]
    jout = sys.argv[sys.argv.index("--json")+1] if "--json" in sys.argv else None
    gdir = sys.argv[sys.argv.index("--golden")+1] if "--golden" in sys.argv else None
    rows = []
    print(f'{"file":28}{"side":>6}{"top":>6}{"content":>8}{"voff":>7}  flags')
    for f in sorted(glob.glob(os.path.join(d, "[0-9]*.png"))):
        r = analyze_one(f)
        if gdir: r.update(golden_diff(f, gdir))
        rows.append(r)
        note = "; ".join(r["flags"]) or ("像素diff " + str(r.get("golden_diff_pct")) if gdir else "OK")
        print(f'{r["file"]:28}{r["sidebar_pct"]:5.1f}%{r["topbar_pct"]:5.1f}%'
              f'{r["content_pct"]:7.1f}%{r["vcenter_offset_px"]:+6}px  {note}')
    flagged = [r for r in rows if r["flags"]]
    print(f'\n{len(rows)} 屏检查,{len(flagged)} 屏有旗标')
    if jout:
        json.dump({"screens": rows, "flagged": len(flagged)}, open(jout,"w",encoding="utf-8"),
                  ensure_ascii=False, indent=1)
        print("机读报告 ->", jout)

if __name__ == "__main__":
    main()
