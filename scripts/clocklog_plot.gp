# Static plot of a finished --clocklog CSV (LBE-1425 GPS timing telemetry).
#
# usage (interactive window, -p keeps it open after the script ends):
#   gnuplot -p -e "csv='run.csv'" scripts/clocklog_plot.gp
# usage (write a PNG, no display needed):
#   gnuplot -e "csv='run.csv'; out='run.png'" scripts/clocklog_plot.gp
# usage (ASCII in-terminal, over SSH / no X):
#   gnuplot -e "csv='run.csv'; dumb=1" scripts/clocklog_plot.gp
#
# Honesty (matches what --clocklog records, see issue #17):
#   * X-axis is the u-blox iTOW (GPS time-of-week), NOT host arrival time.
#   * Untrusted samples (valid=0: no/sub-2D fix, or unknown accuracy) are drawn
#     grey, never mixed in with trusted data.
#   * A gap row (a missed second) is flagged red and the trusted trend line is
#     BROKEN across it -- the gap is never interpolated over.
#   * Sentinel accuracies (-1 = "unknown") are dropped, not plotted as a value.
# NAV-CLOCK is the receiver's self-reported clock solution, NOT an independent
# measurement of the disciplined OUT1/OUT2 output.
#
# CSV columns: iTOW_s,clkB_ns,clkD_nsps,tAcc_ns,fAcc_pss,fixType,numSV,valid,gap

if (!exists("csv")) csv = "run.csv"
set datafile separator ","
set datafile commentschars "#"

if (exists("out")) {
	set term pngcairo size 1000,600
	set output out
} else { if (exists("dumb")) {
	set term dumb size 120,32
} }

set title "LBE-1425 GPS timing (u-blox NAV-CLOCK self-report)"
set xlabel "GPS time-of-week iTOW (s)"
set ylabel "time accuracy tAcc (ns)"
set y2label "freq accuracy fAcc (ps/s)"
set ytics nomirror
set y2tics
set grid
set key outside top center horizontal

# Column shorthands (1-based): 1 iTOW_s, 4 tAcc_ns, 5 fAcc_pss, 8 valid, 9 gap.
# 1/0 yields an undefined point -> not plotted, and breaks any line through it.
plot \
  csv using 1:($9==0 && $8==1 && $4>=0 ? $4 : 1/0) axes x1y1 \
       with linespoints lc rgb "#1f77b4" pt 7 ps 0.6 title "tAcc (trusted)", \
  csv using 1:($8==0 && $4>=0 ? $4 : 1/0) axes x1y1 \
       with points lc rgb "#999999" pt 6 ps 0.6 title "tAcc (untrusted)", \
  csv using 1:($9==1 && $4>=0 ? $4 : 1/0) axes x1y1 \
       with points lc rgb "#d62728" pt 7 ps 1.2 title "gap", \
  csv using 1:($8==1 && $5>=0 ? $5 : 1/0) axes x1y2 \
       with lines lc rgb "#2ca02c" dt 2 title "fAcc (trusted)"

if (exists("out")) { unset output }
