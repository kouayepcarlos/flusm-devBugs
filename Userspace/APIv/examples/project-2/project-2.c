#include "../../include/usm/usm.h"


int main(int argc, char* argv[]) {
    usm_parse_args(argv, argc);
    usm_launch(&dev_usm_ops);
}

