#ifndef UCP_INIT_RESOURCE_H
#define UCP_INIT_RESOURCE_H

#include <ucp/api/ucp.h>

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/time.h>

#include <log.h>

#include <sockaddr_util.h>

extern ucp_worker_h g_worker;
extern ucp_ep_h g_ep;
extern const char *am_msg_str; // "active message"

extern int *tempbuffer;

static void failure_handler(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    //ucs_status_t *arg_status = (ucs_status_t *)arg;

    printf("[0x%x] failure handler called with status %d (%s)\n",
           (unsigned int)pthread_self(), status, ucs_status_string(status));

    //*arg_status = status;
}

static void send_handler(void *request, ucs_status_t status, void *ctx)
{
    //struct ucx_context *context = (struct ucx_context *)request;
    const char *str             = (const char *)ctx;

    //context->completed = 1;

    printf("[0x%x] send handler called for \"%s\" with status %d (%s)\n",
           (unsigned int)pthread_self(), str, status,
           ucs_status_string(status));
}

static ucs_status_t ucx_wait(ucp_worker_h ucp_worker, ucs_status_ptr_t request,
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
        
        //request->completed = 0;
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

int init_worker(ucp_context_h ucp_context, ucp_worker_h *ucp_worker_p){
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

int init_endpoint_ip(ucp_worker_h ucp_worker, const char *address_str, ucp_ep_h *ep)
{
    ucp_ep_params_t ep_params;
    ucs_status_t status;
    struct sockaddr_storage connect_addr;

    set_sock_addr(address_str, &connect_addr, 13337);

    ep_params.field_mask       = UCP_EP_PARAM_FIELD_FLAGS       |
                                 UCP_EP_PARAM_FIELD_SOCK_ADDR   |
                                 UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                 UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.err_mode         = UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb   = failure_handler;
    ep_params.err_handler.arg  = NULL;
    ep_params.flags            = UCP_EP_PARAMS_FLAGS_CLIENT_SERVER;
    ep_params.sockaddr.addr    = (struct sockaddr*)&connect_addr;
    ep_params.sockaddr.addrlen = sizeof(connect_addr);

    status = ucp_ep_create(ucp_worker, &ep_params, ep);
    if(status != UCS_OK){
        return -1;
    }

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

    
    if(init_endpoint_conn(g_worker, conn_request, &g_ep) != 0){
        log_error("init_endpoint_conn failed!\n");
    }
    // if (context->conn_request == NULL) {
    //     context->conn_request = conn_request;
    // } else {
    //     /* The server is already handling a connection request from a client,
    //      * reject this new one */
    //     printf("Rejecting a connection request. "
    //            "Only one client at a time is supported.\n");
    //     status = ucp_listener_reject(context->listener, conn_request);
    //     if (status != UCS_OK) {
    //         fprintf(stderr, "server failed to reject a connection request: (%s)\n",
    //                 ucs_status_string(status));
    //     }
    // }
}

int init_listener(ucp_worker_h ucp_worker, ucp_listener_h *listener_p, const char *address_str)
{
    struct sockaddr_storage listen_addr;
    ucp_listener_params_t listener_params;
    ucp_listener_attr_t listener_attr;
    ucs_status_t status;

    char ip_str[IP_STRING_LEN];
    char port_str[PORT_STRING_LEN];

    set_sock_addr(address_str, &listen_addr, 13337);

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
    
    send_param.cb.send      = send_handler;
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
   log_trace("[0x%x] active message recive handler called for data length: %d with status %d (%s)\n",
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
        request              = ucp_am_recv_data_nbx(g_worker,
                                                    data,
                                                    tempbuffer, 
                                                    length,
                                                    &params);
        
        ucs_status_t status = ucx_wait(g_worker, request, "recv",
                                       am_msg_str);
        return UCS_OK;
    }

    /* Message delivered with eager protocol, data should be available
     * immediately
     */
    // am_data_desc.is_rndv = 0;

    // iov = am_data_desc.recv_buf;
    // offset = 0;
    // for (idx = 0; idx < iov_cnt; idx++) {
    //     mem_type_memcpy(iov[idx].buffer, UCS_PTR_BYTE_OFFSET(data, offset),
    //                     iov[idx].length);
    //     offset += iov[idx].length;
    // }
    memcpy(tempbuffer, data, length);
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



#endif // UCP_INIT_RESOURCE_H