#include "../gamenative_openxr_unix.h"

#define IMPORT __declspec(dllimport)
#define STDCALL __attribute__((stdcall))

typedef int smoke_status;
typedef smoke_status (STDCALL *SmokeUnixCall)(unsigned int code, void* args);
IMPORT void* STDCALL LoadLibraryA(const char* name);
IMPORT void* STDCALL GetProcAddress(void* module, const char* name);
IMPORT void STDCALL ExitProcess(unsigned long exitCode);

void mainCRTStartup(void) {
    struct gn_unix_init_args args;
    void* module = LoadLibraryA("gamenative_xr_unixbridge.dll");
    if (!module) ExitProcess(10);
    SmokeUnixCall unix_call =
        (SmokeUnixCall)GetProcAddress(module, "gnWineUnixCall");
    if (!unix_call) ExitProcess(20);
    args.abi_version = GN_UNIX_ABI_VERSION;
    args.result = GN_UNIX_ERROR_UNAVAILABLE;
    smoke_status status = unix_call(GN_UNIX_INIT, &args);
    ExitProcess(status ? 30 : (args.result == GN_UNIX_SUCCESS ? 0 : 40));
}
