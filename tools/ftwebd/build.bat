@echo off
REM SPDX-License-Identifier: GPL-2.0-or-later
REM Build ftwebd.exe with mingw (posix threads: winlibs "posix" builds).
REM -static links winpthreads in, so the exe needs no dll beside it.
REM LIBFT: path to the repo's libft directory (default ..\..\libft).
if not defined LIBFT set LIBFT=..\..\libft
REM libft: the job file and its checks, and strip.print's parser (it
REM knows the emulsions, so the print chain links too).
gcc -std=c99 -O2 -Wall -Wextra -static -I%LIBFT% -o ftwebd.exe ftwebd.c http.c task.c proc_win.c %LIBFT%\ft.c %LIBFT%\ft_holder.c %LIBFT%\ft_focus.c %LIBFT%\ft_focus_io.c %LIBFT%\ft_job.c %LIBFT%\ft_plan.c %LIBFT%\ft_print_file.c %LIBFT%\ft_emulsion.c %LIBFT%\ft_print.c %LIBFT%\ft_pm_classic.c %LIBFT%\ft_pm_frontier.c %LIBFT%\ft_pm_lut3d.c %LIBFT%\ft_pm_stock.c %LIBFT%\ft_curve.c %LIBFT%\ft_render.c %LIBFT%\ft_render_auto.c %LIBFT%\ft_resize.c -lws2_32 -lm
