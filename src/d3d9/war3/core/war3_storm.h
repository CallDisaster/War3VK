#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace dxvk::war3 {

    class War3Storm {
    public:
        static War3Storm& get();

        // Load a file from MPQ (or local if configured in war3)
        // Returns true if successful, populates buffer.
        bool loadFile(const std::string& path, std::vector<uint8_t>& buffer);

        // Check if file exists in MPQ/Local
        bool exists(const std::string& path);
        
        void init(HMODULE hStorm);

    private:
        War3Storm() = default;

        HMODULE m_hStorm = nullptr;
        
        // Function Pointers
        typedef BOOL  (WINAPI *SFileLoadFile_t)(LPCSTR, void**, uint32_t*, uint32_t, void*);
        typedef BOOL  (WINAPI *SFileUnloadFile_t)(void*);
        typedef BOOL  (WINAPI *SFileExists_t)(LPCSTR);
        
        SFileLoadFile_t m_pSFileLoadFile = nullptr;
        SFileUnloadFile_t m_pSFileUnloadFile = nullptr;
        SFileExists_t m_pSFileExists = nullptr;
    };

}
