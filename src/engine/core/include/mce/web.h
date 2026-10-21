#pragma once
// Browser services for web builds (M-WEB, ADR-0039). Every function is a no-op (or returns false)
// outside a browser, so callers need no #ifdef.
//
//   - Persistent files: IndexedDB backs /persistent (crash::user_data_root() on the web), so saves
//     and settings survive a reload. mount_persistent() loads it once at start-up (platform::init);
//     persist() writes it back after a save, and the page also writes it back every few seconds and
//     when it is hidden or closed, which covers every other writer under that folder.
//   - Audio unlock: browsers keep audio silent until the first click, tap or key press;
//     install_audio_unlock() resumes it from that event (platform::init installs it).
//   - fetch_file(): downloads a file next to the page (the cooked pack) into the virtual file system,
//     reporting progress through the page's Module.setStatus.

#include "mce/types.h"

namespace mce::web {

// True in a browser build.
bool active();

// Mounts IndexedDB at /persistent and waits for its content. Idempotent.
void mount_persistent();

// Starts writing /persistent back to IndexedDB (asynchronous; overlapping requests coalesce).
void persist();

// Resumes the page's audio on its first user gesture. Idempotent.
void install_audio_unlock();

// Waits up to `ms` with the last frame still on the page (Module.mceLingering is true), so a test can
// screenshot a run that is about to end; the page's Module.mceRelease() ends the wait early.
void linger(u32 ms);

// Downloads `url` (relative to the page) into `path`, creating its folder. False on a network or
// HTTP error, and always outside a browser.
bool fetch_file(const char* url, const char* path);

}  // namespace mce::web
