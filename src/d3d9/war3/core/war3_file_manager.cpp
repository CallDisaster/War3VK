#include "war3_file_manager.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <filesystem>
#include <cstring>

namespace dxvk::war3 {

    FileManager& FileManager::get() {
        static FileManager instance;
        return instance;
    }

    void FileManager::initialize(HMODULE stormDll) {
        if (!stormDll) {
            Logger::warn("FileManager: Storm.dll handle is null");
            return;
        }
        
        // 通过序号获取函数指针
        m_pLoadFileEx = reinterpret_cast<SFileLoadFileEx_t>(GetProcAddress(stormDll, (LPCSTR)281));
        m_pUnloadFile = reinterpret_cast<SFileUnloadFile_t>(GetProcAddress(stormDll, (LPCSTR)280));
        m_pExists     = reinterpret_cast<SFileExists_t>(GetProcAddress(stormDll, (LPCSTR)288));
        m_pHasFile    = reinterpret_cast<SFileHasFile_t>(GetProcAddress(stormDll, (LPCSTR)289));
        
        m_initialized = (m_pLoadFileEx && m_pUnloadFile);
        
        if (m_initialized) {
            Logger::info("FileManager: Initialized successfully");
        } else {
            Logger::err("FileManager: Failed to load Storm functions");
        }
    }

    bool FileManager::readFromMpq(const std::string& path, std::vector<uint8_t>& outBuffer) {
        if (!m_initialized || !m_pLoadFileEx || !m_pUnloadFile) {
            return false;
        }
        
        void* buffer = nullptr;
        uint32_t size = 0;
        
        // 调用 SFileLoadFileEx
        // 参数: hArchive=NULL (搜索所有已加载的MPQ), szFileName, ppBuffer, pdwBufferSize,
        //       dwExtraSize=0, dwSearchScope=0 (SFILE_OPEN_FROM_MPQ), lpOverlapped=NULL
        if (m_pLoadFileEx(NULL, path.c_str(), &buffer, &size, 0, 0, NULL)) {
            if (buffer && size > 0) {
                outBuffer.resize(size);
                std::memcpy(outBuffer.data(), buffer, size);
                m_pUnloadFile(buffer);
                return true;
            }
            if (buffer) {
                m_pUnloadFile(buffer);
            }
        }
        
        return false;
    }

    bool FileManager::readFromLocal(const std::string& path, std::vector<uint8_t>& outBuffer) {
        try {
            std::filesystem::path fsPath(path);
            
            if (!std::filesystem::exists(fsPath)) {
                return false;
            }
            
            std::ifstream file(fsPath, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                return false;
            }
            
            auto fileSize = file.tellg();
            if (fileSize <= 0) {
                return false;
            }
            
            file.seekg(0, std::ios::beg);
            outBuffer.resize(static_cast<size_t>(fileSize));
            file.read(reinterpret_cast<char*>(outBuffer.data()), fileSize);
            file.close();
            
            return true;
        } catch (const std::exception& e) {
            Logger::warn(str::format("FileManager: Failed to read local file '", path, "': ", e.what()));
            return false;
        }
    }

    bool FileManager::readFile(const std::string& path, std::vector<uint8_t>& outBuffer, bool searchLocal) {
        // 优先尝试 MPQ
        if (readFromMpq(path, outBuffer)) {
            return true;
        }
        
        // 失败后尝试本地文件系统
        if (searchLocal && readFromLocal(path, outBuffer)) {
            return true;
        }
        
        return false;
    }

    bool FileManager::fileExists(const std::string& path, bool searchLocal) {
        // 检查 MPQ
        if (m_initialized && m_pExists) {
            if (m_pExists(path.c_str())) {
                return true;
            }
        }
        
        // 检查本地
        if (searchLocal) {
            try {
                if (std::filesystem::exists(path)) {
                    return true;
                }
            } catch (...) {}
        }
        
        return false;
    }

} // namespace dxvk::war3
