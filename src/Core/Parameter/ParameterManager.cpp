#include "Core/Parameter/ParameterManager.h"

namespace paramcad {

Parameter& ParameterManager::add(std::string name, double value, UnitType unit) {
    auto item = std::make_unique<Parameter>(std::move(name), value, unit);
    auto& ref = *item;
    items_.push_back(std::move(item));
    return ref;
}

Parameter* ParameterManager::findById(ObjectId id) noexcept {
    for (auto& item : items_) if (item->id() == id) return item.get();
    return nullptr;
}

Parameter* ParameterManager::findByName(std::string_view name) noexcept {
    for (auto& item : items_) if (item->name() == name) return item.get();
    return nullptr;
}

} // namespace paramcad
