#include "drampool_daemon.h"

int main(int argc, char** argv)
{
    UC::DRAMPOOL::DramPoolDaemon daemon;
    return daemon.Run(argc, argv);
}
