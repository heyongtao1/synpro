#ifndef _SYNPRO_H
#define _SYNPRO_H

#include "coroutine.h"
#include <memory>
#include <iostream>
class Synpro{
public:
    void init()
    {
        routinecenter = std::make_shared<RoutineCenter>();
        routinecenter->init();
        routinecenter->create_routine_poll();
    }
    void bind_listen(unsigned short port)
    {
        routinecenter->bind_listen(port);
    }
    void create(FUNC_CB fun, int delay_sec)
    {
        routinecenter->routine_delay_resume(routinecenter->create(fun),delay_sec);
    }
    void start()
    {
        routinecenter->routine_poll();
    }
public:
    ~Synpro()
    {
        routinecenter->close();
    }
private:
    std::shared_ptr<RoutineCenter> routinecenter;
};

#endif