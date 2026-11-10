# Sourced by the r11 jobs (not a job itself: the agent runs only <n>.sh). Same backends as r8-r10.
B=/work/repo/build/linux-gcc-release
P=$B/src/player/mce_player
[[ -x $P ]] || P=$(find $B -maxdepth 4 -type f -name mce_player | head -1)
export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
unset SDL_VIDEO_DRIVER
XV=(xvfb-run -a -s "-screen 0 1920x1080x24")
gl() { env -u MCE_RHI SDL_VIDEODRIVER=offscreen "$@"; }
vk() { env -u SDL_VIDEODRIVER MCE_RHI=sdlgpu VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json "${XV[@]}" "$@"; }
sw() { env -u SDL_VIDEODRIVER -u MCE_RHI LIBGL_ALWAYS_SOFTWARE=1 "${XV[@]}" "$@"; }
lvp() { env -u SDL_VIDEODRIVER MCE_RHI=sdlgpu VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json "${XV[@]}" "$@"; }
PK=$B/samples/kit3d_tour/project/assets.mce
SC=scenes/tour_cloudshadow.mcscene
TR=$B/tests/render_gl/test_render_gl; TG=$B/tests/render_gl/test_golden_gl
mkdir -p /work/out /work/cap
