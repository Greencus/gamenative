/*
 * ARM64EC Wine PE companion for the GameNativeVR OpenXR unixlib.
 *
 * ARM64EC Wine uses an ARM64EC builtin PE module from aarch64-windows to
 * associate a native aarch64 Unix library.  A regular x86_64 Wine builtin
 * cannot perform that association, even though ARM64EC can load ordinary
 * x86_64 game DLLs.  Keep this source free of SDK headers so it can be built
 * by LLVM using the Wine package's own ARM64EC import and CRT archives.
 * The final ARM64X image must use the package's 64 KiB section and file
 * alignment; Wine rejects a hybrid linked with the desktop PE defaults.
 */

typedef unsigned int gn_dword;
typedef int gn_bool;
typedef unsigned long long gn_unixlib_handle_t;
typedef int (*gn_unix_call_dispatcher_t)(
    gn_unixlib_handle_t handle, unsigned int code, void *args);

extern gn_unixlib_handle_t __wine_unixlib_handle;
extern gn_unix_call_dispatcher_t __wine_unix_call_dispatcher;
extern int __wine_init_unix_call(void);

__declspec(dllexport)
int gnWineUnixCall(unsigned int code, void *args)
{
    return __wine_unix_call_dispatcher(__wine_unixlib_handle, code, args);
}

gn_bool DllMain(void *instance, gn_dword reason, void *reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == 1 /* DLL_PROCESS_ATTACH */)
        return __wine_init_unix_call() == 0;
    return 1;
}
