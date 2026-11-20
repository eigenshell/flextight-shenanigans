# Centered 65mm film in 70mm backs (Mamiya RB67, Pentax 645)

Goal: shoot 65mm motion-picture negative (Kodak Vision3 etc.) in a 70mm
back with the film **centered** on the gate and the back's original
sprocket-driven frame metering **still working**. Existing community mods
don't do both. Two backs are covered so far: the Mamiya RB67 70mm Film Holder
(`rb67.py`, this section) and the Pentax 645 70mm insert (`pentax645.py`,
[below](#pentax-645-70mm-insert)). Both use the same film geometry
(`film_geometry.py`).

![lateral layout](layout.svg)

## How the RB67 70mm back works (relevant parts)

- Made 1975 to 2008: a Pro-S generation back that Mamiya also sold
  alongside the Pro-SD line. Slightly larger than a 120 Pro-S back. 6x7
  gate. Uses standard daylight-load 70mm metal cassettes for feed and
  take-up. It also has an optional vacuum port that pulls the film flat
  against the back plate. The Rollei 6000 Magazin 70 is a re-engineered
  version of this same back.
- **Frame metering is done by a single sprocket wheel** at one end of the
  feed roller shaft, next to the carriage side wall. Type II perforations
  (KS-shaped, 4.75mm pitch) turn it, and it drives both the frame counter
  and the advance lock. With unperforated film, or thin aerial stock that
  slips on the sprocket, the back mis-spaces frames or won't lock.
- Access: lift the film carriage out of the shell and take off the
  carriage bottom. A small retaining plate (two small Phillips screws) holds
  a white plastic guide roller. Remove the plate and roller and the sprocket
  wheel is exposed on the end of the roller shaft.

## Existing approaches

| Approach | Film position | Metering | Notes |
|---|---|---|---|
| Stock back | 70mm Type II only | sprocket | 65mm teeth land on solid film |
| Mercury "all-perf" mod | 65mm on **offset** spools | friction: teeth ground off, ¼" orthodontic elastic in the groove | Irreversible, the band can slip off, and the film is off center |
| Mercury custom RB 65/70 back | centered | lever stroke to printed lines on the back (counter bypassed) | Sold out, and you have to judge spacing by eye |
| Pentax 645 65mm mod | max offset | trim outer ⅓ of the stock teeth | Works only because flush 65mm perfs partly overlap Type II perfs. Image loses 3.8mm |
| **This design** | **centered** | **new sprocket, teeth moved inboard** | Reversible: swap the original wheel back in |

One inconsistency to note: Mercury's spool page says "Centered" spools suit
"Mamiya 70mm backs", but their compatibility table lists the stock RB 70mm
holder (after the all-perf mod) with **Offset** spools. Offset spools are
what make the 65mm film reach the elastic band on the stock-position wheel.

## The numbers (run `python3 film_geometry.py`)

| | value | source |
|---|---|---|
| 65mm width | 64.97 mm | SMPTE ST 145 |
| 65mm edge → perforation | 2.97 mm | SMPTE ST 145 |
| perforation (across × along) | 2.794 × 1.981 mm | ST 145 / ST 119 |
| pitch KS-1870 / KS-1866 | 4.750 / 4.740 mm | ST 145 |
| 70mm print (perf 65mm) edge → perf | 5.46 mm | SMPTE ST 119 |
| 70mm Type II edge → perf | ~2.0 mm | **approximate**: DoD doc (0.079in) and the Pentax mod geometry agree |
| 65mm centered: edge offset | 2.52 mm | derived |
| **sprocket tooth shift inboard** | **3.49 mm** | derived (≈ ST 119 margin − Type II margin) |
| clear width between 65mm perf rows | 53.44 mm | derived |
| perf intrusion into the 56mm frame | 1.28 mm per long edge | derived |

The perforation size and pitch are the same as Type II. So the new sprocket
keeps the stock tooth count and pitch diameter, and the counter and lock
gearing still turn the same amount per frame. Only the axial position of
the tooth ring changes. Most camera negative uses the KS-1866 short pitch,
which is 0.21% shorter than Type II. That is about 0.15mm over a frame and
gets absorbed by tooth clearance.

**Frame-edge perforations:** a full 56mm frame centered on 65mm film
overlaps the perforation rows by about 1.3mm on each long edge, so perfs
will show in the image. You can crop to ~53.4mm, or accept it (it looks
like an IMAX frame). You can't fix this by moving the film.

## RB67 parts (`rb67.py`, CadQuery)

1. **`offset_sprocket`**: replacement wheel for the roller shaft. Same
   bore, same wall-side face, but the hub extends ~3.5mm inboard and carries
   the tooth ring there. D-flat bore plus an M1.6 set screw. Print it in
   resin to check fit. For real use, machine it in brass or aluminium, or
   have it SLS/MJF printed. Teeth this small (≈2.4 × 1.6 × 0.9mm) need
   resin or machining.
2. **`spool_spacer`**: a 2.6mm C-ring that snaps onto the core of a stock
   70mm spool against each flange. It turns a 70mm spool into a centered
   65mm spool. You need 4 per load.

`python3 rb67.py` (and `python3 pentax645.py`) write STEP and STL files to `out/`, which is not
committed. `python3 render_layout.py` regenerates `layout.svg`.

## RB67 measurement checklist (everything tagged MEASURE in the code)

Nothing about the RB67 back's internals is published (no service manual
turned up). Before making any part, measure the following:

- [ ] Sprocket: tooth count, outer and root diameter, axial width, axial
      position of the tooth centerline relative to the wall-side face.
- [ ] How the sprocket is fixed to the shaft: pressed, pinned, D-flat or
      set screw. Also whether it comes off without disturbing the
      counter/lock linkage.
- [ ] Shaft diameter and any flat.
- [ ] Free axial length of bare shaft inboard of the sprocket. The new hub
      needs ~7.2mm against the stock ~3mm.
- [ ] Position of the sprocket teeth relative to the gate's film edge rails.
      This confirms the datum, i.e. that the Type II perf center sits ~3.4mm
      from the edge.
- [ ] Clearance under the white guide roller and retaining plate. The plate
      may need relieving where the new hub passes.
- [ ] 70mm spool: core diameter, face-to-face flange gap, flange diameter,
      and any tape slot.
- [ ] Type II film strip: edge → perforation, to replace the 2.0mm estimate.
- [ ] Whether the far end of the roller has a matching disc, and whether
      65mm film still rides on it when centered.

Photos of the sprocket with a caliper next to it would be enough to firm up
most of these.

## Pentax 645 70mm insert

### What it is

- Fits the Pentax 645, 645N and 645NII. It is an *insert*: it goes into the
  body's film chamber and has no dark slide, so you can't swap it mid-roll.
  The cassettes sit in a strongly offset hump behind the body, which is why
  it shipped with an eyepiece extension.
- The NII manual gives "about 95 exposures" per 70mm load and says 70mm
  loads automatically. 15ft ÷ 95 ≈ 48mm per frame, which is **10
  perforations (47.5mm)**: a 41.5mm frame plus about 6mm gap. This is
  inferred from the exposure count, not stated in the manual.
- The body's motor does the winding. Film is metered by **one roller with
  a sprocket ring at each end**, engaging both Type II perforation rows. A
  brass pinion on the datum-side end of the roller passes its rotation on
  to the frame counting (photos in Mercury's mod sheet).
- Mercury: it "can't be modded to all-perf, due to the stiffness and small
  diameter of its frame counting roller". So the friction-band trick used
  on the RB67 is out.

### Existing 65mm mod (Mercury)

Pick one end of the roller. Use a knife to trim the **outer 1/3–40%** of
each tooth on that ring. Load 65mm on **Max Offset** spools, with the thin
side of the spool toward the modded ring, so the film runs flush against
that side. The 65mm perforations then catch the trimmed teeth. Type II
70mm still works afterwards. Mercury states the cost as "3.8mm less image
area" on the far edge. `film_geometry.py` reproduces that (3.79mm), which
cross-checks the 56mm gate and the datum assumptions.

### Centered alternative (`pentax645.py`)

This back is a better fit for the centered approach than the RB67:

- Both sprocket rings can be moved, so **both** 65mm perforation rows are
  driven. Lateral registration comes from the sprocket itself rather than
  from spool flanges.
- The new rings sit exactly 56.24mm apart (SMPTE dimension M), each 3.49mm
  inboard of stock.
- Tooth count, pitch diameter and the pinion seat stay as stock, so the
  body still counts 10 perforations per frame. KS-1866 negative is 0.1mm
  short over 10 perforations, which is negligible.
- Image: centered loses **1.28mm on each long edge**; max offset loses
  **3.79mm on one edge**. Centered is better and symmetric.
- It is a swap part, not a modification, so the stock roller stays
  untouched for 70mm. The trade-off is that the new roller *only* works
  with 65mm: Type II perforations fall outside the new rings and would
  ride on solid film.
- Frames per load at 10 perforations: about 96 on 15ft of 70mm and about 83
  on Mercury's 13ft 65mm loads, before leader.
- The part is about 12mm in diameter and 80mm long with 0.9mm teeth, so it
  can't be made by FDM printing. Get it turned in aluminium or brass with
  the teeth cut separately, or resin-printed to test fit and then machined.
  It includes a 0.3mm relief across the image area so only the edges
  touch.
- You also need the spool spacers from `rb67.py`: the Pentax uses the same
  70mm cassettes and spools.

### Pentax measurement checklist

- [ ] How the roller comes out: E-clips, a pressed pinion, or a removable
      side plate? Can the brass pinion be pulled and re-pressed?
- [ ] Teeth per ring, root and outer diameter, ring centres from each end
      face, body length, journal diameters and lengths, and the pinion seat
      (diameter, flat or knurl).
- [ ] Whether the roller is a solid part or a sleeve on a steel shaft. If
      it's a sleeve, make only the sleeve and keep the shaft and pinion.
- [ ] Rotation per frame at the pinion. Count turns while dry-firing with
      a scrap 70mm strip; it should match 47.5mm ÷ (π × pitch diameter).
- [ ] Wrap angle of the film around the roller (how many teeth engage).
- [ ] Cassette chamber end-float, to make sure the centered spools don't
      bind the hump's light traps.

## Risks / open questions (both backs)

- The guide rollers and gate rails may not keep 65mm film from wandering
  sideways. A positively engaged sprocket gives some lateral registration,
  but you might also need edge-guide collars on the rollers.
- The wrap angle on the sprocket is unknown. If only 1 or 2 teeth engage,
  the film could ride up. Check how far the film wraps around the roller.
- 65mm Vision3 has rem-jet backing. Check that it doesn't shed in the back
  or gum up the teeth.
- Fallback: Mercury's "lever stroke to a printed line" bypass of the
  counter/lock works with any film and can coexist with this design.

## Sources

- Zach Horton / Mercury Works, [RB67 all-perf mod](https://www.shoot70mm.zachhorton.com/RB67_all-perf_mod.html) and [Cameras & Backs](https://www.shoot70mm.zachhorton.com/cameras_backs.html)
- shoot70mm.com, [65mm Film](https://shoot70mm.com/65mm_film.html) (compatibility table, Pentax 645 and Linhof mod PDFs)
- Mercury Works, [65mm cassette spool set](https://mercuryworks.store/products/65mm-cassette-spool-set)
- Mercury Works, Pentax 645 70mm Back: 65mm Mod (PDF, linked from the 65mm page) and [Pentax 645 70mm back listing](https://mercuryworks.store/products/pentax-645-70mm-film-insert)
- Pentax [645NII manual](https://www.cameramanuals.org/pentax_pdf/pentax_645_nii.pdf) (specifications: ~95 exposures on 70mm, automatic 70mm loading), via [butkus.org](https://www.butkus.org/chinon/pentax/pentax_645nii/pentax_645nii.htm)
- SMPTE [ST 145:2004](https://pub.smpte.org/pub/st145/st0145-2004_stable2015.pdf) (65mm KS) and [ST 119:2011](https://pub.smpte.org/doc/st119/20111003-pub/st0119-2011.pdf) (70mm perforated 65mm)
- Type II margin hint: [DoD Optical Systems Group doc 451-00](https://www.trmc.osd.mil/wiki/download/attachments/113019925/451-00.pdf?api=v2) (seen via a search excerpt only)
