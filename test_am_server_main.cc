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


int main(){
    ucp_context_h context;
    ucp_listener_h listener;

    tempbuffer = (int*)malloc(64 * sizeof(int));

    for(int i = 0; i < 64; i++){
        tempbuffer[i] = 0;
    }

    if(init_context(&context) != 0){
        log_error("Failed to initialize context");
        return -1;
    }

    if(init_worker(context, &g_worker) != 0){
        log_error("Failed to initialize worker");
        return -1;
    }

    if(init_listener(g_worker, &listener, NULL) != 0){

    }

    register_am_recv_callback(g_worker);

    while(1){
        ucp_worker_progress(g_worker);
        if(tempbuffer[1] == 1){
            for(int i = 0; i < 64; i++){
                log_info("tempbuffer[%d] = %d", i, tempbuffer[i]);
            }
            break;
        }
    }

    return 0;
}
