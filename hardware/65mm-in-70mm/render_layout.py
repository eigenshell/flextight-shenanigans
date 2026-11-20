"""Draw the lateral film layout: Type II 70mm vs centered 65mm vs RB67 gate.

    python3 render_layout.py   # writes layout.svg next to this file
"""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

import film_geometry as fg


def draw(path: Path) -> None:
    fig, ax = plt.subplots(figsize=(12, 4.2))
    rows = [
        ("70mm still, Type II", 0.0, fg.W70_STILL, fg.E70_TYPE2, "#9fb7d9"),
        ("65mm centered", fg.centered_65_offset(), fg.W65, fg.E65, "#e8b27d"),
    ]
    y = 3.0
    for label, x0, w, e, color in rows:
        ax.add_patch(Rectangle((x0, y), w, 0.8, fc=color, ec="black", lw=0.8))
        for px in (x0 + e, x0 + w - e - fg.PERF_W):
            ax.add_patch(Rectangle((px, y + 0.2), fg.PERF_W, 0.4, fc="white", ec="black", lw=0.6))
        ax.text(-1, y + 0.4, label, ha="right", va="center", fontsize=9)
        y -= 1.2

    g = fg.gate_band()
    ax.add_patch(Rectangle((g.lo, y), g.width, 0.8, fc="#cfe8cf", ec="black", lw=0.8))
    ax.text(-1, y + 0.4, "6x7 / 645 gate (56mm across)", ha="right", va="center", fontsize=9)
    y -= 1.2

    t2 = fg.perf_band_type2()
    p65 = fg.perf_band_65(fg.centered_65_offset())
    ax.add_patch(Rectangle((t2.lo, y), t2.width, 0.8, fc="#bbbbbb", ec="black", hatch="//", lw=0.8))
    ax.add_patch(Rectangle((p65.lo, y), p65.width, 0.8, fc="#d9534f", ec="black", lw=0.8))
    ax.text(-1, y + 0.4, "sprocket tooth: stock / new", ha="right", va="center", fontsize=9)
    ax.annotate(
        "",
        xy=(p65.center, y + 1.0),
        xytext=(t2.center, y + 1.0),
        arrowprops=dict(arrowstyle="->", lw=1.2),
    )
    ax.text((t2.center + p65.center) / 2, y + 1.25, f"+{fg.sprocket_shift():.2f}mm", ha="center", fontsize=8)

    # Perf intrusion into the image area.
    for x in (g.lo, g.hi - fg.perf_intrusion_into_gate()):
        ax.axvspan(x, x + fg.perf_intrusion_into_gate(), color="red", alpha=0.15)
    ax.text(
        35,
        -0.9,
        f"red bands: 65mm perfs reach {fg.perf_intrusion_into_gate():.2f}mm into each long edge of the 56mm frame",
        ha="center",
        fontsize=8,
    )

    ax.axvline(0, color="black", lw=0.5, ls=":")
    ax.text(0, 4.2, "datum edge (sprocket side)", fontsize=8, ha="left")
    ax.set_xlim(-28, 72)
    ax.set_ylim(-1.3, 4.6)
    ax.set_yticks([])
    ax.set_xlabel("mm across the film path from datum edge")
    ax.set_xticks(range(0, 71, 5))
    for s in ("left", "right", "top"):
        ax.spines[s].set_visible(False)
    ax.set_title("65mm in a 70mm back: lateral layout (RB67 and Pentax 645)", fontsize=11)
    fig.tight_layout()
    fig.savefig(path)


if __name__ == "__main__":
    out = Path(__file__).parent / "layout.svg"
    draw(out)
    print("wrote", out)
