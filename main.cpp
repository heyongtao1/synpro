#include <iostream>
#include <memory>
#include <signal.h>
#include "synpro.h"
using namespace std;

std::shared_ptr<Synpro> synpro = std::make_shared<Synpro>();

void term(int sig)
{
    synpro->close();
}

int main()
{
    signal(SIGTERM,term);
    synpro->init();
    synpro->bind_listen(9999);
    synpro->start();
}