// An old steam_api for the loader's refusal: it has SteamAPI_Init but no manual dispatch.

#if defined(_WIN32)
#define STUB_EXPORT extern "C" __declspec(dllexport)
#else
#define STUB_EXPORT extern "C" __attribute__((visibility("default")))
#endif

STUB_EXPORT bool SteamAPI_Init() { return true; }
STUB_EXPORT void SteamAPI_Shutdown() {}
