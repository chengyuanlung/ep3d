#include "Core/Drawing/DrawingTables.h"

#include <cmath>
#include <utility>

namespace paramcad {

Linetype::Linetype(std::string name, std::string description, std::vector<double> pattern)
    : id_(ObjectIdGenerator::Next()),
      name_(std::move(name)),
      description_(std::move(description)),
      pattern_(std::move(pattern)) {}

Linetype::Linetype(ObjectId id, std::string name, std::string description,
                   std::vector<double> pattern)
    : id_(RestoreObjectId(id)),
      name_(std::move(name)),
      description_(std::move(description)),
      pattern_(std::move(pattern)) {}

double Linetype::patternLength() const noexcept {
    double total = 0.0;
    for (const double segment : pattern_) total += std::fabs(segment);
    return total;
}

Layer::Layer(std::string name, int color, std::string linetype)
    : id_(ObjectIdGenerator::Next()),
      name_(std::move(name)),
      color_(color),
      linetype_(std::move(linetype)) {}

Layer::Layer(ObjectId id, std::string name, int color, std::string linetype, bool on, bool frozen,
             bool locked, int lineweight)
    : id_(RestoreObjectId(id)),
      name_(std::move(name)),
      color_(color),
      linetype_(std::move(linetype)),
      on_(on),
      frozen_(frozen),
      locked_(locked),
      lineweight_(lineweight) {}

} // namespace paramcad
