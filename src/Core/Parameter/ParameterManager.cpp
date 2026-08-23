#include "Core/Parameter/ParameterManager.h"

namespace paramcad {

Parameter& ParameterManager::add(std::string name, double value, UnitType unit,
                                 ValueDomain domain) {
    auto item = std::make_unique<Parameter>(std::move(name), value, unit, domain);
    auto& ref = *item;
    items_.push_back(std::move(item));
    return ref;
}

Parameter& ParameterManager::restore(ObjectId id, std::string name, double value, UnitType unit,
                                     std::string expression, ParameterState state,
                                     ValueDomain domain) {
    auto item = std::make_unique<Parameter>(id, std::move(name), value, unit,
                                            std::move(expression), state, domain);
    auto& ref = *item;
    items_.push_back(std::move(item));
    return ref;
}

bool ParameterManager::remove(ObjectId id) noexcept {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if ((*it)->id() == id) {
            items_.erase(it);
            return true;
        }
    }
    return false;
}

Parameter* ParameterManager::findById(ObjectId id) noexcept {
    for (auto& item : items_) if (item->id() == id) return item.get();
    return nullptr;
}

Parameter* ParameterManager::findByName(std::string_view name) noexcept {
    for (auto& item : items_) if (item->name() == name) return item.get();
    return nullptr;
}

const Parameter* ParameterManager::findById(ObjectId id) const noexcept {
    for (const auto& item : items_) if (item->id() == id) return item.get();
    return nullptr;
}

const Parameter* ParameterManager::findByName(std::string_view name) const noexcept {
    for (const auto& item : items_) if (item->name() == name) return item.get();
    return nullptr;
}

} // namespace paramcad
