for i in $(seq 1 60); do
  if grep -q BUILD_DONE /work/build.log; then echo "BUILD_DONE"; break; fi
  if grep -q -E "FAILED:|CMake Error|ninja: build stopped" /work/build.log; then echo "BUILD_FAILED"; grep -E -m5 -A6 "FAILED:|CMake Error" /work/build.log; break; fi
  echo "[$i] $(tail -c 200 /work/build.log | tr '\n' ' ' | tail -c 160)"
  sleep 60
done
