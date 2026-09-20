from pinout.core import Group, Image
from pinout.components.layout import Diagram_2Rows
from pinout.components.pinlabel import PinLabelGroup, PinLabel # PinLabelを追加
from pinout.components.text import TextBlock
from pinout.components import leaderline as lline
from pinout.components.legend import Legend

# Text
TITLE = "<tspan class='h1'>Analog Mimic 01</tspan>"

DESCRIPTION = """Ultra-low-cost analog front-end emulator 
using a sub-dollar MCU.
Mixed-signal prototyping interface."""

LEGEND = [
    ("Analog", "analog"),
    ("I2C / SWD", "comms"),
    ("Ground", "gnd"),
    ("System", "gpio"),
    ("Power", "pwr"),
]

# Left header (上から順)
LEFT_HEADER = [
    [("AIN", "analog")],
    [("VCC", "pwr")],
    [("GND", "gnd")],
]

# Right header (上から順)
RIGHT_HEADER = [
    [("AOUT", "analog")],
    [("GND", "gnd")],
    [("SCL", "comms")],
    [("SDA", "comms")],
    [("GND", "gnd")],
    [("VCC", "pwr")],
]

# Bottom header (左から順: Pin 1 -> Pin 5)
LOWER_HEADER = [
    [("RESET", "gpio")],
    [("GND", "gnd")],
    [("SWCLK", "comms")],
    [("SWDIO", "comms")],
    [("VCC", "pwr")],
]

# 1. キャンバス全体を大きくして余白を調整 (幅800, 全体高さ800, 画像エリア高さ550)
diagram = Diagram_2Rows(850, 800, 600, "diagram")
diagram.add_stylesheet("../pinout_styles.css", embed=True)

# 2. 画像の配置位置 (左150px, 上50px)
graphic = diagram.panel_01.add(Group(100, 50))
hardware = graphic.add(Image("mimic01.png", embed=True))

# 3. 各ピンの基準座標 (ペイントソフト等で測ったピクセル座標を入力してください)
# ！！！重要！！！
# ここで指定する (x, y) は、読み込んだ画像上での各ピンの「中心座標」です。
# 画像の解像度に合わせて数値を書き換えてください。
hardware.add_coord("left_start", 69, 112)    # 左側の一番上(AIN)の座標
hardware.add_coord("right_start", 576, 66)   # 右側の一番上(AOUT)の座標
hardware.add_coord("bottom_start", 254, 320)  # 下側の一番左(RESET)の座標

# ピン同士の間隔（ピッチ）
hardware.add_coord("pin_pitch_v", 0, 46)      # 縦方向のピン間隔
hardware.add_coord("pin_pitch_h", 46, 0)      # 横方向のピン間隔
  

# 左側のピンラベル
graphic.add(
    PinLabelGroup(
        x=hardware.coord("left_start").x,
        y=hardware.coord("left_start").y,
        pin_pitch=hardware.coord("pin_pitch_v", raw=True),
        label_start=(80, 0),
        label_pitch=(0, 30),
        scale=(-1, 1),
        labels=LEFT_HEADER,
    )
)

# 右側のピンラベル
graphic.add(
    PinLabelGroup(
        x=hardware.coord("right_start").x,
        y=hardware.coord("right_start").y,
        pin_pitch=hardware.coord("pin_pitch_v", raw=True),
        label_start=(80, 0),
        label_pitch=(0, 30),
        labels=RIGHT_HEADER,
    )
)

# ==========================================================
# 下側ピンラベルの左右振り分け配置（修正版）
# ==========================================================
# 2つの PinLabelGroup を使って階段状に配置します。
pitch_h = hardware.coord("pin_pitch_h").x
graphic.add(PinLabelGroup(
    x=hardware.coord("bottom_start").x,
    y=hardware.coord("bottom_start").y,
    scale=(-1, 1),             # 左下にラベルを伸ばす
    pin_pitch=hardware.coord("pin_pitch_h", raw=True), # ピン間のピッチ（横）
    label_start=(60, 60),     # 最初のラベルの位置（下に50px、左に60px）
    label_pitch=(0, 30),       # 各ラベルを縦に30pxずつ階段状にズラす
    labels=LOWER_HEADER,
    leaderline=lline.Curved(direction="vh"), # 曲がる引き出し線
))



# --- パネル2(テキスト・凡例) ---

# 5. タイトルの配置調整 (y=30にして上端とのスペースを確保)
title_block = diagram.panel_02.add(
    TextBlock(TITLE, x=20, y=30, line_height=18, tag="panel title_block")
)

# 6. 説明文の配置調整 (y=80にしてタイトルとのスペースを確保, 幅を固定して見やすく)
diagram.panel_02.add(
    TextBlock(
        DESCRIPTION, x=20, y=80, width=450, 
        height=diagram.panel_02.height - title_block.height - 100, 
        line_height=18, tag="panel text_block"
    )
)

# 7. 凡例 (Legend) の配置調整 (キャンバス幅が1200になったため、x=900に配置して右端からはみ出さないように)
legend = diagram.panel_02.add(
    Legend(LEGEND, x=600, y=8)
)

###############################################################
# python ..\gen_transparent_png.py mimic01_pcb.png mimic01.png
# py -m pinout.manager --export pinout_diagram.py mimic01.svg
###############################################################
