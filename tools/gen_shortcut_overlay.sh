#!/bin/sh
# 重新生成快捷方式角标素材 res/shortcut_overlay.ico
#
# 素材来源
#   res/Emblem-symbolic-link.svg —— Tango 图标主题的同名图标。作者声明无条件
#   供任何人使用：Tango 0.8.90 的 COPYING 原文为
#   "The icons in this repository are herefore released into the Public Domain."
#   故本素材为公有领域，随 MIT 的 WFM 分发不产生任何附加义务。
#
# 原图全部图层保留，只改箭头配色（走了两条弯路才定下来）
#   res/Emblem-symbolic-link.svg 是 Inkscape 画的「徽章」：浅灰径向渐变底板
#   （#ffffff → #dcdcdc）+ 一圈白色内描边 + 右下角投影 + 一条画箭头的 <path>。
#   它缩到角标尺寸之所以糊，**问题只在箭头对比度**：fill:#888a85 还带 0.7 不
#   透明度，叠在同样是浅灰的底板上几乎同色，缩到 5×5 像素就整块拉平了。
#   底板 / 内白描边 / 投影本身是好的——缩下来仍有立体感，正是它们让角标看起来
#   与 Windows / Wine 的观感一致。所以这里**只动箭头那一条 path**：
#       fill:#888a85 → ARROW_COLOR，元素 opacity 0.7 → 1
#   其余图层一律原样保留。
#   （弯路一：整张原样渲染 —— 就是上面说的糊。弯路二：只把箭头单独抠出来、
#    丢掉全部装饰层再补白描边 —— 在 16px 槽位下是个又粗又黑的钩子，更难看。）
#
# ARROW_COLOR 就是箭头颜色，默认深蓝 #1e3a6e。灰阶 #2b2b2b 偏中性，与 Windows
# 原角标的深蓝黑调子差一点；换成深蓝后叠在浅色图标上更贴近 Windows 观感。
# 想再调就这么跑（不必改脚本）：
#   ARROW_COLOR='#14346b' ./tools/gen_shortcut_overlay.sh
# 只改这一处，底板 / 白描边 / 投影全部沿用原图，所以任何配色都不会糊。
#
# 变换步骤（顺序不能换）
#   1. 原样读入 SVG，只改箭头那条 path 的着色。用 fill:#888a85 定位箭头，并要求
#      它全文件只出现一次（这份素材的指纹），次数不对就直接报错退出
#   2. rsvg-convert 渲染成 256×256 PNG（超采样，缩小后边缘才干净）
#   3. 逆时针旋转 90° —— 该图标的原生朝向与 Windows 角标不符，转过来才一致
#      （与 Wine 原角标的箭头轮廓 IoU：逆时针 0.267 / 顺时针 0.194 /
#      不旋转 0.129 / 180° 0.122，逆时针最优）
#   4. 裁掉透明边 → 缩到 20×20 → 贴到 48×48 画布的左下角
#
# 为什么是这些数字
#   20/48≈42% 的占位是刻意与 content_view.c 的合成几何配套的：那里把整帧缩到
#   槽位边长的 75%（SHORTCUT_ARROW_PERCENT），两者相乘 ≈31%，即角标最终约占
#   图标边长的 31%（与更换素材前一致，观感不变）。
#   48px 是因为 48 能被 12（16px 槽位）和 24（32px 槽位）整除，缩小走
#   downsampleIconPixels() 的整数倍盒式均值，不会出现插值脏边。
#
# 依赖：rsvg-convert（librsvg）+ ImageMagick（magick 或 convert）+ python3
set -e
cd "$(dirname "$0")/.."

SVG=res/Emblem-symbolic-link.svg
OUT=res/shortcut_overlay.ico
ARROW_COLOR=${ARROW_COLOR:-#1e3a6e}   # 箭头颜色，改这个就换配色
DARK_LUM_MAX=${DARK_LUM_MAX:-100}     # 自检：箭头像素亮度上限（换浅色配色时调大）
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# 1) 只改箭头那条 path 的着色，其余图层原封不动
python3 - "$SVG" "$TMP/badge.svg" "$ARROW_COLOR" <<'PY'
import re, sys

src, dst, new_fill = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(src, encoding='utf-8').read()

ARROW_FILL = 'fill:#888a85'
NEW_FILL = 'fill:' + new_fill
if not re.fullmatch(r'#[0-9a-fA-F]{6}', new_fill):
    sys.exit('错误：ARROW_COLOR=%r 不是 #rrggbb 格式' % new_fill)
n = s.count(ARROW_FILL)
if n != 1:
    sys.exit('错误：%s 里 %s 出现 %d 次（预期恰好 1 次）——箭头定位特征已失效，'
             '请先确认源图标的配色有没有变' % (src, ARROW_FILL, n))

# 同一条 path 的 style 里既有 fill 也有元素级 opacity（0.7，箭头原本半透明，
# 叠在浅灰底板上才看不清）。两处必须一起改，只改 fill 会得到一只灰箭头。
def fix_style(m):
    st = m.group(0).replace(ARROW_FILL, NEW_FILL)
    return re.sub(r'opacity:[0-9.]+', 'opacity:1.0000000', st, count=1)

s, k = re.subn(r'style="[^"]*%s[^"]*"' % re.escape(ARROW_FILL), fix_style, s)
if k != 1:
    sys.exit('错误：带 %s 的 style 属性命中 %d 处（预期 1 处）' % (ARROW_FILL, k))

open(dst, 'w', encoding='utf-8').write(s)
PY

# 2) 渲染 → 3) 逆时针 90° → 4) 裁边 → 缩到 20x20 → 贴 48x48 左下角
rsvg-convert -w 256 -h 256 "$TMP/badge.svg" -o "$TMP/badge.png"

if command -v magick >/dev/null 2>&1; then
    IM=magick
else
    IM=convert
fi
"$IM" "$TMP/badge.png" -rotate -90 -trim +repage -resize 20x20 \
      -background none -gravity SouthWest -extent 48x48 "$OUT"

# 自检：成品必须是一个近似正方形、落在画布左下角 20x20 的角标，而且里面得
# 真的有深色箭头。底板选错图层、或者箭头着色没生效时，形状/暗像素会立刻异常，
# 这里直接报错，避免半成品又被编进 exe。
python3 - "$OUT" "$DARK_LUM_MAX" <<'PY'
import sys
import numpy as np
from PIL import Image

path, lum_max = sys.argv[1], float(sys.argv[2])
im = Image.open(path).convert('RGBA')
bb = im.getbbox()
if bb is None:
    sys.exit('错误：%s 是空图' % path)
w, h = bb[2] - bb[0], bb[3] - bb[1]
ratio = w / h if h else 0
if not (0.90 <= ratio <= 1.11) or w > 20 or h > 20:
    sys.exit('错误：%s 内容 bbox=%s（%dx%d，宽高比 %.2f），不像一个正方形角标' % (path, bb, w, h, ratio))

a = np.asarray(im).astype(np.int16)
g = a[28:48, 0:20]
lum = g[..., :3].mean(axis=2)
al = g[..., 3]
dark = int(((lum < lum_max) & (al > 200)).sum())
if dark < 10:
    sys.exit('错误：%s 左下角只有 %d 个比亮度 %.0f 更暗的像素——箭头着色八成没生效'
             '（对比度不足）。若确实换了浅色配色，用 DARK_LUM_MAX=<更大值> 重跑'
             % (path, dark, lum_max))

print('已生成 %s（内容 bbox=%s，%dx%d，深色箭头像素 %d）' % (path, bb, w, h, dark))
PY
