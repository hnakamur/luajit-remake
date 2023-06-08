#include "baseline_jit_codegen_helper.h"
#include "runtime_utils.h"
#include "bytecode_builder.h"

// These tables are generated by Deegen
//
extern "C" const BytecodeBaselineJitTraits deegen_baseline_jit_bytecode_trait_table[];
extern "C" const BaselineJitFunctionEntryLogicTraits deegen_baseline_jit_function_entry_logic_trait_table_nova[];
extern "C" const BaselineJitFunctionEntryLogicTraits deegen_baseline_jit_function_entry_logic_trait_table_va[];
extern "C" const uint8_t deegen_baseline_jit_generic_ic_jit_allocation_stepping_table[];

// The implementation of this function is generated by Deegen
// Note that this function prototype is also hardcoded!
//
extern "C" void deegen_baseline_jit_do_codegen_impl(DeegenBaselineJitCodegenControlStruct*);

static size_t WARN_UNUSED ALWAYS_INLINE RoundUpToPowerOfTwoAlignment(size_t value, size_t alignmentMustBePowerOf2)
{
    assert(is_power_of_2(alignmentMustBePowerOf2));
    value += alignmentMustBePowerOf2 - 1;
    size_t mask = ~(alignmentMustBePowerOf2 - 1);
    return value & mask;
}

static BaselineJitFunctionEntryLogicTraits WARN_UNUSED ALWAYS_INLINE GetBaselineJitFunctionEntryLogicTrait(bool takesVarArg, size_t numFixedArgs)
{
    if (!takesVarArg)
    {
        constexpr size_t x_novaThres = x_baselineJitFunctionEntrySpecializeThresholdForNonVarargsFunction;
        if (numFixedArgs <= x_novaThres)
        {
            return deegen_baseline_jit_function_entry_logic_trait_table_nova[numFixedArgs];
        }
        else
        {
            return deegen_baseline_jit_function_entry_logic_trait_table_nova[x_novaThres + 1];
        }
    }
    else
    {
        constexpr size_t x_vaThres = x_baselineJitFunctionEntrySpecializeThresholdForVarargsFunction;
        if (numFixedArgs <= x_vaThres)
        {
            return deegen_baseline_jit_function_entry_logic_trait_table_va[numFixedArgs];
        }
        else
        {
            return deegen_baseline_jit_function_entry_logic_trait_table_va[x_vaThres + 1];
        }
    }
}

using BytecodeOpcodeTy = DeegenBytecodeBuilder::BytecodeBuilder::BytecodeOpcodeTy;

BaselineCodeBlock* NO_INLINE deegen_baseline_jit_do_codegen(CodeBlock* cb)
{
    // Each CodeBlock should be codegen'ed only once.
    // Be extra careful to catch such bugs, as these will not show up as correctness issues but cause silent performance regressions.
    //
    ReleaseAssert(cb->m_baselineCodeBlock == nullptr);

    uint8_t* bytecodeStream = cb->GetBytecodeStream();
    uint8_t* bytecodeStreamEnd = bytecodeStream + cb->m_bytecodeLength - DeegenBytecodeBuilder::BytecodeBuilder::x_numExtraPaddingAtEnd;

    // Get the function entry logic trait based on the function prototype
    //
    BaselineJitFunctionEntryLogicTraits fnPrologueInfo = GetBaselineJitFunctionEntryLogicTrait(cb->m_hasVariadicArguments, cb->m_numFixedArguments);

    size_t fastPathCodeLen = fnPrologueInfo.m_fastPathCodeLen;
    size_t slowPathCodeLen = fnPrologueInfo.m_slowPathCodeLen;
    size_t dataSectionCodeLen = fnPrologueInfo.m_dataSecCodeLen;
    size_t numLateCondBrPatches = 0;
    size_t slowPathDataStreamLen = 0;
    size_t numBytecodes = 0;

    // Compute various buffer sizes from the bytecode stream
    // This could have been done right away at the time the bytecode were generated, but doing so would be quite a bit more cumbersome
    // due to the ReplaceBytecode API and the data section alignment requirement.
    //
    // So we simply make a pass through the stream here, which shouldn't be expensive after all since there's no indirect branches.
    //
    {
        uint8_t* ptr = bytecodeStream;
        while (ptr < bytecodeStreamEnd)
        {
            BytecodeOpcodeTy opcode = UnalignedLoad<BytecodeOpcodeTy>(ptr);
            assert(opcode < DeegenBytecodeBuilder::BytecodeBuilder::GetTotalBytecodeKinds());
            BytecodeBaselineJitTraits trait = deegen_baseline_jit_bytecode_trait_table[opcode];
            fastPathCodeLen += trait.m_fastPathCodeLen;
            slowPathCodeLen += trait.m_slowPathCodeLen;
            dataSectionCodeLen = RoundUpToPowerOfTwoAlignment(dataSectionCodeLen, trait.m_dataSectionAlignment);
            dataSectionCodeLen += trait.m_dataSectionCodeLen;
            numLateCondBrPatches += trait.m_numCondBrLatePatches;
            slowPathDataStreamLen += trait.m_slowPathDataLen;
            numBytecodes += 1;
            ptr += trait.m_bytecodeLength;
        }
        TestAssert(ptr == bytecodeStreamEnd);
        TestAssert(UnalignedLoad<BytecodeOpcodeTy>(ptr) == DeegenBytecodeBuilder::BytecodeBuilder::GetTotalBytecodeKinds());
    }

    // Determine the layout of the generated code:
    //     [ Data Section ] [ Fast Path ] [ Slow Path ]
    // Note that however, the codegen may overwrite at most 7 more bytes after each section, so allocation must account for that.
    //
    constexpr size_t x_maxBytesCodegenFnMayOverwrite = 7;
    size_t fastPathSectionOffset = dataSectionCodeLen;
    if (dataSectionCodeLen > 0)
    {
        // Only add the padding if the data section is not empty (it is often empty),
        // since if the data section is empty, the codegen won't write anything at all so the padding is not needed.
        // This way we don't waste 16 bytes if the data section is empty.
        //
        fastPathSectionOffset += x_maxBytesCodegenFnMayOverwrite;
    }
    // Make the function entry address 16-byte aligned
    //
    fastPathSectionOffset = RoundUpToMultipleOf<16>(fastPathSectionOffset);

    // 'x_maxBytesCodegenFnMayOverwrite' bytes of NOP needs to be populated between the fast path code and
    // the slow path code sections to avoid breaking debugger disassembler
    //
    size_t fastPathSectionEnd = fastPathSectionOffset + fastPathCodeLen;

    size_t slowPathSectionOffset = fastPathSectionEnd + x_maxBytesCodegenFnMayOverwrite;
    size_t slowPathSectionEnd = slowPathSectionOffset + slowPathCodeLen;

    size_t totalJitRegionSize = slowPathSectionEnd + x_maxBytesCodegenFnMayOverwrite;

    // TODO: right now the data section is also marked executable because we just use one mmap for simplicity..
    //
    VM* vm = VM::GetActiveVMForCurrentThread();
    vm->IncrementNumTotalBaselineJitCompilations();
    JitMemoryAllocator* jitAlloc = vm->GetJITMemoryAlloc();
    void* regionVoidPtr = jitAlloc->AllocateGivenSize(totalJitRegionSize);
    assert(regionVoidPtr != nullptr);

    uint8_t* dataSecPtr = reinterpret_cast<uint8_t*>(regionVoidPtr);

    // This is required in order for all the computations above about the data section size to hold
    //
    assert(reinterpret_cast<uintptr_t>(dataSecPtr) % x_baselineJitMaxPossibleDataSectionAlignment == 0);

    uint8_t* fastPathSecPtr = dataSecPtr + fastPathSectionOffset;
    uint8_t* slowPathSecPtr = dataSecPtr + slowPathSectionOffset;

    uint8_t* fastPathSecTrueEnd = fastPathSecPtr + fastPathCodeLen;
    uint8_t* slowPathSecTrueEnd = slowPathSecPtr + slowPathCodeLen;

    // Set up the BaselineCodeBlock
    //
    BaselineCodeBlock* bcb = BaselineCodeBlock::Create(cb,
                                                       SafeIntegerCast<uint32_t>(numBytecodes),
                                                       SafeIntegerCast<uint32_t>(slowPathDataStreamLen),
                                                       fastPathSecPtr /*jitCodeEntry*/,
                                                       dataSecPtr /*jitRegionStart*/,
                                                       SafeIntegerCast<uint32_t>(totalJitRegionSize));

    BaselineCodeBlock::SlowPathDataAndBytecodeOffset* slowPathDataIndexArray = bcb->m_sbIndex;
    uint8_t* slowPathDataStreamStart = bcb->GetSlowPathDataStreamStart();

    // Allocate the temporary array for LateCondBrPatches
    //
    BaselineJitCondBrLatePatchRecord* condBrLatePatchList = new BaselineJitCondBrLatePatchRecord[numLateCondBrPatches];
    Auto(delete [] condBrLatePatchList);

    // Emit the function entry logic
    //
    fnPrologueInfo.m_emitterFn(fastPathSecPtr, slowPathSecPtr, dataSecPtr);

    // Set up the control struct and invoke the baseline JIT compiler implementation to emit the function body
    //
    {
        DeegenBaselineJitCodegenControlStruct ctl;
        ctl.m_jitFastPathAddr = fastPathSecPtr + fnPrologueInfo.m_fastPathCodeLen;
        ctl.m_jitSlowPathAddr = slowPathSecPtr + fnPrologueInfo.m_slowPathCodeLen;
        ctl.m_jitDataSecAddr = dataSecPtr + fnPrologueInfo.m_dataSecCodeLen;
        ctl.m_condBrPatchesArray = condBrLatePatchList;
        ctl.m_slowPathDataPtr = slowPathDataStreamStart;
        ctl.m_slowPathDataIndexArray = slowPathDataIndexArray;
        ctl.m_codeBlock32 = static_cast<uint64_t>(static_cast<uint32_t>(reinterpret_cast<uint64_t>(cb)));
        ctl.m_initialSlowPathDataOffset = static_cast<uint64_t>(slowPathDataStreamStart - reinterpret_cast<uint8_t*>(bcb));
        ctl.m_bytecodeStream = bytecodeStream;

#ifndef NDEBUG
        ctl.m_actualJitFastPathEnd = nullptr;
        ctl.m_actualJitSlowPathEnd = nullptr;
        ctl.m_actualJitDataSecEnd = nullptr;
        ctl.m_actualCondBrPatchesArrayEnd = nullptr;
        ctl.m_actualSlowPathDataEnd = nullptr;
        ctl.m_actualSlowPathDataIndexArrayEnd = nullptr;
        ctl.m_actualCodeBlock32End = 0;
        ctl.m_actualSlowPathDataOffsetEnd = 0;
        ctl.m_actualBytecodeStreamEnd = nullptr;
#endif

        deegen_baseline_jit_do_codegen_impl(&ctl);

        // In debug mode, assert that everything is as expected
        //
        assert(ctl.m_actualJitFastPathEnd == fastPathSecTrueEnd);
        assert(ctl.m_actualJitSlowPathEnd == slowPathSecTrueEnd);
        assert(ctl.m_actualJitDataSecEnd == dataSecPtr + dataSectionCodeLen);
        assert(ctl.m_actualCondBrPatchesArrayEnd == condBrLatePatchList + numLateCondBrPatches);
        assert(ctl.m_actualSlowPathDataEnd == slowPathDataStreamStart + slowPathDataStreamLen);
        assert(ctl.m_actualSlowPathDataIndexArrayEnd == slowPathDataIndexArray + numBytecodes);
        assert(ctl.m_actualCodeBlock32End == static_cast<uint64_t>(static_cast<uint32_t>(reinterpret_cast<uint64_t>(cb))));
        assert(ctl.m_actualSlowPathDataOffsetEnd = static_cast<uint64_t>(slowPathDataStreamStart + slowPathDataStreamLen - reinterpret_cast<uint8_t*>(bcb)));
        assert(ctl.m_actualBytecodeStreamEnd == bytecodeStreamEnd);
    }

    // Sanity check that the SlowPathDataIndex array makes sense
    //
#ifndef NDEBUG
    {
        assert(bcb->m_numBytecodes > 0);
        uint32_t bsBase = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(cb->GetBytecodeStream()));
        uint32_t spdBase = SafeIntegerCast<uint32_t>(reinterpret_cast<uintptr_t>(bcb->GetSlowPathDataStreamStart()) - reinterpret_cast<uintptr_t>(bcb));
        assert(bcb->m_sbIndex[0].m_bytecodePtr32 == bsBase);
        assert(bcb->m_sbIndex[0].m_slowPathDataOffset == spdBase);
        for (size_t i = 0; i + 1 < bcb->m_numBytecodes; i++)
        {
            {
                uint32_t curVal = bcb->m_sbIndex[i].m_bytecodePtr32 - bsBase;
                uint32_t nextVal = bcb->m_sbIndex[i + 1].m_bytecodePtr32 - bsBase;
                assert(curVal < cb->m_bytecodeLength);
                assert(nextVal < cb->m_bytecodeLength);
                assert(curVal < nextVal);
            }
            {
                uint32_t curVal = bcb->m_sbIndex[i].m_slowPathDataOffset;
                uint32_t nextVal = bcb->m_sbIndex[i + 1].m_slowPathDataOffset;
                assert(spdBase <= curVal && curVal < spdBase + bcb->m_slowPathDataStreamLength);
                assert(spdBase <= nextVal && nextVal < spdBase + bcb->m_slowPathDataStreamLength);
                assert(curVal < nextVal);
            }
        }
    }
#endif

    // Patch the late-patch conditional branch destinations based on LateCondBrPatches
    // Note that the records from the same bytecode come sequentially, which usually have the same dstBytecode
    // so we can cache the most recent lookup result to speed up things a bit.
    //
    if (numLateCondBrPatches > 0)
    {
        uint32_t cachedDstBytecodePtr32 = condBrLatePatchList[0].m_dstBytecodePtrLow32bits - 1;
        size_t cachedBytecodeIndexLookupResult = 0;
        for (size_t i = 0; i < numLateCondBrPatches; i++)
        {
            BaselineJitCondBrLatePatchRecord& rec = condBrLatePatchList[i];
            uint32_t dstBytecodePtr32 = rec.m_dstBytecodePtrLow32bits;
            size_t bytecodeIndex;
            if (dstBytecodePtr32 == cachedDstBytecodePtr32)
            {
                bytecodeIndex = cachedBytecodeIndexLookupResult;
            }
            else
            {
                bytecodeIndex = bcb->GetBytecodeIndexFromBytecodePtrLower32Bits(dstBytecodePtr32);
                cachedDstBytecodePtr32 = dstBytecodePtr32;
                cachedBytecodeIndexLookupResult = bytecodeIndex;
            }

            assert(bytecodeIndex < bcb->m_numBytecodes);
            assert(bcb->m_sbIndex[bytecodeIndex].m_bytecodePtr32 == dstBytecodePtr32);
            uint8_t* slowPathDataStruct = bcb->GetSlowPathDataAtBytecodeIndex(bytecodeIndex);

            // Currently the slowPathData always start with the opcode, followed immediately by the jitAddr for this bytecode
            //
            uint32_t jitAddr = UnalignedLoad<uint32_t>(slowPathDataStruct + sizeof(BytecodeOpcodeTy));
            rec.Patch(jitAddr, static_cast<uint32_t>(bytecodeIndex));
        }
    }

    // There is a 'x_maxBytesCodegenFnMayOverwrite' byte gap between fast path and slow path
    // Populate ud2 + N * nop for sanity and to avoid breaking debugger disassembler.
    //
    // And also do the same at the end of slow path, so that the full [jitCodeEntry, jitRegionEnd) recorded
    // in BaselineCodeBlock is filled with disassemblable instructions
    //
    {
        auto populateCodeGap = [](uint8_t* buf) ALWAYS_INLINE
        {
            static_assert(x_maxBytesCodegenFnMayOverwrite >= 2 /*length of ud2 instruction*/);
            // ud2: 0x0f, 0x0b
            //
            buf[0] = 0x0f;
            buf[1] = 0x0b;
            // nop: 0x90
            //
            for (size_t i = 2; i < x_maxBytesCodegenFnMayOverwrite; i++)
            {
                buf[i] = 0x90;
            }
        };

        populateCodeGap(fastPathSecTrueEnd);
        populateCodeGap(slowPathSecTrueEnd);
    }

    // Update best entry point from interpreter code to baseline JIT code
    //
    assert(cb->m_bestEntryPoint == cb->m_owner->GetInterpreterEntryPoint());
    cb->UpdateBestEntryPoint(bcb->m_jitCodeEntry);
    assert(cb->m_bestEntryPoint == bcb->m_jitCodeEntry);
    return bcb;
}

JitGenericInlineCacheEntry* WARN_UNUSED JitGenericInlineCacheEntry::Create(VM* vm,
                                                                           SpdsPtr<JitGenericInlineCacheEntry> nextNode,
                                                                           uint16_t icTraitKind)
{
    JitGenericInlineCacheEntry* entry = vm->AllocateFromSpdsRegionUninitialized<JitGenericInlineCacheEntry>();
    ConstructInPlace(entry);
    entry->m_nextNode = nextNode;
    entry->m_traitKind = icTraitKind;
    uint8_t allocationStepping = deegen_baseline_jit_generic_ic_jit_allocation_stepping_table[icTraitKind];
    entry->m_jitRegionLengthStepping = allocationStepping;
    entry->m_jitAddr = vm->GetJITMemoryAlloc()->AllocateGivenStepping(allocationStepping);
    return entry;
}

void* WARN_UNUSED JitGenericInlineCacheSite::Insert(uint16_t traitKind)
{
    assert(m_numEntries < x_maxJitGenericInlineCacheEntries);
    VM* vm = VM::GetActiveVMForCurrentThread();
    JitGenericInlineCacheEntry* entry = JitGenericInlineCacheEntry::Create(vm, TCGet(m_linkedListHead), traitKind);
    TCSet(m_linkedListHead, SpdsPtr<JitGenericInlineCacheEntry> { entry });
    m_numEntries++;
    return entry->m_jitAddr;
}

BaselineCodeBlockAndEntryPoint NO_INLINE WARN_UNUSED deegen_prepare_tier_up_into_baseline_jit(HeapPtr<CodeBlock> cbHeapPtr)
{
    CodeBlock* cb = TranslateToRawPointer(cbHeapPtr);
    BaselineCodeBlock* bcb = deegen_baseline_jit_do_codegen(cb);
    return {
        .baselineCodeBlock = bcb,
        .entryPoint = bcb->m_jitCodeEntry
    };
}

BaselineCodeBlockAndEntryPoint NO_INLINE WARN_UNUSED deegen_prepare_osr_entry_into_baseline_jit(CodeBlock* cb, void* curBytecode)
{
    BaselineCodeBlock* bcb;
    if (cb->m_baselineCodeBlock != nullptr)
    {
        // It is possible that at this moment the baseline JIT code has already been generated,
        // e.g., function F calls itself, the call triggers the codegen, so the callee F executed in baseline JIT,
        // but the caller F is still in interpreter mode after the call returns. The caller F will trigger
        // an OSR entry and reach here the next time it executes a bytecode that qualifies for OSR entry,
        // at which time F is already compiled.
        //
        bcb = cb->m_baselineCodeBlock;
    }
    else
    {
        bcb = deegen_baseline_jit_do_codegen(cb);
    }

    size_t bytecodeIndex = bcb->GetBytecodeIndexFromBytecodePtr(curBytecode);
    uint8_t* slowPathDataStruct = bcb->GetSlowPathDataAtBytecodeIndex(bytecodeIndex);

    // Currently the slowPathData always start with the opcode, followed immediately by the jitAddr for this bytecode
    //
    uint32_t jitAddr = UnalignedLoad<uint32_t>(slowPathDataStruct + sizeof(BytecodeOpcodeTy));

    return {
        .baselineCodeBlock = bcb,
        .entryPoint = reinterpret_cast<void*>(static_cast<uint64_t>(jitAddr))
    };
}
