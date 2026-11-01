#!/usr/bin/env bash
# RunPod GPU pod agent (Ubuntu 24.04 + NVIDIA runtime). Started as the pod's command: installs the build deps,
# prints a driver/GL/Vulkan discovery report, then polls the git branch for `.runpod-jobs/<n>.sh` files and runs
# each new one in order (output goes to the pod log). It terminates the pod itself after MCE_POD_MAX_HOURS (safety net).
set -uo pipefail
REPO_DIR="${REPO_DIR:-/work/repo}"
MAX_H="${MCE_POD_MAX_HOURS:-4}"
START=$(date +%s)
cd "$REPO_DIR"
BRANCH="$(git rev-parse --abbrev-ref HEAD)"

self_terminate() {
    echo "##AGENT self-terminate: $1"
    if [[ -n "${RUNPOD_POD_ID:-}" && -n "${RUNPOD_API_KEY:-}" ]]; then
        curl -sS -X DELETE -H "Authorization: Bearer $RUNPOD_API_KEY" "https://rest.runpod.io/v1/pods/$RUNPOD_POD_ID" || true
    fi
}

export DEBIAN_FRONTEND=noninteractive
echo "##AGENT install deps"
apt-get install -y -qq --no-install-recommends cmake ninja-build gcc g++ python3 python3-pil xvfb xauth ccache zstd \
    libgl1-mesa-dri libgl-dev libegl-dev libx11-dev libxext-dev libxcursor-dev libxrandr-dev libxi-dev libxss-dev \
    libxtst-dev libxfixes-dev libxkbcommon-dev mesa-vulkan-drivers mesa-utils vulkan-tools libvulkan1 pciutils \
    libwayland-dev libdrm-dev pkg-config > /tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; }

echo "##AGENT discovery"
nvidia-smi || true
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv || true
ls /usr/lib/x86_64-linux-gnu | grep -i -E 'nvidia|libGLX|libEGL' | head -30 || true
ls /usr/share/vulkan/icd.d /etc/vulkan/icd.d 2>/dev/null || true
echo "NVIDIA_DRIVER_CAPABILITIES=${NVIDIA_DRIVER_CAPABILITIES:-unset}"
vulkaninfo --summary 2>&1 | head -40 || true
nproc; free -g | head -2; df -h / | tail -1
echo "##AGENT discovery done"

NEXT=1
while true; do
    if (( $(date +%s) - START > MAX_H * 3600 )); then self_terminate "max lifetime ${MAX_H}h"; sleep 600; exit 0; fi
    git fetch -q origin "$BRANCH" 2>/dev/null && git reset -q --hard "origin/$BRANCH" 2>/dev/null
    job=".runpod-jobs/$NEXT.sh"
    if [[ -f "$job" ]]; then
        echo "##JOB $NEXT start $(date -u +%H:%M:%S)"
        bash "$job" 2>&1
        echo "##JOB $NEXT end rc=$? $(date -u +%H:%M:%S)"
        NEXT=$((NEXT + 1))
    else
        sleep 20
    fi
done
