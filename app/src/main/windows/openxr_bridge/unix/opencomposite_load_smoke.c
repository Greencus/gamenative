#define IMPORT __declspec(dllimport)
#define STDCALL __attribute__((stdcall))

IMPORT unsigned int STDCALL GetEnvironmentVariableA(
    const char* name, char* buffer, unsigned int size);
IMPORT void* STDCALL LoadLibraryA(const char* name);
IMPORT int STDCALL FreeLibrary(void* module);
IMPORT void STDCALL ExitProcess(unsigned long exitCode);

void mainCRTStartup(void) {
    char path[2048];
    unsigned int length = GetEnvironmentVariableA(
        "GAMENATIVE_OPENCOMPOSITE_PROBE", path, sizeof(path));
    if (length == 0 || length >= sizeof(path)) ExitProcess(10);
    void* module = LoadLibraryA(path);
    if (!module) ExitProcess(20);
    if (!FreeLibrary(module)) ExitProcess(30);
    ExitProcess(0);
}
