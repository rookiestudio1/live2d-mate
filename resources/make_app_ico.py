#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
產生 Windows 用的 resources/app.ico。

為什麼要有這支腳本：.ico 是二進位產物，光看檔案無法知道每個尺寸是從哪張圖來的。
小尺寸不能直接把 1024² 的插畫縮下去 —— 縮到 16² 只會糊成一團色塊，
所以 16／32 兩個「小圖示帶」改用系統匣那組已經簡化過的線稿：

  | ico 尺寸 | 來源           | 縮放 |
  |----------|----------------|------|
  | 16       | tray.png (32²) | ÷2   |
  | 32       | tray@2x.png (64²) | ÷2 |
  | 其餘     | icon.png (1024²)  | 直接縮 |

tray 那兩張刻意用 2 倍圖再縮一半（而不是直接畫 16²），LANCZOS 下線條比較實。

用法：python resources/make_app_ico.py   （需要 Pillow）
"""

from pathlib import Path
from PIL import Image

HERE = Path(__file__).parent

# Windows 會依 DPI 縮放挑用的尺寸：16/20/24 是小圖示（標題列、檔案總管清單），
# 32/40/48 是工作列與 Alt-Tab，256 給檔案總管的超大圖示與釘選。
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]

# 特例：這些尺寸不從 icon.png 來（見檔頭表格）。
OVERRIDES = {16: "tray.png", 32: "tray@2x.png"}


def load(name: str) -> Image.Image:
  im = Image.open(HERE / name)
  return im if im.mode == "RGBA" else im.convert("RGBA")


def main() -> None:
  base = load("icon.png")
  frames = {}
  for size in SIZES:
    src = load(OVERRIDES[size]) if size in OVERRIDES else base
    frames[size] = src.resize((size, size), Image.LANCZOS)

  out = HERE / "app.ico"
  # 兩個坑，順序不能亂：
  # 1. Pillow 的 ICO 編碼器拿「被 save 的那張」的尺寸當上限，比它大的 sizes 會被
  #    整個跳過 —— 所以基底必須是最大的 256²，其餘全部走 append_images。
  # 2. 編碼器會在 [base] + append_images 裡找尺寸剛好相符的直接寫入，
  #    找不到才自己縮；OVERRIDES 的特例就是靠這個生效的。
  largest = max(SIZES)
  frames[largest].save(out, format="ICO", sizes=[(s, s) for s in SIZES],
                       append_images=[frames[s] for s in SIZES if s != largest])
  print(f"已寫出 {out}（{len(SIZES)} 種尺寸：{', '.join(map(str, SIZES))}）")


if __name__ == "__main__":
  main()
