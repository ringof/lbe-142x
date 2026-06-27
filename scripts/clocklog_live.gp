# Live strip chart of a --clocklog CSV as it grows (LBE-1425 GPS timing).
#
# Start the log, then point gnuplot at it:
#   ./build/bin/lbe-142x --clocklog >> run.csv &
#   gnuplot -e "csv='run.csv'" scripts/clocklog_live.gp            # qt window
#   gnuplot -e "csv='run.csv'; dumb=1" scripts/clocklog_live.gp    # ASCII / SSH
#
# Options (gnuplot -e "name=value; ..."):
#   csv      path to the growing CSV            (default "run.csv")
#   window   sliding window width, seconds      (default 120)
#   refresh  redraw period, seconds             (default 1)
#   dumb     if set, render ASCII in-terminal (no X needed)
#
# Honesty rules are identical to clocklog_plot.gp: iTOW x-axis, untrusted
# samples greyed, gaps flagged red and never interpolated, sentinels dropped.
# Ctrl-C to stop. An empty/just-started file is fine -- it is skipped until at
# least two samples exist.

if (!exists("csv")) csv = "run.csv"
if (!exists("window")) window = 120
if (!exists("refresh")) refresh = 1
set datafile separator ","
set datafile commentschars "#"

# Pick an interactive terminal this gnuplot actually has compiled in
# (qt > wxt > x11); fall back to ASCII. Force ASCII with dumb=1. (Not every
# build ships qt -- e.g. gnuplot-nox or some conda builds -- so don't assume.)
if (exists("dumb")) {
	set term dumb size 120,32
} else { if (strstrt(GPVAL_TERMINALS, "qt") > 0) {
	set term qt noraise size 1000,600 title "LBE-1425 clocklog (live)"
} else { if (strstrt(GPVAL_TERMINALS, "wxt") > 0) {
	set term wxt noraise size 1000,600 title "LBE-1425 clocklog (live)"
} else { if (strstrt(GPVAL_TERMINALS, "x11") > 0) {
	set term x11 noraise size 1000,600 title "LBE-1425 clocklog (live)"
} else {
	print "clocklog_live: no qt/wxt/x11 terminal in this gnuplot; using ASCII."
	set term dumb size 120,32
} } } }

set xlabel "GPS time-of-week iTOW (s)"
set ylabel "time accuracy tAcc (ns)"
# 0-based so a tight, near-constant tAcc renders as a flat low line instead of
# collapsing to an empty y range (a locked GPSDO can pin tAcc at e.g. 4 ns).
set yrange [0:*]
set grid
set key outside top center horizontal

# Redraw loop. Each pass: sliding window = the most recent `window` seconds of
# data (X clamped to it); skip drawing until at least two samples exist so an
# empty/just-started file is harmless. (A while loop, not the deprecated
# `reread`.)
while (1) {
	stats csv using 1 nooutput
	if (exists("STATS_records") && STATS_records >= 2) {
		xhi = STATS_max
		set xrange [xhi - window : xhi]
		plot \
		  csv using 1:($9==0 && $8==1 && $4>=0 ? $4 : 1/0) \
		       with linespoints lc rgb "#1f77b4" pt 7 ps 0.6 title "tAcc (trusted)", \
		  csv using 1:($8==0 && $4>=0 ? $4 : 1/0) \
		       with points lc rgb "#999999" pt 6 ps 0.6 title "tAcc (untrusted)", \
		  csv using 1:($9==1 && $4>=0 ? $4 : 1/0) \
		       with points lc rgb "#d62728" pt 7 ps 1.2 title "gap"
	}
	pause refresh
}
