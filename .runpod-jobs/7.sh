#!/usr/bin/env bash
# r7 job 7: check that -window 1920x1080 gives a 1080p frame under SDL offscreen (the .mcperf header has width 0), and the 720p cost for comparison.
cd /work/repo; B=build/linux-gcc-release; mkdir -p /tmp/xdg /work/perf; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg
P=$B/src/player/mce_player; K=$B/samples/kit3d_tour/project/assets.mce
for w in 1920x1080 none; do
  a=(); [ $w != none ] && a=(-window $w)
  SDL_VIDEODRIVER=offscreen MCE_RHI=gl $P -pack $K -scene scenes/tour_shafts.mcscene "${a[@]}" -perf_capture /work/perf/res_$w.mcperf -sim.fast_forward 1 -sim.max_catchup 1 -r.vsync 0 -perf_warmup 60 -frames 300 -platform.error_box 0 -pause_on_focus_loss 0 -r.quality high -capture /work/perf/res_$w.ppm > /work/perf/res_$w.log 2>&1
  echo "window=$w rc=$? ppm: $(head -c 20 /work/perf/res_$w.ppm 2>/dev/null | tr '\n' ' ' | cut -c1-16) gpu: $(python3 -c "import json;s=json.load(open('/work/perf/res_$w.mcperf'))['summary']['gpu_ms'];print(s['avg'],s['p50'])" 2>&1)"
  grep -i -E 'window|resolution|1920|1280' /work/perf/res_$w.log | head -3
done
