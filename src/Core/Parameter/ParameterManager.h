#pragma once

#include "Core/Parameter/Parameter.h"
#include <memory>
#include <string_view>
#include <vector>

namespace paramcad {

class ParameterManager {
public:
    Parameter& add(std::string name, double value, UnitType unit);
    // Restore path (deserialization): adds a parameter that keeps its
    // persisted id and state.
    Parameter& restore(ObjectId id, std::string name, double value, UnitType unit,
                       std::string expression, ParameterState state);
    Parameter* findById(ObjectId id) noexcept;
    Parameter* findByName(std::string_view name) noexcept;
    const std::vector<std::unique_ptr<Parameter>>& items() const noexcept { return items_; }

private:
    std::vector<std::unique_ptr<Parameter>> items_;
};

} // namespace paramcad
