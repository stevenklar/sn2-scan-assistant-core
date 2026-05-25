#include <ScannerAssistCore.hpp>

#define SCANNERASSISTCORE_MOD_API __declspec(dllexport)
extern "C"
{
    SCANNERASSISTCORE_MOD_API RC::CppUserModBase* start_mod()
    {
        return new ScannerAssistCore::Mod();
    }

    SCANNERASSISTCORE_MOD_API void uninstall_mod(RC::CppUserModBase* mod)
    {
        delete mod;
    }
}
