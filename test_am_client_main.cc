#include <ucp/api/ucp.h>
#include <string.h>
#include <stdlib.h> // Include the <stdlib.h> header file to define the `free` function
#include <log.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

#include <ucp_init_resource.h>

ucp_worker_h g_worker;
ucp_ep_h g_ep;

const char *am_msg_str = "active message";
int *tempbuffer;
std::vector<int*> buffer_ptrs;


int main(int argc, char **argv){
    ucp_context_h context;
    ucp_listener_h listener;

    tempbuffer = (int*)malloc(64 * sizeof(int));

    for(int i = 0; i < 64; i++){
        tempbuffer[i] = i;
    }

    if(init_context(&context) != 0){
        log_error("Failed to initialize context");
        return -1;
    }

    if(init_worker(context, &g_worker) != 0){
        log_error("Failed to initialize worker");
        return -1;
    }

    // 检查命令行参数数量
    if(argc < 2){
        log_error("No IP address provided");
        return -1;
    }

    // 第一个命令行参数作为 IP 地址
    char *ip_address = argv[1];

    if(init_endpoint_ip(g_worker, ip_address, 13337, &g_ep) != 0){
        log_error("Failed to initialize endpoint");
        return -1;
    }

    am_send_block(g_worker, g_ep, tempbuffer, 64 * sizeof(int));
}
