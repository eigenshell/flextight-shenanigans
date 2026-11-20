"""Parametric CadQuery model: parts to run centered 65mm film in an RB67 70mm back.

Parts
-----
1. offset_sprocket()  Replacement for the RB67's frame-counting sprocket. It
   keeps the original tooth count and pitch diameter, so the back's
   frame-counting and advance-lock gearing still sees the same rotation per
   frame. Only the tooth ring moves inboard (by ~3.5mm, see film_geometry.py)
   so it lands in the perforations of 65mm film centered on the path.
2. spool_spacer()     Snap-on C-ring that sits against each flange of a
   standard 70mm spool, narrowing it to 65mm so the film is centered.

Everything tagged MEASURE in `RB67` / `Spool70` is a placeholder: those
dimensions are not published. Take the back apart (the sprocket is behind
the two-screw retaining plate under the film carriage, see README.md),
measure, then edit the values. Run:

    python3 rb67.py                 # writes STEP + STL to ./out
"""

import math
from dataclasses import dataclass, field
from pathlib import Path

import cadquery as cq

import film_geometry as fg


@dataclass
class RB67:
    # Original sprocket. ALL MEASURE.
    n_teeth: int = 12  # count the teeth (or tooth stubs) on yours
    sprocket_root_d: float | None = None  # None: derive from n_teeth + pitch
    sprocket_width: float = 3.0  # axial thickness of the original wheel
    tooth_center_from_outer_face: float = 1.5  # axial, from the wall-side face
    shaft_d: float = 3.0  # roller shaft diameter at the sprocket
    shaft_flat: float = 0.4  # depth of D-flat on the shaft (0 = round + set screw)
    # Max axial room inboard of the original sprocket before the hub would hit
    # something. From the photos the shaft is bare there, but MEASURE.
    max_hub_length: float = 12.0


@dataclass
class Spool70:
    # Kodak-style 70mm cassette spool. ALL MEASURE.
    core_d: float = 14.0
    flange_inner_gap: float = 70.4  # face-to-face distance between flanges
    flange_d: float = 40.0
    tape_slot_w: float = 0.0  # width of a slot in the core, if any


@dataclass
class Params:
    rb: RB67 = field(default_factory=RB67)
    spool: Spool70 = field(default_factory=Spool70)
    film_t: float = 0.14  # Vision3 65mm on acetate is ~0.14mm
    tooth_clear_lat: float = 0.35  # tooth narrower than perf, across film
    tooth_clear_long: float = 0.35  # tooth narrower than perf, along film
    tooth_h: float = 0.9  # tooth height above the root diameter
    spacer_clear: float = 0.15  # total lateral float for the film between spacers


def pitch_diameter(p: Params) -> float:
    """Diameter at the film's neutral axis for n_teeth * 4.75mm circumference."""
    return p.rb.n_teeth * fg.PITCH_TYPE2 / math.pi


def root_diameter(p: Params) -> float:
    if p.rb.sprocket_root_d is not None:
        return p.rb.sprocket_root_d
    return pitch_diameter(p) - p.film_t


def add_tooth_ring(body: cq.Workplane, r_root: float, n_teeth: int, z: float, p: Params) -> cq.Workplane:
    """Union a ring of KS-perforation teeth onto `body` at axial position z.

    The body's axis is Z. Teeth are rounded-top blocks sized to the
    perforation minus clearance.
    """
    w_lat = fg.PERF_W - p.tooth_clear_lat
    w_long = fg.PERF_H - p.tooth_clear_long
    tooth = (
        cq.Workplane("XY")
        .box(w_long, p.tooth_h + 0.3, w_lat, centered=(True, False, True))
        .edges("|Z and >Y")
        .fillet(min(w_long, p.tooth_h) * 0.45)
        .translate((0, r_root - 0.3, z))
    )
    for i in range(n_teeth):
        body = body.union(tooth.rotate((0, 0, 0), (0, 0, 1), 360.0 * i / n_teeth))
    return body


def offset_sprocket(p: Params) -> cq.Workplane:
    """Sprocket hub, Z axis = shaft axis, Z=0 = original wall-side face.

    +Z points inboard (toward the middle of the film).
    """
    rb = p.rb
    shift = fg.sprocket_shift()
    tooth_z = rb.tooth_center_from_outer_face + shift
    tooth_w_lat = fg.PERF_W - p.tooth_clear_lat
    hub_len = tooth_z + tooth_w_lat / 2 + 1.0
    if hub_len > rb.max_hub_length:
        raise ValueError(f"hub {hub_len:.2f}mm exceeds measured room {rb.max_hub_length}mm")

    r_root = root_diameter(p) / 2
    hub = cq.Workplane("XY").circle(r_root).extrude(hub_len)

    hub = add_tooth_ring(hub, r_root, rb.n_teeth, tooth_z, p)

    # Bore with optional D-flat.
    r = rb.shaft_d / 2
    bore = cq.Workplane("XY").circle(r).extrude(hub_len)
    if rb.shaft_flat > 0:
        cut = cq.Workplane("XY").center(0, r - rb.shaft_flat / 2 + r).rect(2 * r, 2 * r).extrude(hub_len)
        bore = bore.cut(cut)
    hub = hub.cut(bore)

    # M1.6 set screw tapping hole, radial through the flat side so the screw
    # bears on the shaft flat, in the solid part of the hub outboard of the
    # teeth.
    ss_z = min(rb.sprocket_width / 2, tooth_z - tooth_w_lat / 2 - 0.9)
    ss = cq.Workplane("XZ").center(0, ss_z).circle(0.65).extrude(-r_root - 1)
    hub = hub.cut(ss)
    return hub


def spool_spacer(p: Params) -> cq.Workplane:
    """C-ring that clips onto the spool core against a flange.

    Two per spool, four per load (feed + take-up).
    """
    s = p.spool
    t = (s.flange_inner_gap - fg.W65 - p.spacer_clear) / 2
    r_in = s.core_d / 2
    # Outer radius: well inside the flange, but large enough to guide the
    # film edge for the full roll diameter (keep below flange_d).
    r_out = s.flange_d / 2 - 1.0
    ring = cq.Workplane("XY").circle(r_out).circle(r_in + 0.05).extrude(t)
    # Opening for snapping on: slightly narrower than the core.
    gap = s.core_d * 0.8
    ring = ring.cut(cq.Workplane("XY").center(0, -r_in - r_out / 2).rect(gap, r_out + 2 * r_in).extrude(t))
    if s.tape_slot_w > 0:
        ring = ring.cut(cq.Workplane("XY").center(0, r_in).rect(s.tape_slot_w, 2.0).extrude(t))
    return ring


def build(out_dir: Path, p: Params | None = None) -> dict[str, cq.Workplane]:
    p = p or Params()
    out_dir.mkdir(parents=True, exist_ok=True)
    parts = {"offset_sprocket": offset_sprocket(p), "spool_spacer": spool_spacer(p)}
    for name, wp in parts.items():
        cq.exporters.export(wp, str(out_dir / f"{name}.step"))
        cq.exporters.export(wp, str(out_dir / f"{name}.stl"), tolerance=0.01, angularTolerance=0.1)
    return parts


if __name__ == "__main__":
    p = Params()
    print(fg.summary())
    print(f"sprocket: {p.rb.n_teeth} teeth, pitch dia {pitch_diameter(p):.2f}mm, root dia {root_diameter(p):.2f}mm")
    print(f"spool spacer thickness {(p.spool.flange_inner_gap - fg.W65 - p.spacer_clear) / 2:.2f}mm")
    build(Path(__file__).parent / "out", p)
    print("wrote", Path(__file__).parent / "out")
