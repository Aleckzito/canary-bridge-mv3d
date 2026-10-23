#include "canary_server.hpp"
#include "lib/di/container.hpp"

#ifdef _WIN32
#include <windows.h>
#include <iostream>

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exceptionInfo) {
    void* crashAddr = exceptionInfo->ExceptionRecord->ExceptionAddress;
    DWORD code      = exceptionInfo->ExceptionRecord->ExceptionCode;

    HMODULE hMod = nullptr;
    char modName[MAX_PATH] = "unknown";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(crashAddr), &hMod);
    if (hMod) {
        GetModuleFileNameA(hMod, modName, MAX_PATH);
    }

    std::cerr << "\n[CRASH] Canary exploto!"
              << "\n  Codigo:      0x" << std::hex << code
              << "\n  Instruccion: " << crashAddr
              << "\n  Modulo:      " << modName;

    if (code == EXCEPTION_ACCESS_VIOLATION && exceptionInfo->ExceptionRecord->NumberParameters >= 2) {
        const bool isWrite = (exceptionInfo->ExceptionRecord->ExceptionInformation[0] == 1);
        const ULONG_PTR accessAddr = exceptionInfo->ExceptionRecord->ExceptionInformation[1];
        std::cerr << "\n  Acceso:      " << (isWrite ? "escritura" : "lectura")
                  << " en 0x" << accessAddr;
    }

    std::cerr << std::dec << "\n" << std::endl;
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(CrashHandler);
#endif
	return inject<CanaryServer>().run();
}
