#include "bytecode_definition_utils.h"
#include "deegen_api.h"

#include "bytecode.h"

static void NO_RETURN GlobalGetMetamethodCallContinuation(TValue /*tvIndex*/)
{
    Return(GetReturnValue(0));
}

static void NO_RETURN GlobalGetImpl(TValue tvIndex)
{
    assert(tvIndex.Is<tString>());
    HeapPtr<HeapString> index = tvIndex.As<tString>();
    HeapPtr<TableObject> base = GetFEnvGlobalObject();

retry:
    GetByIdICInfo icInfo;
    TableObject::PrepareGetById(base,  UserHeapPointer<HeapString> { index }, icInfo /*out*/);
    TValue result = TableObject::GetById(base, index, icInfo);

    TValue metamethodBase;
    TValue metamethod;

    if (unlikely(icInfo.m_mayHaveMetatable && result.Is<tNil>()))
    {
        TableObject::GetMetatableResult gmr = TableObject::GetMetatable(base);
        if (gmr.m_result.m_value != 0)
        {
            HeapPtr<TableObject> metatable = gmr.m_result.As<TableObject>();
            metamethod = GetMetamethodFromMetatable(metatable, LuaMetamethodKind::Index);
            if (!metamethod.Is<tNil>())
            {
                metamethodBase = TValue::Create<tTable>(base);
                goto handle_metamethod;
            }
        }
    }
    Return(result);

handle_metamethod:
    // If 'metamethod' is a function, we should invoke the metamethod, throwing out an error if fail
    // Otherwise, we should repeat operation on 'metamethod' (i.e., recurse on metamethod[index])
    //
    if (likely(metamethod.Is<tHeapEntity>()))
    {
        HeapEntityType mmType = metamethod.GetHeapEntityType();
        if (mmType == HeapEntityType::Function)
        {
            MakeCall(metamethod.As<tFunction>(), metamethodBase, tvIndex, GlobalGetMetamethodCallContinuation);
        }
        else if (mmType == HeapEntityType::Table)
        {
            base = metamethod.As<tTable>();
            goto retry;
        }
    }

    // Now we know 'metamethod' is not a function or table, so we should locate its own exotic '__index' metamethod..
    // The difference is that if the metamethod is nil, we need to throw an error
    //
    metamethodBase = metamethod;
    metamethod = GetMetamethodForValue(metamethod, LuaMetamethodKind::Index);
    if (metamethod.Is<tNil>())
    {
        // TODO: make error message consistent with Lua
        //
        ThrowError("bad type for GetById");
    }
    goto handle_metamethod;
}

DEEGEN_DEFINE_BYTECODE(GlobalGet)
{
    Operands(
        Constant("index")
    );
    Result(BytecodeValue);
    Implementation(GlobalGetImpl);
    Variant();
}

DEEGEN_END_BYTECODE_DEFINITIONS
