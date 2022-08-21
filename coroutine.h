#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>
 
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include <functional>
 
#include <map>
#include <list>
 
#define MAX_ROUTINE         1024
#define MAX_STACK_SIZE_KB   128
#define MAX_EVENT_SIZE      10240
enum { UNUSED = 0, IDLE = 1, RUNNING = 2 };
 
typedef void (*STD_ROUTINE_FUNC)(void);
typedef std::function<void()> FUNC_CB;
 
typedef struct {
    ucontext_t          ctx;
    char                stack[ MAX_STACK_SIZE_KB * 1024 ];
    FUNC_CB    func;
    int                 status;
    int                 wait_fd;
    int                 events;
} RoutineContext;
 
class RoutineCenter{
    //time : rid
    typedef std::map<unsigned int, std::list<int> > TimeoutMap;
public:
    void init() {
        srand(time(NULL));
        this->running_id = -1;
    }
    
    void routine_wrap() {
        int running_id = this->running_id;
        std::cout << "running_id " << running_id << std::endl;
        if ( running_id < 0 ) 
        {
            puts("current context don't attach to any routine except main.");
            return;
        }
        this->routines[running_id].func();
        if(this->routines[running_id].wait_fd != -1) 
        {
            std::cout << "客户端下线：" << this->routines[running_id].wait_fd << std::endl;
            mod_event(this->routines[running_id].wait_fd, EPOLLIN, EPOLL_CTL_DEL, running_id);
            ::close(this->routines[running_id].wait_fd);
        }
        this->routines[running_id].wait_fd = -1;    
        this->routines[running_id].status = UNUSED;
        this->routine_cnt--;
    }
    
    int create( FUNC_CB routine_proc) {
        int new_id = -1;
        for (int i = 0; i < MAX_ROUTINE; i++) {
            if ( this->routines[i].status == UNUSED ) {
                new_id = i;
                break;
            }
        }
    
        if ( new_id < 0 ) {
            puts("max routine number reached. no more routine.");
            return -1;
        }
    
        ucontext_t* pctx = &(this->routines[new_id].ctx);
        getcontext(pctx);
    
        pctx->uc_stack.ss_sp    = this->routines[new_id].stack;
        pctx->uc_stack.ss_size  = MAX_STACK_SIZE_KB * 1024;
        pctx->uc_stack.ss_flags = 0;
        pctx->uc_link           = &(this->main);
        STD_ROUTINE_FUNC fun = reinterpret_cast<STD_ROUTINE_FUNC>(&RoutineCenter::routine_wrap);
        makecontext(pctx, fun, 1 ,this);
    
        this->routines[new_id].status   = IDLE;
        this->routines[new_id].func     = routine_proc;
        this->routines[new_id].wait_fd  = -1;
        this->routine_cnt++;
        return new_id;
    }
    
    int yield() {
        if ( this->running_id < 0 ) {
            puts("no routine running except main.");
            return 0;
        }
        int running_id          = this->running_id;
        RoutineContext* info    = &(this->routines[running_id]);
        info->status            = IDLE;
        info->events            = 0;
        swapcontext( &(info->ctx), &(this->main) );
        return 0;   
    }
    
    int resume(int id, int events = 0) {
        if ( id < 0 || id >= MAX_ROUTINE ) {
            puts("routine id out of bound.");
            return -1;
        }
        int running_id          = this->running_id;
        if (id == running_id) {
            puts("current routine is running already.");
            return 0;
        }
        if (this->routines[id].status != IDLE) {
            puts("target routine is not in idel status. can't resume");
            return -1;
        }
    
        this->running_id            = id;
        this->routines[id].status   = RUNNING;
        this->routines[id].events   = events;
        if (running_id < 0) {
            // in main
            std::cout << "in main " << this->running_id    <<  std::endl;
            swapcontext( &(this->main), &(this->routines[id].ctx));
            this->running_id = -1;
        } else {
            // in other routine
            std::cout << "in other routine" << std::endl;
            this->routines[running_id].status = IDLE;
            swapcontext( &(this->routines[running_id].ctx), &(this->routines[id].ctx) );
            this->running_id = running_id;
        }
        return 0;
    }
    
    int routine_id() { return this->running_id; }
    
    void set_nonblocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        int ret = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (ret < 0) {
            perror("set nonblocking fail.");
            exit(-1);
        }
    }
    
    void mod_event(int fd, int events, int op, int routine_id) {
        struct epoll_event ev = {0};
        if ( EPOLL_CTL_DEL != op ) {
            ev.data.fd= routine_id;
            this->routines[routine_id].wait_fd = fd;
        }
        ev.events = events;
        epoll_ctl(this->epoll_fd, op, fd, &ev);
    }
    
int routine_read(int fd, char* buff, int size) {
    mod_event(fd, EPOLLIN, EPOLL_CTL_MOD, routine_id());
    while (!(this->routines[routine_id()].events & EPOLLIN)) {
        yield();    
    }
    while (this->routines[routine_id()].events & EPOLLIN) {
        int need    = size;
        int readin  = 0;
        while (need > 0) {
            int ret = read(fd, buff + readin, need);
            if (ret < 0) {
                if(errno == EINTR ||errno == EAGAIN ||errno == EWOULDBLOCK)
                     break;
                else return 0;
            } 
            else if(ret == 0)
            {
                return 0;
            }
            else {
                readin += ret;
                need -= ret;
            }
        }
        mod_event(fd, EPOLLOUT, EPOLL_CTL_MOD, routine_id());
        return readin;
    } 
    printf("routine[%d][%s]routine system ran out of order.\n", routine_id(), __func__);
    return 0;
}
 
int routine_write(int fd, char* buff, int size) {
    //mod_event(fd, EPOLLOUT, EPOLL_CTL_ADD, routine_id());
    while (!(this->routines[routine_id()].events & EPOLLOUT)) {
        yield();    
    }
    while (this->routines[routine_id()].events & EPOLLOUT) {
        int need    = size;
        int wrout   = 0;
        while (need > 0) {
            int ret = write(fd, buff + wrout, need);
            if (ret < 0) {
                if(errno == EINTR ||errno == EAGAIN ||errno == EWOULDBLOCK)
                     break;
                else return 0;
            } 
            else if(ret == 0)
            {
                return 0;
            }
            else {
                wrout += ret;
                need -= ret;
            }
        }
        mod_event(fd, EPOLLIN, EPOLL_CTL_MOD, routine_id());
        return wrout;
    }
    printf("routine[%d][%s]routine system ran out of order.\n", routine_id(), __func__);
    return 0;
}
    
    void routine_delay_resume(int rid, int delay_sec) {
        if (delay_sec <= 0) {
            resume(rid);
            return;
        }
        this->timeout_map[time(NULL) + delay_sec].push_back(rid);
    }
    
    void routine_sleep(int time_sec) {
        routine_delay_resume(routine_id(), time_sec);
        yield();
    }
    
    int routine_nearest_timeout() {
        if (this->timeout_map.empty()) {
            return 6000; // default epoll timeout
        }
        unsigned int now    = time(NULL);
        int diff            = this->timeout_map.begin()->first - now;
        return diff < 0 ? 0 : diff * 1000;
    }
    
    void routine_resume_timeout() {
        // printf("[epoll] process timeout\n");
        if ( this->timeout_map.empty() ) {
            return;
        }
        unsigned int timestamp      = this->timeout_map.begin()->first;
    
        if (timestamp > time(NULL)) {
            return;
        }
    
        std::list<int>& routine_ids = this->timeout_map.begin()->second;
        for (int i : routine_ids) {
            resume(i);
        }
        this->timeout_map.erase(timestamp);
    }
    
    void routine_resume_event(int n) {
        // printf("[epoll] process event\n");
        for (int i = 0; i < n; i++) {
            int rid = this->events[i].data.fd;
            resume(rid, this->events[i].events);
        }
    }
    
    void create_routine_poll() {
        this->epoll_fd = epoll_create1 (0); 
    
        if (this->epoll_fd == -1) {
            perror ("epoll_create");
            exit(-1);
        }
    }
    
    void routine_poll() {
    
        for (;;) {
            int n = epoll_wait (this->epoll_fd, this->events, MAX_EVENT_SIZE, routine_nearest_timeout());
            // printf("[epoll] event_num:%d\n", n);
            routine_resume_timeout();
            routine_resume_event(n);
        }
    }
    
    void echo_server_routine() {
        int conn_fd = this->routines[routine_id()].wait_fd;
        
        printf("routine[%d][%s] server start. conn_fd: %d\n", routine_id(), __func__, conn_fd);
    
        for (;;) {
            printf("routine[%d][%s] loop start. conn_fd: %d\n", routine_id(), __func__, conn_fd);
            char buf[512] = {0};
            int n       = 0;
            n = routine_read( conn_fd, buf, sizeof (buf) );
            if (n < 0) 
            {
                perror("server read error.");
                break;
            }
            if (n == 0) 
            {
                break;
            }
            buf[n] = '\0';
            printf("routine[%d][%s] server read: %s\n", routine_id(), __func__, buf);
            unsigned int in_ts = time(NULL);
            routine_sleep(1);
            unsigned int out_ts= time(NULL);
            
            char obuf[512] = {0};
            snprintf(obuf, sizeof(obuf), "%s rev_ts:%u sent_ts:%u\n", buf, in_ts, out_ts);
            printf("routine[%d][%s] server write: %s", routine_id(), __func__, obuf);
    
            n = routine_write(conn_fd, obuf, strlen(obuf) + 1);
            if (n < 0) {
                perror("server write error.");
                break;
            }
            if (n == 0) 
            {
                break;
            }
            
        }
        printf("routine[%d][%s] server start. conn_fd: %d\n", routine_id(), __func__, conn_fd);
    }
    
    void request_accept() {
        for (;;) {
            struct sockaddr_in addr     = {0};
            socklen_t           slen    = sizeof(addr);
            int fd = accept(this->routines[routine_id()].wait_fd, (struct sockaddr*)&addr, &slen);
            struct sockaddr_in peer = {0};
            int ret = getpeername(fd, (struct sockaddr*)&peer, &slen);
            if (ret < 0) {
                perror("getpeername error.");
                exit(-1);
            }
            printf("routine[%d][%s] accept from %s conn_fd:%d\n", routine_id(), __func__, inet_ntoa(peer.sin_addr), fd);
            set_nonblocking(fd);
            int rid = create(std::bind(&RoutineCenter::echo_server_routine,this) );
            this->routines[rid].wait_fd = fd;
            mod_event(fd, EPOLLIN, EPOLL_CTL_ADD, rid);
    
            //resume(rid);
    
            yield();
        }
    }
    
    void bind_listen(unsigned short port) {
        int listen_fd           = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr = {0};
        addr.sin_family         = AF_INET;
        addr.sin_port           = htons(port);
        addr.sin_addr.s_addr    = INADDR_ANY;
        int ret = bind( listen_fd, (struct sockaddr*)&addr, sizeof(struct sockaddr) );
        if (ret < 0) {
            perror("bind fail.");
            exit(-1);
        }
        ret = listen( listen_fd, 20 );
        if (ret < 0) {
            perror("listen fail.");
            exit(-1);
        }
        printf("routine[%d] listen bind at port: %u\n", routine_id(), port);
        set_nonblocking( listen_fd );
        int rid = create(std::bind(&RoutineCenter::request_accept,this));
        mod_event( listen_fd, EPOLLIN, EPOLL_CTL_ADD, rid );
    }
public:
    void close()
    {
        for(int i=0;i<MAX_ROUTINE;i++)
        {
            if(routines[i].wait_fd != -1)
            {
                ::close(routines[i].wait_fd);
                routines[i].wait_fd = -1;
            }   
        }

        for(auto it = timeout_map.begin();it != timeout_map.end();it++)
        {
            auto rid_ls = it->second;
            rid_ls.clear();
        }
        timeout_map.clear();
    }
private:
    struct epoll_event events[MAX_EVENT_SIZE];
    RoutineContext    routines[MAX_ROUTINE];
    TimeoutMap        timeout_map;
    ucontext_t        main;
    int               epoll_fd;
    int               running_id;
    int               routine_cnt;
};
 