# mce_relay_web_probe (M-WEB): the WebSocket relay client in a browser against a native mce_relay
# (main.cpp). Web builds only.
mce_add_executable(mce_relay_web_probe UMBRELLA SOURCES main.cpp DEPS mce::mce_net)
