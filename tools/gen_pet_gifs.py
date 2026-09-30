#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_pet_gifs.py —— 生成 AniDesk-Qt 内置宠物形象（assets/pets/*.gif）

为五个状态各生成一段带 Alpha 透明通道的 GIF 动画：
  idle.gif      待机：上下轻浮 + 周期性眨眼
  talking.gif   说话：嘴巴快速张合
  thinking.gif  思考：缓慢摇晃 + 头顶渐隐的思考泡泡
  happy.gif     开心：蹦跳 + 环绕星光 + 大笑
  sleeping.gif  睡觉：深呼吸起伏 + 飘起的 Z z

用法：
  python tools/gen_pet_gifs.py

依赖：pip install Pillow
"""
import math
import os

from PIL import Image, ImageDraw

SIZE = 192
OUT_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "assets", "pets"))

# 配色（RGBA）
BODY = (111, 199, 255, 255)      # 天蓝色小团子
OUTLINE = (52, 120, 180, 255)
INK = (45, 55, 66, 255)          # 眼睛/嘴的深色
MOUTH = (96, 44, 50, 255)        # 口腔
BLUSH = (255, 130, 150, 95)      # 腮红

# 兼容不同版本的 Pillow：FASTOCTREE 量化支持 RGBA 透明通道
try:
    FASTOCTREE = Image.Quantize.FASTOCTREE
except AttributeError:  # pragma: no cover - 旧版 Pillow
    FASTOCTREE = Image.FASTOCTREE


# ---------------------------------------------------------------- 画笔工具 --
def blob(d, cx, cy, rx, ry):
    """身体：圆滚滚的椭圆 + 描边"""
    d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=BODY, outline=OUTLINE, width=4)


def eyes(d, cx, cy, dx, ry, blink=False, look=(0, 0)):
    """两只眼睛；blink=True 画闭眼横线，look 为瞳孔偏移"""
    for sx in (-1, 1):
        ex = cx + sx * dx
        if blink:
            d.line([ex - 9, cy, ex + 9, cy], fill=INK, width=4)
        else:
            d.ellipse([ex - 11, cy - ry, ex + 11, cy + ry],
                      fill=(255, 255, 255, 255), outline=INK, width=2)
            px, py = ex + look[0], cy + look[1]
            d.ellipse([px - 5, py - 5, px + 5, py + 5], fill=INK)


def smile(d, cx, cy, half_w=10, big=False):
    """微笑弧线（PIL 角度顺时针，90° 在正下方 → 底部弧线即微笑）"""
    h = 16 if big else 12
    d.arc([cx - half_w, cy - 6, cx + half_w, cy - 6 + h], 20, 160, fill=INK, width=3)


def blush(d, cx, cy, dx):
    for sx in (-1, 1):
        d.ellipse([cx + sx * dx - 9, cy - 5, cx + sx * dx + 9, cy + 7], fill=BLUSH)


def sparkle(d, x, y, r, color):
    d.line([x - r, y, x + r, y], fill=color, width=3)
    d.line([x, y - r, x, y + r], fill=color, width=3)


def z_glyph(d, x, y, s, color, w=3):
    """用三条线画一个 Z"""
    d.line([x, y, x + s, y], fill=color, width=w)
    d.line([x + s, y, x, y + s], fill=color, width=w)
    d.line([x, y + s, x + s, y + s], fill=color, width=w)


# ---------------------------------------------------------------- 渲染函数 --
def r_idle(d, i, n):
    t = i / n
    bob = 3 * math.sin(2 * math.pi * t)
    cx, cy = 96, 108 + bob
    blob(d, cx, cy, 62, 56)
    eyes(d, cx, cy - 14, 24, 14, blink=i in (14, 15))   # 循环后段眨一次眼
    smile(d, cx, cy + 16)
    blush(d, cx, cy + 8, 44)


def r_talking(d, i, n):
    t = i / n
    bob = 2 * math.sin(4 * math.pi * t)
    cx, cy = 96, 106 + bob
    blob(d, cx, cy, 62, 56)
    eyes(d, cx, cy - 14, 24, 14)
    open_h = 3 + 8 * abs(math.sin(2 * math.pi * t * 2))  # 嘴巴快速张合
    d.ellipse([cx - 9, cy + 10 - open_h, cx + 9, cy + 16 + open_h],
              fill=MOUTH, outline=INK, width=2)
    blush(d, cx, cy + 8, 44)


def r_thinking(d, i, n):
    t = i / n
    sway = 5 * math.sin(2 * math.pi * t)                 # 缓慢左右摇晃
    cx, cy = 96 + sway, 108
    blob(d, cx, cy, 62, 56)
    eyes(d, cx, cy - 16, 24, 12, look=(4, -4))           # 瞳孔望向右上方
    d.line([cx - 8, cy + 18, cx + 10, cy + 18], fill=INK, width=3)  # 抿嘴
    blush(d, cx, cy + 8, 44)
    ph = t * 2.5
    for k in range(3):                                   # 头顶渐隐的思考泡泡
        p = (ph + k * 0.33) % 1.0
        a = int(210 * max(0.0, 1.0 - p))
        x = cx + 44 + k * 10
        y = 30 - k * 9 - p * 8
        r = 4 + k * 2
        d.ellipse([x - r, y - r, x + r, y + r], fill=(235, 232, 255, a))


def r_happy(d, i, n):
    t = i / n
    hop = abs(math.sin(2 * math.pi * t)) * 14            # 蹦跳
    cx, cy = 96, 110 - hop
    blob(d, cx, cy, 62, 56)
    eyes(d, cx, cy - 14, 24, 15)
    smile(d, cx, cy + 14, half_w=14, big=True)           # 大笑
    blush(d, cx, cy + 10, 46)
    for k in range(4):                                   # 环绕的星光
        p = (t + k / 4) % 1.0
        a = int(220 * (1 - p))
        ang = p * 2 * math.pi
        x = 96 + 82 * math.cos(ang)
        y = 66 + 46 * math.sin(ang)
        sparkle(d, x, y, 5, (255, 230, 120, a))


def r_sleeping(d, i, n):
    t = i / n
    ry = 56 + 3 * math.sin(2 * math.pi * t)              # 深呼吸起伏
    cx, cy = 96, 110
    blob(d, cx, cy, 64, ry)
    for sx in (-1, 1):                                   # 闭眼（下弯弧线）
        ex = cx + sx * 24
        d.arc([ex - 10, cy - 22, ex + 10, cy - 4], 0, 180, fill=INK, width=4)
    d.ellipse([cx - 4, cy + 12, cx + 4, cy + 18], fill=MOUTH)
    for k in range(2):                                   # 飘起的 Z z
        p = (t + k * 0.5) % 1.0
        a = int(230 * (p / 0.2 if p < 0.2 else (1 - p) / 0.8))
        s = 8 + k * 4 + 4 * p
        z_glyph(d, cx + 54 + k * 16, cy - 74 - p * 24 - k * 8, s, (240, 246, 255, a))


# ---------------------------------------------------------------- 输出封装 --
def make_gif(name, n_frames, delay_ms, render):
    frames = []
    for i in range(n_frames):
        img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))  # 全透明画布
        render(ImageDraw.Draw(img), i, n_frames)
        frames.append(img.quantize(colors=255, method=FASTOCTREE))

    path = os.path.join(OUT_DIR, name)
    frames[0].save(
        path,
        save_all=True,
        append_images=frames[1:],
        duration=delay_ms,
        loop=0,
        disposal=2,  # 每帧恢复为背景，保证透明区域干净
        transparency=frames[0].info.get("transparency", 0),
    )
    print("written:", path)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    make_gif("idle.gif", 20, 90, r_idle)
    make_gif("talking.gif", 12, 70, r_talking)
    make_gif("thinking.gif", 24, 90, r_thinking)
    make_gif("happy.gif", 16, 70, r_happy)
    make_gif("sleeping.gif", 24, 110, r_sleeping)

    # 简单自检：帧数、透明索引、角落像素应为全透明
    for name in ("idle", "talking", "thinking", "happy", "sleeping"):
        im = Image.open(os.path.join(OUT_DIR, name + ".gif"))
        corner = im.convert("RGBA").getpixel((0, 0))
        print(f"check {name}: frames={im.n_frames} transparency={im.info.get('transparency')} corner_a={corner[3]}")


if __name__ == "__main__":
    main()
