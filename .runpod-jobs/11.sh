cd /work/repo
python3 - <<'PY'
import base64, io
from PIL import Image
for n in ("openworld_sample_player_gpu_gpu", "openworld_sample_player_gpu_sw"):
    im = Image.open(f"/work/cmp/{n}.ppm").convert("RGB").resize((192, 108), Image.LANCZOS)
    b = io.BytesIO(); im.save(b, "JPEG", quality=55)
    print("##B64", n, len(b.getvalue())); print(base64.b64encode(b.getvalue()).decode())
PY
