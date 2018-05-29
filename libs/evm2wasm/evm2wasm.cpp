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

string evm2wast(const string& input, bool tracing) {
  (void)input;
  (void)tracing;
  // FIXME: do evm magic here
  for(int i = 0; i< input.length(); ++i) {
    auto op = opcodes.find(input[i]);
    // std::cout << std::get<0>(*op) << std::endl;
  };
  return "(module (export \"main\" (func $main)) (func $main))";
}

string evm2wasm(const string& input, bool tracing) {
  return wast2wasm(evm2wast(input, tracing));
}

}
