"""Film and back geometry for running centered 65mm film in a Mamiya RB67 70mm back.

All lateral positions are measured across the film path from the *datum
edge*: the edge of a 70mm film on the side where the RB67 frame-counting
sprocket sits (the bottom of the back when mounted on the camera).

Values tagged SOURCE come from published standards. Values tagged APPROX are
inferred from secondary sources and must be checked against real film. Values
tagged MEASURE are placeholders for dimensions of the RB67 back itself that
have not been published anywhere I could find. Measure them with calipers
before printing or machining anything.
"""

from dataclasses import dataclass

# --- 65mm camera negative, SMPTE ST 145:2004 (KS perforation) -------- SOURCE
W65 = 64.97  # film width
E65 = 2.97  # film edge -> outer edge of perforation
PERF_W = 2.794  # perforation width (across film)
PERF_H = 1.981  # perforation height (along film)
PITCH_65_LONG = 4.750  # KS-1870 (print / some camera stocks)
PITCH_65_SHORT = 4.740  # KS-1866 (most camera negative)
M65 = 56.24  # lateral perforation displacement (same-side edges of the two rows)

# Sanity check of the standard's own numbers: M is the distance between the
# same-side edges of the two perf rows, i.e. W - 2E - C.
assert abs((W65 - 2 * E65 - PERF_W) - M65) < 0.01

# --- 70mm print stock perforated 65mm, SMPTE ST 119:2011 ------------- SOURCE
# The same perf rows as 65mm, with 2.49mm of extra margin on each side.
W70_PRINT = 69.95
E70_PRINT = 5.46

# --- 70mm still film, Type II perforations (ISO 14546 / ANSI PH1.10) --
# Perforation size and pitch are identical to KS-1870 (stated in ST 145
# annex A.4). The edge margin is not in any free document I could find:
#  * a DoD film document gives a 0.079in (2.0mm) margin for Type II
#    aerographic film;
#  * Mercury Works' Pentax 645 65mm mod (65mm pushed flush against the
#    datum edge, cut the *outer* 1/3-40% of each Type II tooth) implies the
#    Type II row sits about 1mm closer to the edge than the 65mm row, i.e.
#    E ~= 2.97 - 1.0 ~= 2.0mm.
# Both agree, but measure a strip of Type II film to confirm.
W70_STILL = 70.0  # nominal                                            APPROX
E70_TYPE2 = 2.0  # edge -> outer edge of perforation                   APPROX
PITCH_TYPE2 = 4.750

# --- Mamiya RB67 6x7 film gate ----------------------------------------
GATE_H = 56.0  # across the film (short side of the 6x7 frame)         APPROX
GATE_W = 69.5  # along the film                                        APPROX


@dataclass(frozen=True)
class Band:
    """A lateral band across the film path, [lo, hi] from the datum edge."""

    lo: float
    hi: float

    @property
    def center(self) -> float:
        return (self.lo + self.hi) / 2

    @property
    def width(self) -> float:
        return self.hi - self.lo


def centered_65_offset(path_width: float = W70_STILL) -> float:
    """Datum edge -> near edge of a 65mm film centered in a 70mm path."""
    return (path_width - W65) / 2


def perf_band_type2() -> Band:
    return Band(E70_TYPE2, E70_TYPE2 + PERF_W)


def perf_band_65(offset: float) -> Band:
    """Near-side perforation row of 65mm film whose edge sits at `offset`."""
    return Band(offset + E65, offset + E65 + PERF_W)


def gate_band(path_width: float = W70_STILL) -> Band:
    m = (path_width - GATE_H) / 2
    return Band(m, path_width - m)


def sprocket_shift(path_width: float = W70_STILL) -> float:
    """How far inboard the sprocket teeth must move for centered 65mm."""
    return perf_band_65(centered_65_offset(path_width)).center - perf_band_type2().center


def perf_intrusion_into_gate(path_width: float = W70_STILL) -> float:
    """How far each 65mm perf row reaches into the 56mm image (per side)."""
    off = centered_65_offset(path_width)
    return max(0.0, perf_band_65(off).hi - gate_band(path_width).lo)


def far_perf_band_65(offset: float) -> Band:
    """Far-side perforation row of 65mm film whose near edge sits at `offset`."""
    hi = offset + W65 - E65
    return Band(hi - PERF_W, hi)


def max_offset_compromised_band(path_width: float = W70_STILL) -> float:
    """65mm pushed flush to the datum edge (Mercury "Max Offset", Pentax 645).

    Width of image on the far side that has perforations or no film in it.
    Mercury's Pentax 645 mod sheet says 3.8mm, which cross-checks the gate
    and datum assumptions above.
    """
    return max(0.0, gate_band(path_width).hi - far_perf_band_65(0.0).lo)


def frames_per_load(load_ft: float, perfs_per_frame: int, pitch: float = PITCH_TYPE2) -> int:
    return int(load_ft * 304.8 // (perfs_per_frame * pitch))


def summary() -> str:
    off = centered_65_offset()
    t2 = perf_band_type2()
    p65 = perf_band_65(off)
    g = gate_band()
    clear_between_perfs = W65 - 2 * (E65 + PERF_W)
    lines = [
        f"65mm centered in 70mm path: film edge {off:.2f}mm from datum",
        f"Type II perf row:     {t2.lo:.2f} .. {t2.hi:.2f}  (center {t2.center:.2f})",
        f"65mm perf row:        {p65.lo:.2f} .. {p65.hi:.2f}  (center {p65.center:.2f})",
        f"=> sprocket teeth must move {sprocket_shift():.2f}mm inboard",
        f"6x7 gate (56mm):      {g.lo:.2f} .. {g.hi:.2f}",
        f"clear width between 65mm perf rows: {clear_between_perfs:.2f}mm",
        f"=> perfs intrude {perf_intrusion_into_gate():.2f}mm into each long edge of the frame",
        f"max offset instead: {max_offset_compromised_band():.2f}mm compromised on one edge",
        f"pitch mismatch KS-1866 vs Type II: "
        f"{(PITCH_TYPE2 - PITCH_65_SHORT) / PITCH_TYPE2 * 100:.2f}% "
        f"({15 * (PITCH_TYPE2 - PITCH_65_SHORT):.2f}mm over 15 perfs)",
    ]
    return "\n".join(lines)


if __name__ == "__main__":
    print(summary())
