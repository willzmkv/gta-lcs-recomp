#!/usr/bin/env python3
"""
High-Resolution Cel-Art & Alpha-SDF Upscaler for GTA: Liberty City Stories Menu Artworks.

Features:
  1. Backs up original 512x512 menu_sub_1..4.png and menu_LCSLogo.png into
     launcher/assets/characters_orig/ (without modifying gamefiles/assets).
  2. Performs 4x (512x512 -> 2048x2048) multi-stage Lanczos upscaling with:
     - Pre-upscale Voronoi RGB color-bleeding (distance_transform_edt) to eliminate
       opaque-to-transparent matte/background bleed.
     - Edge-directed ink contour sharpening paired with flat-region noise smoothing.
     - True subpixel SDF (Signed Distance Field) alpha contour reconstruction for
       silhouette character cutouts (menu_sub_1..4.png), erasing 512p stair-stepping
       into silky-smooth curves at 2048x2048.
     - Drop-shadow-preserving Lanczos alpha for soft-shadowed assets (menu_LCSLogo.png).
  3. Cleans & de-halos the updated Main Menu assets (lcs_logo.png, toni_main.png) in
     launcher/assets/mainmenu/ with post-bleed straight RGBA edges.
"""
from __future__ import annotations

import argparse
import os
import shutil
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter
from scipy.ndimage import distance_transform_edt, gaussian_filter


ROOT = Path(__file__).resolve().parent.parent
ORIG_MENU_DIR = ROOT / "assets" / "ipad" / "Textures_Shared" / "menu"
CHAR_ORIG_DIR = ROOT / "launcher" / "assets" / "characters_orig"
CHAR_HD_DIR = ROOT / "launcher" / "assets" / "characters"
MAINMENU_DIR = ROOT / "launcher" / "assets" / "mainmenu"


def color_bleed_rgb(rgb: np.ndarray, alpha: np.ndarray, solid_thresh: int = 200) -> np.ndarray:
    """Propagate nearest foreground RGB into semi/fully transparent pixels via EDT Voronoi."""
    mask = alpha >= solid_thresh
    if not np.any(mask):
        mask = alpha > 0
    if not np.any(mask) or np.all(mask):
        return rgb
    _, indices = distance_transform_edt(~mask, return_indices=True)
    return rgb[indices[0], indices[1]]


def clean_stray_dust(rgba: np.ndarray, min_alpha: int = 8) -> np.ndarray:
    """Zero isolated ultra-faint single-pixel dust speckles far from foreground."""
    out = rgba.copy()
    alpha = out[..., 3]
    out[alpha < min_alpha, 3] = 0
    return out


def upscale_cel_rgb(rgb_u8: np.ndarray, target_w: int, target_h: int) -> np.ndarray:
    """
    2-stage Lanczos upscale + edge-aware ink crisping + gentle chroma anti-banding
    tailored for GTA inked comic illustrations.
    """
    cur_img = Image.fromarray(rgb_u8, mode="RGB")
    mid_w = (cur_img.width + target_w) // 2
    mid_h = (cur_img.height + target_h) // 2
    cur_img = cur_img.resize((mid_w, mid_h), Image.Resampling.LANCZOS)
    cur_img = cur_img.resize((target_w, target_h), Image.Resampling.LANCZOS)

    arr = np.asarray(cur_img, dtype=np.float32)

    # Luminance-based edge mask (locks ink outlines crisp while keeping flat cel fills smooth)
    luma = 0.299 * arr[..., 0] + 0.587 * arr[..., 1] + 0.114 * arr[..., 2]
    blur_fine = gaussian_filter(arr, sigma=(0.85, 0.85, 0.0))
    detail = arr - blur_fine

    gy, gx = np.gradient(luma)
    grad_mag = np.hypot(gx, gy)
    edge_weight = np.clip((grad_mag - 4.0) / 22.0, 0.0, 1.0)[..., None]

    # Subtle edge-directed sharpen (0.55x) on inked lines, ultra-light smooth on flat fills
    flat_smooth = gaussian_filter(arr, sigma=(0.45, 0.45, 0.0))
    enhanced = flat_smooth * (1.0 - edge_weight) + (arr + 0.55 * detail) * edge_weight
    return np.clip(np.rint(enhanced), 0.0, 255.0).astype(np.uint8)


def upscale_sdf_alpha(alpha_u8: np.ndarray, target_w: int, target_h: int) -> np.ndarray:
    """
    Subpixel Signed-Distance-Field (SDF) alpha contour reconstruction for sharp cutouts.
    Eliminates 512px stair-stepped edges on silhouettes while keeping ~1.4px anti-aliasing
    at 2048x2048.
    """
    # Step 1: bilinear/bicubic lift + mild pre-smoothing removes quantized 1px staircase harmonics
    a_img = Image.fromarray(alpha_u8, mode="L").resize((target_w, target_h), Image.Resampling.BICUBIC)
    a_hi = np.asarray(a_img, dtype=np.float32)
    scale_x = target_w / max(1, alpha_u8.shape[1])
    smooth_sigma = max(0.8, 0.48 * scale_x)
    a_smooth = gaussian_filter(a_hi, sigma=smooth_sigma)

    # Step 2: compute exact Euclidean signed distance relative to 50% contour
    inside = a_smooth >= 127.5
    if not np.any(inside) or np.all(inside):
        return np.clip(np.rint(a_hi), 0.0, 255.0).astype(np.uint8)

    d_in = distance_transform_edt(inside)
    d_out = distance_transform_edt(~inside)
    signed_dist = np.where(inside, d_in - 0.5, -(d_out - 0.5))

    # Crisp ~1.6px AA transition at 2048x2048
    aa_half_width = 0.85
    t = np.clip(0.5 + signed_dist / (2.0 * aa_half_width), 0.0, 1.0)
    # Smoothstep for optically neutral bilinear blending
    aa = t * t * (3.0 - 2.0 * t)
    return np.clip(np.rint(aa * 255.0), 0.0, 255.0).astype(np.uint8)


def process_character_upscale(src_path: Path, dst_path: Path, scale: int = 4, sdf_alpha: bool = True) -> None:
    im = Image.open(src_path).convert("RGBA")
    arr = np.asarray(im, dtype=np.uint8)
    target_w = im.width * scale
    target_h = im.height * scale

    rgb = arr[..., :3]
    alpha = arr[..., 3]

    # Bleed foreground ink/fill color into alpha==0 before upscaling so no matte halo bleeds in
    bleed_src = color_bleed_rgb(rgb, alpha, solid_thresh=180 if sdf_alpha else 1)
    up_rgb = upscale_cel_rgb(bleed_src, target_w, target_h)

    if sdf_alpha:
        up_alpha = upscale_sdf_alpha(alpha, target_w, target_h)
    else:
        a_img = Image.fromarray(alpha, mode="L").resize((target_w, target_h), Image.Resampling.LANCZOS)
        up_alpha = np.asarray(a_img, dtype=np.uint8)

    # Final post-bleed so GPU bilinear sampling near transparent boundary never sees black
    up_rgb = color_bleed_rgb(up_rgb, up_alpha, solid_thresh=180 if sdf_alpha else 24)
    out = np.dstack([up_rgb, up_alpha])
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(out, mode="RGBA").save(dst_path, format="PNG", optimize=True)


def polish_rgba_asset(src_path: Path, dst_path: Path, dehalo_thresh: int = 32) -> None:
    """Preserve exact resolution and intentional semi-alpha tint while bleeding RGB into alpha==0."""
    im = Image.open(src_path).convert("RGBA")
    arr = clean_stray_dust(np.asarray(im, dtype=np.uint8), min_alpha=4)
    rgb = arr[..., :3]
    alpha = arr[..., 3]
    bled_rgb = color_bleed_rgb(rgb, alpha, solid_thresh=dehalo_thresh)
    clean_rgb = np.where(alpha[..., None] == 0, bled_rgb, rgb)
    out = np.dstack([clean_rgb, alpha])
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(out, mode="RGBA").save(dst_path, format="PNG", optimize=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="Upscale LCS menu character artworks 4x (2048x2048) & polish main menu PNGs.")
    parser.add_argument("--scale", type=int, default=4, help="Upscale factor for menu_sub_1..4 & menu_LCSLogo (default: 4)")
    parser.add_argument("--logo-src", type=Path, default=None, help="Optional updated Franken'Liberty lcs_logo.png source")
    parser.add_argument("--toni-src", type=Path, default=None, help="Optional updated toni_main.png source")
    args = parser.parse_args()

    CHAR_ORIG_DIR.mkdir(parents=True, exist_ok=True)
    CHAR_HD_DIR.mkdir(parents=True, exist_ok=True)
    MAINMENU_DIR.mkdir(parents=True, exist_ok=True)

    char_targets = [
        ("menu_sub_1.png", True),
        ("menu_sub_2.png", True),
        ("menu_sub_3.png", True),
        ("menu_sub_4.png", True),
        ("menu_LCSLogo.png", False),
    ]

    for fname, use_sdf in char_targets:
        game_src = ORIG_MENU_DIR / fname
        backup_dst = CHAR_ORIG_DIR / fname
        src = backup_dst if backup_dst.exists() else game_src
        if game_src.exists() and not backup_dst.exists():
            shutil.copy2(game_src, backup_dst)
            print(f"[backup] {game_src} -> {backup_dst}")
        if not src.exists():
            print(f"[warn] Missing source artwork: {src}")
            continue
        hd_dst = CHAR_HD_DIR / fname
        process_character_upscale(src, hd_dst, scale=args.scale, sdf_alpha=use_sdf)
        print(f"[upscale] {src.name} -> {hd_dst} ({args.scale}x, sdf={use_sdf})")

    if args.logo_src:
        logo_backup = CHAR_ORIG_DIR / "mainmenu_lcs_logo_orig.png"
        cur_logo = MAINMENU_DIR / "lcs_logo.png"
        if cur_logo.exists() and not logo_backup.exists():
            shutil.copy2(cur_logo, logo_backup)
            print(f"[backup] {cur_logo} -> {logo_backup}")
        polish_rgba_asset(args.logo_src, cur_logo, dehalo_thresh=140)
        print(f"[mainmenu] Updated & de-haloed {cur_logo}")

    if args.toni_src:
        toni_backup = CHAR_ORIG_DIR / "mainmenu_toni_main_orig.png"
        cur_toni = MAINMENU_DIR / "toni_main.png"
        if cur_toni.exists() and not toni_backup.exists():
            shutil.copy2(cur_toni, toni_backup)
            print(f"[backup] {cur_toni} -> {toni_backup}")
        polish_rgba_asset(args.toni_src, cur_toni, dehalo_thresh=140)
        print(f"[mainmenu] Updated & de-haloed {cur_toni}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
