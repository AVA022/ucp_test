#include <mpi.h>
#include <ucp/api/ucp.h>
#include <string.h>
#include <stdlib.h> // Include the <stdlib.h> header file to define the `free` function
#include <log.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>

#include <xml_parser.h>

#include <unordered_map>
#include <chrono>

struct ucx_context {
    int             completed;
};


static ucs_status_t ep_status   = UCS_OK;
static const char *data_msg_str = "UCX data message";
static const ucp_tag_t tag      = 0x1337a880u;
static const ucp_tag_t tag_mask = UINT64_MAX;

static int * input_buffer;
static int ** input_chunk_ptrs;
int * output_buffer;
int ** output_chunk_buffer;
int * scratch_buffer;
int ** scratch_chunk_buffer;



static int BUFFER_SIZE_INT = 1024;
int CHUNK_SIZE_INT;

static ucp_address_t*** g_all_workeraddress;
static ucp_worker_h *g_workers;

sem_t sem1;
sem_t sem2;

int world_size, world_rank;

XMLParser xmlparser;

std::unordered_map<std::string, sem_t*> sem_hash;

// static bool init_chunk_ptrs(int chunk_num, enum buffer_type type)
// {
//     int ***chunk_ptrs_p;
//     switch(type){
//         case INPUT:
//             chunk_ptrs_p = &input_chunk_ptrs;
//             break;
//         case OUTPUT:
//             chunk_ptrs_p = &output_chunk_buffer;
//             break;
//         case SCRATCH:
//             chunk_ptrs_p = &scratch_chunk_buffer;
//             break;
//     }
//     *chunk_ptrs_p = (int **)malloc(chunk_num * sizeof(int *));
//     if (*chunk_ptrs_p == NULL) {
//         return false;
//     }

//     for (int i = 0; i < NUM_CHUNK; i++) {
//         *chunk_ptrs_p[i] = input_buffer + i * VECTOR_SIZE / NUM_CHUNK;
//     }

//     return true;
// }

static void free_chunk_ptrs()
{
    //free(input_chunk_ptrs);
}

static int init_buffer_chunk(int chunk_num, enum buffer_type type)
{
    //初始化buffer
    int **buffer;
    switch (type)
    {
    case INPUT:
        buffer = &input_buffer;
        //log_debug("init input_buffer\n");
        break;
    case OUTPUT:
        buffer = &output_buffer;
        break;
    case SCRATCH:
        buffer = &scratch_buffer;
        break;
    }

    *buffer = (int *)malloc(CHUNK_SIZE_INT * chunk_num * sizeof(int));
    if (*buffer == NULL) {
        return -1;
    }

    //log_debug("start init buffer\n");
    for (int i = 0; i < CHUNK_SIZE_INT * chunk_num; i++) {
        (*buffer)[i]= i;
    }
    //log_debug("init buffer success\n");

    //初始化指向chunk的指针
    int ***chunk_ptrs_p;
    switch(type){
        case INPUT:
            chunk_ptrs_p = &input_chunk_ptrs;
            break;
        case OUTPUT:
            chunk_ptrs_p = &output_chunk_buffer;
            break;
        case SCRATCH:
            chunk_ptrs_p = &scratch_chunk_buffer;
            break;
    }
    *chunk_ptrs_p = (int **)malloc(chunk_num * sizeof(int *));
    if (*chunk_ptrs_p == NULL) {
        return false;
    }

    for (int i = 0; i < chunk_num; i++) {
        (*chunk_ptrs_p)[i] = *buffer + i * CHUNK_SIZE_INT;
    }

    return 0;
}

static void free_vector_buffer()
{
    //free(input_buffer);
}

static bool check_result_vector_buffer()
{
    for (int i = 0; i < BUFFER_SIZE_INT; i++) {
        //printf("input_buffer[%d] = %d\n", i, input_buffer[i]);
        if (input_buffer[i] != 4 * i) {
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
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

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
                              UCP_OP_ATTR_FIELD_DATATYPE ;
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
    int ret = send_block(ucp_worker, ep, input_chunk_ptrs[src_chunk_id], dst_chunk_id, tag_mask, CHUNK_SIZE_INT * sizeof(int));
    if (ret != 0) {
        return false;
    }
    return true;
}

static bool chunk_recieve(ucp_worker_h ucp_worker, ucp_tag_t chunk_id)
{
    int ret = receive_block(ucp_worker, chunk_id, tag_mask, input_chunk_ptrs[chunk_id]);
    if (ret != 0) {
        return false;
    }
    return true;
}

static void local_reduce(int *src_vector, int *dst_vector, int size)
{
    for(int i = 0; i < size; i++)
    {
        if(world_rank == 0){
            //log_debug("src_vector[%d]: %d, dst_vector[%d]: %d", i, src_vector[i], i, dst_vector[i]);
        }
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
 * @return Returns 0 on success, -1 on failure.
 */
static int recieve_reduce_copy_send(ucp_worker_h ucp_worker, ucp_ep_h ep, int src_chunk_id, ucp_tag_t dst_chunk_id)
{
    int *scratch_chunk = (int *)malloc(CHUNK_SIZE_INT * sizeof(int));

    if(receive_block(ucp_worker, dst_chunk_id, tag_mask, scratch_chunk) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }

    local_reduce(scratch_chunk, input_chunk_ptrs[dst_chunk_id], CHUNK_SIZE_INT);

    //假装copy了，感觉在cpu中没什么区别
    free(scratch_chunk);


    if(!chunk_send(ucp_worker, ep, dst_chunk_id, dst_chunk_id)){
        log_error("Failed to send chunk to %d", dst_chunk_id);
        return -1;
    }

    return 0;
}

static int recieve_reduce_send(ucp_worker_h ucp_worker, ucp_ep_h ep, int src_chunk_id, ucp_tag_t dst_chunk_id){
    int *scratch_chunk = (int *)malloc(CHUNK_SIZE_INT * sizeof(int));

    if(receive_block(ucp_worker, dst_chunk_id, tag_mask, scratch_chunk) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }

    local_reduce(scratch_chunk, input_chunk_ptrs[dst_chunk_id], CHUNK_SIZE_INT);

    free(scratch_chunk);


    if(!chunk_send(ucp_worker, ep, dst_chunk_id, dst_chunk_id)){
        log_error("Failed to send chunk to %d", dst_chunk_id);
        return -1;
    }

    return 0;
}

static int recieve_reduce_copy(ucp_worker_h ucp_worker, int src_chunk_id, ucp_tag_t dst_chunk_id)
{
    int *scratch_chunk = (int *)malloc(CHUNK_SIZE_INT * sizeof(int));
    //log_debug("aaaaaaaaaaaaaa %d", scratch_chunk[0]);
    if(receive_block(ucp_worker, dst_chunk_id, tag_mask, scratch_chunk) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }
    //log_debug("aaaaaaaaaaaaaa %d", scratch_chunk[0]);
    local_reduce(scratch_chunk, input_chunk_ptrs[dst_chunk_id], CHUNK_SIZE_INT);

    //假装copy了，感觉在cpu中没什么区别
    free(scratch_chunk);

    return 0;

}



static int recieve_copy_send(ucp_worker_h ucp_worker, ucp_ep_h ep, int src_chunk_id, enum buffer_type src_buffer,
                            ucp_tag_t dst_chunk_id, enum buffer_type dst_buffer){

    if(!chunk_recieve(ucp_worker, dst_chunk_id)){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }

    if(!chunk_send(ucp_worker, ep, dst_chunk_id, dst_chunk_id)){
        log_error("Failed to send chunk to %d", dst_chunk_id);
        return -1;
    }

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


int connect_to_peer(int self_rank, int peer_rank, ucp_worker_h ucp_worker, ucp_ep_h *ep)
{
    int peer_tb = -1;
    for(const auto& tb: xmlparser.ranks[peer_rank]->tbs){
        if(tb->recv == self_rank){
            peer_tb = tb->id;
            break;
        }   
    }
    if(peer_tb == -1){
        log_error("peer_tb not found!\n");
        return -1;
    }

    log_debug("rankid: %d, peer_rank: %d, peer_tb: %d\n", self_rank, peer_rank, peer_tb);
    if(init_endpoint(ucp_worker, g_all_workeraddress[peer_rank][peer_tb], ep) != 0){
        log_error("init_endpoint failed!\n");
        return -1;
    }
    log_debug("complete connect to peer success!\n");

    return 0;
}

static int rank0_comunication(){
    ucp_ep_h ep;
    if(init_endpoint(g_workers[0], g_all_workeraddress[1][0], &ep) != 0){
        log_error("init_endpoint failed!\n");
        return -1;
    }

    input_chunk_ptrs[0][0] = 100;
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

    log_debug("input_chunk_ptrs[0][0]: %d\n", input_chunk_ptrs[0][0]);
    return 0;
}

void *rank0_thread0_communucation(void *arg){
    
    if(recieve_reduce_copy(g_workers[0],1,1)!=0){
        log_error("recieve_reduce_copy failed!\n");
    };
    sem_post(sem_hash["0/0"]);

    sem_wait(sem_hash["1/0"]);
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
    sem_post(sem_hash["1/0"]);

    sem_wait(sem_hash["0/0"]);
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

    if(connect_to_peer(1, 0, g_workers[1], &ep) != 0){
        log_error("connect_to_peer failed!\n");
    }

    // if(init_endpoint(g_workers[1], g_all_workeraddress[0][0], &ep) != 0){
    //     log_error("init_endpoint failed!\n");
    // }
    
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

void init_sem_hash(int world_rank){
    for(const auto& tb: xmlparser.ranks[world_rank]->tbs){
        for(const auto& step: tb->steps){
            if(step->hasdep == 1){
                sem_t *sem = (sem_t *)malloc(sizeof(sem_t));
                sem_init(sem, 0, 0);
                std::string hash_key = std::to_string(tb->id) + "/" + std::to_string(step->s);
                // if(world_rank == 0)
                //     log_debug("init_sem_hash: %s\n", hash_key.c_str());
                sem_hash[hash_key] = sem;
            }
        }
    }
    
}

void* interpreter(void* arg){
    int tb_id = *(int *)arg;
    const auto& tb = xmlparser.ranks[world_rank]->tbs[tb_id];
    ucp_ep_h ep;
    ucp_worker_h worker = g_workers[tb_id];
    if(tb->send != -1){
        connect_to_peer(world_rank, tb->send, worker, &ep);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for(const auto& step: tb->steps){
        if(step->depid != -1){
            std::string hash_key = std::to_string(step->depid) + "/" + std::to_string(step->deps);
            log_info("rankid:%d tbid:%d wait for %s\n", world_rank, tb_id, hash_key.c_str());
            sem_wait(sem_hash[hash_key]);
        }

        switch (step->type)
        {
        case SEND:
            log_debug("rankid:%d tbid:%d start /  send srcoff: %d / dstoff: %d", world_rank, tb_id, step->srcoff, step->dstoff);
            if(chunk_send(worker, ep, step->srcoff, step->dstoff) != true){
                log_error("chunk_send failed!\n");
            }
            break;
        
        case RECV:
            log_debug("rankid:%d tbid:%d start recv / send srcoff: %d / dstoff: %d", world_rank, tb_id, step->srcoff, step->dstoff);
            if(chunk_recieve(worker, step->dstoff) != true){
                log_error("chunk_recieve failed!\n");
            }
            break;

        case RECV_REDUCE_COPY:
            log_debug("rankid:%d tbid:%d start recieve_reduce_copy / send srcoff: %d / dstoff: %d", world_rank, tb_id, step->srcoff, step->dstoff);
            if(recieve_reduce_copy(worker, step->srcoff, step->dstoff) != 0){
                log_error("recieve_reduce_copy failed!\n");
            }
            break;

        case RECV_REDUCE_COPY_SEND:
            log_debug("rankid:%d tbid:%d start recieve_reduce_copy_send  / send srcoff: %d / dstoff: %d", world_rank, tb_id, step->srcoff, step->dstoff);
            if(recieve_reduce_copy_send(worker, ep, step->srcoff, step->dstoff) != 0){
                log_error("recieve_reduce_copy_send failed!\n");
            }
            break;
        case RECV_REDUCE_SEND:
            log_debug("rankid:%d tbid:%d start recieve_reduce_send  / send srcoff: %d / dstoff: %d", world_rank, tb_id, step->srcoff, step->dstoff);
            if(recieve_reduce_send(worker, ep, step->srcoff, step->dstoff) != 0){
                log_error("recieve_reduce_send failed!\n");
            }
            break;
        case RECV_COPY_SEND:
            log_debug("rankid:%d tbid:%d start recieve_copy_send  / send srcoff: %d / dstoff: %d", world_rank, tb_id, step->srcoff, step->dstoff);
            if(recieve_copy_send(worker, ep, step->srcoff, INPUT, step->dstoff, OUTPUT) != 0){
                log_error("recieve_copy_send failed!\n");
            }
            break;

        default:
            log_error("unknown step type!\n");
        }
        if(step->hasdep == 1){
            std::string hash_key = std::to_string(tb_id) + "/" + std::to_string(step->s);
            log_info("rankid:%d tbid:%d post %s\n", world_rank, tb_id, hash_key.c_str());
            sem_post(sem_hash[hash_key]);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();  // 结束时间
    std::chrono::duration<double, std::milli> elapsed = end - start;  // 计算耗时
    log_info("time cost: %f s", 0.001 * elapsed.count());

    return NULL;
}

int start_thread(){
    pthread_t* threads = (pthread_t *)malloc(xmlparser.ranks[world_rank]->tbs.size() * sizeof(pthread_t));

    for(const auto& tb: xmlparser.ranks[world_rank]->tbs){
        pthread_create(&threads[tb->id], NULL, interpreter, &tb->id);
    }

    for(const auto& tb: xmlparser.ranks[world_rank]->tbs){
        pthread_join(threads[tb->id], NULL);
    }
    
    free(threads);
    return 0;
}

int init_buffer_pool(int buffer_size_int){
    BUFFER_SIZE_INT = buffer_size_int;
    auto& rank_intr = xmlparser.ranks[world_rank];
    CHUNK_SIZE_INT = buffer_size_int / xmlparser.nchunksperloop;

    if(rank_intr->i_chunks != 0){
        if(init_buffer_chunk(rank_intr->i_chunks, INPUT) != 0){
            log_error("init_buffer_chunk failed!\n");
            return -1;
        }
    }

    if(rank_intr->o_chunks != 0){
        init_buffer_chunk(rank_intr->o_chunks, OUTPUT);
    }

    if(rank_intr->s_chunks != 0){
        init_buffer_chunk(rank_intr->s_chunks, SCRATCH);
    }

    return 0;
}

int main(int argc, char **argv) {
    xmlparser.parseXMLAndFillStructs("allreduce_better.xml");
    //xmlparser.displayRanks();
    //log_info("%d", xmlparser.ranks[0]->tbs[0]->steps[0]->hasdep);
    //alocate g_workers
    g_workers = (ucp_worker_h *)malloc(2 * sizeof(ucp_worker_h));
    //log_info("%d", xmlparser.nchunksperloop);



    sem_init(&sem1, 0, 0);
    sem_init(&sem2, 0, 0);

    MPI_Init(&argc, &argv);

    //printf("Hello, world!\n");

    
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    init_sem_hash(world_rank);

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




    MPI_Finalize();

    init_buffer_pool(1024 * 1024);
    // for(int i = 0; i < CHUNK_SIZE_INT; i ++)
    //     log_info("%d", input_chunk_ptrs[3][i]);

    start_thread();
    if(input_buffer == NULL){
        log_error("input_buffer is NULL");
        return -1;
    }
    log_info("start check");

    // if(world_rank == 0)
    //     for(int i = 0; i < BUFFER_SIZE_INT; i ++)
    //         log_info("%d", input_buffer[i]);

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