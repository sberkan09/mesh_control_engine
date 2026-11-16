# FetchContent PATCH_COMMAND for pl_mpeg (cmake/Dependencies.cmake), run in the pl_mpeg source tree. Upstream's
# corrupt-video check in plm_video_process_macroblock bounds the block's start address, but a half-pel motion
# vector reads one row (vertical) or one column (horizontal) past the block, so a damaged .mpg read past the plane
# (heap-buffer-overflow found by M-FUZZ). The patch takes those from the bound. Idempotent.

set(file "pl_mpeg.h")
file(READ "${file}" text)
if(text MATCHES "MCE: half-pel")
    return()
endif()
set(old "\tunsigned int max_address = (dw * (self->mb_height * block_size - block_size + 1) - block_size);\n")
set(new "${old}\tmax_address -= (odd_v ? dw : 0) + (odd_h ? 1 : 0); /* MCE: half-pel reads one row / column past the block */\n")
string(FIND "${text}" "${old}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "patch_pl_mpeg: max_address anchor not found in ${file} (pl_mpeg changed: update the patch)")
endif()
string(REPLACE "${old}" "${new}" text "${text}")
file(WRITE "${file}" "${text}")
