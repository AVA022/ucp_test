#include <mpi.h>
#include <ucp/api/ucp.h>
#include <string.h>
#include <stdlib.h> // Include the <stdlib.h> header file to define the `free` function
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <unistd.h>

#include <xml_parser.h>
#include <log.h>
#include <sockaddr_util.h>

#include <unordered_map>
#include <chrono>
#include <queue>
#include <mutex>

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

const char *am_msg_str = "active message";

std::chrono::duration<double, std::milli> elapsed;

static int BUFFER_SIZE_INT = 1024;
int CHUNK_SIZE_INT;

static ucp_address_t*** g_all_workeraddress;
static ucp_worker_h *g_workers;

pthread_t* g_threads;

static ucp_worker_h server_worker;
static ucp_ep_h server_ep;
static ucp_listener_h server_listener;

sem_t sem_4_main_wait;
sem_t sem_4_th_wait;

int world_size, world_rank;

XMLParser xmlparser;

std::unordered_map<std::string, sem_t*> sem_hash;
std::unordered_map<pthread_t, int> send_num_hash;

std::queue<int*> recv_buffer_ptr_queue;
std::mutex queue_mutex;

int16_t server_port[2] = {13337, 23337};

bool g_offload_flag = false;

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

    for (int i = 0; i < CHUNK_SIZE_INT * chunk_num; i++) {
        (*buffer)[i]= i;
    }

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

int ** select_chunk_type(buffer_type buffer){
    int **chunk_ptrs;
    switch(buffer){
        case INPUT:
            chunk_ptrs = input_chunk_ptrs;
            break;
        case OUTPUT:
            chunk_ptrs = output_chunk_buffer;
            break;
        case SCRATCH:
            chunk_ptrs = scratch_chunk_buffer;
            break;
        default:
            log_error("unknow buffer type\n");
            break;
    }
    return chunk_ptrs;
}

static void free_vector_buffer()
{
    //free(input_buffer);
}

static bool check_result_vector_buffer(int rank_size)
{
    for (int i = 0; i < BUFFER_SIZE_INT; i++) {
        //printf("input_buffer[%d] = %d\n", i, input_buffer[i]);
        if (input_buffer[i] != rank_size * i) {
            return false;
        }
    }

    return true;
}

static void failure_handler(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    //ucs_status_t *arg_status = (ucs_status_t *)arg;

    log_error("[0x%x] failure handler called with status %d (%s)\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status));

    //*arg_status = status;
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
                              UCP_PARAM_FIELD_NAME|
                              UCP_PARAM_FIELD_MT_WORKERS_SHARED;
    ucp_params.features     = UCP_FEATURE_TAG | UCP_FEATURE_AM;
    ucp_params.mt_workers_shared = 1;
    
    ucp_params.name            = "hello_world";

    status = ucp_init(&ucp_params, config, ucp_context_p);


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

static ucs_status_t ucx_wait(ucp_worker_h ucp_worker, ucs_status_ptr_t request,
                             const char *op_str, const char *data_str)
{
    ucs_status_t status;

    if (UCS_PTR_IS_ERR(request)) {
        status = UCS_PTR_STATUS(request);
    } else if (UCS_PTR_IS_PTR(request)) {

        struct timeval start, end;
        gettimeofday(&start, NULL);

        while (ucp_request_check_status(request) == UCS_INPROGRESS)
        {
            ucp_worker_progress(ucp_worker);
        }

        gettimeofday(&end, NULL);
        long seconds = end.tv_sec - start.tv_sec;
        long microseconds = end.tv_usec - start.tv_usec;
        double elapsed = seconds + microseconds*1e-6;
        log_trace("elapsed time: %f\n", elapsed);
        
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
    log_trace("[0x%x] receive handler called with status %d (%s), length %lu\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status),
           info->length);
}

static int receive_block(ucp_worker_h ucp_worker, ucp_tag_t tag,ucp_tag_t tag_mask, void* buffer)
{
    ucs_status_ptr_t request;
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

    request = ucp_tag_msg_recv_nbx(ucp_worker, buffer, info_tag.length, msg_tag,
                                   &recv_param);
    status  = ucx_wait(ucp_worker, request, "receive", data_msg_str);

    if(status != UCS_OK)
    {
        return -1;
    }else{
        return 0;
    }

}

static void tag_send_handler(void *request, ucs_status_t status, void *ctx)
{
    free(ctx);

    log_trace("[0x%x] send handler called for \"%s\" with status %d (%s)\n",
           (unsigned int)pthread_self(), "UCX tag message", status,
           ucs_status_string(status));

    send_num_hash[pthread_self()] --;
}

static void am_send_handler(void *request, ucs_status_t status, void *ctx)
{
    log_trace("[0x%x] send handler called for \"%s\" with status %d (%s)\n",
           (unsigned int)pthread_self(), "UCX am message", status,
           ucs_status_string(status));
}


static int send_block(ucp_worker_h ucp_worker, ucp_ep_h ep, void * buffer, ucp_tag_t tag, ucp_tag_t tag_mask, size_t length)
{
    ucs_status_ptr_t request;
    ucs_status_t status;
    ucp_request_param_t send_param;

    void *temp_buffer = (void *)malloc(length);
    memcpy(temp_buffer, buffer, length);

    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA|
                              UCP_OP_ATTR_FLAG_NO_IMM_CMPL;
    send_param.cb.send      = tag_send_handler;
    send_param.user_data    = (void*)temp_buffer;
    request                 = ucp_tag_send_nbx(ep, temp_buffer, length, tag,
                                               &send_param);
    // status                  = ucx_wait(ucp_worker, request, "send",
    //                                    data_msg_str);

    send_num_hash[pthread_self()] ++;

    status = UCS_OK;
    if (status != UCS_OK) {
        return -1;
    }else{
        return 0;
    }
}




static int chunk_send(ucp_worker_h ucp_worker, ucp_ep_h ep, buffer_type src_buffer, int src_chunk_id, buffer_type dst_buffer ,ucp_tag_t dst_chunk_id, int count)
{   
    int **chunk_ptrs = select_chunk_type(src_buffer);
    int ret = send_block(ucp_worker, ep, chunk_ptrs[src_chunk_id], dst_chunk_id, tag_mask, CHUNK_SIZE_INT * sizeof(int) * count);
    if (ret != 0) {
        return -1;
    }
    return 0;
}

static int chunk_recieve(ucp_worker_h ucp_worker, buffer_type dst_buffer, ucp_tag_t dst_chunk_id, int count)
{
    int **chunk_ptrs = select_chunk_type(dst_buffer);
    int ret = receive_block(ucp_worker, dst_chunk_id, tag_mask, chunk_ptrs[dst_chunk_id]);
    if (ret != 0) {
        return -1;
    }
    return 0;
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

static int reduce(buffer_type src_buffer, int src_chunk_id, buffer_type dst_buffer, ucp_tag_t dst_chunk_id, int count)
{
    int **src_chunk_ptrs = select_chunk_type(src_buffer);
    int **dst_chunk_ptrs = select_chunk_type(dst_buffer);
    local_reduce(src_chunk_ptrs[src_chunk_id], dst_chunk_ptrs[dst_chunk_id], CHUNK_SIZE_INT * count);

    return 0;
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
static int recieve_reduce_copy_send(ucp_worker_h ucp_worker, ucp_ep_h ep, buffer_type src_buffer , int src_chunk_id, buffer_type dst_buffer, ucp_tag_t dst_chunk_id, int count)
{
    int *scratch_chunk = (int *)malloc(CHUNK_SIZE_INT * sizeof(int) * count);

    if(receive_block(ucp_worker, dst_chunk_id, tag_mask, scratch_chunk) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }

    int **chunk_ptrs = select_chunk_type(dst_buffer);

    local_reduce(scratch_chunk, chunk_ptrs[dst_chunk_id], CHUNK_SIZE_INT * count);

    //假装copy了，感觉在cpu中没什么区别
    free(scratch_chunk);



    if(chunk_send(ucp_worker, ep, dst_buffer, dst_chunk_id, dst_buffer, dst_chunk_id, count) != 0){
        log_error("Failed to send chunk to %d", dst_chunk_id);
        return -1;
    }

    return 0;
}

static int recieve_reduce_send(ucp_worker_h ucp_worker, ucp_ep_h ep, 
            buffer_type src_buffer , int src_chunk_id, buffer_type dst_buffer, ucp_tag_t dst_chunk_id, int count)
{
   
    int *scratch_chunk = (int *)malloc(CHUNK_SIZE_INT * sizeof(int) * count);

    if(receive_block(ucp_worker, dst_chunk_id, tag_mask, scratch_chunk) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }

    int **chunk_ptrs = select_chunk_type(dst_buffer);

    local_reduce(scratch_chunk, chunk_ptrs[dst_chunk_id], CHUNK_SIZE_INT * count);

    //假装copy了，感觉在cpu中没什么区别
    free(scratch_chunk);

    if(chunk_send(ucp_worker, ep, dst_buffer, dst_chunk_id, dst_buffer, dst_chunk_id, count) != 0){
        log_error("Failed to send chunk to %d", dst_chunk_id);
        return -1;
    }

    return 0;
}

static int recieve_reduce_copy(ucp_worker_h ucp_worker, ucp_ep_h ep, 
            buffer_type src_buffer , int src_chunk_id, buffer_type dst_buffer, ucp_tag_t dst_chunk_id, int count)
{

    int *scratch_chunk = (int *)malloc(CHUNK_SIZE_INT * sizeof(int) * count);
    if(receive_block(ucp_worker, dst_chunk_id, tag_mask, scratch_chunk) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }

    int **chunk_ptrs = select_chunk_type(dst_buffer);

    local_reduce(scratch_chunk, chunk_ptrs[dst_chunk_id], CHUNK_SIZE_INT * count);

    //假装copy了，感觉在cpu中没什么区别
    free(scratch_chunk);

    return 0;

}



static int recieve_copy_send(ucp_worker_h ucp_worker, ucp_ep_h ep, buffer_type src_buffer, int src_chunk_id,  
                            buffer_type dst_buffer, ucp_tag_t dst_chunk_id, int count){


    if(chunk_recieve(ucp_worker, dst_buffer ,dst_chunk_id, count) != 0){
        log_error("Failed to receive chunk from %d", src_chunk_id);
        return -1;
    }

    if(chunk_send(ucp_worker, ep, dst_buffer, dst_chunk_id, dst_buffer, dst_chunk_id, count) != 0){
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
                                UCP_EP_PARAM_FIELD_ERR_HANDLER ;
                                
    ep_params.address         = peer_addr;
    ep_params.err_mode        = UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb  = failure_handler;
    ep_params.err_handler.arg = NULL;

    status = ucp_ep_create(ucp_worker, &ep_params, ep);
    if(status != UCS_OK){
        return -1;
    }

    return 0;
}


static void allgather_addresses(ucp_worker_h *workers, int num_workers_per_process, int world_size, int world_rank){
    
    ucp_address_t **local_addrs = (ucp_address_t **)malloc(num_workers_per_process * sizeof(ucp_address_t*));
    size_t *local_addr_lengths = (size_t*)malloc(num_workers_per_process * sizeof(size_t));
    //log_debug("0");
   // printf("0\n");
    // Assuming UCP workers have been initialized
    for (int i = 0; i < num_workers_per_process; i++) {
        ucp_worker_get_address(workers[i], &local_addrs[i], &local_addr_lengths[i]);
        //log_info("loop %d", i);
    }
    //log_debug("1");
    //printf("1\n");

    // Collect all worker address lengths
    size_t *all_address_lengths = (size_t*)malloc(world_size * num_workers_per_process * sizeof(size_t));
    if(all_address_lengths == NULL){
        log_error("malloc all_address_lengths failed!\n");
    }
    //log_info("start allgather address length\n");
    MPI_Allgather(local_addr_lengths, num_workers_per_process, MPI_UNSIGNED_LONG,
                  all_address_lengths, num_workers_per_process, MPI_UNSIGNED_LONG, MPI_COMM_WORLD);
    //log_info("complete allgather address length\n");

    // Calculate send buffer size and displacements
    int *send_displs = (int *)malloc(num_workers_per_process * sizeof(int));
    send_displs[0] = 0;
    //log_debug("2.1");
    for (int i = 1; i < num_workers_per_process; i++) {
        send_displs[i] = send_displs[i - 1] + local_addr_lengths[i - 1];
    }
    //log_debug("2");

    int send_buffer_size = send_displs[num_workers_per_process - 1] + local_addr_lengths[num_workers_per_process - 1];
    char *send_buffer = (char *)malloc(send_buffer_size);
    for (int i = 0; i < num_workers_per_process; i++) {
        memcpy(send_buffer + send_displs[i], local_addrs[i], local_addr_lengths[i]);
    }
    //log_debug("3");

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
    // if (world_rank == 0) {
    //     printf("Address of first worker of first process: %p\n", (void*)worker_addresses[0][0]);
    // }

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


int connect_to_peer(int self_rank, int peer_rank, ucp_worker_h ucp_worker, ucp_ep_h *ep, int channel)
{
    int peer_tb = -1;
    for(const auto& tb: xmlparser.ranks[peer_rank]->tbs){
        if(tb->recv == self_rank && tb->chan == channel){
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





int init_endpoint_conn(ucp_worker_h worker,
                    ucp_conn_request_h conn_request,
                    ucp_ep_h *server_ep)
{
    ucp_ep_params_t ep_params;
    ucs_status_t    status;

    /* Server creates an ep to the client on the data worker.
     * This is not the worker the listener was created on.
     * The client side should have initiated the connection, leading
     * to this ep's creation */
    ep_params.field_mask      = UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                UCP_EP_PARAM_FIELD_CONN_REQUEST;
    ep_params.conn_request    = conn_request;
    ep_params.err_handler.cb  = failure_handler;
    ep_params.err_handler.arg = NULL;

    status = ucp_ep_create(worker, &ep_params, server_ep);
    log_trace("ucp_ep_create status: %s\n", ucs_status_string(status));
    if (status != UCS_OK) {
        log_error("failed to create an endpoint on the server: (%s)\n",
                ucs_status_string(status));
        return -1;
    }
    
    return 0;
}

/**
 * The callback on the server side which is invoked upon receiving a connection
 * request from the client.
 */
void server_conn_handle_cb(ucp_conn_request_h conn_request, void *arg)
{
    //ucx_server_ctx_t *context = arg;
    ucp_conn_request_attr_t attr;
    char ip_str[IP_STRING_LEN];
    char port_str[PORT_STRING_LEN];
    ucs_status_t status;

    attr.field_mask = UCP_CONN_REQUEST_ATTR_FIELD_CLIENT_ADDR;
    status = ucp_conn_request_query(conn_request, &attr);
    if (status == UCS_OK) {
        printf("Server received a connection request from client at address %s:%s\n",
               sockaddr_get_ip_str(&attr.client_address, ip_str, sizeof(ip_str)),
               sockaddr_get_port_str(&attr.client_address, port_str, sizeof(port_str)));
    } else if (status != UCS_ERR_UNSUPPORTED) {
        fprintf(stderr, "failed to query the connection request (%s)\n",
                ucs_status_string(status));
    }

    
    if(init_endpoint_conn(server_worker, conn_request, &server_ep) != 0){
        log_error("init_endpoint_conn failed!\n");
    }

}

int init_listener(ucp_worker_h ucp_worker, ucp_listener_h *listener_p, const char *address_str,int16_t port)
{
    struct sockaddr_storage listen_addr;
    ucp_listener_params_t listener_params;
    ucp_listener_attr_t listener_attr;
    ucs_status_t status;

    char ip_str[IP_STRING_LEN];
    char port_str[PORT_STRING_LEN];

    set_sock_addr(address_str, &listen_addr, port);

    listener_params.field_mask         = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                                         UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
    listener_params.sockaddr.addr      = (const struct sockaddr*)&listen_addr;
    listener_params.sockaddr.addrlen   = sizeof(listen_addr);
    listener_params.conn_handler.cb    = server_conn_handle_cb;
    listener_params.conn_handler.arg   = NULL;

    status = ucp_listener_create(ucp_worker, &listener_params, listener_p);
    if (status != UCS_OK) {
        log_error("failed to create a listener: (%s)\n", ucs_status_string(status));
        return -1;
    }

    listener_attr.field_mask = UCP_LISTENER_ATTR_FIELD_SOCKADDR;
    status = ucp_listener_query(*listener_p, &listener_attr);
    if (status != UCS_OK) {
        log_error("failed to query the listener: (%s)\n", ucs_status_string(status));
        return -1;
    }

    log_info( "server is listening on IP %s port %s\n",
            sockaddr_get_ip_str(&listener_attr.sockaddr, ip_str, IP_STRING_LEN),
            sockaddr_get_port_str(&listener_attr.sockaddr, port_str, PORT_STRING_LEN));

    return 0;
}

int am_send_block(ucp_worker_h ucp_worker, ucp_ep_h ep, void * buffer, size_t length)
{   
    ucs_status_ptr_t request;
    ucs_status_t status;
    ucp_request_param_t send_param;

    send_param.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                              UCP_OP_ATTR_FIELD_USER_DATA;
    
    send_param.cb.send      = am_send_handler;
    send_param.user_data    = (void*)am_msg_str;
    request                 = ucp_am_send_nbx(ep, 0, NULL, 0ul, buffer,
                                         length, &send_param);
    status                  = ucx_wait(ucp_worker, request, "send",
                                       am_msg_str);
    if (status != UCS_OK) {
        return -1;
    }else{
        return 0;
    }
}

static void am_recv_cb(void *request, ucs_status_t status, size_t length,
                       void *user_data)
{
   log_info("[0x%x] active message recive handler called for data length: %d with status %d (%s)\n",
           (unsigned int)pthread_self(), length, status,
           ucs_status_string(status));
}

ucs_status_t ucp_am_data_cb(void *arg, const void *header, size_t header_length,
                            void *data, size_t length,
                            const ucp_am_recv_param_t *param)
{
    ucp_dt_iov_t *iov;
    size_t idx;
    size_t offset;

    if (header_length != 0) {
        fprintf(stderr, "received unexpected header, length %ld", header_length);
    }

    int *tempbuffer = (int *)malloc(length);

    if (param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) {
        /* Rendezvous request arrived, data contains an internal UCX descriptor,
         * which has to be passed to ucp_am_recv_data_nbx function to confirm
         * data transfer.
         */
        // am_data_desc.is_rndv = 1;
        // am_data_desc.desc    = data;
        ucp_request_param_t params;
        ucs_status_ptr_t    request;
        params.op_attr_mask = UCP_OP_ATTR_FIELD_CALLBACK |
                          UCP_OP_ATTR_FIELD_USER_DATA;
        params.cb.recv_am    = am_recv_cb;
        request              = ucp_am_recv_data_nbx(server_worker,
                                                    data,
                                                    tempbuffer, 
                                                    length,
                                                    &params);
        
        ucs_status_t status = ucx_wait(server_worker, request, "recv",
                                       am_msg_str);
        
        {
        std::lock_guard<std::mutex> lock(queue_mutex);
        recv_buffer_ptr_queue.push(tempbuffer);
        }
        return UCS_OK;
    }


    memcpy(tempbuffer, data, length);
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        recv_buffer_ptr_queue.push(tempbuffer);
    }
    
    return UCS_OK;
}

ucs_status_t register_am_recv_callback(ucp_worker_h worker)
{
    ucp_am_handler_param_t param;

    param.field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID |
                       UCP_AM_HANDLER_PARAM_FIELD_CB |
                       UCP_AM_HANDLER_PARAM_FIELD_ARG;
    param.id         = 0;
    param.cb         = ucp_am_data_cb;
    param.arg        = worker; /* not used in our callback */

    return ucp_worker_set_am_recv_handler(worker, &param);
}
