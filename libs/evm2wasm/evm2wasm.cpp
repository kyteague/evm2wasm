#include <cmath>

#include <set>
#include <string>

#include <fmt/format.h>

#include <wasm-binary.h>
#include <wasm-s-parser.h>
#include <wasm-validator.h>

#include "evm2wasm.h"
#include "wast-async.h"
#include "wast.h"

using namespace std;

using namespace fmt::literals;

namespace evm2wasm
{
string wast2wasm(const string& input, bool debug)
{
    wasm::Module module;

    try
    {
        if (debug)
            std::cerr << "s-parsing..." << std::endl;
        // FIXME: binaryen 1.37.28 actually modifies the input...
        //        as a workaround make a copy here
        string tmp = input;
        wasm::SExpressionParser parser(const_cast<char*>(tmp.c_str()));
        wasm::Element& root = *parser.root;
        if (debug)
            std::cerr << "w-parsing..." << std::endl;
        wasm::SExpressionWasmBuilder builder(module, *root[0]);
    }
    catch (wasm::ParseException& p)
    {
        if (debug)
        {
            std::cerr << "error in parsing input" << std::endl;
            p.dump(std::cerr);
        }
        return string();
    }

    if (!wasm::WasmValidator().validate(module))
    {
        if (debug)
            std::cerr << "module is invalid" << std::endl;
        return string();
    }

    if (debug)
        std::cerr << "binarification..." << std::endl;
    wasm::BufferWithRandomAccess buffer(debug);
    wasm::WasmBinaryWriter writer(&module, buffer, debug);
    writer.write();

    if (debug)
        std::cerr << "writing to output..." << std::endl;

    ostringstream output;
    buffer.writeTo(output);

    if (debug)
        std::cerr << "Done." << std::endl;

    return output.str();
}

string evm2wast(const string& evmCode, bool stackTrace, bool useAsyncAPI, bool inlineOps)
{
    // FIXME: do evm magic here
    // this keep track of the opcode we have found so far. This will be used to
    // to figure out what .wast files to include
    std::set<opcodeEnum> opcodesUsed;
    std::set<opcodeEnum> ignoredOps = {opcodeEnum::JUMP, opcodeEnum::JUMPI, opcodeEnum::JUMPDEST,
        opcodeEnum::POP, opcodeEnum::STOP, opcodeEnum::INVALID};
    std::vector<std::string> callbackTable;

    // an array of found segments
    std::vector<JumpSegment> jumpSegments;

    // the transcompiled EVM code
    fmt::MemoryWriter wast;
    fmt::MemoryWriter segment;
    //
    // keeps track of the gas that each section uses
    int gasCount = 0;

    // used for pruning dead code
    bool jumpFound = false;

    // the accumulative stack difference for the current segment
    int segmentStackDelta = 0;
    int segmentStackHigh = 0;
    int segmentStackLow = 0;

    // adds stack height checks to the beginning of a segment
    auto addStackCheck = [&segment, &segmentStackHigh, &segmentStackLow, &segmentStackDelta]() {
        fmt::MemoryWriter check;
        if (segmentStackHigh != 0)
        {
            check << "(if (i32.gt_s (get_global $sp) (i32.const {check}))\n\
                        (then (unreachable)))"_format("check"_a = ((1023 - segmentStackHigh) * 32));
        }

        if (segmentStackLow != 0)
        {
            check << "(if (i32.gt_s (get_global $sp) (i32.const {check}))\n\
                        (then (unreachable)))"_format("check"_a = (-segmentStackLow * 32 - 32));
        }

        check << segment.str();
        segment = std::move(check);
        segmentStackHigh = 0;
        segmentStackLow = 0;
        segmentStackDelta = 0;
    };

    // add a metering statment at the beginning of a segment
    auto addMetering = [&segment, &wast, &gasCount]() {
        wast << "(call $useGas (i64.const {gasCount}))"_format("gasCount"_a = gasCount)
             << segment.str();
        segment.clear();
        gasCount = 0;
    };

    // finishes off a segment
    auto endSegment = [&segment, &addStackCheck, &addMetering]() {
        segment << ")";
        addStackCheck();
        addMetering();
    };

    for (size_t pc = 0; pc < evmCode.length(); pc++)
    {
        auto opint = evmCode[pc];
        auto op = opcodes(opint);

        std::string bytes;
        gasCount += op.fee;

        segmentStackDelta += op.on;
        if (segmentStackDelta > segmentStackHigh)
        {
            segmentStackHigh = segmentStackDelta;
        }

        segmentStackDelta -= op.off;
        if (segmentStackDelta < segmentStackLow)
        {
            segmentStackLow = segmentStackDelta;
        }

        switch (op.name)
        {
        case opcodeEnum::JUMP:
            jumpFound = true;
            segment << "\
                ;; jump\n\
                   (set_local $jump_dest (call $check_overflow \n\
                                          (i64.load (get_global $sp))\n\
                                          (i64.load (i32.add (get_global $sp) (i32.const 8)))\n\
                                          (i64.load (i32.add (get_global $sp) (i32.const 16)))\n\
                                          (i64.load (i32.add (get_global $sp) (i32.const 24)))))\n\
                   (set_global $sp (i32.sub (get_global $sp) (i32.const 32)))\n\
                       (br $loop)";
            opcodesUsed.insert(opcodeEnum::check_overflow);
            pc = findNextJumpDest(evmCode, pc);
            break;
        case opcodeEnum::JUMPI:
            jumpFound = true;
            segment << "set_local $jump_dest (call $check_overflow \n\
                          (i64.load (get_global $sp))\n\
                          (i64.load (i32.add (get_global $sp) (i32.const 8)))\n\
                          (i64.load (i32.add (get_global $sp) (i32.const 16)))\n\
                          (i64.load (i32.add (get_global $sp) (i32.const 24)))))\n\n\
                         (set_global $sp (i32.sub (get_global $sp) (i32.const 64)))\n\
                         (br_if $loop (i32.eqz (i64.eqz (i64.or\n\
                           (i64.load (i32.add (get_global $sp) (i32.const 32)))\n\
                           (i64.or\n\
                             (i64.load (i32.add (get_global $sp) (i32.const 40)))\n\
                             (i64.or\n\
                               (i64.load (i32.add (get_global $sp) (i32.const 48)))\n\
                               (i64.load (i32.add (get_global $sp) (i32.const 56)))\n\
                             )\
                           )\
                        ))))\n";
            opcodesUsed.insert(opcodeEnum::check_overflow);
            addStackCheck();
            addMetering();
            break;
        case opcodeEnum::JUMPDEST:
            endSegment();
            jumpSegments.push_back({number : pc, type : "jump_dest"});
            gasCount = 1;
            break;
        case opcodeEnum::GAS:
            segment << "(call $GAS)\n";
            addMetering();
            break;
        case opcodeEnum::LOG:
            segment << "(call $LOG (i32.const " << op.number << "))\n";
            break;
        case opcodeEnum::DUP:
        case opcodeEnum::SWAP:
            // adds the number on the stack to SWAP
            segment << "(call ${opname} (i32.const {opnumber}))\n"_format(
                "opname"_a = opcodeToString(op.name), "opnumber"_a = (op.number - 1));
            break;
        case opcodeEnum::PC:
            segment << "(call $PC (i32.const {pc}))\n"_format("pc"_a = pc);
            break;
        case opcodeEnum::PUSH:
        {
            pc++;
            size_t sliceSize = std::min(op.number, 32ul);
            bytes = evmCode.substr(pc, pc + sliceSize);
            pc += op.number;
            if (op.number < 32)
            {
                bytes.insert(bytes.begin(), 32 - op.number, '0');
            }
            auto bytesRounded = ceil(op.number / 8);
            fmt::MemoryWriter push;
            int q = 0;
            // pad the remaining of the word with 0
            for (; q < 4 - bytesRounded; q++)
            {
                fmt::MemoryWriter pad;
                pad << "(i64.const 0)";
                pad << push.str();
                push = std::move(pad);
            }

            for (; q < 4; q++)
            {
                auto int64 = reinterpret_cast<int64_t>(bytes.substr(q * 8, q * 8 + 8).c_str());
                push << "(i64.const {int64})"_format("int64"_a = int64);
            }

            segment << fmt::format("(call $PUSH {push})", "push"_a = push.str());
            pc--;
            break;
        }
        case opcodeEnum::POP:
            // do nothing
            break;
        case opcodeEnum::STOP:
            segment << "(br $done)";
            if (jumpFound)
            {
                pc = findNextJumpDest(evmCode, pc);
            }
            else
            {
                // the rest is dead code;
                pc = evmCode.length();
            }
            break;
        case opcodeEnum::SELFDESTRUCT:
        case opcodeEnum::RETURN:
            segment << "(call $" << opcodeToString(op.name) << ") (br $done)\n";
            if (jumpFound)
            {
                pc = findNextJumpDest(evmCode, pc);
            }
            else
            {
                // the rest is dead code
                pc = evmCode.length();
            }
            break;
        case opcodeEnum::INVALID:
            segment.clear();
            segment << "(unreachable)";
            pc = findNextJumpDest(evmCode, pc);
            break;

        default:
            if (useAsyncAPI && callbackFuncs.find(op.name) != callbackFuncs.end())
            {
                std::string cbFunc = (*callbackFuncs.find(op.name)).second;
                auto result = std::find(std::begin(callbackTable), std::end(callbackTable), cbFunc);
                size_t index = result - std::begin(callbackTable);
                if (result == std::end(callbackTable))
                {
                    callbackTable.push_back(cbFunc);
                    index = callbackFuncs.size();
                }
                segment << "(call $${op.name} (i32.const {index}))\n"_format("index"_a = index);
            }
            else
            {
                // use synchronous API
                segment << "(call ${opname})\n"_format("opname"_a = opcodeToString(op.name));
            }
            break;
        }

        if (ignoredOps.find(op.name) == std::end(ignoredOps))
        {
            opcodesUsed.insert(op.name);
        }

        auto stackDelta = op.on - op.off;
        // update the stack pointer
        if (stackDelta != 0)
        {
            segment << "(set_global $sp(i32.add(get_global $sp)(i32.const {stackDelta})))\n"_format(
                "stackDelta"_a = stackDelta * 32);
        }

        // creates a stack trace
        if (stackTrace)
        {
            segment << "(call $stackTrace(i32.const {pc})(i32.const {opint})( \
                        i32.const {gasCount})(get_global $sp))\n"_format(
                "pc"_a = pc, "opint"_a = opint, "gasCount"_a = gasCount);
        }

        // adds the logic to save the stack pointer before exiting to wiat to for a callback
        // note, this must be done before the sp is updated above^
        if (useAsyncAPI && callbackFuncs.find(op.name) != std::end(callbackFuncs))
        {
            segment << "(set_global $cb_dest (i32.const {jumpSegmentsLength})) \
                            (br $done))"_format("jumpSegmentsLength"_a = jumpSegments.size() + 1);
            jumpSegments.push_back({number : 0, type : "cb_dest"});
        }
    }

    endSegment();

    std::string wastStr = wast.str();
    wast.clear();
    wast << assembleSegments(jumpSegments) << wastStr << "))";

    auto wastFiles = wastSyncInterface;  // default to synchronous interface
    if (useAsyncAPI)
    {
        wastFiles = wastAsyncInterface;
    }

    std::vector<std::string> imports;
    std::vector<std::string> funcs;
    // inline EVM opcode implemention
    if (inlineOps)
    {
        std::tie(funcs, imports) = resolveFunctions(opcodesUsed, wastFiles);
    }

    // import stack trace function
    if (stackTrace)
    {
        imports.push_back("(import \"debug\" \"printMemHex\" (func $printMem (param i32 i32)))");
        imports.push_back("(import \"debug\" \"print\" (func $print (param i32)))");
        imports.push_back(
            "(import \"debug\" \"evmTrace\" (func $stackTrace (param i32 i32 i32 i32)))");
    }
    imports.push_back("(import \"ethereum\" \"useGas\" (func $useGas (param i64)))");

    wastStr = wast.str();
    funcs.push_back(wastStr);
    wastStr = buildModule(funcs, imports, callbackTable);
    return wastStr;
}

// given an array for segments builds a wasm module from those segments
// @param {Array} segments
// @return {String}
std::string assembleSegments(const std::vector<JumpSegment>& segments)
{
    auto wasm = buildJumpMap(segments);

    for (size_t index = 0; index < segments.size(); ++index)
    {
        wasm = "(block ${index} {wasm}"_format("index"_a = index + 1, "wasm"_a = wasm);
    }

    std::string result =
        "\
  (func $main\
    (export \"main\")\
    (local $jump_dest i32)\
    (set_local $jump_dest (i32.const -1))\
\
    (block $done\
      (loop $loop\
        {wasm}"_format("wasm"_a = wasm);
    return result;
}

string evm2wasm(const string& input, bool tracing)
{
    return wast2wasm(evm2wast(input, tracing));
}

std::string opcodeToString(opcodeEnum opcode)
{
    switch (opcode)
    {
    case opcodeEnum::STOP:
        return "STOP";
    case opcodeEnum::ADD:
        return "ADD";
    case opcodeEnum::MUL:
        return "MUL";
    case opcodeEnum::SUB:
        return "SUB";
    case opcodeEnum::DIV:
        return "DIV";
    case opcodeEnum::SDIV:
        return "SDIV";
    case opcodeEnum::MOD:
        return "MOD";
    case opcodeEnum::SMOD:
        return "SMOD";
    case opcodeEnum::ADDMOD:
        return "ADDMOD";
    case opcodeEnum::MULMOD:
        return "MULMOD";
    case opcodeEnum::EXP:
        return "EXP";
    case opcodeEnum::SIGNEXTEND:
        return "SIGNEXTEND";
    case opcodeEnum::LT:
        return "LT";
    case opcodeEnum::GT:
        return "GT";
    case opcodeEnum::SLT:
        return "SLT";
    case opcodeEnum::SGT:
        return "SGT";
    case opcodeEnum::EQ:
        return "EQ";
    case opcodeEnum::ISZERO:
        return "ISZERO";
    case opcodeEnum::AND:
        return "AND";
    case opcodeEnum::OR:
        return "OR";
    case opcodeEnum::XOR:
        return "XOR";
    case opcodeEnum::NOT:
        return "NOT";
    case opcodeEnum::BYTE:
        return "BYTE";
    case opcodeEnum::SHA3:
        return "SHA3";
    case opcodeEnum::ADDRESS:
        return "ADDRESS";
    case opcodeEnum::BALANCE:
        return "BALANCE";
    case opcodeEnum::ORIGIN:
        return "ORIGIN";
    case opcodeEnum::CALLER:
        return "CALLER";
    case opcodeEnum::CALLVALUE:
        return "CALLVALUE";
    case opcodeEnum::CALLDATALOAD:
        return "CALLDATALOAD";
    case opcodeEnum::CALLDATASIZE:
        return "CALLDATASIZE";
    case opcodeEnum::CALLDATACOPY:
        return "CALLDATACOPY";
    case opcodeEnum::CODESIZE:
        return "CODESIZE";
    case opcodeEnum::CODECOPY:
        return "CODECOPY";
    case opcodeEnum::GASPRICE:
        return "GASPRICE";
    case opcodeEnum::EXTCODESIZE:
        return "EXTCODESIZE";
    case opcodeEnum::EXTCODECOPY:
        return "EXTCODECOPY";
    case opcodeEnum::BLOCKHASH:
        return "BLOCKHASH";
    case opcodeEnum::COINBASE:
        return "COINBASE";
    case opcodeEnum::TIMESTAMP:
        return "TIMESTAMP";
    case opcodeEnum::NUMBER:
        return "NUMBER";
    case opcodeEnum::DIFFICULTY:
        return "DIFFICULTY";
    case opcodeEnum::GASLIMIT:
        return "GASLIMIT";
    case opcodeEnum::POP:
        return "POP";
    case opcodeEnum::MLOAD:
        return "MLOAD";
    case opcodeEnum::MSTORE:
        return "MSTORE";
    case opcodeEnum::MSTORE8:
        return "MSTORE8";
    case opcodeEnum::SLOAD:
        return "SLOAD";
    case opcodeEnum::SSTORE:
        return "SSTORE";
    case opcodeEnum::JUMP:
        return "JUMP";
    case opcodeEnum::JUMPI:
        return "JUMPI";
    case opcodeEnum::PC:
        return "PC";
    case opcodeEnum::MSIZE:
        return "MSIZE";
    case opcodeEnum::GAS:
        return "GAS";
    case opcodeEnum::JUMPDEST:
        return "JUMPDEST";
    case opcodeEnum::PUSH:
        return "PUSH";
    case opcodeEnum::DUP:
        return "DUP";
    case opcodeEnum::SWAP:
        return "SWAP";
    case opcodeEnum::LOG:
        return "LOG";
    case opcodeEnum::CREATE:
        return "CREATE";
    case opcodeEnum::CALL:
        return "CALL";
    case opcodeEnum::CALLCODE:
        return "CALLCODE";
    case opcodeEnum::RETURN:
        return "RETURN";
    case opcodeEnum::DELEGATECALL:
        return "DELEGATECALL";
    case opcodeEnum::SELFDESTRUCT:
        return "SELFDESTRUCT";
    default:
        abort();
    }
}

Op opcodes(int op)
{
    auto result = codes.find(op);
    std::tuple<opcodeEnum, int, int, int> code;
    if (result == std::end(codes))
    {
        code = std::make_tuple(opcodeEnum::INVALID, 0, 0, 0);
    }
    else
    {
        code = (*result).second;
    };
    auto opcode = std::get<0>(code);
    unsigned int number;

    switch (opcode)
    {
    case opcodeEnum::LOG:
        number = op - 0xa0;
        break;

    case opcodeEnum::PUSH:
        number = op - 0x5f;
        break;

    case opcodeEnum::DUP:
        number = op - 0x7f;
        break;

    case opcodeEnum::SWAP:
        number = op - 0x8f;
        break;

    default:
        number = -1;
    }

    return {opcode, std::get<1>(code), std::get<2>(code), std::get<3>(code), number};
}

// returns the index of the next jump destination opcode in given EVM code in an
// array and a starting index
// @param {Array} evmCode
// @param {Integer} index
// @return {Integer}
size_t findNextJumpDest(const std::string& evmCode, size_t i)
{
    for (; i < evmCode.length(); i++)
    {
        auto opint = evmCode[i];
        auto op = opcodes(opint);
        switch (op.name)
        {
        case opcodeEnum::PUSH:
            // skip add how many bytes where pushed
            i += op.number;
            break;
        case opcodeEnum::JUMPDEST:
            return --i;
        default:
            break;
        }
    }
    return --i;
}

// Ensure that dependencies are only imported once (use the Set)
// @param {Set} funcSet a set of wasm function that need to be linked to their dependencies
// @return {Set}
std::set<opcodeEnum> resolveFunctionDeps(const std::set<opcodeEnum>& funcSet)
{
    std::set<opcodeEnum> funcs = funcSet;
    for (auto&& func : funcSet)
    {
        auto deps = depMap.find(func);
        if (deps != depMap.end())
        {
            for (auto&& dep : (*deps).second)
            {
                funcs.insert(dep);
            }
        }
    }
    return funcs;
}

/**
 * given a Set of wasm function this return an array for wasm equivalents
 * @param {Set} funcSet
 * @return {Array}
 */
std::tuple<std::vector<std::string>, std::vector<std::string>> resolveFunctions(
    const std::set<opcodeEnum>& funcSet, std::map<opcodeEnum, WastCode> wastFiles)
{
    std::vector<std::string> funcs;
    std::vector<std::string> imports;
    for (auto&& func : resolveFunctionDeps(funcSet))
    {
        funcs.push_back(wastFiles[func].wast);
        imports.push_back(wastFiles[func].imports);
    }
    return std::tuple<std::vector<std::string>, std::vector<std::string>>{funcs, imports};
}

/**
 * builds a wasm module
 * @param {Array} funcs the function to include in the module
 * @param {Array} imports the imports for the module's import table
 * @return {string}
 */
std::string buildModule(const std::vector<std::string>& funcs,
    const std::vector<std::string>& imports, const std::vector<std::string>& callbacks)
{
    fmt::MemoryWriter funcBuf;
    for (auto&& func : funcs)
    {
        funcBuf << func;
    }

    fmt::MemoryWriter callbackTableBuf;
    if (callbacks.size() > 0)
    {
        fmt::MemoryWriter callbacksBuf;
        callbacksBuf.write(callbacks[0]);
        for (size_t i = 1; i < callbacks.size(); ++i)
        {
            callbacksBuf << " " << callbacks[i];
        }
        callbackTableBuf << "\
    (table\
      (export \"callback\") ;; name of table\
        anyfunc\
        (elem {callbacksStr}) ;; elements will have indexes in order\
      )"_format("callbacksStr"_a = callbacksBuf.str());
    }

    fmt::MemoryWriter importsBuf;
    if (imports.size() > 0)
    {
        importsBuf << imports[0];
        for (size_t i = 1; i < imports.size(); ++i)
        {
            importsBuf << "\n" << imports[i];
        }
    }

    return "\
(module\
  {importsStr}\
  (global $cb_dest (mut i32) (i32.const 0))\
  (global $sp (mut i32) (i32.const -32))\
  (global $init (mut i32) (i32.const 0))\
\
  ;; memory related global\
  (global $memstart i32  (i32.const 33832))\
  ;; the number of 256 words stored in memory\
  (global $wordCount (mut i64) (i64.const 0))\
  ;; what was charged for the last memory allocation\
  (global $prevMemCost (mut i64) (i64.const 0))\
\
  ;; TODO: memory should only be 1, but can\'t resize right now\
  (memory 500)\
  (export \"memory\" (memory 0))\
\
  {callbackTableStr}\
\
  {funcStr}\
)"_format("importsStr"_a = importsBuf.str(), "callbackTableStr"_a = callbackTableBuf.str(),
        "funcStr"_a = funcBuf.str());
}

// Builds the Jump map, which maps EVM jump location to a block label
// @param {Array} segments
// @return {String}
std::string buildJumpMap(const std::vector<JumpSegment>& segments)
{
    fmt::MemoryWriter wasmBuf;
    wasmBuf << "(unreachable)";

    fmt::MemoryWriter brTableBuf;
    for (size_t index = 0; index < segments.size(); ++index)
    {
        auto&& seg = segments[index];
        brTableBuf << " $" << (index + 1);
        if (seg.type == "jump_dest")
        {
            std::string wasmStr = wasmBuf.str();
            wasmBuf.clear();
            wasmBuf << "(if (i32.eq (get_local $jump_dest) (i32.const {segnumber}))\
                (then (br {index}))\
                (else {wasm}))"_format(
                "segnumber"_a = seg.number, "index"_a = index + 1, "wasm"_a = wasmBuf.str());
        }
    }

    std::string wasmStr = wasmBuf.str();
    wasmBuf.clear();
    wasmBuf << "\
  (block $0\
    (if\
      (i32.eqz (get_global $init))\
      (then\
        (set_global $init (i32.const 1))\
        (br $0))\
      (else\
        ;; the callback dest can never be in the first block\
        (if (i32.eq (get_global $cb_dest) (i32.const 0)) \
          (then\
            {wasm}\
          )\
          (else \
            ;; return callback destination and zero out $cb_dest \
            get_global $cb_dest\
            (set_global $cb_dest (i32.const 0))\
            (br_table $0 {brTable})\
          )))))"_format("wasm"_a = wasmStr, "brTable"_a = brTableBuf.str());

    return wasmBuf.str();
}
};
