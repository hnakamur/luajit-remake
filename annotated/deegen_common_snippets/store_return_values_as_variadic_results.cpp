#include "force_release_build.h"

#include "define_deegen_common_snippet.h"
#include "runtime_utils.h"

static void DeegenSnippet_StoreReturnValuesAsVariadicResults(CoroutineRuntimeContext* coroCtx, uint64_t* stackBase, uint64_t* retStart, uint64_t numRet)
{
    coroCtx->m_variadicRetSlotBegin = static_cast<int32_t>(retStart - stackBase);
    coroCtx->m_numVariadicRets = static_cast<uint32_t>(numRet);
}

DEFINE_DEEGEN_COMMON_SNIPPET("StoreReturnValuesAsVariadicResults", DeegenSnippet_StoreReturnValuesAsVariadicResults)

