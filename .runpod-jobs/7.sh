cd /work/repo
apt-get install -y -qq python3-numpy > /dev/null 2>&1
python3 scripts/ci/gpu_compare.py build/linux-gcc-release sample_seasons_gpu_autumn sample_seasons_gpu_winter sample_water_gpu sample_weather_gpu@120 sample_weather_gpu@600 openworld_sample_player_gpu sample_daynight_gpu 2>&1 | head -230
