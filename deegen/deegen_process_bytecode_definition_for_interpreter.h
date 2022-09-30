#pragma once

#include "misc_llvm_helper.h"

namespace dast {

struct ProcessBytecodeDefinitionForInterpreterResult
{
    std::unique_ptr<llvm::Module> m_processedModule;
    std::vector<std::string> m_generatedClassNames;
    // Has same length as m_generatedClassNames, each subvector holding the names of all the variants, in the same order as the opcode used by the builder
    //
    std::vector<std::vector<std::string>> m_allExternCDeclarations;
    std::string m_generatedHeaderFile;
};

ProcessBytecodeDefinitionForInterpreterResult WARN_UNUSED ProcessBytecodeDefinitionForInterpreter(std::unique_ptr<llvm::Module> module);

}   // namespace dast