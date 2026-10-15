#include "mce/web.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

// The page-side halves. `Module.mcePersist` coalesces overlapping syncs: IDBFS warns about (and can
// interleave) two FS.syncfs calls in flight.
EM_ASYNC_JS(void, mce_web_mount_persistent, (), {
    if (Module.mcePersist) return;
    let busy = false, again = false;
    const sync = () => {
        if (busy) { again = true; return; }
        busy = true;
        FS.syncfs(false, (err) => {
            busy = false;
            if (err) console.warn("mce: saving to IndexedDB failed", err);
            if (again) { again = false; sync(); }
        });
    };
    Module.mcePersist = sync;
    try { FS.mkdir("/persistent"); } catch (e) {}
    FS.mount(IDBFS, {}, "/persistent");
    await new Promise((resolve) => FS.syncfs(true, (err) => {
        if (err) console.warn("mce: loading from IndexedDB failed", err);
        resolve();
    }));
    setInterval(sync, 5000);
    addEventListener("pagehide", sync);
    document.addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden") sync(); });
});

EM_JS(void, mce_web_persist, (), {
    if (Module.mcePersist) Module.mcePersist();
});

EM_ASYNC_JS(int, mce_web_fetch_file, (const char* url_c, const char* path_c), {
    const url = UTF8ToString(url_c), path = UTF8ToString(path_c);
    const status = (text) => { if (Module.setStatus) Module.setStatus(text); };
    try {
        const response = await fetch(url);
        if (!response.ok) return 0;
        const total = Number(response.headers.get("Content-Length")) || 0;
        const reader = response.body.getReader();
        const chunks = [];
        let received = 0;
        for (;;) {
            const { done, value } = await reader.read();
            if (done) break;
            chunks.push(value);
            received += value.length;
            status(total ? "Downloading " + url + "... (" + received + "/" + total + ")" : "Downloading " + url + "... " + received + " bytes");
        }
        const bytes = new Uint8Array(received);
        let at = 0;
        for (const c of chunks) { bytes.set(c, at); at += c.length; }
        const slash = path.lastIndexOf("/");
        if (slash > 0) FS.mkdirTree(path.substring(0, slash));
        FS.writeFile(path, bytes);
        status("");
        return 1;
    } catch (e) {
        console.warn("mce: fetching " + url + " failed", e);
        return 0;
    }
});
#endif

namespace mce::web {

#if defined(__EMSCRIPTEN__)
bool active() { return true; }
void mount_persistent() { mce_web_mount_persistent(); }
void persist() { mce_web_persist(); }
bool fetch_file(const char* url, const char* path) { return url && path && mce_web_fetch_file(url, path) != 0; }
#else
bool active() { return false; }
void mount_persistent() {}
void persist() {}
bool fetch_file(const char*, const char*) { return false; }
#endif

}  // namespace mce::web
