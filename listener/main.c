#include "main_passive.h"

int main(int argc, char **argv) {
#ifdef __has_attribute
#if __has_attribute(musttail)
    __attribute__((musttail))
#endif
#endif
    return main_passive(argc, argv);
}
