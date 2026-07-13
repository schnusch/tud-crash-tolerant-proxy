#include "main_passive.h"

int main(int argc, char **argv) {
    __attribute__((musttail))
    return main_passive(argc, argv);
}
