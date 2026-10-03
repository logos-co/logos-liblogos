// bin/logos_runtime: the runtime in a process of its own, which an app spawns.
#include "logos_core.h"

int main(int argc, char* argv[])
{
    return logos_runtime_host_main(argc, argv);
}
