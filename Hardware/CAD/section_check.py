"""截面切片校验：填充式截面，材料填色 / 空腔留白，直观看出壳体壁厚与挖空。"""
import trimesh
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties
from matplotlib.patches import PathPatch
from matplotlib.path import Path

# 中文字体（与 preview.py 一致），避免标题/标签显示成方框乱码
fp = FontProperties(fname="C:/Windows/Fonts/msyh.ttc")
plt.rcParams["font.family"] = fp.get_name()
plt.rcParams["axes.unicode_minus"] = False

D = "c:/Users/XUEFEI/Documents/MyProject/AirType/Hardware/CAD"
lower = trimesh.load(f"{D}/airtype_case_lower.stl")
upper = trimesh.load(f"{D}/airtype_case_upper.stl")

FILL = "#9dc4e8"      # 材料填充色
EDGE = "#205a8a"      # 轮廓线色


def poly_to_world_2d(poly, T, plane):
    """把 to_planar 提取的 shapely polygon 映射回世界坐标，再投影到指定平面。"""
    rings = [poly.exterior] + list(poly.interiors)
    parts = []
    for ring in rings:
        c = np.array(ring.coords)
        pts2 = np.column_stack([c[:, 0], c[:, 1],
                                np.zeros(len(c)), np.ones(len(c))])
        world = (T @ pts2.T).T[:, :3]
        if plane == "XZ":
            p = world[:, [0, 2]]
        elif plane == "XY":
            p = world[:, [0, 1]]
        elif plane == "YZ":
            p = world[:, [1, 2]]
        parts.append(p)
    return parts


def draw_slice(mesh, origin, normal, plane, ax, title, xlim, ylim, xlabel, ylabel):
    """在给定平面切一片，用填充多边形画出材料截面。"""
    sec = mesh.section(plane_origin=origin, plane_normal=normal)
    if sec is None or len(sec.entities) == 0:
        ax.set_title(title + "\n(无截面)", color="#c0392b")
        ax.set_xlim(*xlim); ax.set_ylim(*ylim)
        return 0

    p2, T = sec.to_planar()
    polys = p2.polygons_full
    if len(polys) == 0:
        polys = list(p2.polygons_closed)

    count = 0
    for poly in polys:
        parts = poly_to_world_2d(poly, T, plane)
        verts, codes = [], []
        for p in parts:
            verts.append(p[0]); codes.append(Path.MOVETO)
            for v in p[1:]:
                verts.append(v); codes.append(Path.LINETO)
            verts.append(p[0]); codes.append(Path.CLOSEPOLY)
        path = Path(np.array(verts), codes)
        ax.add_patch(PathPatch(path, facecolor=FILL, edgecolor=EDGE,
                               linewidth=0.9))
        count += 1

    ax.set_title(title, fontsize=10)
    ax.set_xlim(*xlim); ax.set_ylim(*ylim)
    ax.set_aspect("equal")
    ax.set_xlabel(xlabel); ax.set_ylabel(ylabel)
    ax.grid(alpha=0.3)
    return count


fig, axes = plt.subplots(2, 3, figsize=(15, 10))
fig.suptitle("AirType 外壳截面校验（蓝色=材料，白色=空腔/孔）", fontsize=14)

# 1. 下壳电池仓中部 Y=28 -> XZ：应见 U 形凹槽（两侧壁+底）
n1 = draw_slice(lower, [0, 28, 0], [0, 1, 0], "XZ", axes[0, 0],
                "下壳·电池仓截面 (Y=28)", (-2, 36), (-2, 26), "X", "Z")

# 2. 下壳上段 Y=68 -> XZ：应见平底板 + 侧壁（无电池仓）
n2 = draw_slice(lower, [0, 68, 0], [0, 1, 0], "XZ", axes[0, 1],
                "下壳·上段截面 (Y=68)", (-2, 36), (-2, 26), "X", "Z")

# 3. 下壳电池仓俯视 Z=8 -> XY：应见电池仓矩形 + 四角螺丝孔柱
n3 = draw_slice(lower, [0, 0, 8], [0, 0, 1], "XY", axes[0, 2],
                "下壳·电池仓俯视 (Z=8)", (-2, 36), (0, 58), "X", "Y")

# 4. 上壳板槽区 Y=68 -> XZ：应见右侧壁 C 口缺口 + 顶部板槽
n4 = draw_slice(upper, [0, 68, 0], [0, 1, 0], "XZ", axes[1, 0],
                "上壳·板槽/C口 (Y=68)", (-2, 36), (11, 27), "X", "Z")

# 5. 上壳顶面出音孔 Z=25.5 -> XY：应见 Ø4 出音孔 + 4 个螺丝孔
n5 = draw_slice(upper, [0, 0, 25.5], [0, 0, 1], "XY", axes[1, 1],
                "上壳·顶面出音孔 (Z=25.5)", (-2, 36), (88, 112), "X", "Y")

# 6. 上壳右侧壁 X=33 -> YZ：应见 C 口矩形开口 (Y≈68, Z≈16.5)
n6 = draw_slice(upper, [33, 0, 0], [1, 0, 0], "YZ", axes[1, 2],
                "上壳·C口侧壁 (X=33)", (50, 86), (11, 27), "Y", "Z")

plt.tight_layout(rect=[0, 0, 1, 0.96])
out = f"{D}/section_check.png"
plt.savefig(out, dpi=130, facecolor="white")
print("saved", out)
print("polygons per view:", [n1, n2, n3, n4, n5, n6])
