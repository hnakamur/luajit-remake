#pragma once

#include "tvalue.h"

class CodeBlock;

// APIs for accessing some environment state
//
struct DeegenEnv
{
    // Only available for use inside a return continuation
    // Get the return value for the specified ordinal
    //
    template<size_t ord>
    static TValue WARN_UNUSED GetRetVal();

    // Only available for use inside a return continuation
    // Store the whole return values as variadic ret
    //
    static void StoreReturnValuesAsVariadicRet();

    // Access the current CodeBlock
    //
    static CodeBlock* WARN_UNUSED GetCodeBlock();
};

enum MakeCallOption
{
    NoOption,
    DontProfileInInterpreter
};

// These names are hardcoded for our LLVM IR processor to locate
//
extern "C" void NO_RETURN DeegenImpl_ReturnValue(TValue value);
extern "C" void NO_RETURN DeegenImpl_ReturnNone();
extern "C" void NO_RETURN DeegenImpl_ReturnValueAndBranch(TValue value);
extern "C" void NO_RETURN DeegenImpl_ReturnNoneAndBranch();
extern "C" void NO_RETURN DeegenImpl_MakeCall_ReportContinuationAfterCall(void* handler, void* func);
extern "C" void* WARN_UNUSED DeegenImpl_MakeCall_ReportParam(void* handler, TValue arg);
extern "C" void* WARN_UNUSED DeegenImpl_MakeCall_ReportParamList(void* handler, const TValue* argBegin, size_t numArgs);
extern "C" void* WARN_UNUSED DeegenImpl_MakeCall_ReportTarget(void* handler, uint64_t target);
extern "C" void* WARN_UNUSED DeegenImpl_MakeCall_ReportOption(void* handler, size_t option);
extern "C" void* WARN_UNUSED DeegenImpl_StartMakeCallInfo();
extern "C" void* WARN_UNUSED DeegenImpl_StartMakeCallPassingVariadicResInfo();
extern "C" void* WARN_UNUSED DeegenImpl_StartMakeInPlaceCallInfo();
extern "C" void* WARN_UNUSED DeegenImpl_StartMakeInPlaceCallPassingVariadicResInfo();
extern "C" void* WARN_UNUSED DeegenImpl_StartMakeTailCallInfo();
extern "C" void* WARN_UNUSED DeegenImpl_StartMakeTailCallPassingVariadicResInfo();
extern "C" void* WARN_UNUSED DeegenImpl_StartMakeInPlaceTailCallInfo();
extern "C" void* WARN_UNUSED DeegenImpl_StartMakeInPlaceTailCallPassingVariadicResInfo();

// Return zero or one value as the result of the operation
//
inline void ALWAYS_INLINE NO_RETURN Return(TValue value)
{
    DeegenImpl_ReturnValue(value);
}

inline void ALWAYS_INLINE NO_RETURN Return()
{
    DeegenImpl_ReturnNone();
}

// Return the result of the current bytecode, and additionally informs that control flow should not fallthrough to
// the next bytecode, but redirected to the conditional branch target of this bytecode
//
inline void ALWAYS_INLINE NO_RETURN ReturnAndBranch(TValue value)
{
    DeegenImpl_ReturnValueAndBranch(value);
}

inline void ALWAYS_INLINE NO_RETURN ReturnAndBranch()
{
    DeegenImpl_ReturnNoneAndBranch();
}

void NO_RETURN Error(const char* msg);

namespace detail {

template<typename... Args>
struct MakeCallArgHandlerImpl;

template<>
struct MakeCallArgHandlerImpl<std::nullptr_t>
{
    static void NO_RETURN ALWAYS_INLINE handle(void* handler, std::nullptr_t /*nullptr*/)
    {
        DeegenImpl_MakeCall_ReportContinuationAfterCall(handler, nullptr);
    }
};

template<typename... ContinuationFnArgs>
struct MakeCallArgHandlerImpl<void(*)(ContinuationFnArgs...)>
{
    static void NO_RETURN ALWAYS_INLINE handle(void* handler, void(*func)(ContinuationFnArgs...))
    {
        DeegenImpl_MakeCall_ReportContinuationAfterCall(handler, reinterpret_cast<void*>(func));
    }
};

template<typename... ContinuationFnArgs>
struct MakeCallArgHandlerImpl<NO_RETURN void(*)(ContinuationFnArgs...)>
{
    static void NO_RETURN ALWAYS_INLINE handle(void* handler, void(*func)(ContinuationFnArgs...))
    {
        DeegenImpl_MakeCall_ReportContinuationAfterCall(handler, reinterpret_cast<void*>(func));
    }
};

template<typename... Args>
struct MakeCallArgHandlerImpl<TValue, Args...>
{
    static void NO_RETURN ALWAYS_INLINE handle(void* handler, TValue arg, Args... remainingArgs)
    {
        MakeCallArgHandlerImpl<Args...>::handle(DeegenImpl_MakeCall_ReportParam(handler, arg), remainingArgs...);
    }
};

template<typename T, typename... Args>
struct MakeCallArgHandlerImpl<TValue*, T, Args...>
{
    static_assert(std::is_integral_v<T> && !std::is_signed_v<T> && !std::is_same_v<T, bool>);

    static void NO_RETURN ALWAYS_INLINE handle(void* handler, TValue* argBegin, T numArgs, Args... remainingArgs)
    {
        MakeCallArgHandlerImpl<Args...>::handle(DeegenImpl_MakeCall_ReportParamList(handler, argBegin, static_cast<size_t>(numArgs)), remainingArgs...);
    }
};

template<typename T, typename... Args>
struct MakeCallArgHandlerImpl<const TValue*, T, Args...>
{
    static_assert(std::is_integral_v<T> && !std::is_signed_v<T> && !std::is_same_v<T, bool>);

    static void NO_RETURN ALWAYS_INLINE handle(void* handler, const TValue* argBegin, T numArgs, Args... remainingArgs)
    {
        MakeCallArgHandlerImpl<Args...>::handle(DeegenImpl_MakeCall_ReportParamList(handler, argBegin, static_cast<size_t>(numArgs)), remainingArgs...);
    }
};

template<typename... Args>
struct MakeCallHandlerImpl;

template<typename... Args>
struct MakeCallHandlerImpl<HeapPtr<FunctionObject>, Args...>
{
    static void NO_RETURN ALWAYS_INLINE handle(void* handler, HeapPtr<FunctionObject> target, Args... args)
    {
        MakeCallArgHandlerImpl<Args...>::handle(DeegenImpl_MakeCall_ReportTarget(handler, reinterpret_cast<uint64_t>(target)), args...);
    }
};

template<typename... Args>
struct MakeCallHandlerImpl<MakeCallOption, HeapPtr<FunctionObject>, Args...>
{
    static void NO_RETURN ALWAYS_INLINE handle(void* handler, MakeCallOption option, HeapPtr<FunctionObject> target, Args... args)
    {
        MakeCallHandlerImpl<HeapPtr<FunctionObject>, Args...>::handle(DeegenImpl_MakeCall_ReportOption(handler, static_cast<size_t>(option)), target, args...);
    }
};

constexpr size_t x_stackFrameHeaderSlots = 4;

template<typename... ContinuationFnArgs>
void NO_RETURN ALWAYS_INLINE ReportInfoForInPlaceCall(void* handler, TValue* layoutBegin, size_t numArgs, void(*continuationFn)(ContinuationFnArgs...))
{
    handler = DeegenImpl_MakeCall_ReportTarget(handler, layoutBegin[0].m_value);
    handler = DeegenImpl_MakeCall_ReportParamList(handler, layoutBegin + x_stackFrameHeaderSlots, numArgs);
    DeegenImpl_MakeCall_ReportContinuationAfterCall(handler, reinterpret_cast<void*>(continuationFn));
}

inline void NO_RETURN ALWAYS_INLINE ReportInfoForInPlaceTailCall(void* handler, TValue* layoutBegin, size_t numArgs)
{
    handler = DeegenImpl_MakeCall_ReportTarget(handler, layoutBegin[0].m_value);
    handler = DeegenImpl_MakeCall_ReportParamList(handler, layoutBegin + x_stackFrameHeaderSlots, numArgs);
    DeegenImpl_MakeCall_ReportContinuationAfterCall(handler, nullptr);
}

}   // namespace detail

// Make a call to a guest language function, with pre-layouted frame holding the target function and the arguments.
// Specifically, layoutBegin[0] must hold a HeapPtr<FunctionObject>, and layoutBegin[stackFrameHdrSize, stackFrameHdrSize + numArgs) must hold all the arguments.
// Note that this also implies that everything >= layoutBegin are invalidated after the call
//
template<typename... ContinuationFnArgs>
void NO_RETURN ALWAYS_INLINE MakeInPlaceCall(TValue* layoutBegin, size_t numArgs, void(*continuationFn)(ContinuationFnArgs...))
{
    detail::ReportInfoForInPlaceCall(DeegenImpl_StartMakeInPlaceCallInfo(), layoutBegin, numArgs, continuationFn);
}

// Same as above, except that the variadic results from the immediate preceding opcode are appended to the end of the argument list
//
template<typename... ContinuationFnArgs>
void NO_RETURN ALWAYS_INLINE MakeInPlaceCallPassingVariadicRes(TValue* layoutBegin, size_t numArgs, void(*continuationFn)(ContinuationFnArgs...))
{
    detail::ReportInfoForInPlaceCall(DeegenImpl_StartMakeInPlaceCallPassingVariadicResInfo(), layoutBegin, numArgs, continuationFn);
}

// The tail call versions
//
inline void NO_RETURN ALWAYS_INLINE MakeInPlaceTailCall(TValue* layoutBegin, size_t numArgs)
{
    detail::ReportInfoForInPlaceTailCall(DeegenImpl_StartMakeInPlaceTailCallInfo(), layoutBegin, numArgs);
}

inline void NO_RETURN ALWAYS_INLINE MakeInPlaceTailCallPassingVariadicRes(TValue* layoutBegin, size_t numArgs)
{
    detail::ReportInfoForInPlaceTailCall(DeegenImpl_StartMakeInPlaceTailCallPassingVariadicResInfo(), layoutBegin, numArgs);
}

// Make a call to a guest language function.
// The new call frame is set up at the end of the current function's stack frame.
// This requires copying, so it's less efficient, but also more flexible.
//
// The parameters are (in listed order):
// 1. An optional Option flag
// 2. The target function (a HeapPtr<FunctionObject> value)
// 3. The arguments, described as a list of TValue and (TValue*, size_t)
// 4. The continuation function
//
template<typename... Args>
void NO_RETURN ALWAYS_INLINE MakeCall(Args... args)
{
    detail::MakeCallHandlerImpl<Args...>::handle(DeegenImpl_StartMakeCallInfo(), args...);
}

// Same as above, except additionally passing varret
//
template<typename... Args>
void NO_RETURN ALWAYS_INLINE MakeCallPassingVariadicRes(Args... args)
{
    detail::MakeCallHandlerImpl<Args...>::handle(DeegenImpl_StartMakeCallPassingVariadicResInfo(), args...);
}

// The tail call versions
//
template<typename... Args>
void NO_RETURN ALWAYS_INLINE MakeTailCall(Args... args)
{
    detail::MakeCallHandlerImpl<Args..., std::nullptr_t>::handle(DeegenImpl_StartMakeTailCallInfo(), args..., nullptr);
}

template<typename... Args>
void NO_RETURN ALWAYS_INLINE MakeTailCallPassingVariadicRes(Args... args)
{
    detail::MakeCallHandlerImpl<Args..., std::nullptr_t>::handle(DeegenImpl_StartMakeTailCallPassingVariadicResInfo(), args..., nullptr);
}

namespace detail {

template<typename Lambda>
inline constexpr auto lambda_functor_member_pointer_v = &Lambda::operator();

template<typename Lambda>
constexpr const void* lambda_functor_member_pointer_pointer_v = &lambda_functor_member_pointer_v<Lambda>;

void ImplPreserveLambdaInfo(const void* /*closure state address*/, const void* /*closure function address's address*/);

template<typename Lambda>
void NO_INLINE PreserveLambdaInfo(const Lambda& lambda)
{
    ImplPreserveLambdaInfo(static_cast<const void*>(&lambda), lambda_functor_member_pointer_pointer_v<Lambda>);
}

enum class SwitchCaseTag : int32_t { tag = 1 };
enum class SwitchDefaultTag : int32_t { tag = 2 };

constexpr SwitchCaseTag x_switchCaseTag = SwitchCaseTag::tag;
constexpr SwitchDefaultTag x_switchDefaultTag = SwitchDefaultTag::tag;

template<typename LambdaCond, typename LambdaAction>
void NO_INLINE MakeSwitchCaseClause(const LambdaCond& lambdaCond, const LambdaAction& lambdaAction)
{
    static_assert(std::is_invocable_v<LambdaCond>, "switch-case case clause must be a lambda with no parameters");
    static_assert(std::is_same_v<std::invoke_result_t<LambdaCond>, bool>, "switch-case case clause must return bool");
    static_assert(std::is_invocable_v<LambdaAction>, "switch-case action clause must be a lambda with no parameters");
    static_assert(std::is_same_v<std::invoke_result_t<LambdaAction>, void>, "switch-case action clause must return void");
    PreserveLambdaInfo(lambdaCond);
    PreserveLambdaInfo(lambdaAction);
}

template<typename LambdaAction>
void NO_INLINE MakeSwitchDefaultClause(const LambdaAction& lambdaAction)
{
    static_assert(std::is_invocable_v<LambdaAction>, "switch-case action clause must be a lambda with no parameters");
    static_assert(std::is_same_v<std::invoke_result_t<LambdaAction>, void>, "switch-case action clause must return void");
    PreserveLambdaInfo(lambdaAction);
}

inline void ALWAYS_INLINE MakeSwitch() { }

template<typename LambdaCond, typename LambdaAction, typename... Args>
void NO_INLINE MakeSwitch(const SwitchCaseTag&, const LambdaCond& lambdaCond, const LambdaAction& lambdaAction, const Args&... args)
{
    MakeSwitchCaseClause(lambdaCond, lambdaAction);
    MakeSwitch(args...);
}

template<typename LambdaAction, typename... Args>
void NO_INLINE MakeSwitch(const SwitchDefaultTag&, const LambdaAction& lambdaAction, const Args&... args)
{
    MakeSwitchDefaultClause(lambdaAction);
    MakeSwitch(args...);
}

}   // namespace detail

struct SwitchOnMutuallyExclusiveCases
{
    template<typename... Args>
    NO_INLINE SwitchOnMutuallyExclusiveCases(const Args&... args)
    {
        detail::MakeSwitch(args...);
    }
};

#define CASE(...) ::detail::x_switchCaseTag, [&]() -> bool { return (__VA_ARGS__); }, [&]() -> void
#define DEFAULT() ::detail::x_switchDefaultTag, [&]() -> void