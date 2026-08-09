//===- SVFIRAdapter.h -- SVFIR to abstract-domain symbols ----*- C++ -*-===//

#ifndef SVF_AE_SVFIR_ADAPTER_H
#define SVF_AE_SVFIR_ADAPTER_H

#include "AE/Core/NonRelationalDomain.h"

#include <map>
#include <vector>

namespace SVF
{

class FunObjVar;
class ObjVar;
class SVFIR;
class ValVar;

/// Owns the IR-specific identity mapping. Abstract-domain states only see
/// Variable and Location; they never depend on SVF NodeID or SVFIR classes.
class SVFIRAdapter
{
public:
    explicit SVFIRAdapter(const SVFIR& svfir);

    bool contains(const ValVar& value) const;
    bool contains(const ObjVar& object) const;

    AbstractDomain::Variable variable(const ValVar& value) const;
    const AbstractDomain::VariableDeclaration& declaration(
        AbstractDomain::Variable variable) const;
    AbstractDomain::Location location(const ObjVar& object) const;
    AbstractDomain::Variable contentVariable(const ObjVar& object) const;
    const ObjVar& object(AbstractDomain::Location location) const;

    const AbstractDomain::VariableEnvironment&
    environment(const FunObjVar* function = nullptr) const;

    const AbstractDomain::MemoryLayout& memoryLayout() const
    {
        return memoryLayout_;
    }

    const std::vector<const ValVar*>& trackedValues() const
    {
        return trackedValues_;
    }
    const std::vector<const ObjVar*>& trackedObjects() const
    {
        return trackedObjects_;
    }

    AbstractDomain::LinearExpression linearExpression(
        const std::vector<std::pair<const ValVar*, AbstractDomain::Rational>>&
            terms,
        AbstractDomain::Rational constant = {}) const;
    AbstractDomain::TreeExpression treeExpression(const ValVar& value) const;

private:
    std::map<const ValVar*, AbstractDomain::Variable> variables_;
    std::map<AbstractDomain::Variable,
             AbstractDomain::VariableDeclaration> declarations_;
    std::map<const ObjVar*, AbstractDomain::Location> locations_;
    std::map<AbstractDomain::Location, const ObjVar*> objects_;
    std::map<const ObjVar*, AbstractDomain::Variable> contentVariables_;
    std::vector<const ValVar*> trackedValues_;
    std::vector<const ObjVar*> trackedObjects_;
    AbstractDomain::VariableEnvironment globalEnvironment_;
    std::map<const FunObjVar*, AbstractDomain::VariableEnvironment>
        functionEnvironments_;
    AbstractDomain::MemoryLayout memoryLayout_;
};

} // namespace SVF

#endif // SVF_AE_SVFIR_ADAPTER_H
