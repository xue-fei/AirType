"""
AirType 手持话筒外壳 v4 参数化建模
=====================================
用 trimesh + shapely 纯 Python 生成上下壳 STL，不依赖商业 CAD。

坐标系（单位 mm）：
  X = 壳体宽度 34（左=天线端 X小，右=C口端 X大）
  Y = 壳体长度 110（Y=0 底部封闭，Y=110 顶部出音）
  Z = 壳体厚度 26（Z=0 背面/下壳，Z=26 正面/上壳）
  分模面在 Z=13：下壳 Z∈[0,13]，上壳 Z∈[13,26]

运行：python build_case.py
输出：同目录 airtype_case_lower.stl / airtype_case_upper.stl

设计依据见 .workbuddy/memory/2026-11-23.md
"""

import math
import trimesh
from shapely.geometry import Polygon
from shapely.affinity import translate

# ---------------- 参数 ----------------
W = 34.0          # 宽度 X
L = 110.0         # 长度 Y
T = 26.0          # 厚度 Z
SPLIT = 13.0      # 分模面 Z
CORNER_R = 4.0    # 圆角半径
WALL = 2.0        # 壁厚

# 电池仓 103040（净尺寸，沿 X=宽30.8, Y=长40.8, Z=厚11）
BAT_W, BAT_L, BAT_H = 30.8, 40.8, 11.0
BAT_Y0 = 8.0      # 电池仓底部距壳体底端
BAT_Z0 = WALL     # 电池仓底 = 内底

# SuperMini 板槽 22.52×18，平放（长边沿 X）
BOARD_L, BOARD_W = 22.52, 18.0
SLOT_GAP = 0.19       # 单边间隙
SLOT_W = 1.4          # 槽宽（板厚向）
SLOT_DEPTH = 2.5      # 槽深
BOARD_Y0 = 60.0       # 板槽起始 Y（上半段）
BOARD_X_CENTER = W / 2

# C 口开孔 9×3（右侧壁 X=W）
C口_W, C口_H = 9.0, 3.0
C口_Y = 68.0          # C口中心 Y（对齐板位置）
C口_Z = SPLIT + 3.5   # C口中心 Z（上壳）

# 出音孔
SOUND_D = 4.0
MESH_D, MESH_DEPTH = 6.5, 1.0     # 防尘网槽
FOAM_D, FOAM_DEPTH = 9.0, 0.8     # 泡棉垫圈槽

# 螺丝（4×M2 沉头）
SCREW_INSET = 4.5
SCREW_D = 2.0
SCREW_HEAD_D = 3.8
SCREW_HEAD_DEPTH = 1.2
BOSS_OD = 5.0         # 下壳螺丝柱外径
# 螺丝 XY 位置
SCREW_POS = [
    (SCREW_INSET, SCREW_INSET),
    (W - SCREW_INSET, SCREW_INSET),
    (SCREW_INSET, L - SCREW_INSET),
    (W - SCREW_INSET, L - SCREW_INSET),
]

OUTPUT_DIR = "c:/Users/XUEFEI/Documents/MyProject/AirType/Hardware/CAD"


def rounded_box_xy(width, length, r, z0, z1, res=16):
    """在 XY 平面生成圆角矩形，沿 Z 拉伸 z0->z1
    最终外轮廓恰好是 [0,width]×[0,length]，圆角半径 r。
    做法：先构造内缩 r 的矩形，再 buffer(r) 向外扩张，得到正确外尺寸。"""
    poly = Polygon([(r, r), (width-r, r), (width-r, length-r), (r, length-r)])
    poly = poly.buffer(r, resolution=res, join_style=2)
    mesh = trimesh.creation.extrude_polygon(poly, height=(z1 - z0))
    mesh.apply_translation((0, 0, z0))
    return mesh


def box(x0, x1, y0, y1, z0, z1):
    m = trimesh.creation.box([x1-x0, y1-y0, z1-z0])
    m.apply_translation([(x0+x1)/2, (y0+y1)/2, (z0+z1)/2])
    return m


def cyl(radius, z0, z1, cx, cy, sections=48):
    m = trimesh.creation.cylinder(radius=radius, height=z1-z0, sections=sections)
    m.apply_translation((cx, cy, (z0+z1)/2))
    return m


def cut(base, tool, label=""):
    """布尔减，带校验"""
    global CUT_COUNT
    CUT_COUNT += 1
    result = base.difference(tool, engine="manifold")
    if not result.is_winding_consistent:
        print(f"  [warn] winding inconsistent after cut: {label}")
    return result


def join(base, tool, label=""):
    """布尔并，带校验"""
    result = base.union(tool, engine="manifold")
    if not result.is_winding_consistent:
        print(f"  [warn] winding inconsistent after union: {label}")
    return result


CUT_COUNT = 0


def build_lower():
    """下壳 Z∈[0,13]：电池仓 + 螺丝柱 + 止口凹槽"""
    print("== 下壳 ==")
    # 主实体：圆角矩形盒 Z 0->13
    shell = rounded_box_xy(W, L, CORNER_R, 0, SPLIT)

    # 挖内腔：从 Z=WALL 到分模面，四周留壁厚；但电池仓区单独处理
    # 内腔 XY 圆角矩形（缩进 WALL）
    inner = rounded_box_xy(W-2*WALL, L-2*WALL, CORNER_R-WALL,
                           WALL, SPLIT + 1, res=12)
    inner.apply_translation((WALL, WALL, 0))
    shell = cut(shell, inner, "inner cavity")

    # 电池仓：在内腔基础上，底仍为 WALL，四周卡壁额外加厚
    # 做法：电池仓是内腔下部的一个凹槽，宽 30.8 长40.8
    bat = box((W-BAT_W)/2, (W+BAT_W)/2, BAT_Y0, BAT_Y0+BAT_L,
              WALL, WALL + BAT_H)
    # 电池仓已包含在内腔里（内腔更宽），这里把它作为"保留实心"处理不合适
    # 实际上内腔就是电池仓的外框；电池由卡壁固定，卡壁=内腔壁。
    # 为简化：内腔净宽=30.8 正好夹住电池（宽34-2*WALL=30）→ 需要内腔宽30.8
    # 修正内腔宽度：让内腔净宽=30.8，即 WALL=(34-30.8)/2=1.6
    return shell


def build_lower_v2():
    """下壳（修正版）：内腔净宽=电池宽30.8，壁厚1.6"""
    print("== 下壳 ==")
    global WALL
    wall = (W - BAT_W) / 2.0   # 1.6
    shell = rounded_box_xy(W, L, CORNER_R, 0, SPLIT)

    # 内腔（XY 缩进 wall，Z 从 wall 到底）
    inner = rounded_box_xy(W-2*wall, L-2*wall, CORNER_R-wall,
                           wall, SPLIT + 1, res=12)
    inner.apply_translation((wall, wall, 0))
    shell = cut(shell, inner, "inner cavity (fits battery width)")

    # 电池仓长度方向：底部加一道挡壁（Y=BAT_Y0 处），顶部挡壁在 BAT_Y0+BAT_L
    # 用内腔长度=电池长度+少量，两端靠挡壁定位。这里内腔 L-2wall=106.8 太长，
    # 增加电池仓前后挡块（实心），缩小电池可移动范围到 BAT_L
    # 前挡块 Y∈[wall, BAT_Y0]
    front = box(wall, W-wall, wall, BAT_Y0, wall, SPLIT+1)
    shell = join(shell, front, 'front wall')
    # 后挡块：只做 3mm 薄壁定位电池，不填满上半段内腔（留走线空间）
    back = box(wall, W-wall, BAT_Y0+BAT_L, BAT_Y0+BAT_L+3.0,
               wall, SPLIT+1)
    shell = join(shell, back, 'back wall')

    # 出线缺口：挖穿 3mm 后挡壁，中线 1.2 宽通到底
    notch = box(W/2-0.6, W/2+0.6,
                BAT_Y0+BAT_L-0.2, BAT_Y0+BAT_L+3.2,
                wall-0.1, SPLIT+1)
    shell = cut(shell, notch, "wire notch")

    # 4 个螺丝柱（从底 Z=0 到分模面 SPLIT）
    for sx, sy in SCREW_POS:
        boss = cyl(BOSS_OD/2, 0, SPLIT, sx, sy)
        shell = join(shell, boss, 'screw boss')
    # 螺丝柱中心钻 M2 底孔（Ø1.6），从底贯穿到分模面
    for sx, sy in SCREW_POS:
        hole = cyl(0.8, -0.5, SPLIT+0.5, sx, sy)
        shell = cut(shell, hole, "M2 pilot hole")

    # 止口凹槽：分模面处一圈浅槽，宽 2，深 0.6（配 1.6 壁厚）
    lip_outer = rounded_box_xy(W, L, CORNER_R, SPLIT-0.6, SPLIT+0.5)
    lip_inner = rounded_box_xy(W-2*2.2, L-2*2.2, CORNER_R-2.2,
                               SPLIT-0.7, SPLIT+0.6)
    lip_inner.apply_translation((2.2, 2.2, 0))
    lip_ring = lip_outer.difference(lip_inner)
    shell = cut(shell, lip_ring, "tongue-groove lip")

    return shell


def build_upper():
    """上壳 Z∈[13,26]：板槽 + 出音孔 + C口孔 + 螺丝沉头孔 + 止口凸台"""
    print("== 上壳 ==")
    shell = rounded_box_xy(W, L, CORNER_R, SPLIT, T)

    # 内腔：从分模面往上挖，留壁厚 2，顶部保留 WALL
    inner = rounded_box_xy(W-2*WALL, L-2*WALL, CORNER_R-WALL,
                           SPLIT-1, T-WALL, res=12)
    inner.apply_translation((WALL, WALL, 0))
    shell = cut(shell, inner, "inner cavity")

    # SuperMini 板槽：两块侧壁上的横槽，间距 = 板长 22.52 + 2*gap = 22.9
    slot_pitch = BOARD_L + 2*SLOT_GAP   # 22.9
    x_left = (W - slot_pitch)/2
    x_right = x_left + slot_pitch
    z_slot_lo = T - WALL - 3.0          # 槽内底高度（距顶保留）
    # 左右槽：从内壁向壁厚里挖 SLOT_W 深
    left_slot = box(x_left-SLOT_W, x_left+0.6,
                    BOARD_Y0, BOARD_Y0+BOARD_W,
                    z_slot_lo, z_slot_lo+SLOT_W)
    right_slot = box(x_right-0.6, x_right+SLOT_W,
                     BOARD_Y0, BOARD_Y0+BOARD_W,
                     z_slot_lo, z_slot_lo+SLOT_W)
    shell = cut(shell, left_slot, "board slot L")
    shell = cut(shell, right_slot, "board slot R")

    # C 口开孔：右侧壁 X=W，9(Y)×3(Z) 矩形，带 1mm 挡边（挡边=孔周边不做通）
    cop = box(W-3.0, W+0.5,
              C口_Y-C口_W/2, C口_Y+C口_W/2,
              C口_Z-C口_H/2, C口_Z+C口_H/2)
    shell = cut(shell, cop, "USB-C port")

    # 顶部 Ø4 出音孔（沿 Z 从顶面向下）
    sound = cyl(SOUND_D/2, T-WALL-1, T+0.5, W/2, L-8)
    shell = cut(shell, sound, "sound hole")
    # 防尘网槽 Ø6.5 深1（从顶）
    mesh = cyl(MESH_D/2, T-MESH_DEPTH, T+0.5, W/2, L-8)
    shell = cut(shell, mesh, "mesh recess")
    # 泡棉垫圈槽 Ø9 深0.8
    foam = cyl(FOAM_D/2, T-FOAM_DEPTH, T+0.5, W/2, L-8)
    shell = cut(shell, foam, "foam recess")

    # PTT 按键孔（侧壁上部，X 右侧，靠顶部）
    ptt = box(W-3.0, W+0.5, L-28, L-20, SPLIT+6, SPLIT+11)
    shell = cut(shell, ptt, "PTT button hole")

    # 电源开关孔（侧壁下部）
    pwr = box(W-3.0, W+0.5, 22, 30, SPLIT+4, SPLIT+8)
    shell = cut(shell, pwr, "power switch hole")

    # 4 个 M2 沉头通孔（从顶面 Z=T 贯穿到分模面）
    for sx, sy in SCREW_POS:
        through = cyl(SCREW_D/2, SPLIT-0.5, T+0.5, sx, sy)
        shell = cut(shell, through, "M2 through")
        head = cyl(SCREW_HEAD_D/2, T-SCREW_HEAD_DEPTH, T+0.5, sx, sy)
        shell = cut(shell, head, "M2 countersink")

    # 止口凸台：分模面向下凸 0.6，与下壳凹槽配合
    lip_outer = rounded_box_xy(W-2*0.3, L-2*0.3, CORNER_R-0.3,
                               SPLIT-0.6, SPLIT+0.5)
    lip_inner = rounded_box_xy(W-2*2.0, L-2*2.0, CORNER_R-2.0,
                               SPLIT-0.7, SPLIT+0.6)
    lip_inner.apply_translation((2.0, 2.0, 0))
    lip_ring = lip_outer.difference(lip_inner)
    shell = join(shell, lip_ring, 'lip')

    return shell


def check(mesh, name):
    print(f"{name}: watertight={mesh.is_watertight} "
          f"winding={mesh.is_winding_consistent} "
          f"vol={mesh.volume:.1f} faces={len(mesh.faces)}")
    if not mesh.is_watertight:
        print(f"  [ERROR] {name} is not watertight!")
    return mesh.is_watertight


def main():
    lower = build_lower_v2()
    check(lower, "lower")
    upper = build_upper()
    check(upper, "upper")

    # 导出 STL（二进制，单位 mm）
    lower.export(f"{OUTPUT_DIR}/airtype_case_lower.stl")
    upper.export(f"{OUTPUT_DIR}/airtype_case_upper.stl")
    print(f"Exported: {OUTPUT_DIR}/airtype_case_lower.stl")
    print(f"Exported: {OUTPUT_DIR}/airtype_case_upper.stl")
    print(f"Total boolean cuts: {CUT_COUNT}")


if __name__ == "__main__":
    main()
