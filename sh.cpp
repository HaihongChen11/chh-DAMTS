#include <iostream>
#include <thread>

//epoll

//主线程，等accpet，如果有就创建一个线程去执行处理客户端的操作。
int main()
{
    bind;
    listen;
    epoll_create();
    thread(epoll_handle).detach();
    while(1)
    {
        int client = accept(socket,10);
        epoll_ctl(client);
    }
}
//epoll vector,int fd socket
void epoll_handle()
{
    while(1)
    {
        int client = epoll_wait();
        thread(client_handle, client).detach();
    }
    
}

//子线程，不会阻塞主线程
void client_handle(int client)
{
    recv();
    return;
}