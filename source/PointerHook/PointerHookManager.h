#pragma once

#include <typeindex>
#include <shared_mutex>
#include <unordered_map>

#include "IPointerHook.h"
#include "Logger.h"

class PointerHookManager
{
public:
    PointerHookManager(const PointerHookManager&) = delete;
    PointerHookManager& operator=(const PointerHookManager&) = delete;

    static PointerHookManager& GetInstance()
    {
        static PointerHookManager gInstance;
        return gInstance;
    }

    template<class T, class... Args, std::enable_if_t<std::is_base_of_v<IPointerHook, T>, int> = 0>
    void Add(Args&&... args)
    {
        std::type_index idx(typeid(T));

        auto [it, inserted] = m_hookMap.try_emplace(idx, nullptr);
        if (!inserted) {
            LOGI("[PointerHookManager] Hook %s already exists", it->second->GetName().c_str());
            return;
        }

        auto hack = std::make_unique<T>(std::forward<Args>(args)...);
        hack->Initialize();
        hack->InstallHook();

        LOGI("[PointerHookManager] Add: %s", hack->GetName().c_str());
        it->second = std::move(hack);
    }

    template<class T, std::enable_if_t<std::is_base_of_v<IPointerHook, T>, int> = 0>
    void Remove()
    {
        std::type_index idx(typeid(T));
        if (auto it = m_hookMap.find(idx); it != m_hookMap.end()) {
            LOGI("[PointerHookManager] Remove: %s", it->second->GetName().c_str());
            m_hookMap.erase(it);
        } else {
            LOGI("[PointerHookManager] Hook %s not found", idx.name());
        }
    }

    template<class T, class... Args, std::enable_if_t<std::is_base_of_v<IPointerHook, T>, int> = 0>
    void Enable(Args&&... args)
    {
        std::type_index idx(typeid(T));
        if (auto it = m_hookMap.find(idx); it != m_hookMap.end()) {
            LOGI("[PointerHookManager] Enable: %s", it->second->GetName().c_str());
            it->second->InstallHook();
        } else {
            LOGI("[PointerHookManager] Hook %s not found", idx.name());
        }
    }

    template<class T, std::enable_if_t<std::is_base_of_v<IPointerHook, T>, int> = 0>
    void Disable()
    {
        std::type_index idx(typeid(T));
        if (auto it = m_hookMap.find(idx); it != m_hookMap.end()) {
            LOGI("[PointerHookManager] Disable: %s", it->second->GetName().c_str());
            it->second->RestoreHook();
        } else {
            LOGI("[PointerHookManager] Hook %s not found", idx.name());
        }
    }

    template<class T, class... Args, std::enable_if_t<std::is_base_of_v<IPointerHook, T>, int> = 0>
    void AddByName(const std::string& name, Args&&... args)
    {
        auto [it, inserted] = m_namedHookMap.try_emplace(name, nullptr);
        if (!inserted) {
            LOGI("[PointerHookManager] Named hook %s already exists", name.c_str());
            return;
        }

        auto hack = std::make_unique<T>(std::forward<Args>(args)...);
        hack->Initialize();
        hack->InstallHook();

        LOGI("[PointerHookManager] AddByName: %s", hack->GetName().c_str());
        it->second = std::move(hack);
    }

    void RemoveByName(const std::string& name)
    {
        auto it = m_namedHookMap.find(name);
        if (it != m_namedHookMap.end()) {
            LOGI("[PointerHookManager] RemoveByName: %s", it->second->GetName().c_str());
            m_namedHookMap.erase(it);
        } else {
            LOGI("[PointerHookManager] Named hook %s not found", name.c_str());
        }
    }

    void EnableByName(const std::string& name)
    {
        auto it = m_namedHookMap.find(name);
        if (it != m_namedHookMap.end()) {
            LOGI("[PointerHookManager] EnableByName: %s", it->second->GetName().c_str());
            it->second->InstallHook();
        } else {
            LOGI("[PointerHookManager] Named hook %s not found", name.c_str());
        }
    }

    void DisableByName(const std::string& name)
    {
        auto it = m_namedHookMap.find(name);
        if (it != m_namedHookMap.end()) {
            LOGI("[PointerHookManager] DisableByName: %s", it->second->GetName().c_str());
            it->second->RestoreHook();
        } else {
            LOGI("[PointerHookManager] Named hook %s not found", name.c_str());
        }
    }

protected:
    PointerHookManager() {}
    ~PointerHookManager() {}

private:
    std::shared_mutex m_mutex;
    std::unordered_map<std::type_index, std::unique_ptr<IPointerHook>> m_hookMap;
    std::unordered_map<std::string, std::unique_ptr<IPointerHook>> m_namedHookMap;
};
