#include <iostream>
#include <memory>
#include <signal.h>
#include "synpro.h"
using namespace std;

void term(int sig)
{

}

int main()
{
    
    std::shared_ptr<Synpro> synpro = std::make_shared<Synpro>();
    synpro->init();
    synpro->bind_listen(9999);
    synpro->start();
}