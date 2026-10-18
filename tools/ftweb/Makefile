# SPDX-License-Identifier: GPL-2.0-or-later
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
LIBFT = ../../libft

all: ftweb_cli purity-web

# the engine natively, driven from files (tests/test_ftweb.py)
ftweb_cli: ftweb_cli.c ftweb.c ftweb.h $(LIBFT)/libft.a
	$(CC) $(CFLAGS) -I$(LIBFT) -o $@ ftweb_cli.c ftweb.c $(LIBFT)/libft.a -lm

ftweb.o: ftweb.c ftweb.h
	$(CC) $(CFLAGS) -I$(LIBFT) -c -o $@ ftweb.c

# the engine is browser tier
purity-web: ftweb.o
	@sh $(LIBFT)/test/purity_web.sh ftweb.o

$(LIBFT)/libft.a: FORCE
	$(MAKE) -C $(LIBFT) libft.a

# ---- wasm32 (wasi-sdk: https://github.com/WebAssembly/wasi-sdk, 34) ----
# ftweb.wasm: the page's engine, a reactor exporting the ftw_* calls.
# ftweb_cli.wasm: ftweb_cli as a WASI program (tests: wasm == native).
# Only libft's browser tier links: another unit fails the link.
WASI_SDK ?= $(HOME)/.local/opt/wasi-sdk-34.0-x86_64-linux
WCC = $(WASI_SDK)/bin/clang --target=wasm32-wasip1
WCFLAGS = -std=c99 -O2 -Wall -Wextra -Wpedantic -I$(LIBFT)
WLDFLAGS = -Wl,-z,stack-size=1048576 -Wl,--gc-sections -Wl,--strip-all
WEB_UNITS = ft ft_curve ft_render ft_render_auto ft_density ft_dplan \
	ft_plan ft_geom ft_share ft_output ft_icc ft_resize ft_atrous \
	ft_noise_model ft_nr ft_sharpen ft_look ft_print ft_pm_classic \
	ft_pm_stock ft_pm_frontier ft_pm_lut3d ft_emulsion ft_print_file \
	ft_image ft_job ft_focus ft_holder ft_jpeg ft_zip ft_share_cut \
	ft_dng ft_calib ft_develop ft_detect ft_preview ft_share_print
WEB_OBJ = $(addprefix wasm/,$(addsuffix .o,$(WEB_UNITS)))
EXPORTS = ftw_new ftw_free ftw_error ftw_out ftw_out_len ftw_text \
	ftw_emulsions ftw_resolutions ftw_holders ftw_lens ftw_job_load ftw_plan_load ftw_job_json \
	ftw_plan ftw_job_text ftw_frame_box \
	ftw_print_load ftw_print_new ftw_print_emulsion ftw_print_get \
	ftw_print_clicks ftw_print_rotate ftw_print_rotation ftw_print_name \
	ftw_print_set_name ftw_print_text ftw_cut \
	ftw_frames ftw_measure ftw_jpeg ftw_display ftw_zip ftw_alloc \
	ftw_preview_load ftw_preview_display ftw_preview_print \
	ftw_preview_px ftw_live_rows \
	ftw_release

wasm: ftweb.wasm ftweb_cli.wasm

# the page's copy: ftwebd serves it from its www folder (not in git)
www: ../ftwebd/www/ftweb.wasm

../ftwebd/www/ftweb.wasm: ftweb.wasm
	cp ftweb.wasm $@

wasm/%.o: $(LIBFT)/%.c $(LIBFT)/*.h
	@mkdir -p wasm
	$(WCC) $(WCFLAGS) -c -o $@ $<

wasm/ftweb.o: ftweb.c ftweb.h
	@mkdir -p wasm
	$(WCC) $(WCFLAGS) -c -o $@ ftweb.c

ftweb.wasm: wasm/ftweb.o $(WEB_OBJ)
	$(WCC) -mexec-model=reactor $(WLDFLAGS) \
	    $(addprefix -Wl$(comma)--export=,$(EXPORTS)) -o $@ $^ -lm

ftweb_cli.wasm: ftweb_cli.c wasm/ftweb.o $(WEB_OBJ)
	$(WCC) $(WCFLAGS) $(WLDFLAGS) -o $@ $^ -lm

comma = ,

clean:
	rm -rf ftweb_cli ftweb.o wasm ftweb.wasm ftweb_cli.wasm \
	    ../ftwebd/www/ftweb.wasm

FORCE:
.PHONY: all clean purity-web wasm www FORCE
