#include <ucp/api/ucp.h>
#include <string.h>
#include <stdlib.h> // Include the <stdlib.h> header file to define the `free` function
#include <log.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <chrono>

#include <ucp_init_resource.h>

ucp_worker_h g_worker;
ucp_ep_h g_ep;

const char *am_msg_str = "active message";

std::vector<int*> buffer_ptrs;
int receive_num = 0;
int request_num = 10;

int flag = 0;
int BUFFER_INT_SIZE = 1024 * 1024 * 32;



// 矩阵加法函数，返回结果矩阵
std::vector<std::vector<int>> addMatrices(const std::vector<std::vector<int>>& a, const std::vector<std::vector<int>>& b) {
    int rows = a.size();
    int cols = a[0].size();
    std::vector<std::vector<int>> result(rows, std::vector<int>(cols));

    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            result[i][j] = a[i][j] + b[i][j];
        }
    }

    return result;
}

// 执行矩阵加法并测量时间的函数

std::chrono::duration<double> performMatrixAddition() {
    // 初始化矩阵大小
    int size = 1000; // 1000x1000 矩阵
    std::vector<std::vector<int>> mat1(size, std::vector<int>(size, 1));
    std::vector<std::vector<int>> mat2(size, std::vector<int>(size, 2));

    // 开始计时
    auto start = std::chrono::high_resolution_clock::now();

    // 执行矩阵加法
    for(int i = 0; i < 100; i++){
        std::vector<std::vector<int>> result = addMatrices(mat1, mat2);
    }
    // 结束计时
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    log_info("Time taken to add two 1000x1000 matrices: %f seconds", elapsed.count());
    return elapsed;
}


void *wait_allreduce(void *arg){
    while(receive_num < request_num){
        ucp_worker_progress(g_worker);
    }
    return NULL;
}




int main(int argc, char **argv){
    ucp_context_h context;
    ucp_listener_h listener;

    std::chrono::duration<double> pure_net_time;
    std::chrono::duration<double> total_time;
    

    // for(int i = 0; i < request_num; i++){
    //     buffer_ptrs.push_back((int*)malloc(BUFFER_INT_SIZE * sizeof(int)));
    //     for(int j = 0; j < BUFFER_INT_SIZE; j++){
    //         buffer_ptrs[i][j] = i * BUFFER_INT_SIZE + j;
    //     }
    // }

  
    buffer_ptrs.push_back((int*)malloc(BUFFER_INT_SIZE * sizeof(int)));
    for(int i = 0; i < BUFFER_INT_SIZE; i++){
        buffer_ptrs[0][i] = i;
    }


    //初始化 context
    if(init_context(&context) != 0){
        log_error("Failed to initialize context");
        return -1;
    }

    //初始化 worker
    if(init_worker(context, &g_worker) != 0){
        log_error("Failed to initialize worker");
        return -1;
    }

///////////////////////////////////////////////////////////////////////////////////////
    // 检查命令行参数数量
    if(argc < 3){
        log_error("No IP address provided or port number provided");
        return -1;
    }

    // 第一个命令行参数作为 IP 地址,第二个命令行参数作为端口号
    char *ip_address = argv[1];
    char *endptr;
    long port = strtol(argv[2], &endptr, 10);

    // 检查转换是否成功
    if (*endptr != '\0' || endptr == argv[1]) {
        log_error("Invalid port number: %s\n", argv[1]);
        return 1;
    }

    // 检查是否在 int16_t 的有效范围内
    if (port < INT16_MIN || port > INT16_MAX) {
        log_error("Port number out of range: %ld\n", port);
        return 1;
    }

    // 将端口号转换为 int16_t
    int16_t port_number = (int16_t)port;
    printf("The port number is: %d\n", port_number);
///////////////////////////////////////////////////////////////////////////////////////

    //初始化 endpoint
    if(init_endpoint_ip(g_worker, ip_address,port_number,&g_ep) != 0){
        log_error("Failed to initialize endpoint");
        return -1;
    }

    client_register_am_recv_callback(g_worker);

////////////////////////////////////////////////////////////////////////////////////////

    
    //compute pure net time
    {
        auto start = std::chrono::high_resolution_clock::now();

        for(int i = 0; i < request_num; i ++){
            if(am_send_block(g_worker, g_ep, buffer_ptrs[0], BUFFER_INT_SIZE * sizeof(int)) != 0){
                log_error("Failed to send active message");
                return -1;
            }          
        }

        while(receive_num < request_num){
                ucp_worker_progress(g_worker);
        }
        receive_num = 0;
        auto end = std::chrono::high_resolution_clock::now();
        pure_net_time = end - start;
        log_info("Total time taken to : %f seconds", pure_net_time.count());
    }


    auto start = std::chrono::high_resolution_clock::now();

    for(int i = 0; i < request_num; i ++){
        if(am_send_block(g_worker, g_ep, buffer_ptrs[0], BUFFER_INT_SIZE * sizeof(int)) != 0){
            log_error("Failed to send active message");
            return -1;
        }
    }

    pthread_t thread;
    pthread_create(&thread, NULL, wait_allreduce, NULL);
    
    //do_somecompute
    std::chrono::duration<double> compute_time = performMatrixAddition();


    // Wait for the thread to finish
    pthread_join(thread, NULL);
    

    auto end = std::chrono::high_resolution_clock::now();
    total_time = end - start;
    log_info("Total time taken to : %f seconds", total_time.count());

    log_info("pure net time: %f, compute time: %f, total time: %f", pure_net_time.count(), compute_time.count(), total_time.count());

    log_info("overlapping ratio: %f", (pure_net_time.count() + compute_time.count() - total_time.count()) / total_time.count());

    return 0;
}
