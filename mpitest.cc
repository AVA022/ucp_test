#include <mpi.h>
#include <ucp/api/ucp.h>
#include <string.h>
#include <stdlib.h> // Include the <stdlib.h> header file to define the `free` function
#include <log.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

struct ucx_context {
    int             completed;
};

static ucs_status_t ep_status   = UCS_OK;
static const char *data_msg_str = "UCX data message";
static const ucp_tag_t tag      = 0x1337a880u;
static const ucp_tag_t tag_mask = UINT64_MAX;

static int * vector_buffer;
static int ** chunk_ptrs;
static const int VECTOR_SIZE = 64;
static const int NUM_CHUNK = 2;

static ucp_address_t*** g_all_workeraddress;
static ucp_worker_h *g_workers;

sem_t sem1;
sem_t sem2;

static bool init_chunk_ptrs()
{
    chunk_ptrs = (int **)malloc(NUM_CHUNK * sizeof(int *));
    if (chunk_ptrs == NULL) {
        return false;
    }

    for (int i = 0; i < NUM_CHUNK; i++) {
        chunk_ptrs[i] = vector_buffer + i * VECTOR_SIZE / NUM_CHUNK;
    }

    return true;
}

static void free_chunk_ptrs()
{
    free(chunk_ptrs);
}

static int init_vector_buffer()
{
    vector_buffer = (int *)malloc(VECTOR_SIZE * sizeof(int));
    if (vector_buffer == NULL) {
        return -1;
    }

    for (int i = 0; i < VECTOR_SIZE; i++) {
        vector_buffer[i] = i;
    }

    return 0;
}

static void free_vector_buffer()
{
    free(vector_buffer);
}

static bool check_result_vector_buffer()
{
    for (int i = 0; i < VECTOR_SIZE; i++) {
        if (vector_buffer[i] != 2 * i) {
            return false;
        }
    }

    return true;
}

static void failure_handler(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    ucs_status_t *arg_status = (ucs_status_t *)arg;

    printf("[0x%x] failure handler called with status %d (%s)\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status));

    *arg_status = status;
}

static void request_init(void *request)
{
    struct ucx_context *contex = (struct ucx_context *)request;

    contex->completed = 0;
}





static int init_context(ucp_context_h *ucp_context_p){
    ucp_params_t ucp_params;
    ucp_config_t *config;
    ucs_status_t status;

    memset(&ucp_params, 0, sizeof(ucp_params));

    status = ucp_config_read(NULL, NULL, &config);
    if(status != UCS_OK){
        log_error("ucp_config_read failed!\n");
        return -1;
    }

    ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES |
                              UCP_PARAM_FIELD_REQUEST_SIZE |
                              UCP_PARAM_FIELD_REQUEST_INIT |
                              UCP_PARAM_FIELD_NAME|
                              UCP_PARAM_FIELD_MT_WORKERS_SHARED;
    ucp_params.features     = UCP_FEATURE_TAG;
    ucp_params.mt_workers_shared = 1;
    
    ucp_params.request_size    = sizeof(struct ucx_context);
    ucp_params.request_init    = request_init;
    ucp_params.name            = "hello_world";

    status = ucp_init(&ucp_params, config, ucp_context_p);

    // if (print_config) {
    //     ucp_config_print(config, stdout, NULL, UCS_CONFIG_PRINT_CONFIG);
    // }

    ucp_config_release(config);
    if(status != UCS_OK){
        log_error("ucp_init failed!\n");
        return -1;
    }

    return 0;
}

static int init_worker(ucp_context_h ucp_context, ucp_worker_h *ucp_worker_p){
    ucp_worker_attr_t worker_attr;
    ucp_worker_params_t worker_params;
    ucs_status_t status;

    memset(&worker_params, 0, sizeof(worker_params));

    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SERIALIZED;

    status = ucp_worker_create(ucp_context, &worker_params, ucp_worker_p);
    if(status != UCS_OK){
        log_error("ucp_worker_create failed!\n");
        return -1;
    }

    worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS;

    status = ucp_worker_query(*ucp_worker_p, &worker_attr);
    if(status != UCS_OK){
        log_error("ucp_worker_query failed!\n");
        return -1;
    }

    return 0;
}

static ucs_status_t ucx_wait(ucp_worker_h ucp_worker, struct ucx_context *request,
                             const char *op_str, const char *data_str)
{
    ucs_status_t status;

    if (UCS_PTR_IS_ERR(request)) {
        status = UCS_PTR_STATUS(request);
    } else if (UCS_PTR_IS_PTR(request)) {
        // while (!request->completed) {
        //     ucp_worker_progress(ucp_worker);
        // }
        struct timeval start, end;
        gettimeofday(&start, NULL);

        while (ucp_request_check_status(request) == UCS_INPROGRESS)
        {
            //printf("1\n");
            ucp_worker_progress(ucp_worker);
        }

        gettimeofday(&end, NULL);
        long seconds = end.tv_sec - start.tv_sec;
        long microseconds = end.tv_usec - start.tv_usec;
        double elapsed = seconds + microseconds*1e-6;
        log_trace("elapsed time: %f\n", elapsed);
        
        request->completed = 0;
        status             = ucp_request_check_status(request);
        ucp_request_free(request);
    } else {
        status = UCS_OK;
    }

    if (status != UCS_OK) {
        fprintf(stderr, "unable to %s %s (%s)\n", op_str, data_str,
                ucs_status_string(status));
    } else {
        log_trace("in ucx_wait status : %s", ucs_status_string(status));
        log_trace("finish to %s %s\n", op_str, data_str);
    }

    return status;
}

static void recv_handler(void *request, ucs_status_t status,
                         const ucp_tag_recv_info_t *info, void *user_data)
{
    struct ucx_context *context = (struct ucx_context *)request;

    context->completed = 1;

    printf("[0x%x] receive handler called with status %d (%s), length %lu\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status),
           info->length);
}

static int receive_block(ucp_worker_h ucp_worker, ucp_tag_t tag,ucp_tag_t tag_mask, void* buffer)
{
    struct ucx_context *request;
    ucp_tag_recv_info_t info_tag;
    ucp_tag_message_h msg_tag;
    ucs_status_t status;
    ucp_request_param_t recv_param;

    for (;;) {
        /* Probing incoming events in non-block mode */
        msg_tag = ucp_tag_probe_nb(ucp_worker, tag, tag_mask, 1, &info_tag);
        if (msg_tag != NULL) {
            /* Message arrived */
            break;
        }
        
        ucp_worker_progress(ucp_worker);
    }

    recv_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_DATATYPE |
                              UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
    recv_param.datatype     = ucp_dt_make_contig(1);
    recv_param.cb.recv      = recv_handler;

    request = (struct ucx_context *)ucp_tag_msg_recv_nbx(ucp_worker, buffer, info_tag.length, msg_tag,
                                   &recv_param);
    status  = ucx_wait(ucp_worker, request, "receive", data_msg_str);

    if(status != UCS_OK)
    {
        return -1;
    }else{
        return 0;
    }

}

static void send_handler(void *request, ucs_status_t status, void *ctx)
{
    struct ucx_context *context = (struct ucx_context *)request;
    const char *str             = (const char *)ctx;

    context->completed = 1;

    printf("[0x%x] send handler called for \"%s\" with status %d (%s)\n",
           (unsigned int)pthread_self(), str, status,
           ucs_status_string(status));
}


static int send_block(ucp_worker_h ucp_worker, ucp_ep_h ep, void * buffer, ucp_tag_t tag, ucp_tag_t tag_mask, size_t length)
{
    struct ucx_context *request;
    ucs_status_t status;
    ucp_request_param_t send_param;

    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    send_param.cb.send      = send_handler;
    send_param.user_data    = (void*)data_msg_str;
    request                 = (struct ucx_context *)ucp_tag_send_nbx(ep, buffer, length, tag,
                                               &send_param);
    status                  = ucx_wait(ucp_worker, request, "send",
                                       data_msg_str);
    if (status != UCS_OK) {
        return -1;
    }else{
        return 0;
    }
}

static bool chunk_send(ucp_worker_h ucp_worker, ucp_ep_h ep, int src_chunk_id, ucp_tag_t dst_chunk_id)
{
    int ret = send_block(ucp_worker, ep, chunk_ptrs[src_chunk_id], dst_chunk_id, tag_mask, (VECTOR_SIZE / NUM_CHUNK) * sizeof(int));
    if (ret != 0) {
        return false;
    }
    return true;
}

static bool chunk_recieve(ucp_worker_h ucp_worker, ucp_tag_t chunk_id)
{
    int ret = receive_block(ucp_worker, chunk_id, tag_mask, chunk_ptrs[chunk_id]);
    if (ret != 0) {
        return false;
    }
    return true;
}

static void local_reduce(int *src_vector, int *dst_vector, int size)
{
    for(int i = 0; i < size; i++)
    {
        dst_vector[i] += src_vector[i];
    }
}


/**
 * @brief This function receives a chunk of data, performs a local reduction operation on it, and then sends it to another destination.
 * 
 * @param ucp_worker The UCP worker handle.
 * @param ep The UCP endpoint handle.
 * @param src_chunk_id The ID of the source chunk.实际上没被用，这取决于xml文件中的配置
 * @param dst_chunk_id The ID of the destination chunk.既是本地接收的chunk的id，也是目的地的chunk的id
 * @return Returns true if the operation is successful, false otherwise.
 */
static bool recieve_reduce_copy_send(ucp_worker_h ucp_worker, ucp_ep_h ep, int src_chunk_id, ucp_tag_t dst_chunk_id)
{
    int *scratch_chunk = (int *)malloc((VECTOR_SIZE / NUM_CHUNK) * sizeof(int));

    if(receive_block(ucp_worker, dst_chunk_id, tag_mask, scratch_chunk) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return false;
    }

    local_reduce(scratch_chunk, chunk_ptrs[dst_chunk_id], (VECTOR_SIZE / NUM_CHUNK));

    //假装copy了，感觉在cpu中没什么区别
    free(scratch_chunk);


    if(!chunk_send(ucp_worker, ep, dst_chunk_id, dst_chunk_id)){
        log_error("Failed to send chunk to %d", dst_chunk_id);
        return false;
    }

    return true;

}

static int recieve_reduce_copy(ucp_worker_h ucp_worker, int src_chunk_id, ucp_tag_t dst_chunk_id)
{
    int *scratch_chunk = (int *)malloc((VECTOR_SIZE / NUM_CHUNK) * sizeof(int));

    if(receive_block(ucp_worker, dst_chunk_id, tag_mask, scratch_chunk) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }

    local_reduce(scratch_chunk, chunk_ptrs[dst_chunk_id], (VECTOR_SIZE / NUM_CHUNK));

    //假装copy了，感觉在cpu中没什么区别
    free(scratch_chunk);

    return 0;

}

static int init_endpoint(ucp_worker_h ucp_worker, ucp_address_t *peer_addr, ucp_ep_h *ep)
{
    ucp_ep_params_t ep_params;
    ucs_status_t status;

    ep_params.field_mask      = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                                UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE |
                                UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                UCP_EP_PARAM_FIELD_USER_DATA;
    ep_params.address         = peer_addr;
    ep_params.err_mode        = UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb  = failure_handler;
    ep_params.err_handler.arg = NULL;
    ep_params.user_data       = &ep_status;

    status = ucp_ep_create(ucp_worker, &ep_params, ep);
    if(status != UCS_OK){
        return -1;
    }

    return 0;
}


bool allgather_addresses_len(int * local_worker_addr_len, int * all_worker_addr_len, int worker_size){
    MPI_Allgather(local_worker_addr_len, worker_size, MPI_INT, all_worker_addr_len, worker_size, MPI_INT, MPI_COMM_WORLD);
    return true;
}

static void allgather_addresses(ucp_worker_h *workers, int num_workers_per_process, int world_size, int world_rank){
    ucp_address_t **local_addrs = (ucp_address_t **)malloc(num_workers_per_process * sizeof(ucp_address_t*));
    size_t *local_addr_lengths = (size_t*)malloc(num_workers_per_process * sizeof(size_t));

    // Assuming UCP workers have been initialized
    for (int i = 0; i < num_workers_per_process; i++) {
        ucp_worker_get_address(workers[i], &local_addrs[i], &local_addr_lengths[i]);
    }

    // Collect all worker address lengths
    size_t *all_address_lengths = (size_t*)malloc(world_size * num_workers_per_process * sizeof(size_t));
    MPI_Allgather(local_addr_lengths, num_workers_per_process, MPI_UNSIGNED_LONG,
                  all_address_lengths, num_workers_per_process, MPI_UNSIGNED_LONG, MPI_COMM_WORLD);

    // Calculate send buffer size and displacements
    int *send_displs = (int *)malloc(num_workers_per_process * sizeof(int));
    send_displs[0] = 0;
    for (int i = 1; i < num_workers_per_process; i++) {
        send_displs[i] = send_displs[i - 1] + local_addr_lengths[i - 1];
    }

    int send_buffer_size = send_displs[num_workers_per_process - 1] + local_addr_lengths[num_workers_per_process - 1];
    char *send_buffer = (char *)malloc(send_buffer_size);
    for (int i = 0; i < num_workers_per_process; i++) {
        memcpy(send_buffer + send_displs[i], local_addrs[i], local_addr_lengths[i]);
    }

    int *recvcounts = (int *)malloc(world_size * sizeof(int));
    int *displs = (int *)malloc(world_size * sizeof(int));
    displs[0] = 0;
    for (int i = 0; i < world_size; i++) {
        recvcounts[i] = 0;
        for (int j = 0; j < num_workers_per_process; j++) {
            recvcounts[i] += all_address_lengths[i * num_workers_per_process + j];
        }
        if (i > 0) {
            displs[i] = displs[i - 1] + recvcounts[i - 1];
        }
    }

    char *recv_buffer = (char *)malloc(displs[world_size - 1] + recvcounts[world_size - 1]);
    MPI_Allgatherv(send_buffer, send_buffer_size, MPI_BYTE,
                   recv_buffer, recvcounts, displs, MPI_BYTE, MPI_COMM_WORLD);

    // Build a 2D array for accessing addresses
    ucp_address_t ***worker_addresses = (ucp_address_t ***)malloc(world_size * sizeof(ucp_address_t**));
    for (int i = 0; i < world_size; i++) {
        worker_addresses[i] = (ucp_address_t **)malloc(num_workers_per_process * sizeof(ucp_address_t*));
        int offset = 0;
        for (int j = 0; j < num_workers_per_process; j++) {
            worker_addresses[i][j] = (ucp_address_t*)(recv_buffer + displs[i] + offset);
            offset += all_address_lengths[i * num_workers_per_process + j];
        }
    }
    g_all_workeraddress = worker_addresses;

    // Example usage
    if (world_rank == 0) {
        printf("Address of first worker of first process: %p\n", (void*)worker_addresses[0][0]);
    }

    //free resouces that malloc in this function
    free(displs);
    free(recvcounts);
    free(send_buffer);
    free(send_displs);
    free(all_address_lengths);
    free(local_addr_lengths);
    free(local_addrs);
}

static int init_all_workers_ucp(ucp_worker_h *workers, ucp_context_h ucp_context, int num_workers_per_process){
    for (int i = 0; i < num_workers_per_process; i++) {
        if(init_worker(ucp_context, &workers[i]) != 0){
            log_error("init the %d worker failed!\n", i);
            return -1;
        }
    }
    return 0;
}

static int rank0_comunication(){
    ucp_ep_h ep;
    if(init_endpoint(g_workers[0], g_all_workeraddress[1][0], &ep) != 0){
        log_error("init_endpoint failed!\n");
        return -1;
    }

    chunk_ptrs[0][0] = 100;
    if(chunk_send(g_workers[0], ep, 0, 0) != true){
        log_error("chunk_send failed!\n");
        return -1;
    }

    return 0;
}

static int rank1_comunication(){
    if(chunk_recieve(g_workers[0], 0) != true){
        log_error("chunk_recieve failed!\n");
        return -1;
    }

    log_debug("chunk_ptrs[0][0]: %d\n", chunk_ptrs[0][0]);
    return 0;
}

void *rank0_thread0_communucation(void *arg){
    
    if(recieve_reduce_copy(g_workers[0],1,1)!=0){
        log_error("recieve_reduce_copy failed!\n");
    };
    sem_post(&sem1);

    sem_wait(&sem2);
    if(chunk_recieve(g_workers[0], 0) != true){
        log_error("chunk_recieve failed!\n");
    }

    return NULL;
}

void *rank0_thread1_communucation(void *arg){
    ucp_ep_h ep;
    if(init_endpoint(g_workers[1], g_all_workeraddress[1][0], &ep) != 0){
        log_error("init_endpoint failed!\n");
    }

    log_trace("rank0_thread1_communucation: send chunk0 start\n");
    if(chunk_send(g_workers[1], ep, 0, 0) != true){
        log_error("chunk_send failed!\n");
    }
    sem_post(&sem2);

    sem_wait(&sem1);
    if(chunk_send(g_workers[1], ep, 1, 1) != true){
        log_error("chunk_send failed!\n");
    }

    return NULL;
}

void *rank1_thread0_communucation(void *arg){
    if(recieve_reduce_copy(g_workers[0],0,0)!=0){
        log_error("recieve_reduce_copy failed!\n");
    };
    sem_post(&sem1);

    sem_wait(&sem2);
    if(chunk_recieve(g_workers[0], 1) != true){
        log_error("chunk_recieve failed!\n");
    }

    return NULL;
}

void *rank1_thread1_communucation(void *arg){
    ucp_ep_h ep;
    if(init_endpoint(g_workers[1], g_all_workeraddress[0][0], &ep) != 0){
        log_error("init_endpoint failed!\n");
    }

    log_trace("rank0_thread1_communucation: send chunk0 start\n");
    if(chunk_send(g_workers[1], ep, 1, 1) != true){
        log_error("chunk_send failed!\n");
    }
    sem_post(&sem2);

    sem_wait(&sem1);
    if(chunk_send(g_workers[1], ep, 0, 0) != true){
        log_error("chunk_send failed!\n");
    }

    return NULL;
}







int main(int argc, char **argv) {
    //alocate g_workers
    g_workers = (ucp_worker_h *)malloc(2 * sizeof(ucp_worker_h));

    sem_init(&sem1, 0, 0);
    sem_init(&sem2, 0, 0);

    MPI_Init(&argc, &argv);

    //printf("Hello, world!\n");

    int world_size, world_rank;
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    // 初始化UCP
    ucp_context_h ucp_context;

    if(init_context(&ucp_context) != 0){
        log_error("init_context failed!\n");
        return -1;
    }

    //init_worker
    if(init_all_workers_ucp(g_workers, ucp_context, 2) != 0){
        log_error("init_all_workers_ucp failed!\n");
        return -1;
    }

    log_debug("begin to allgather addresses\n");
    allgather_addresses(g_workers, 2, world_size, world_rank);
    log_debug("finish allgather addresses\n");


    // // 获取本地UCX worker地址
    // ucp_address_t *local_addr;
    // size_t local_addr_length;
    // ucp_worker_get_address(ucp_worker, &local_addr, &local_addr_length);

    // // 分配空间以存储所有worker的地址长度和位移
    // size_t *all_address_lengths = (size_t *)malloc(world_size * sizeof(size_t));
    // int *displs = (int *)malloc(world_size * sizeof(int));  // 位移数组
    // int *recvcounts = (int *)malloc(world_size * sizeof(int));

    // // 通过MPI收集所有进程的地址长度
    // MPI_Allgather(&local_addr_length, 1, MPI_UNSIGNED_LONG, all_address_lengths, 1, MPI_UNSIGNED_LONG, MPI_COMM_WORLD);
    // if(world_rank == 0)
    //     for(int i = 0; i < world_size; i++) {
    //         printf("%ld\n", all_address_lengths[i]);
    //     }

    // //将size_t转换为int
    // for (int i = 0; i < world_size; i++) {
    //     if (all_address_lengths[i] > INT_MAX) {
    //         fprintf(stderr, "Error: Address length too large at index %d\n", i);
    //         // 处理错误，例如清理内存并退出
    //         free(recvcounts);
    //         free(displs);
    //         return 1;
    //     }
    //     recvcounts[i] = (int)all_address_lengths[i];
    // }


    // // 准备位移数组
    // displs[0] = 0;
    // for (int i = 1; i < world_size; i++) {
    //     displs[i] = displs[i - 1] + recvcounts[i - 1];
    // }
    // if(world_rank == 0)
    //     for(int i = 0; i < world_size; i++) {
    //         printf("%d\n", displs[i]);
    //     }

    // // 分配接收缓冲区
    // char *recv_buffer = (char *)malloc(displs[world_size - 1] + recvcounts[world_size - 1]);
    // if(world_rank == 0)
    //     printf("recv_buffer size: %d\n", displs[world_size - 1] + recvcounts[world_size - 1]);

    // ucp_address_t **all_worker_addresses = (ucp_address_t **)malloc(world_size * sizeof(ucp_address_t*));


    // // 使用MPI发送和接收所有进程的地址
    // MPI_Allgatherv(local_addr, local_addr_length, MPI_BYTE, recv_buffer, recvcounts, displs, MPI_BYTE, MPI_COMM_WORLD);

    // // 将接收缓冲区中的数据分配给指针数组
    // for (int i = 0; i < world_size; i++) {
    //     all_worker_addresses[i] = (ucp_address_t *)(recv_buffer + displs[i]);
    // }

    MPI_Finalize();

    // 用收到的地址创建UCP端点
    // if (world_rank != 0) { // 示例：每个非0号进程与0号进程建立连接
    //     ucp_ep_h ep;
    //     ucp_ep_params_t ep_params;
    //     ucs_status_t status;
    //     ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
    //     ep_params.address = all_worker_addresses[0];  // 此处假设0号进程的地址位于数组开始位置
    //     status= ucp_ep_create(ucp_worker, &ep_params, &ep);
    //     if(status != UCS_OK){
    //         log_error("ucp_ep_create failed!\n");
    //         return -1;
    //     }
    // }



    // ucp_ep_h ep;
    // int i = world_rank ? 0 : 1;
    // if(init_endpoint(ucp_worker, all_worker_addresses[i], &ep) != 0){
    //     log_error("init_endpoint failed!\n");
    //     return -1;
    // }



    init_vector_buffer();
    init_chunk_ptrs();

    // if(world_rank == 0){
    //     if(!chunk_send(ucp_worker, ep, 0, 0)) {
    //         log_error("Failed to send chunk 0");
    //     }

    //     if(!recieve_reduce_copy_send(ucp_worker, ep, 1, 1)){
    //         log_error("Failed to receive reduce copy send");
    //     }

    //     if(!chunk_recieve(ucp_worker, 0)){
    //         log_error("Failed to receive chunk 0");
    //     }

    //     if(check_result_vector_buffer()){
    //         log_info("Test success");
    //     }else{
    //         log_error("Test failed");
    //     }
    // }
    // else {
    //     if(!chunk_send(ucp_worker, ep, 1, 1)) {
    //         log_error("Failed to send chunk 0");
    //     }

    //     if(!recieve_reduce_copy_send(ucp_worker, ep, 0, 0)){
    //         log_error("Failed to receive reduce copy send");
    //     }

    //     if(!chunk_recieve(ucp_worker, 1)){
    //         log_error("Failed to receive chunk 0");
    //     }

    //     if(check_result_vector_buffer()){
    //         log_info("Test success");
    //     }else{
    //         log_error("Test failed");
    //     }
    // }
    pthread_t tid1;
    pthread_t tid2;

    if(world_rank == 0){
        pthread_create(&tid1, NULL, rank0_thread0_communucation, NULL);
        pthread_create(&tid2, NULL, rank0_thread1_communucation, NULL);
    
        pthread_join(tid1, NULL);
        pthread_join(tid2, NULL);
    }else{
        pthread_create(&tid1, NULL, rank1_thread0_communucation, NULL);
        pthread_create(&tid2, NULL, rank1_thread1_communucation, NULL);

        pthread_join(tid1, NULL);
        pthread_join(tid2, NULL);
    }

    if(check_result_vector_buffer()){
        log_debug("allreduce Test success");
    }
    else{
        log_error("Test failed");
    }

    // 清理资源
    // free(recv_buffer);
    // free(all_worker_addresses);
    // free(displs);
    // free(all_address_lengths);
    free_chunk_ptrs();
    free_vector_buffer();
    //ucp_worker_release_address(ucp_worker, local_addr);
    
    //release all address
    // for(int i = 0; i < 2; i++){
    //     ucp_worker_release_address(g_workers[i], g_all_workeraddress[world_rank][i]);
    // }



    //destroy all workers
    for(int i = 0;i < 2; i++ ){
        ucp_worker_destroy(g_workers[i]);
    }

    ucp_cleanup(ucp_context);

    
    return 0;
}