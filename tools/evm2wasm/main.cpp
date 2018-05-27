#include <string>
#include <iostream>
#include <fstream>
#include <streambuf>

#include <evm2wasm.h>

using namespace std;


int main(int argc, char **argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <EVM file> [--wast]" << endl;
        return 1;
    }

    bool wast = false;
    if (argc == 3) {
        wast = (string(argv[2]) == "--wast");
        if(!wast) {
            cerr << "Usage: " << argv[0] << " <EVM file> [--wast]" << endl;
            return 1;
        }
    }

    ifstream input(argv[1]);
    if (!input.is_open()) {
        cerr << "File not found: " << argv[1] << endl;
        return 1;
    }

    string str(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>()
    );

    std::vector<uint8_t> byteCode;
    for (auto c : str) {
      byteCode.push_back(static_cast<uint8_t>(c));
    }

    if (wast) {
        cout << evm2wasm::evm2wast(byteCode) << endl;
    } else {
        cout << evm2wasm::evm2wasm(byteCode) << endl;
    }

    return 0;
}
