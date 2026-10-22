@echo off
REM SPDX-License-Identifier: GPL-2.0-or-later
REM Build ftpoc.exe (32-bit!). See ftpoc.c header for why 32-bit.
REM   i686-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -static-libgcc -I..\..\libft -o ftpoc.exe ftpoc.c ftio_win.c <libft\*.c, listed below>
REM -static-libgcc keeps the binary self-contained (no libgcc dll needed).
REM LIBFT: path to the repo's libft directory (default ..\..\libft).
REM DETAIL: tools\detail (the ring and pass worker of detailed scans).
if not defined LIBFT set LIBFT=..\..\libft
if not defined DETAIL set DETAIL=..\detail
where cl >nul 2>nul
if %errorlevel%==0 (
    cl /O2 /I%LIBFT% /I%DETAIL% ftpoc.c ftio_win.c %LIBFT%\ft.c %LIBFT%\ft_geom.c %LIBFT%\ft_holder.c %LIBFT%\ft_endian.c %LIBFT%\ft_focus.c %LIBFT%\ft_focus_io.c %LIBFT%\ft_calib.c %LIBFT%\ft_job.c %LIBFT%\ft_job_io.c %LIBFT%\ft_png.c %LIBFT%\ft_detect.c %LIBFT%\ft_preview.c %LIBFT%\ft_progress.c %LIBFT%\ft_plan.c %LIBFT%\ft_maint.c %LIBFT%\ft_upload.c %LIBFT%\ft_image.c %LIBFT%\ft_develop.c %LIBFT%\ft_stream.c %LIBFT%\ft_density.c %LIBFT%\ft_tiff_ifd.c %LIBFT%\ft_tiff.c %LIBFT%\ft_dplan.c %LIBFT%\ft_detail.c %LIBFT%\ft_share.c %LIBFT%\ft_share_cut.c %LIBFT%\ft_share_cut_io.c %LIBFT%\ft_noise_model.c %LIBFT%\ft_output.c %LIBFT%\ft_icc.c %DETAIL%\ring.c %DETAIL%\worker.c
) else (
    i686-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -static-libgcc -I%LIBFT% -I%DETAIL% -o ftpoc.exe ftpoc.c ftio_win.c %LIBFT%\ft.c %LIBFT%\ft_geom.c %LIBFT%\ft_holder.c %LIBFT%\ft_focus.c %LIBFT%\ft_focus_io.c %LIBFT%\ft_calib.c %LIBFT%\ft_job.c %LIBFT%\ft_job_io.c %LIBFT%\ft_png.c %LIBFT%\ft_detect.c %LIBFT%\ft_preview.c %LIBFT%\ft_progress.c %LIBFT%\ft_plan.c %LIBFT%\ft_maint.c %LIBFT%\ft_upload.c %LIBFT%\ft_image.c %LIBFT%\ft_develop.c %LIBFT%\ft_stream.c %LIBFT%\ft_density.c %LIBFT%\ft_tiff_ifd.c %LIBFT%\ft_tiff.c %LIBFT%\ft_dplan.c %LIBFT%\ft_detail.c %LIBFT%\ft_share.c %LIBFT%\ft_share_cut.c %LIBFT%\ft_share_cut_io.c %LIBFT%\ft_noise_model.c %LIBFT%\ft_output.c %LIBFT%\ft_icc.c %DETAIL%\ring.c %DETAIL%\worker.c
)
