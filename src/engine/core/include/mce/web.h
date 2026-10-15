#pragma once
// Browser services for web builds (M-WEB, ADR-0039). Every function is a no-op (or returns false)
// outside a browser, so callers need no #ifdef.
//
//   - Persistent files: IndexedDB backs /persistent (crash::user_data_root() on the web), so saves
//     and settings survive a reload. mount_persistent() loads it once at start-up (platform::init);
//     persist() writes it back after a save, and the page also writes it back every few seconds and
//     when it is hidden or closed, which covers every other writer under that folder.
//   - fetch_file(): downloads a file next to the page (the cooked pack) into the virtual file system,
//     reporting progress through the page's Module.setStatus.

namespace mce::web {

// True in a browser build.
bool active();

// Mounts IndexedDB at /persistent and waits for its content. Idempotent.
void mount_persistent();

// Starts writing /persistent back to IndexedDB (asynchronous; overlapping requests coalesce).
void persist();

// Downloads `url` (relative to the page) into `path`, creating its folder. False on a network or
// HTTP error, and always outside a browser.
bool fetch_file(const char* url, const char* path);

}  // namespace mce::web
