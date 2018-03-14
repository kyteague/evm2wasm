#include <evm2wasm.h>

#include <wasm-binary.h>
#include <wasm-s-parser.h>
#include <wasm-validator.h>

using namespace std;

namespace evm2wasm {

string wast2wasm(const string& input, bool debug) {
  wasm::Module module;

  try {
    if (debug) std::cerr << "s-parsing..." << std::endl;
    // FIXME: binaryen 1.37.28 actually modifies the input...
    //        as a workaround make a copy here
    string tmp = input;
    wasm::SExpressionParser parser(const_cast<char*>(tmp.c_str()));
    wasm::Element& root = *parser.root;
    if (debug) std::cerr << "w-parsing..." << std::endl;
    wasm::SExpressionWasmBuilder builder(module, *root[0]);
  } catch (wasm::ParseException& p) {
    if (debug) {
      std::cerr << "error in parsing input" << std::endl;
      p.dump(std::cerr);
    }
    return string();
  }

  if (!wasm::WasmValidator().validate(module)) {
    if (debug) std::cerr << "module is invalid" << std::endl;
    return string();
  }

  if (debug) std::cerr << "binarification..." << std::endl;
  wasm::BufferWithRandomAccess buffer(debug);
  wasm::WasmBinaryWriter writer(&module, buffer, debug);
  writer.write();

  if (debug) std::cerr << "writing to output..." << std::endl;

  ostringstream output;
  buffer.writeTo(output);

  if (debug) std::cerr << "Done." << std::endl;
  
  return output.str();
}

string evm2wast(const string& evmCode, bool tracing) {
  // FIXME: do evm magic here
  // this keep track of the opcode we have found so far. This will be used to
  // to figure out what .wast files to include
  std::set<std::string> opcodesUsed;
  std::set<std::string> ignoredOps = {"JUMP", "JUMPI", "JUMPDEST", "POP", "STOP", "INVALID"};

  // the transcompiled EVM code
  std::string wast;
  std::string segment;
  //
  // keeps track of the gas that each section uses
  int gasCount = 0;

  // used for pruning dead code
  bool jumpFound = false;

  // the accumulative stack difference for the current segment
  int segmentStackDeta = 0;
  int segmentStackHigh = 0;
  int segmentStackLow = 0;

  // adds stack height checks to the beginning of a segment
  auto addStackCheck = [&segmentStackHigh, &segmentStackLow, &segmentStackDeta]() {
    std::string check;
    if(segmentStackHigh != 0) {
      check = std::string("(if (i32.gt_s (get_global $sp) (i32.const ${(1023 - segmentStackHigh) * 32}))\n") +
              std::string("  (then (unreachable)))");
    }

    if (segmentStackLow != 0) {
      check += std::string("(if (i32.lt_s (get_global $sp) (i32.const ${-segmentStackLow * 32 - 32}))\n") +
                std::string("  (then (unreachable)))");
    }

    std::string segment = check + segment;
    segmentStackHigh = 0;
    segmentStackLow = 0;
    segmentStackDeta = 0;
  };

  // add a metering statment at the beginning of a segment
  auto addMetering = [&segment, &wast, &gasCount]() {
    wast += "(call $useGas (i64.const ${gasCount})) " + segment;
    segment = "";
    gasCount = 0;
  };

  // finishes off a segment
  auto endSegment = [&segment, &addStackCheck, &addMetering]() {
    segment += ")";
    addStackCheck();
    addMetering();
  };

  for (unsigned int pc = 0; pc < evmCode.length(); pc++) {
    auto result = opcodes.find(evmCode[pc]);
    std::tuple<opcodeEnum, int, int, int> t = (*result).second;
    opcodeEnum op = std::get<0>(t);
    std::string opname = opcodeToString(op);
    int baseCost = std::get<1>(t);
    int offStack = std::get<2>(t);
    int onStack = std::get<3>(t);
    std::cout << "OPNAME: " << opname << " BASE COST: " << baseCost
      << " OFF STACK: " << offStack << " ON STACK: " << onStack << std::endl;

    // TODO: fill in this table.
    switch(op) {
      case opcodeEnum::JUMP:
        break;
      default:
        break;
    }
  }
  return "(module (export \"main\" (func $main)) (func $main))";
}

string evm2wasm(const string& input, bool tracing) {
  return wast2wasm(evm2wast(input, tracing));
}

std::string opcodeToString(opcodeEnum opcode) {
  switch(opcode) {
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
          return "";
  }
};

}
