#!/usr/bin/env python3
"""生成快捷方式角标的「改配色前后」对照图（肉眼确认用，不参与构建）。

用法
    python3 tools/preview_shortcut_overlay.py [-o 输出.png] [候选颜色 ...]

默认候选：#2b2b2b（上一版深灰）/#1b3358/#1e3a6e（当前）/ #1d4ed8。
输出默认 .workbuddy/shortcut-overlay-preview.png（.workbuddy 已在 .gitignore 里）。

对照图分两块
  上：三列并排 —— 上一版深灰素材 / 当前 res/shortcut_overlay.ico / Wine 参照，
      每列给「素材原样」+「叠到 res/main.ico 后的 32px、16px 实际像素」
  下：候选配色色带 —— 每个候选给 48×48 素材与 32px 槽位真实像素，
      用来一次性拍板选哪个颜色（改色后重跑 tools/gen_shortcut_overlay.sh 即可）

依赖：rsvg-convert（仅渲染候选色时需要）+ Pillow。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG = os.path.join(ROOT, 'res/Emblem-symbolic-link.svg')
CURRENT = os.path.join(ROOT, 'res/shortcut_overlay.ico')
GREY = os.path.join(ROOT, '.workbuddy/shortcut_overlay-grey-2b2b2b.ico')
MAIN_ICON = os.path.join(ROOT, 'res/main.ico')
WINE_ICON = os.path.join(ROOT, 'wine/dlls/shell32/resources/shortcut.ico')
DEFAULT_OUT = os.path.join(ROOT, '.workbuddy/shortcut-overlay-preview.png')

Z = 4                      # 48px 素材的放大倍数
BG = (250, 250, 250)
ARROW_PCT = 75             # 与 content_view.c 的 SHORTCUT_ARROW_PERCENT 保持一致


def font(sz):
    for p in ('/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc',
              '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, sz)
            except Exception:
                pass
    return ImageFont.load_default()


def load_ico(path, box):
    """读 ico 的最大帧并等比放进 box×box 的画布，左下角对齐。"""
    im = Image.open(path).convert('RGBA')
    if im.size != (box, box):
        s = min(box / im.width, box / im.height)
        im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))),
                       Image.LANCZOS)
    canvas = Image.new('RGBA', (box, box), (0, 0, 0, 0))
    canvas.paste(im, (0, box - im.height), im)
    return canvas


def overlay_for(fill, tmp):
    """按 gen_shortcut_overlay.sh 的规则渲染一份 48×48 角标帧。"""
    s = open(SVG, encoding='utf-8').read()
    assert s.count('fill:#888a85') == 1, '源 svg 的箭头定位特征失效'
    s = s.replace('fill:#888a85', 'fill:' + fill)
    s = re.sub(r'opacity:[0-9.]+', 'opacity:1.0000000', s, count=1)
    src, png = os.path.join(tmp, 'b.svg'), os.path.join(tmp, 'b.png')
    open(src, 'w', encoding='utf-8').write(s)
    subprocess.run(['rsvg-convert', '-w', '256', '-h', '256', src, '-o', png],
                   check=True, capture_output=True)
    im = Image.open(png).convert('RGBA').rotate(-90, expand=True)
    im = im.crop(im.getbbox()).resize((20, 20), Image.LANCZOS)
    canvas = Image.new('RGBA', (48, 48), (0, 0, 0, 0))
    canvas.paste(im, (0, 48 - im.height), im)
    return canvas


def compose(icon, frame, size):
    """content_view.c 的合成几何：整帧缩到槽位边长 75%，贴到图标左下角。"""
    base = icon.resize((size, size), Image.LANCZOS).copy()
    side = max(1, round(size * ARROW_PCT / 100))
    ov = frame.resize((side, side), Image.LANCZOS)
    base.alpha_composite(ov, (0, size - side))
    return base


def on_bg(im, bg, scale=1):
    out = Image.new('RGBA', im.size, bg)
    out.alpha_composite(im)
    out = out.convert('RGB')
    return out.resize((im.width * scale, im.height * scale), Image.NEAREST) if scale > 1 else out


def build(out_path, cands):
    frame_cur = Image.open(CURRENT).convert('RGBA')
    if frame_cur.size != (48, 48):
        sys.exit('错误：%s 不是 48×48' % CURRENT)

    columns = [('当前 res/shortcut_overlay.ico', frame_cur),
               ('Wine 参照（仅比对，不用）', load_ico(WINE_ICON, 48))]
    if os.path.exists(GREY):
        columns.insert(0, ('上一版 深灰 #2b2b2b', Image.open(GREY).convert('RGBA')))
    else:
        print('提示：未找到上一版备份 %s，跳过「改色前」一列' % GREY)
    main_icon = Image.open(MAIN_ICON).convert('RGBA')

    tmp = tempfile.mkdtemp()
    try:
        cands = [(lab, overlay_for(f, tmp)) for lab, f in cands]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    PAD, GAP, LAB_H = 20, 36, 26
    ART = 48 * Z                 # 48px 素材放大后的边长
    S32, S16 = 32 * Z, 16 * Z    # 槽位像素放大后的边长
    SEC_H = 26                   # 每个区块的说明文字高度
    H_ART = SEC_H + LAB_H + ART + 22
    H_SLOT = SEC_H + S32 + 22
    H_CAND = SEC_H + LAB_H + ART + 22
    W = PAD * 2 + 4 * (ART + 16 + S32) + 3 * GAP   # 最宽的候选带决定画布宽度
    H = 46 + H_ART + H_SLOT + H_CAND + 16

    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    f_t, f_s, f_l = font(18), font(14), font(15)

    def paste(im, x, y, scale=Z, bg=(255, 255, 255, 255)):
        img.paste(on_bg(im, bg, scale), (x, y))

    d.text((PAD, 14), '快捷方式角标：改配色前后对照（素材与叠到真实图标的实际像素）',
           fill=(30, 30, 30), font=f_t)
    y = 46

    # ① 素材原样
    d.text((PAD, y), '① 素材原样 48×48（放大 %d×）' % Z, fill=(100, 100, 100), font=f_s)
    for i, (lab, fr) in enumerate(columns):
        x = PAD + i * (ART + GAP)
        d.text((x, y + SEC_H + 4), lab, fill=(50, 50, 50), font=f_l)
        paste(fr, x, y + SEC_H + LAB_H)
    y += H_ART

    # ② 叠到真实图标
    d.text((PAD, y), '② 叠到 res/main.ico 后的实际像素（左 32px / 右 16px）',
           fill=(100, 100, 100), font=f_s)
    for i, (lab, fr) in enumerate(columns):
        x = PAD + i * (S32 + 16 + S16 + GAP)
        paste(compose(main_icon, fr, 32), x, y + SEC_H)
        paste(compose(main_icon, fr, 16), x + S32 + 16, y + SEC_H)
    y += H_SLOT

    # ③ 候选配色
    d.text((PAD, y), '③ 候选配色（箭头上色后的 48×48 与 32px 实际像素）',
           fill=(100, 100, 100), font=f_s)
    for i, (lab, fr) in enumerate(cands):
        x = PAD + i * (ART + 16 + S32 + GAP)
        d.text((x, y + SEC_H + 4), lab, fill=(50, 50, 50), font=f_l)
        paste(fr, x, y + SEC_H + LAB_H)
        paste(compose(main_icon, fr, 32), x + ART + 16, y + SEC_H + LAB_H)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    img.save(out_path)
    print('已生成 %s（%dx%d）' % (out_path, W, H))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out', default=DEFAULT_OUT)
    ap.add_argument('cands', nargs='*',
                    help='候选颜色，形如 "#1e3a6e" 或 "标签=#1e3a6e"')
    a = ap.parse_args()

    if not a.cands:
        a.cands = ['#2b2b2b', '#1b3358', '#1e3a6e', '#1d4ed8']
    cands = []
    for c in a.cands:
        lab, _, fill = c.rpartition('=')
        cands.append((lab or fill, fill))
    build(a.out, cands)


if __name__ == '__main__':
    main()
