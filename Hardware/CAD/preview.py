"""渲染 STL 预览图（z-buffer 软件光栅化，逐像素深度缓冲，无画家算法排序错误）。

- 等轴测投影 + 背面剔除 + 逐像素深度测试 + 平面法线着色
- 下壳从分模面看进去见电池仓；上壳镜像翻转后见板槽/C口；合拢图双色
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
import trimesh

D = "c:/Users/XUEFEI/Documents/MyProject/AirType/Hardware/CAD"
lower = trimesh.load(f"{D}/airtype_case_lower.stl")
upper = trimesh.load(f"{D}/airtype_case_upper.stl")

FONT = "C:/Windows/Fonts/msyh.ttc"
W, H = 560, 620


def render(parts, cam_dir, center, scale, size=(W, H), bg=(250, 250, 252)):
    """parts: [(mesh, color, flip_z), ...]；cam_dir 从相机指向物体。"""
    tri_groups, col_groups = [], []
    for mesh, color, flip_z in parts:
        t = np.array(mesh.triangles, dtype=float)
        if flip_z:
            t[:, :, 2] = 2 * 13.0 - t[:, :, 2]   # 绕分模面 Z=13 镜像
            t[:, [1, 2], :] = t[:, [2, 1], :]    # 修正镜像后的绕序
        tri_groups.append(t)
        col_groups.append(np.tile(np.array(color, float), (len(t), 1)))
    tris = np.concatenate(tri_groups)
    basecolors = np.concatenate(col_groups)

    d = np.array(cam_dir, dtype=float)
    d /= np.linalg.norm(d)
    up = np.array([0.0, 0.0, 1.0])
    right = np.cross(d, up); right /= np.linalg.norm(right)
    cam_up = np.cross(right, d)

    # 面法线 + 背面剔除
    v0, v1, v2 = tris[:, 0], tris[:, 1], tris[:, 2]
    n = np.cross(v1 - v0, v2 - v0)
    nl = np.linalg.norm(n, axis=1)
    n = n / np.where(nl > 0, nl, 1)[:, None]
    keep = (n @ d) < -1e-6
    tris, basecolors, n = tris[keep], basecolors[keep], n[keep]

    # 投影
    c = np.array(center, dtype=float)
    pts = tris - c
    px = (pts @ right) * scale + size[0] / 2
    py = -(pts @ cam_up) * scale + size[1] / 2
    pz = pts @ d                                  # (M,3) 每顶点相机深度，越大越远

    # 平面着色
    light = np.array([0.35, -0.45, 0.82]); light /= np.linalg.norm(light)
    shade = np.clip(np.abs(n @ light), 0, 1) * 0.6 + 0.4
    colors = np.clip(basecolors * shade[:, None], 0, 255).astype(np.uint8)

    # z-buffer 逐像素深度测试
    h, w = size[1], size[0]
    zbuf = np.full((h, w), np.inf, dtype=float)
    cbuf = np.empty((h, w, 3), dtype=np.uint8)
    cbuf[:] = bg

    for i in range(len(tris)):
        x0, x1, x2 = px[i]
        y0, y1, y2 = py[i]
        xmin = max(0, int(np.floor(min(x0, x1, x2))))
        xmax = min(w - 1, int(np.ceil(max(x0, x1, x2))))
        ymin = max(0, int(np.floor(min(y0, y1, y2))))
        ymax = min(h - 1, int(np.ceil(max(y0, y1, y2))))
        if xmin > xmax or ymin > ymax:
            continue
        X, Y = np.meshgrid(np.arange(xmin, xmax + 1), np.arange(ymin, ymax + 1))
        denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(denom) < 1e-12:
            continue
        l0 = ((y1 - y2) * (X - x2) + (x2 - x1) * (Y - y2)) / denom
        l1 = ((y2 - y0) * (X - x2) + (x0 - x2) * (Y - y2)) / denom
        l2 = 1.0 - l0 - l1
        mask = (l0 >= -1e-7) & (l1 >= -1e-7) & (l2 >= -1e-7)
        if not mask.any():
            continue
        Z = l0 * pz[i, 0] + l1 * pz[i, 1] + l2 * pz[i, 2]
        sub_z = zbuf[ymin:ymax + 1, xmin:xmax + 1]
        sub_c = cbuf[ymin:ymax + 1, xmin:xmax + 1]
        upd = mask & (Z < sub_z)
        sub_z[upd] = Z[upd]
        sub_c[upd] = colors[i]

    return Image.fromarray(cbuf, "RGB")


def title(img, text):
    draw = ImageDraw.Draw(img)
    font = ImageFont.truetype(FONT, 26)
    draw.rectangle([0, 0, img.width, 48], fill=(255, 255, 255))
    draw.text((16, 10), text, font=font, fill=(30, 30, 30))
    return img


CAM = (-0.6, -0.5, -0.62)

v1 = title(render([(lower, (74, 144, 217), False)], CAM,
                  center=(17, 55, 7), scale=5.3),
           "下壳内部：电池仓 + 出线缺口")

v2 = title(render([(upper, (224, 139, 62), True)], CAM,
                  center=(17, 55, 7), scale=5.3),
           "上壳内部：板槽 + C口 + 螺丝孔")

v3 = title(render([(lower, (74, 144, 217), False),
                   (upper, (224, 139, 62), False)], CAM,
                  center=(17, 55, 13), scale=5.0),
           "合拢总装（外观）")

canvas = Image.new("RGB", (W * 3, H), (255, 255, 255))
for i, im in enumerate([v1, v2, v3]):
    canvas.paste(im, (i * W, 0))

out = f"{D}/airtype_case_preview.png"
canvas.save(out)
print("saved", out)
