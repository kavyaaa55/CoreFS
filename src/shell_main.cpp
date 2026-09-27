#include "shell.h"
#include <iostream>
#include <stdexcept>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: simplefs_shell <image_path>\n";
        return 1;
    }

    try {
        Shell shell(argv[1]);
        shell.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
