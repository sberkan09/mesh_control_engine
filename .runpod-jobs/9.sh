# soak job 9: clone astc-encoder the way FetchContent does, with a trace.
export GIT_TERMINAL_PROMPT=0
rm -rf /tmp/astc; git --version
GIT_CURL_VERBOSE=1 GIT_TRACE=1 git clone --no-checkout --depth 1 --branch 5.6.0 https://github.com/ARM-software/astc-encoder.git /tmp/astc 2>&1 | grep -v -E "^\* |^[<>] (Accept|User|Host|Pragma|Git|Content|Expires|Cache|Vary|X-|Date|Server|Strict|Access|Allow|Connection|Transfer|Cross)" | tail -25
echo "---- manual again with default creds helper off"
git -c credential.helper= clone --depth 1 --branch 5.6.0 https://github.com/ARM-software/astc-encoder.git /tmp/astc2 2>&1 | tail -5
sed -n 1,45p /work/repo/build/linux-gcc-release/_deps/astcenc-subbuild/astcenc-populate-prefix/tmp/astcenc-populate-gitclone.cmake | grep -n -E "git_tag|git_repository|git_shallow|git_remote|submodule|args" | head -12
