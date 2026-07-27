/*
 * Wine PE companion for the GameNative OpenXR unixlib.
 *
 * Wine 9.x can only associate a Unix-side library with a builtin PE module.
 * Keeping this tiny thunk in Wine's *-windows directory lets the regular
 * (native PE) OpenXR runtime call the Unix implementation on both the older
 * MemoryWineUnixFuncs ABI and the newer Wine unix-call dispatcher ABI.
 */
#include <windows.h>
#include <winternl.h>

/*
 * unixlib.h is intentionally not installed by every libwine-dev package.
 * These are the stable declarations used by Wine 9.x and later.
 */
typedef ULONGLONG gn_unixlib_handle_t;
extern gn_unixlib_handle_t __wine_unixlib_handle;
extern LONG (WINAPI *__wine_unix_call_dispatcher)(
    gn_unixlib_handle_t handle, unsigned int code, void *args);
extern LONG WINAPI __wine_init_unix_call(void);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        return __wine_init_unix_call() == 0;
    return TRUE;
}

__declspec(dllexport)
LONG WINAPI gnWineUnixCall(ULONG code, void *args)
{
    return __wine_unix_call_dispatcher(__wine_unixlib_handle, code, args);
}
