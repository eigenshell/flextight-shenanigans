"""Parametric CadQuery model: a centered-65mm measuring roller for the
Pentax 645 70mm film insert.

The stock insert meters film with one roller that carries a sprocket ring at
each end, engaging both rows of Type II perforations. A brass pinion on the
datum-side end passes the roller's rotation on to the frame counting (the
body's motor winds until the roller has turned one frame's worth: ~95
exposures per 15ft load works out to 10 perforations per frame).

Mercury's existing 65mm mod trims the outer third of the teeth on one ring
and uses "Max Offset" spools to push the film flush to that side. This part
replaces the whole roller instead:

* both tooth rings move inboard by ~3.49mm, so they sit 56.24mm apart
  (SMPTE dimension M) and engage both perforation rows of centered 65mm;
* the pitch diameter, tooth count and pinion interface stay as stock, so
  the camera still counts 10 perforations per frame;
* it is reversible: keep the stock roller for 70mm Type II film.

Everything in `Roller645` is MEASURE: nothing about the insert's internals is
published. Run:

    python3 pentax645.py          # writes STEP + STL to ./out
"""

from dataclasses import dataclass
from pathlib import Path

import cadquery as cq

import film_geometry as fg
from rb67 import Params, add_tooth_ring, pitch_diameter


@dataclass
class Roller645:
    # ALL MEASURE.
    n_teeth: int = 8  # teeth per ring on the stock roller
    body_len: float = 68.0  # roller body between its end shoulders
    # Axial position of the stock datum-side tooth ring centre, measured from
    # the datum-side end face of the body.
    stock_ring_z: float = 2.6
    journal_d: float = 3.0  # shaft ends that sit in the insert frame
    journal_len: float = 4.0
    pinion_seat_d: float = 2.5  # where the brass pinion is pressed on
    pinion_seat_len: float = 3.0
    pinion_flat: float = 0.3  # D-flat depth for the pinion, 0 = plain press fit
    # Relief between the rings so the image area of the film only touches the
    # roller near the edges (reduces scratching risk on the emulsion side).
    relief_depth: float = 0.3
    relief_margin: float = 3.0  # keep full diameter this far inboard of each ring


def centered_roller(p: Params, r: Roller645) -> cq.Workplane:
    """Z = roller axis, Z=0 = datum-side end face of the body, +Z inboard."""
    shift = fg.sprocket_shift()
    z1 = r.stock_ring_z + shift
    z2 = z1 + fg.M65  # second perforation row of 65mm film
    if z2 + fg.PERF_W / 2 > r.body_len:
        raise ValueError("far tooth ring falls off the end of the body; check body_len / stock_ring_z")

    rr = (pitch_diameter_for(r, p) - p.film_t) / 2
    body = cq.Workplane("XY").circle(rr).extrude(r.body_len)

    # Shallow relief groove across the image area.
    lo = z1 + fg.PERF_W / 2 + r.relief_margin
    hi = z2 - fg.PERF_W / 2 - r.relief_margin
    if r.relief_depth > 0 and hi > lo:
        groove = (
            cq.Workplane("XY")
            .workplane(offset=lo)
            .circle(rr + 1)
            .circle(rr - r.relief_depth)
            .extrude(hi - lo)
        )
        body = body.cut(groove)

    body = add_tooth_ring(body, rr, r.n_teeth, z1, p)
    body = add_tooth_ring(body, rr, r.n_teeth, z2, p)

    # Far-end journal.
    body = body.union(
        cq.Workplane("XY").workplane(offset=r.body_len).circle(r.journal_d / 2).extrude(r.journal_len)
    )
    # Datum-end journal, then pinion seat beyond it.
    body = body.union(cq.Workplane("XY").circle(r.journal_d / 2).extrude(-r.journal_len))
    seat = (
        cq.Workplane("XY")
        .workplane(offset=-r.journal_len)
        .circle(r.pinion_seat_d / 2)
        .extrude(-r.pinion_seat_len)
    )
    if r.pinion_flat > 0:
        rs = r.pinion_seat_d / 2
        seat = seat.cut(
            cq.Workplane("XY")
            .workplane(offset=-r.journal_len - r.pinion_seat_len)
            .center(0, rs - r.pinion_flat + rs)
            .rect(2 * rs, 2 * rs)
            .extrude(r.pinion_seat_len)
        )
    return body.union(seat)


def pitch_diameter_for(r: Roller645, p: Params) -> float:
    # Same relation as the RB67 sprocket: n teeth * 4.75mm around the film's
    # neutral axis.
    q = Params(rb=type(p.rb)(n_teeth=r.n_teeth), film_t=p.film_t)
    return pitch_diameter(q)


def build(out_dir: Path) -> cq.Workplane:
    p, r = Params(), Roller645()
    out_dir.mkdir(parents=True, exist_ok=True)
    wp = centered_roller(p, r)
    cq.exporters.export(wp, str(out_dir / "pentax645_roller.step"))
    cq.exporters.export(wp, str(out_dir / "pentax645_roller.stl"), tolerance=0.01, angularTolerance=0.1)
    return wp


if __name__ == "__main__":
    p, r = Params(), Roller645()
    d = pitch_diameter_for(r, p)
    print(f"roller: {r.n_teeth} teeth/ring, pitch dia {d:.2f}mm, rings {fg.M65:.2f}mm apart")
    print(f"rings move {fg.sprocket_shift():.2f}mm inboard each (stock ~{fg.W70_STILL - 2 * fg.perf_band_type2().center:.2f}mm apart)")
    print(f"frame pitch 10 perfs = {10 * fg.PITCH_TYPE2:.1f}mm (645 frame 41.5mm + gap)")
    print(f"frames: 15ft 70mm ~{fg.frames_per_load(15, 10)}, 13ft 65mm ~{fg.frames_per_load(13, 10)} (before leader)")
    print(f"image: centered loses {fg.perf_intrusion_into_gate():.2f}mm per long edge; "
          f"max offset loses {fg.max_offset_compromised_band():.2f}mm on one edge")
    build(Path(__file__).parent / "out")
    print("wrote", Path(__file__).parent / "out")
