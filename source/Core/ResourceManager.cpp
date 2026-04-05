#include "ResourceManager.h"

#include <filesystem>
#include <vector>
#include <algorithm>

#include "Utils/Logger.h"

ResourceManager& ResourceManager::GetInstance() {
    static ResourceManager instance;
    return instance;
}

std::string ResourceManager::findSystemChineseFont() {
    const std::vector<std::string> fontDirs = {
        "/system/fonts",
        "/system/font",
        "/data/fonts"
    };

    const std::vector<std::string> chineseFonts = {
        "MiSansVF.ttf",
        "SysSans-Hans-Regular.ttf",
        "ZUKChinese.ttf"
    };

    // 遍历字体目录
    for (const auto& dir : fontDirs) {
        if (!std::filesystem::exists(dir)) {
            continue;
        }

        // 按优先级查找字体
        for (const auto& fontName : chineseFonts) {
            std::string fontPath = dir + "/" + fontName;
            if (std::filesystem::exists(fontPath)) {
                FLOGI("Found Chinese font: {}", fontPath);
                return fontPath;
            }
        }

        // 如果优先列表中没找到，收集所有符合条件的字体并按大小排序
        try {
            std::vector<std::pair<std::uintmax_t, std::string>> candidateFonts;
            
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    std::string extension = entry.path().extension().string();
                    
                    // 转换为小写比较
                    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
                    std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
                    
                    // 查找包含中文相关关键词的字体
                    if ((extension == ".ttf" || extension == ".otf") &&
                        (filename.find("cjk") != std::string::npos ||
                         filename.find("hans") != std::string::npos ||
                         filename.find("chinese") != std::string::npos ||
                         filename.find("sc") != std::string::npos ||
                         filename.find("noto") != std::string::npos)) {
                        std::uintmax_t fileSize = std::filesystem::file_size(entry.path());
                        candidateFonts.emplace_back(fileSize, entry.path().string());
                    }
                }
            }
            
            // 按文件大小从大到小排序
            if (!candidateFonts.empty()) {
                std::sort(candidateFonts.begin(), candidateFonts.end(), 
                    [](const auto& a, const auto& b) { return a.first > b.first; });
                
                const auto& selectedFont = candidateFonts.front();
                FLOGI("Found Chinese font (fallback, size: {} bytes): {}", 
                     selectedFont.first, selectedFont.second);
                return selectedFont.second;
            }
        } catch (const std::exception& e) {
            FLOGW("Error scanning directory {}: {}", dir, e.what());
        }
    }

    FLOGW("No Chinese font found in system directories");
    return "";
}

bool ResourceManager::initializeFonts(float fontSize) {
    ImGuiIO& io = ImGui::GetIO();

    // 尝试从系统目录加载中文字体
    std::string systemFontPath = findSystemChineseFont();
    
    if (!systemFontPath.empty()) {
        // 使用系统字体
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 2;
        fontConfig.OversampleV = 2;
        fontConfig.PixelSnapH = true;
        // 字体粗细调整: <1.0更细, 1.0正常, >1.0更粗 (建议范围 0.8-1.5)
        fontConfig.RasterizerMultiply = 1.1f;

        m_CurrentFont = io.Fonts->AddFontFromFileTTF(
            systemFontPath.c_str(),
            fontSize,
            &fontConfig,
            io.Fonts->GetGlyphRangesChineseFull());
        
        if (m_CurrentFont) {
            FLOGI("Successfully loaded system Chinese font: {}", systemFontPath);
        } else {
            FLOGE("Failed to load system font from: {}", systemFontPath);
        }
    }
    
    // 如果系统字体加载失败，尝试使用内嵌字体作为备用
    if (!m_CurrentFont) {
        FLOGW("Falling back to embedded font");
        // m_CurrentFont = io.Fonts->AddFontFromMemoryCompressedBase85TTF(Consolas_compressed_data_base85, 20.0f);
        // m_CurrentFont = io.Fonts->AddFontFromMemoryCompressedBase85TTF(SourceHanSansHWSC_VF_compressed_data_base85, 30.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        // m_CurrentFont = io.Fonts->AddFontFromMemoryCompressedBase85TTF(MiSansVF_compressed_data_base85, 27.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        // m_CurrentFont = io.Fonts->AddFontFromMemoryCompressedBase85TTF(LXGWWenKaiMonoRegular_compressed_data_base85, 25.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    }
    
    IM_ASSERT(m_CurrentFont != nullptr);

    return m_CurrentFont != nullptr;
}
