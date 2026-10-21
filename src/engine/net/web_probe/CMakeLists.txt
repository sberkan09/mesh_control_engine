# mce_relay_web_probe (M-WEB): the WebSocket relay client in a browser against a native mce_relay
# (main.cpp). Web builds only.
mce_add_executable(mce_relay_web_probe UMBRELLA SOURCES main.cpp DEPS mce::mce_net)
# mce_rtc_web_probe (M-NET3 step 4): an RtcHost in a browser that connects directly, over the
# browser's RTCPeerConnection, to the native echo member of `mce_relay -echo_mode rtc_probe`.
mce_add_executable(mce_rtc_web_probe UMBRELLA SOURCES rtc_probe.cpp DEPS mce::mce_net)
