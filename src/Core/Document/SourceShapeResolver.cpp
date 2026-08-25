#include "Core/Document/SourceShapeResolver.h"

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace paramcad {

namespace {

SourceShapeResult refuse(std::string message) {
    SourceShapeResult result;
    result.message = std::move(message);
    return result;
}

// Every feature that failed, named. "The part failed" is true and useless:
// the reader's next move is to open that file and fix something, and which
// feature it was is the whole difference between doing that and guessing.
std::string WhyItDidNotBuild(const DocumentBase& document,
                             const DocumentRecomputeReport& report) {
    std::string why;
    for (const RecomputeItemReport& item : report.items) {
        if (item.status == RecomputeStatus::Success || item.message.empty()) continue;
        why += (why.empty() ? "" : "; ") + document.objectName(item.id) + ": " + item.message;
    }
    return why;
}

SourceShapeResult ResolveAssembly(const std::string& sourcePath,
                                  const RecomputeContext& context,
                                  const std::function<void(const DocumentBase&)>& sawDocument) {
    AssemblyLoadResult loaded = loadAssemblyDocumentFromFile(sourcePath);
    if (!loaded)
        return refuse("could not open '" + sourcePath + "': " +
                      (loaded.message.empty() ? std::string("not a readable assembly")
                                              : loaded.message));

    AssemblyDocument& sub = *loaded.document;
    sub.setGeometryKernel(context.kernel);
    sub.setSketchSolver(context.sketchSolver);
    sub.setAssemblySolver(context.assemblySolver);
    // THE CHAIN GROWS BY ONE, so a cycle three levels down is caught by the
    // same check that catches one level down.
    std::vector<std::string> chain;
    if (context.sourceChain != nullptr) chain = *context.sourceChain;
    chain.push_back(sourcePath);
    sub.setSourceChain(std::move(chain));

    const DocumentRecomputeReport built = sub.recompute();
    if (!built.success) {
        const std::string why = WhyItDidNotBuild(sub, built);
        return refuse("'" + sourcePath + "' does not build" +
                      (why.empty() ? "" : " -- " + why));
    }

    // EVERY INSTANCE INSIDE IT, WHERE THE SUB-ASSEMBLY PUT THEM. Composed
    // rather than stored (ADR-M10-002), so moving the thing that holds this
    // moves everything inside it and nothing had to be told.
    std::vector<KernelShape> insides;
    for (const Instance* one : sub.instances()) {
        if (one->currentState() != ComputeState::Valid || !one->currentShape().isValid())
            return refuse("'" + one->name() + "' inside '" + sourcePath + "' has no solid");
        insides.push_back(one->currentShape());
    }
    if (insides.empty()) return refuse("'" + sourcePath + "' has nothing in it");

    const ShapeResult together = context.kernel->compoundOf(insides);
    if (!together)
        return refuse(together.message.empty() ? "could not put '" + sourcePath + "' together"
                                               : together.message);

    if (sawDocument) sawDocument(sub);
    SourceShapeResult result;
    result.ok = true;
    result.shape = together.shape;
    result.wasAssembly = true;
    return result;
}

} // namespace

long long SourceFileStamp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    // FNV-1a over the bytes, with the LENGTH mixed in at the end so a file
    // that is a prefix of another cannot collide with it.
    unsigned long long hash = 1469598103934665603ULL;
    char buffer[8192];
    unsigned long long length = 0;
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        const std::streamsize got = in.gcount();
        for (std::streamsize i = 0; i < got; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= 1099511628211ULL;
        }
        length += static_cast<unsigned long long>(got);
        if (!in) break;
    }
    hash ^= length;
    hash *= 1099511628211ULL;
    // Never 0, which means "could not be read" and must stay distinguishable
    // from a file that happens to hash to nothing.
    return static_cast<long long>(hash == 0 ? 1ULL : hash);
}

bool IsAssemblySourceFile(const std::string& path) {
    // Read from the header rather than from the extension, because the
    // extension is a convention and the header is the format. A file called
    // `.ep3d` that holds an assembly is still an assembly, and guessing from
    // the name would give a message about the wrong thing.
    const AssemblyLoadResult probe = loadAssemblyDocumentFromFile(path);
    if (probe) return true;
    // WrongDocumentType means it opened and is a Part. Anything else --
    // missing, malformed -- is left to the part path to report, because that
    // is where the message a reader can act on already lives.
    return false;
}

SourceShapeResult ResolveSourceShape(
    const std::string& sourcePath, const std::string& bodyName,
    const RecomputeContext& context,
    const std::function<void(const DocumentBase&)>& sawDocument) {
    if (context.kernel == nullptr) return refuse("no geometry kernel configured");
    if (sourcePath.empty()) return refuse("nothing names a model file");

    // A FILE THAT CONTAINS ITSELF has no answer, and finding that out by
    // running off the end of the stack is not a failure a message can be
    // attached to.
    if (context.sourceChain != nullptr)
        for (const std::string& open : *context.sourceChain)
            if (open == sourcePath)
                return refuse("'" + sourcePath +
                              "' contains itself, so there is no answer to what is inside it");

    if (IsAssemblySourceFile(sourcePath))
        return ResolveAssembly(sourcePath, context, sawDocument);

    // READ AGAIN, EVERY REBUILD, and rebuilt from its own features rather than
    // trusted as saved -- an .ep3d holds no geometry at all (ADR-M4-004), so
    // there is nothing else it could mean. The cost is a load and a recompute
    // per caller per pass, which is real; it is paid for the reason
    // ADR-M22-003 gives, that a cache is a second thing that has to be right
    // about when the file changed.
    LoadResult loaded = loadPartDocumentFromFile(sourcePath);
    if (!loaded)
        return refuse("could not open '" + sourcePath + "': " +
                      (loaded.message.empty() ? std::string("not a readable part")
                                              : loaded.message));

    PartDocument& part = *loaded.document;
    part.setGeometryKernel(context.kernel);
    part.setSketchSolver(context.sketchSolver);
    const DocumentRecomputeReport built = part.recompute();
    if (!built.success) {
        const std::string why = WhyItDidNotBuild(part, built);
        return refuse("'" + sourcePath + "' does not build" +
                      (why.empty() ? "" : " -- " + why));
    }

    const Body* chosen = nullptr;
    if (bodyName.empty()) {
        if (part.bodies().size() != 1) {
            std::string names;
            for (const auto& body : part.bodies())
                names += (names.empty() ? "" : ", ") + body->name();
            return refuse("'" + sourcePath + "' holds " + std::to_string(part.bodies().size()) +
                          " bodies, so one of them has to be named: " + names);
        }
        chosen = part.bodies().front().get();
    } else {
        for (const auto& body : part.bodies())
            if (body->name() == bodyName) chosen = body.get();
        if (chosen == nullptr)
            return refuse("'" + sourcePath + "' has no body called '" + bodyName + "'");
    }

    // The chain TIP: the last solid feature, which is the body as it stands
    // after everything that consumed anything.
    const ISolidFeature* tip = nullptr;
    for (const auto& feature : chosen->features())
        if (const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get())) tip = solid;
    if (tip == nullptr || !tip->currentShape().isValid())
        return refuse("'" + chosen->name() + "' in '" + sourcePath + "' has no solid");

    if (sawDocument) sawDocument(part);
    SourceShapeResult result;
    result.ok = true;
    result.shape = tip->currentShape();
    result.wasAssembly = false;
    return result;
}

} // namespace paramcad
