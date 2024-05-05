#include <ucp/api/ucp.h>

#include <string.h>    /* memset */
#include <arpa/inet.h> /* inet_addr */
#include <unistd.h>    /* getopt */
#include <stdlib.h>    /* atoi */

#include <sys/poll.h>
#include <stdio.h>
#include <time.h>
#include <netdb.h>

//#include <log.h>

#define IP_STRING_LEN          50
#define PORT_STRING_LEN        8

ucp_ep_h peer_ep;
ucp_ep_h nouse_ep;

static char* sockaddr_get_ip_str(const struct sockaddr_storage *sock_addr,
                                 char *ip_str, size_t max_size)
{
    struct sockaddr_in  addr_in;
    struct sockaddr_in6 addr_in6;

    switch (sock_addr->ss_family) {
    case AF_INET:
        memcpy(&addr_in, sock_addr, sizeof(struct sockaddr_in));
        inet_ntop(AF_INET, &addr_in.sin_addr, ip_str, max_size);
        return ip_str;
    case AF_INET6:
        memcpy(&addr_in6, sock_addr, sizeof(struct sockaddr_in6));
        inet_ntop(AF_INET6, &addr_in6.sin6_addr, ip_str, max_size);
        return ip_str;
    default:
        return "Invalid address family";
    }
}

static char* sockaddr_get_port_str(const struct sockaddr_storage *sock_addr,
                                   char *port_str, size_t max_size)
{
    struct sockaddr_in  addr_in;
    struct sockaddr_in6 addr_in6;

    switch (sock_addr->ss_family) {
    case AF_INET:
        memcpy(&addr_in, sock_addr, sizeof(struct sockaddr_in));
        snprintf(port_str, max_size, "%d", ntohs(addr_in.sin_port));
        return port_str;
    case AF_INET6:
        memcpy(&addr_in6, sock_addr, sizeof(struct sockaddr_in6));
        snprintf(port_str, max_size, "%d", ntohs(addr_in6.sin6_port));
        return port_str;
    default:
        return "Invalid address family";
    }
}

static void err_cb(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    printf("error handling callback was invoked with status %d (%s)\n",
           status, ucs_status_string(status));
    //connection_closed = 1;
}


/**
 * Create a ucp worker on the given ucp context.
 */
static int init_worker(ucp_context_h ucp_context, ucp_worker_h *ucp_worker)
{
    ucp_worker_params_t worker_params;
    ucs_status_t status;
    int ret = 0;

    memset(&worker_params, 0, sizeof(worker_params));

    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

    status = ucp_worker_create(ucp_context, &worker_params, ucp_worker);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_worker_create (%s)\n", ucs_status_string(status));
        ret = -1;
    }

    return ret;
}

static int init_context(ucp_context_h *ucp_context, ucp_worker_h *ucp_worker)
{
    /* UCP objects */
    ucp_params_t ucp_params;
    ucs_status_t status;
    int ret = 0;

    memset(&ucp_params, 0, sizeof(ucp_params));

    /* UCP initialization */
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_NAME;
    ucp_params.name       = "client_server";

    ucp_params.features = UCP_FEATURE_TAG;

    status = ucp_init(&ucp_params, NULL, ucp_context);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_init (%s)\n", ucs_status_string(status));
        ret = -1;
        goto err;
    }

    ret = init_worker(*ucp_context, ucp_worker);
    if (ret != 0) {
        goto err_cleanup;
    }

    return ret;

err_cleanup:
    ucp_cleanup(*ucp_context);
err:
    return ret;
}

static void listener_conn_handle_cb(ucp_conn_request_h conn_request, void *arg)
{
    ucp_worker_h worker = (ucp_worker_h)arg;
    ucp_conn_request_attr_t attr;
    char ip_str[IP_STRING_LEN];
    char port_str[PORT_STRING_LEN];
    ucs_status_t status;

    attr.field_mask = UCP_CONN_REQUEST_ATTR_FIELD_CLIENT_ADDR;
    status = ucp_conn_request_query(conn_request, &attr);
    if (status == UCS_OK) {
        // log_trace("Server received a connection request from client at address %s:%s\n",
        //        sockaddr_get_ip_str(&attr.client_address, ip_str, sizeof(ip_str)),
        //        sockaddr_get_port_str(&attr.client_address, port_str, sizeof(port_str)));
    } else if (status != UCS_ERR_UNSUPPORTED) {
        fprintf(stderr, "failed to query the connection request (%s)\n",
                ucs_status_string(status));
    }

    ucp_ep_params_t ep_params;

    /* Server creates an ep to the client on the data worker.
     * This is not the worker the listener was created on.
     * The client side should have initiated the connection, leading
     * to this ep's creation */
    ep_params.field_mask      = UCP_EP_PARAM_FIELD_ERR_HANDLER |
                                UCP_EP_PARAM_FIELD_CONN_REQUEST;
    ep_params.conn_request    = conn_request;
    ep_params.err_handler.cb  = err_cb;
    ep_params.err_handler.arg = NULL;

    status = ucp_ep_create(worker, &ep_params, &nouse_ep);
    if (status != UCS_OK) {
        // log_error("failed to create an endpoint on the server: (%s)\n",
        //         ucs_status_string(status));
    }
}

static int init_listener(ucp_worker_h worker, uint16_t port, ucp_listener_h *listener_p)
{
    /* Listen on any IPv4 address and the user-specified port */
    const struct sockaddr_in listen_addr = {
        /* Set IPv4 address family */
        .sin_family = AF_INET,
        /* Set port from the user */
        .sin_port = htons(port),
        .sin_addr = {
            /* Set any address */
            .s_addr = INADDR_ANY
        }
    };
    ucp_listener_params_t listener_params = {
        /* Socket address and conenction handler are specified */
        .field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR | UCP_LISTENER_PARAM_FIELD_CONN_HANDLER,
        /* Listen address */
        .sockaddr = {
            .addr = (const struct sockaddr *)&listen_addr,
            .addrlen = sizeof(listen_addr)
        },
        /* Incoming connection handler */
        .conn_handler = {
            .cb = listener_conn_handle_cb,
            .arg = worker
        }
    };
    ucs_status_t status;

	/* Create UCP listener to accept incoming connections */
	status = ucp_listener_create(worker, &listener_params, listener_p);
	if (status != UCS_OK) {
		//log_error("Failed to create UCP listener: %s", ucs_status_string(status));
		return -1;
	}
    return 0;
}




int main(){
    ucp_context_h context;
    ucp_worker_h worker;
    ucp_listener_h listener;
    int ret = 0;

    ret = init_context(&context, &worker);
    if (ret != 0) {
        goto err;
    }

    //log_trace("1");
    ret = init_listener(worker, 13337, &listener);
    if(ret != 0){
        goto err;
    }




    ucp_listener_destroy(listener);
    //log_trace("2");
    ucp_worker_destroy(worker);
    //log_trace("3");
    ucp_cleanup(context);
    
err:
    //log_trace("Exiting with status %d", ret);
    return ret;    
}