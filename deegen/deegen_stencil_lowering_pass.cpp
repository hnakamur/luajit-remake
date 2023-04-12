#include "deegen_stencil_lowering_pass.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopInfo.h"

namespace dast {

// A piece of magic asm that identifies a slow path.
//     12 * 'hlt'
//     mov rax, 0xdee2333
//     mov rax, 0x114514
//     2 * 'ud2, hlt'
//
// The length of the string is made a multiple of 32 to avoid break anything with code alignment.
//
static std::string WARN_UNUSED GetSlowPathMagicLLVMAsmString()
{
    std::string res = "";
    for (size_t i = 0; i < 12; i++)
    {
        res += "hlt; ";
    }
    res += "movq $$0xdee2333, %rax; movq $$0x114514, %rax; ";
    for (size_t i = 0; i < 2; i++)
    {
        res += "ud2; hlt; ";
    }
    return res;
}

// Similar to above, but used for the "move to fallthrough" hint
//
static std::string WARN_UNUSED GetMoveToFallthoughMagicLLVMAsmString()
{
    std::string res = "";
    for (size_t i = 0; i < 12; i++)
    {
        res += "hlt; ";
    }
    res += "movq $$0x1357986, %rax; movq $$0x32123456, %rax; ";
    for (size_t i = 0; i < 2; i++)
    {
        res += "ud2; hlt; ";
    }
    return res;
}

// Similar to above, but used as the final hot-cold splitting barrier in the final output
// Note that the machine code length of this asm string must be >128 bytes, so that all jumps across the barrier
// take imm32 operands, not imm8 operands.
//
// Also, this step is directly emitted as assembly, so this is not LLVM inline asm syntax, but normal AT&T asm syntax
//
static std::string WARN_UNUSED GetHotColdBarrierMagicATTAsmString()
{
    std::string res = "";
    for (size_t i = 0; i < 35; i++)
    {
        res += "\thlt\n";
    }
    res += "\tmovq\t$0x1919810, %rax\n";
    res += "\tmovq\t$0x1337abcd, %rax\n";
    for (size_t i = 0; i < 37; i++)
    {
        res += "\tud2\n";
        res += "\thlt\n";
    }
    return res;
}

// Locate the above hot-cold barrier in the machine code so we can split it there
//
size_t WARN_UNUSED DeegenStencilLoweringPass::LocateHotColdSplittingBarrier(const std::string& machineCode)
{
    std::vector<uint8_t> pattern;
    for (size_t i = 0; i < 35; i++)
    {
        pattern.push_back(0xf4);
    }
    pattern.insert(pattern.end(), { 0x48, 0xc7, 0xc0, 0x10, 0x98, 0x91, 0x01, 0x48, 0xc7, 0xc0, 0xcd, 0xab, 0x37, 0x13 });
    for (size_t i = 0; i < 37; i++)
    {
        pattern.push_back(0x0f);
        pattern.push_back(0x0b);
        pattern.push_back(0xf4);
    }
    ReleaseAssert(pattern.size() == x_hotColdSplittingBarrierSize);

    auto check = [&](size_t offset) WARN_UNUSED -> bool
    {
        ReleaseAssert(offset + x_hotColdSplittingBarrierSize <= machineCode.length());
        const void* p1 = machineCode.c_str() + offset;
        const void* p2 = pattern.data();
        return (memcmp(p1, p2, x_hotColdSplittingBarrierSize) == 0);
    };

    size_t res = static_cast<size_t>(-1);
    for (size_t offset = 0; offset + x_hotColdSplittingBarrierSize <= machineCode.length(); offset++)
    {
        if (check(offset))
        {
            ReleaseAssert(res == static_cast<size_t>(-1));
            res = offset;
        }
    }
    ReleaseAssert(res != static_cast<size_t>(-1));
    return res;
}

static void EmitSlowPathMagicLLVMAsm(llvm::Instruction* insertBefore)
{
    using namespace llvm;
    LLVMContext& ctx = insertBefore->getContext();
    InlineAsm* ia = InlineAsm::get(FunctionType::get(llvm_type_of<void>(ctx), false /*isVarArg*/),
                                   GetSlowPathMagicLLVMAsmString(),
                                   "" /*constraints*/,
                                   true /*hasSideEffects*/);
    CallInst::Create(ia, "", insertBefore);
}

static void EmitMoveToFallthroughMagicLLVMAsm(llvm::Instruction* insertBefore)
{
    using namespace llvm;
    LLVMContext& ctx = insertBefore->getContext();
    InlineAsm* ia = InlineAsm::get(FunctionType::get(llvm_type_of<void>(ctx), false /*isVarArg*/),
                                   GetMoveToFallthoughMagicLLVMAsmString(),
                                   "" /*constraints*/,
                                   true /*hasSideEffects*/);
    CallInst::Create(ia, "", insertBefore);
}

// A makeshift tool to parse the assembly generated by LLVM
//
struct AsmLine
{
    std::vector<std::string> m_components;
    std::vector<size_t> m_nonWhiteSpaceIdx;

    std::string WARN_UNUSED ToString()
    {
        std::string res = "";
        for (const std::string& comp : m_components) { res += comp; }
        res += "\n";
        return res;
    }

    size_t WARN_UNUSED NumWords() { return m_nonWhiteSpaceIdx.size(); }

    std::string& WARN_UNUSED GetWord(size_t ord)
    {
        ReleaseAssert(ord < m_nonWhiteSpaceIdx.size());
        return m_components[m_nonWhiteSpaceIdx[ord]];
    }

    bool WARN_UNUSED IsLocalLabel()
    {
        if (NumWords() != 1) { return false; }
        return GetWord(0).starts_with(".") && GetWord(0).ends_with(":");
    }

    std::string WARN_UNUSED ParseLabel()
    {
        ReleaseAssert(IsLocalLabel());
        return GetWord(0).substr(0, GetWord(0).length() - 1);
    }

    bool WARN_UNUSED IsCommentOrEmptyLine()
    {
        if (NumWords() == 0) { return true; }
        return GetWord(0).starts_with("#");
    }

    bool WARN_UNUSED IsInstruction()
    {
        if (NumWords() == 0) { return false; }
        if (GetWord(0).starts_with(".") || GetWord(0).starts_with("#") || GetWord(0).ends_with(":")) { return false; }
        return true;
    }

    static AsmLine WARN_UNUSED Parse(const std::string& line)
    {
        AsmLine res;
        if (line.length() == 0)
        {
            return res;
        }

        bool isSpace = std::isspace(line[0]);
        std::string curStr = "";
        for (size_t i = 0; i < line.size(); i++)
        {
            bool b = std::isspace(line[i]);
            if (b != isSpace)
            {
                res.m_components.push_back(curStr);
                if (!isSpace) { res.m_nonWhiteSpaceIdx.push_back(res.m_components.size() - 1); }
                curStr = line[i];
                isSpace = b;
            }
            else
            {
                curStr += line[i];
            }
        }
        res.m_components.push_back(curStr);
        if (!isSpace) { res.m_nonWhiteSpaceIdx.push_back(res.m_components.size() - 1); }
        return res;
    }
};

static std::vector<AsmLine> WARN_UNUSED ReadAssemblyFile(std::string fileContents)
{
    std::vector<AsmLine> res;
    std::stringstream ss(fileContents);
    std::string line;
    while (std::getline(ss, line))
    {
        res.push_back(AsmLine::Parse(line));
    }
    return res;
}

static bool IsMagicAsmPattern(std::vector<AsmLine>& file, size_t lineNumber, const std::string& val1, const std::string& val2)
{
    if (lineNumber + 18 > file.size()) { return false; }
    for (size_t i = 0; i < 12; i++)
    {
        AsmLine& line = file[lineNumber + i];
        if (line.NumWords() != 1) { return false; }
        if (line.GetWord(0) != "hlt") { return false; }
    }

    if (file[lineNumber + 12].NumWords() != 3) { return false; }
    if (file[lineNumber + 12].GetWord(0) != "movq") { return false; }
    if (file[lineNumber + 12].GetWord(1) != val1) { return false; }
    if (file[lineNumber + 12].GetWord(2) != "%rax") { return false; }

    if (file[lineNumber + 13].NumWords() != 3) { return false; }
    if (file[lineNumber + 13].GetWord(0) != "movq") { return false; }
    if (file[lineNumber + 13].GetWord(1) != val2) { return false; }
    if (file[lineNumber + 13].GetWord(2) != "%rax") { return false; }

    if (file[lineNumber + 14].NumWords() != 1) { return false; }
    if (file[lineNumber + 14].GetWord(0) != "ud2") { return false; }
    if (file[lineNumber + 15].NumWords() != 1) { return false; }
    if (file[lineNumber + 15].GetWord(0) != "hlt") { return false; }
    if (file[lineNumber + 16].NumWords() != 1) { return false; }
    if (file[lineNumber + 16].GetWord(0) != "ud2") { return false; }
    if (file[lineNumber + 17].NumWords() != 1) { return false; }
    if (file[lineNumber + 17].GetWord(0) != "hlt") { return false; }

    return true;
}

static bool IsSlowPathHintMagicAsmPattern(std::vector<AsmLine>& file, size_t lineNumber)
{
    return IsMagicAsmPattern(file, lineNumber, "$233710387," /*0xdee2333*/, "$1131796," /*0x114514*/);
}

static bool IsMoveToFallthroughHintMagicAsmPattern(std::vector<AsmLine>& file, size_t lineNumber)
{
    return IsMagicAsmPattern(file, lineNumber, "$20281734," /*0x1357986*/, "$840053846," /*0x32123456*/);
}

static bool LabelCanBeReachedByFallthrough(std::vector<AsmLine>& file, size_t line)
{
    ReleaseAssert(line < file.size());
    ReleaseAssert(file[line].IsLocalLabel());
    if (line == 0) { return true; }
    line--;
    while (true)
    {
        if (file[line].IsLocalLabel())
        {
            // For some reason there are two labels in a row?
            //
            ReleaseAssert(false);
        }
        if (file[line].IsInstruction())
        {
            std::string opcode = file[line].GetWord(0);
            if (opcode == "jmp" || opcode == "jmpq" || opcode == "ud2")
            {
                // The preceding instruction is a barrier (for now, only consider unconditional jump and ud2 for simplicity)
                // so cannot fallthrough
                //
                return false;
            }
            else
            {
                return true;
            }
        }
        if (line == 0) { return true; }
        line--;
    }
}

// Remove all the slow path magic asm, return the separation point of the fast path and
// the slow path (i.e., the first line of the slow path part)
//
// We do this by parsing the assembly code generated by LLVM... ugly, but simpliest to write...
//
static size_t WARN_UNUSED RewriteSlowPathHint(std::vector<AsmLine>& file /*inout*/)
{
    // Find the first occurance of the magic asm
    //
    size_t line = 0;
    while (line < file.size())
    {
        if (IsSlowPathHintMagicAsmPattern(file, line)) { break; }
        line++;
    }
    if (line == file.size())
    {
        // No slow path part, return the end of the function
        //
        return file.size();
    }

    // Find the first label preceding the magic asm, this is the splitting point between the fast and slow path
    //
    while (line > 0)
    {
        if (file[line].IsLocalLabel()) { break; }
        line--;
    }

    // The function that remove all the occurances of magic asm and update 'file'
    //
    auto removeMagicAsm = [&]()
    {
        std::vector<AsmLine> newFile;
        {
            size_t k = 0;
            while (k < file.size())
            {
                if (IsSlowPathHintMagicAsmPattern(file, k))
                {
                    k += 18;
                }
                else
                {
                    newFile.push_back(file[k]);
                    k++;
                }
            }
            ReleaseAssert(k == file.size());
        }

        file = newFile;
    };

    if (!file[line].IsLocalLabel())
    {
        // This is unexpected, but I do see this happen (in debug mode).
        // We might want to do better, but think about it later.
        //
        removeMagicAsm();
        return file.size();
    }

    std::string label = file[line].ParseLabel();

    // Determine if the first instruction preceding the magic asm is a barrier
    // If not, we must manually add a jump, since the fallthrough can no longer happen after we split apart the fast and code path
    //
    bool needToAddJump = LabelCanBeReachedByFallthrough(file, line);
    if (needToAddJump)
    {
        // Add a 'jmp label' right before line 'label'
        //
        file.insert(file.begin() + static_cast<int64_t>(line), AsmLine());
        file[line] = AsmLine::Parse(std::string("\tjmp\t") + label);
        line++;
    }

    removeMagicAsm();

    return line;
}

// A very crude pass that attempts to reorder code so that the fast path can fallthrough to
// the next bytecode, thus saving a jmp instruction
//
// It first find the desired jmp to move by looking at the magic asm pattern injected earlier.
// From that jmp, it searches backward until it finds a JCC or JMP instruction. Then:
//
// For JCC instruction: if it is jumping backward, ignore. Otherwise:
//  /- jcc ...
//  |  XXXXX
//  |  jmp next_bytecode
//  |  YYYYY
//  \->ZZZZZ
//
// If YYYYY fallthroughs to ZZZZZ, ignore. Otherwise we can rewrite it to:
//  /- jncc ...
//  |  ZZZZZ
//  |  YYYYY
//  \->XXXXX
//     jmp next_bytecode
//
// For JMP instruction:
//     jmp ...
//     XXXXX
//     jmp next_bytecode
//     YYYYY
//
// We can simply rewrite it to:
//     jmp ...
//     YYYYY
//     XXXXX
//     jmp next_bytecode
//
// Note that one common pattern is
//  /- jcc ...
//  |  jmp ... -|
//  \->XXXXX    |
//     jmp next |
//     YYYYY  <-|
// After the rewrite the jmp-to-YYYYY would be unnecessary, and we will remove it for better code
//
static void RewriteMoveToFallthroughHint(const std::string& fnName, std::vector<AsmLine>& file /*inout*/)
{
    // Find the first occurance of the magic asm
    //
    size_t line = 0;
    while (line < file.size())
    {
        if (IsMoveToFallthroughHintMagicAsmPattern(file, line)) { break; }
        line++;
    }
    if (line == file.size())
    {
        // Didn't find anything, no work to do
        //
        return;
    }

    // Scan down for the 'jmp next_bytecode' instruction
    //
    size_t jmpNextBc = line;
    while (true)
    {
        ReleaseAssert(jmpNextBc < file.size());
        if (file[jmpNextBc].IsInstruction())
        {
            std::string opcode = file[jmpNextBc].GetWord(0);
            if (opcode == "jmp")
            {
                break;
            }
        }
        jmpNextBc++;
    }

    if (jmpNextBc == file.size() - 1)
    {
        // Already last line, nothing to do
        //
        return;
    }

    // Collect all the labels
    //
    std::unordered_map<std::string /*label*/, size_t /*line*/> labelMap;
    for (size_t i = 0; i < file.size(); i++)
    {
        if (file[i].IsLocalLabel())
        {
            std::string label = file[i].ParseLabel();
            ReleaseAssert(!labelMap.count(label));
            labelMap[label] = i;
        }
    }

    // Scan back for JCC or barriers
    //
    while (true)
    {
        if (file[line].IsInstruction())
        {
            std::string opcode = file[line].GetWord(0);
            if (opcode == "jmp" || opcode == "jmpq" || opcode == "ud2")
            {
                // This is a barrier, we can do rewrite
                //
                //     jmp ...                       jmp ...
                //     XXXXX                ===>     YYYYY
                //     jmp next_bytecode             XXXXX
                //     YYYYY                         jmp next_bytecode
                //
                {
                    std::vector<AsmLine> newFile;
                    for (size_t i = 0; i <= line; i++)
                    {
                        newFile.push_back(file[i]);
                    }
                    for (size_t i = jmpNextBc + 1; i < file.size(); i++)
                    {
                        newFile.push_back(file[i]);
                    }
                    for (size_t i = line + 1; i <= jmpNextBc; i++)
                    {
                        newFile.push_back(file[i]);
                    }
                    ReleaseAssert(newFile.size() == file.size());
                    file = newFile;
                }

                // Attempt to eliminate unnecessary jmps
                //
                std::unordered_set<size_t> linesToRemove;
                for (size_t i = 0; i < file.size(); i++)
                {
                    if (file[i].IsInstruction() && file[i].NumWords() == 2 && file[i].GetWord(0).starts_with("j"))  // is this good enough?
                    {
                        size_t j = i + 1;
                        while (j < file.size() && file[j].IsCommentOrEmptyLine())
                        {
                            j++;
                        }
                        if (j >= file.size())
                        {
                            continue;
                        }
                        if (file[j].IsLocalLabel())
                        {
                            std::string lb = file[j].ParseLabel();
                            if (file[i].GetWord(1) == lb)
                            {
                                linesToRemove.insert(i);
                            }
                        }
                    }
                }

                {
                    std::vector<AsmLine> newFile;
                    for (size_t i = 0; i < file.size(); i++)
                    {
                        if (!linesToRemove.count(i))
                        {
                            newFile.push_back(file[i]);
                        }
                    }
                    file = newFile;
                }

                return;
            }
            else if (opcode.starts_with("j"))  // is this good enough?
            {
                // This is a JCC, we can attempt to rewrite
                //
                ReleaseAssert(file[line].NumWords() == 2);
                std::string dstLabel = file[line].GetWord(1);
                // If the label doesn't exist, it might be a symbol or a label in slow path, don't bother
                //
                if (labelMap.count(dstLabel))
                {
                    size_t definedLine = labelMap[dstLabel];
                    if (definedLine > jmpNextBc)
                    {
                        // We can do the rewrite if 'definedLine' cannot be reached by fallthrough
                        //
                        if (!LabelCanBeReachedByFallthrough(file, definedLine))
                        {
                            //  /- jcc ...                jncc   -|
                            //  |  XXXXX                  ZZZZZ   |
                            //  |  jmp next_bc    ===>    YYYYY   |
                            //  |  YYYYY                  XXXXX <--
                            //  \->ZZZZZ                  jmp next_bc
                            //
                            if (opcode.starts_with("jn"))
                            {
                                opcode = "j" + opcode.substr(2);
                            }
                            else
                            {
                                opcode = std::string("jn") + opcode.substr(1);
                            }
                            file[line].GetWord(0) = opcode;
                            file[line].GetWord(1) = ".Ldgtmp_0";
                            std::vector<AsmLine> newFile;
                            for (size_t i = 0; i <= line; i++)
                            {
                                newFile.push_back(file[i]);
                            }
                            for (size_t i = definedLine; i < file.size(); i++)
                            {
                                newFile.push_back(file[i]);
                            }
                            for (size_t i = jmpNextBc + 1; i < definedLine; i++)
                            {
                                newFile.push_back(file[i]);
                            }
                            newFile.push_back(AsmLine::Parse(".Ldgtmp_0:"));
                            for (size_t i = line + 1; i <= jmpNextBc; i++)
                            {
                                newFile.push_back(file[i]);
                            }
                            ReleaseAssert(newFile.size() == file.size() + 1);
                            file = newFile;
                            return;
                        }
                    }
                }
            }
        }

        if (line == 0)
        {
            fprintf(stderr, "[NOTE] Failed to rewrite JIT stencil to eliminate jmp to fallthrough (function name = %s).\n", fnName.c_str());
            return;
        }
        line--;
    }
}

static void CleanupMoveToFallthroughHint(std::vector<AsmLine>& file /*inout*/)
{
    std::vector<AsmLine> newFile;
    size_t k = 0;
    while (k < file.size())
    {
        if (IsMoveToFallthroughHintMagicAsmPattern(file, k))
        {
            k += 18;
        }
        else
        {
            newFile.push_back(file[k]);
            k++;
        }
    }
    ReleaseAssert(k == file.size());
    file = newFile;
}

// Generate the post-processing ASM output for compilation to machine code
// The fast-and-slow-path separation barrier is inserted
//
std::string WARN_UNUSED GeneratePostProcessingAsmOutput(std::vector<AsmLine>& fastpath, std::vector<AsmLine>& slowpath)
{
    std::string output;
    for (size_t i = 0; i < fastpath.size(); i++)
    {
        output += fastpath[i].ToString();
    }
    output += GetHotColdBarrierMagicATTAsmString();
    for (size_t i = 0; i < slowpath.size(); i++)
    {
        output += slowpath[i].ToString();
    }
    return output;
}

void DeegenStencilLoweringPass::RunIrRewritePhase(llvm::Function* f, const std::string& fallthroughPlaceholderName)
{
    using namespace llvm;
    DominatorTree dom(*f);
    LoopInfo li(dom);
    BranchProbabilityInfo bpi(*f, li);
    BlockFrequencyInfo bfi(*f, bpi, li);
    uint64_t entryFreq = bfi.getEntryFreq();
    ReleaseAssert(entryFreq != 0);

    auto getBBFreq = [&](BasicBlock* bb) WARN_UNUSED -> double
    {
        uint64_t bbFreq = bfi.getBlockFreq(bb).getFrequency();
        return static_cast<double>(bbFreq) / static_cast<double>(entryFreq);
    };

    // Figure out all the cold basic blocks
    //
    std::vector<BasicBlock*> coldBBs;
    for (BasicBlock& bb : *f)
    {
        if (bb.empty())
        {
            continue;
        }
        double bbFreq = getBBFreq(&bb);
        if (bbFreq < 0.02)
        {
            coldBBs.push_back(&bb);
        }
    }

    // For each cold BB, test if it is reachable from the entry without encountering any other cold BB
    // If so, we need to put a magic asm annotation, so that it can be detected in generated code and split it out
    //
    std::vector<BasicBlock*> bbForAsmAnnotation;
    for (BasicBlock* bb : coldBBs)
    {
        SmallPtrSet<BasicBlock*, 8> otherColdBBs;
        for (BasicBlock* o : coldBBs)
        {
            if (o != bb)
            {
                otherColdBBs.insert(o);
            }
        }
        if (isPotentiallyReachable(f->getEntryBlock().getFirstNonPHI(), bb->getFirstNonPHI(), &otherColdBBs, &dom, &li))
        {
            bbForAsmAnnotation.push_back(bb);
        }
    }

    // If requested, find the most frequent BB that fallthroughs to the next bytecode,
    // and annotate it with the move-to-fallthrough hint.
    //
    if (fallthroughPlaceholderName != "")
    {
        double maxFreq = -1;
        BasicBlock* selectedBB = nullptr;
        for (BasicBlock& bb : *f)
        {
            if (bb.empty()) { continue; }
            CallInst* ci = bb.getTerminatingMustTailCall();
            if (ci != nullptr)
            {
                // Must not use 'getCalledFunction', because our callee is forcefully casted from a non-function symbol!
                //
                Value* callee = ci->getCalledOperand();
                if (callee != nullptr && isa<GlobalVariable>(callee) && cast<GlobalVariable>(callee)->getName() == fallthroughPlaceholderName)
                {
                    double bbFreq = getBBFreq(&bb);
                    if (bbFreq > maxFreq)
                    {
                        maxFreq = bbFreq;
                        selectedBB = &bb;
                    }
                }
            }
        }
        if (selectedBB != nullptr && maxFreq >= 0.02)
        {
            CallInst* ci = selectedBB->getTerminatingMustTailCall();
            ReleaseAssert(ci != nullptr);
            EmitMoveToFallthroughMagicLLVMAsm(ci /*insertBefore*/);
        }
        else
        {
            /*
            fprintf(stderr, "[NOTE] Jmp-to-fallthrough rewrite is requested, but %s! Code generation will continue without this rewrite.\n",
                    (selectedBB == nullptr ? "no dispatch to the next bytecode can be found" : "the dispatch to next bytecode is in the slow path"));
            */
        }
    }

    for (BasicBlock* bb : bbForAsmAnnotation)
    {
        EmitSlowPathMagicLLVMAsm(bb->getFirstNonPHI());
    }

    ValidateLLVMFunction(f);
}

std::string WARN_UNUSED DeegenStencilLoweringPass::RunAsmRewritePhase(const std::string& asmFile, const std::string& funcName)
{
    std::vector<AsmLine> file = ReadAssemblyFile(asmFile);
    // Find the end of the function
    //
    size_t functionEndLine = static_cast<size_t>(-1);
    for (size_t i = 0; i < file.size(); i++)
    {
        if (file[i].IsLocalLabel())
        {
            std::string label = file[i].ParseLabel();
            if (label.starts_with(".Lfunc_end"))
            {
                ReleaseAssert(functionEndLine == static_cast<size_t>(-1));
                functionEndLine = i;
            }
        }
    }
    ReleaseAssert(functionEndLine != static_cast<size_t>(-1));

    std::vector<AsmLine> fileFooter;
    for (size_t i = functionEndLine; i < file.size(); i++)
    {
        fileFooter.push_back(file[i]);
    }
    file.resize(functionEndLine);

    size_t separationLine = RewriteSlowPathHint(file /*inout*/);

    std::vector<AsmLine> fastpath, slowpath;
    for (size_t i = 0; i < separationLine; i++)
    {
        fastpath.push_back(file[i]);
    }
    for (size_t i = separationLine; i < file.size(); i++)
    {
        slowpath.push_back(file[i]);
    }

    RewriteMoveToFallthroughHint(funcName, fastpath /*inout*/);
    CleanupMoveToFallthroughHint(fastpath /*inout*/);
    CleanupMoveToFallthroughHint(slowpath /*inout*/);

    // std::string ppAsm = GeneratePostProcessingAsmOutput(fastpath, slowpath);
    // return ppAsm;

    std::string output;
    for (size_t i = 0; i < fastpath.size(); i++)
    {
        output += fastpath[i].ToString();
    }

    output += "\t.section\t.text.deegen_slow,\"ax\",@progbits\n";

    for (size_t i = 0; i < slowpath.size(); i++)
    {
        output += slowpath[i].ToString();
    }

    output += "\t.section\t.text,\"ax\",@progbits\n";

    for (size_t i = 0; i < fileFooter.size(); i++)
    {
        output += fileFooter[i].ToString();
    }

    return output;
}

}   // namespace dast
