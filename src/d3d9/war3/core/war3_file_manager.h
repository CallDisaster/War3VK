#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>

namespace dxvk::war3 {

    /**
     * @class FileManager
     * @brief 统一文件管理器，支持从 MPQ 和本地文件系统读取文件
     * 
     * 基于 MemHack 的 storm.cpp 和 utils.cpp 实现模式。
     */
    class FileManager {
    public:
        /// 获取单例
        static FileManager& get();
        
        /// 初始化 (传入 Storm.dll 句柄)
        void initialize(HMODULE stormDll);
        
        /// 是否已初始化
        bool isInitialized() const { return m_initialized; }
        
        // ==========================================
        // API 1: 从 MPQ 读取 (Storm.dll)
        // ==========================================
        
        /// 从 MPQ 归档读取文件
        /// @param path 文件路径 (MPQ 内部路径, 如 "Units\\HumanUIL.slk")
        /// @param outBuffer 输出缓冲区
        /// @return 成功返回 true
        bool readFromMpq(const std::string& path, std::vector<uint8_t>& outBuffer);
        
        // ==========================================
        // API 2: 从本地文件系统读取
        // ==========================================
        
        /// 从本地文件系统读取文件
        /// @param path 文件路径 (相对或绝对路径)
        /// @param outBuffer 输出缓冲区
        /// @return 成功返回 true
        bool readFromLocal(const std::string& path, std::vector<uint8_t>& outBuffer);
        
        // ==========================================
        // 统一接口
        // ==========================================
        
        /// 统一读取接口: 先尝试 MPQ，失败后尝试本地
        /// @param path 文件路径
        /// @param outBuffer 输出缓冲区
        /// @param searchLocal 是否搜索本地文件系统
        /// @return 成功返回 true
        bool readFile(const std::string& path, std::vector<uint8_t>& outBuffer, bool searchLocal = true);
        
        /// 检查文件是否存在
        /// @param path 文件路径
        /// @param searchLocal 是否搜索本地文件系统
        /// @return 存在返回 true
        bool fileExists(const std::string& path, bool searchLocal = true);
        
    private:
        FileManager() = default;
        ~FileManager() = default;
        
        FileManager(const FileManager&) = delete;
        FileManager& operator=(const FileManager&) = delete;
        
        bool m_initialized = false;
        
        // Storm.dll 函数指针
        // 序号 281: SFileLoadFileEx - 加载文件 (扩展版本)
        // BOOL SFileLoadFileEx(HANDLE hArchive, LPCSTR szFileName, void** ppBuffer, 
        //                      DWORD* pdwBufferSize, DWORD dwExtraSize, DWORD dwSearchScope, 
        //                      LPOVERLAPPED lpOverlapped)
        typedef BOOL (WINAPI *SFileLoadFileEx_t)(HANDLE, LPCSTR, void**, uint32_t*, uint32_t, DWORD, void*);
        
        // 序号 280: SFileUnloadFile - 卸载文件缓冲区
        // BOOL SFileUnloadFile(void* pBuffer)
        typedef BOOL (WINAPI *SFileUnloadFile_t)(void*);
        
        // 序号 288: SFileExists - 文件存在检查 (简化版)
        // BOOL SFileExists(LPCSTR szFileName)
        typedef BOOL (WINAPI *SFileExists_t)(LPCSTR);
        
        // 序号 289: SFileHasFile - 文件存在检查 (指定 Archive)
        // BOOL SFileHasFile(HANDLE hArchive, LPCSTR szFileName, DWORD dwSearchScope)
        typedef BOOL (WINAPI *SFileHasFile_t)(HANDLE, LPCSTR, DWORD);
        
        SFileLoadFileEx_t m_pLoadFileEx = nullptr;
        SFileUnloadFile_t m_pUnloadFile = nullptr;
        SFileExists_t     m_pExists     = nullptr;
        SFileHasFile_t    m_pHasFile    = nullptr;
    };

} // namespace dxvk::war3
