# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
SRC = ft.c ft_geom.c ft_holder.c ft_focus.c ft_focus_io.c ft_calib.c ft_job.c ft_job_io.c ft_png.c ft_detect.c ft_preview.c ft_plan.c ft_maint.c ft_upload.c ft_image.c ft_develop.c ft_dng.c ft_dng_io.c ft_tiff_ifd.c ft_tiff.c ft_out.c ft_curve.c ft_render.c ft_render_auto.c ft_density.c ft_stream.c ft_dplan.c ft_detail.c ft_share.c ft_share_cut.c ft_share_cut_io.c ft_output.c ft_icc.c ft_resize.c ft_atrous.c ft_noise_model.c ft_nr.c ft_sharpen.c ft_look.c ft_print.c ft_pm_classic.c ft_pm_stock.c ft_pm_frontier.c ft_pm_lut3d.c ft_emulsion.c ft_print_file.c ft_progress.c ft_jpeg.c ft_jpeg_io.c ft_zip.c ft_share_print.c ft_stream_detect.c
OBJ = $(SRC:.c=.o)
TESTS = test/test_ft test/test_geom test/test_focus test/test_calib test/test_job test/test_png test/test_detect test/test_preview test/test_plan test/test_maint test/test_upload test/test_develop test/test_curve test/test_render test/test_render_auto test/test_tiff test/test_density test/test_stream test/test_dplan test/test_detail test/test_share test/test_share_cut test/test_dng test/test_output test/test_icc test/test_resize test/test_jpeg test/test_icc_linear test/test_atrous test/test_noise_model test/test_nr test/test_sharpen test/test_look test/test_print test/test_frontier test/test_lut3d test/test_emulsion test/test_print_file test/test_progress test/test_jpeg_ref test/test_zip test/test_share_print test/test_stream_detect
SRC = ft.c ft_geom.c ft_holder.c ft_focus.c ft_focus_io.c ft_calib.c ft_job.c ft_job_io.c ft_png.c ft_detect.c ft_preview.c ft_plan.c ft_dmax.c ft_maint.c ft_upload.c ft_image.c ft_develop.c ft_dng.c ft_dng_io.c ft_tiff_ifd.c ft_tiff.c ft_out.c ft_curve.c ft_render.c ft_render_auto.c ft_density.c ft_stream.c ft_dplan.c ft_detail.c ft_share.c ft_share_cut.c ft_share_cut_io.c ft_output.c ft_icc.c ft_resize.c ft_atrous.c ft_noise_model.c ft_nr.c ft_sharpen.c ft_look.c ft_print.c ft_pm_classic.c ft_pm_stock.c ft_pm_frontier.c ft_pm_lut3d.c ft_emulsion.c ft_print_file.c ft_progress.c ft_jpeg.c ft_jpeg_io.c ft_zip.c ft_share_print.c
OBJ = $(SRC:.c=.o)
TESTS = test/test_ft test/test_geom test/test_focus test/test_calib test/test_job test/test_png test/test_detect test/test_preview test/test_plan test/test_dmax test/test_maint test/test_upload test/test_develop test/test_curve test/test_render test/test_render_auto test/test_tiff test/test_density test/test_stream test/test_dplan test/test_detail test/test_share test/test_share_cut test/test_dng test/test_output test/test_icc test/test_resize test/test_jpeg test/test_icc_linear test/test_atrous test/test_noise_model test/test_nr test/test_sharpen test/test_look test/test_print test/test_frontier test/test_lut3d test/test_emulsion test/test_print_file test/test_progress test/test_jpeg_ref test/test_zip test/test_share_print
# the render core, the 2b encoders, the share planner, the output colour
# units, the NR / sharpen units, looks and the print chain also build for
# the browser (WASM): libm and mem* only
PURE = ft_curve.o ft_render.o ft_render_auto.o ft_density.o ft_dplan.o \
       ft_plan.o ft_dmax.o ft_geom.o ft_share.o ft_output.o ft_icc.o ft_resize.o \
       ft_atrous.o ft_noise_model.o ft_nr.o ft_sharpen.o ft_look.o \
       ft_print.o ft_pm_classic.o ft_pm_stock.o ft_pm_frontier.o \
       ft_pm_lut3d.o ft_emulsion.o ft_print_file.o
# lcms2 is a test oracle only: libft itself never links it
LCMS_LIBS ?= $(shell pkg-config --libs lcms2 2>/dev/null || echo -llcms2)
# libjpeg-turbo is a test oracle only (test_jpeg, test_jpeg_ref): libft's
# own encoder (ft_jpeg.c) writes the same bytes
JPEG_LIBS ?= $(shell pkg-config --libs libjpeg 2>/dev/null || echo -ljpeg)

libft.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

%.o: %.c *.h
	$(CC) $(CFLAGS) -c -o $@ $<

test/test_detect: test/test_detect.c test/synth.c test/synth.h test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_detect.c test/synth.c libft.a -lm

# libtiff reads the tiled writer's files back (tests only, an independent
# reader); libft itself never links it
test/test_tiff: test/test_tiff.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. $$(pkg-config --cflags libtiff-4) -o $@ test/test_tiff.c libft.a $$(pkg-config --libs libtiff-4) -lm

test/test_detail: test/test_detail.c test/synth.c test/synth.h test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. $$(pkg-config --cflags libtiff-4) -o $@ test/test_detail.c test/synth.c libft.a $$(pkg-config --libs libtiff-4) -lm

test/test_stream_detect: test/test_stream_detect.c test/synth.c test/synth.h test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_stream_detect.c test/synth.c libft.a -lm

test/test_share_cut: test/test_share_cut.c test/synth.c test/synth.h test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_share_cut.c test/synth.c libft.a -lm

test/window_dump: test/window_dump.c test/synth.c test/synth.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/window_dump.c test/synth.c libft.a -lm

test/test_icc: test/test_icc.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_icc.c libft.a $(LCMS_LIBS) -lm

test/test_icc_linear: test/test_icc_linear.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ test/test_icc_linear.c libft.a $(LCMS_LIBS) -lm

test/test_jpeg test/test_jpeg_ref: test/%: test/%.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ $< libft.a $(JPEG_LIBS) -lm

test/%: test/%.c test/ftest.h libft.a
	$(CC) $(CFLAGS) -I. -o $@ $< libft.a -lm

# the check must pass the render core and catch an allocating object
purity: $(PURE) ft_image.o
	@sh test/purity.sh $(PURE)
	@sh test/purity.sh ft_image.o >/dev/null; test $$? -eq 1 || \
	    { echo "purity: the check did not reject calloc in ft_image.o"; exit 1; }

test: $(TESTS) purity purity-web
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(OBJ) libft.a $(TESTS) test/png_dump test/window_dump test/web/*.o

# the emulsions' 3D tables (libft/emulsions/README.md): needs Python 3;
# the generated header is committed
luts:
	python3 ../tools/cube2c.py emulsions ft_emulsion_luts.h

# the browser tier (WASM with a libc): heap and formatting into buffers
# are fine; file and stream I/O, threads and processes are not
WEB = $(PURE) ft_image.o ft_job.o ft_focus.o ft_holder.o ft_progress.o \
      ft_jpeg.o ft_zip.o ft_share_cut.o ft_dng.o ft_calib.o ft_develop.o \
      ft_detect.o ft_preview.o ft_share_print.o ft_stream_detect.o
test/web/%.o: test/web/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

purity-web: $(WEB) test/web/web_good.o test/web/web_bad.o
	@sh test/purity_web.sh $(WEB) test/web/web_good.o
	@sh test/purity_web.sh test/web/web_bad.o >/dev/null; test $$? -eq 1 || \
	    { echo "purity-web: the check did not reject fopen"; exit 1; }

.PHONY: test clean purity purity-web luts
