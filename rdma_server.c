/*
 * This is RDMA server side code for assignment 8 in the
 * Advanced Computer Network (ACN) course.
 *
 * Author: Animesh Trivedi
 *         atr@zurich.ibm.com (atrivedi@student.ethz.ch)
 *
 * TODO: Cleanup previously allocated resources in case of an error condition
 */

#include "rdma_common.h"

/* These are the RDMA resources needed to setup an RDMA connection */
/* Event channel, where connection management (cm) related events are relayed */
static struct rdma_event_channel *cm_event_channel = NULL;
static struct rdma_cm_id *cm_server_id = NULL, *cm_client_id = NULL;
static struct ibv_pd *pd = NULL;
static struct ibv_comp_channel *io_completion_channel = NULL;
static struct ibv_cq *cq = NULL;
static struct ibv_qp_init_attr qp_init_attr;
static struct ibv_qp *client_qp = NULL;
/* RDMA memory resources */
static struct ibv_mr *client_metadata_mr = NULL, *server_buffer_mr = NULL, *server_metadata_mr = NULL;
static struct rdma_buffer_attr client_metadata_attr, server_metadata_attr;
static struct ibv_recv_wr client_recv_wr, *bad_client_recv_wr = NULL;
static struct ibv_sge client_recv_sge;

static char* buf_for_rwrite = NULL;
#define BLOCK_SZ 25000000
#define BLOCK_NUM 4
char* block_mem[BLOCK_NUM];
/* When we call this function cm_client_id must be set to a valid identifier.
 * This is where, we prepare client connection before we accept it. This
 * mainly involve pre-posting a receive buffer to receive client side
 * RDMA credentials
 */
static int setup_client_resources()
{
	int ret = -1;
	if (!cm_client_id)
	{
		rdma_error("Client id is still NULL \n");
		return -EINVAL;
	}
	/* We have a valid connection identifier, lets start to allocate
	 * resources. We need:
	 * 1. Protection Domains (PD)
	 * 2. Memory Buffers
	 * 3. Completion Queues (CQ)
	 * 4. Queue Pair (QP)
	 * Protection Domain (PD) is similar to a "process abstraction"
	 * in the operating system. All resources are tied to a particular PD.
	 * And accessing recourses across PD will result in a protection fault.
	 */
	pd = ibv_alloc_pd(cm_client_id->verbs
	                  /* verbs defines a verb's provider,
	                   * i.e an RDMA device where the incoming
	                   * client connection came */);
	if (!pd)
	{
		rdma_error("Failed to allocate a protection domain errno: %d\n",
		           -errno);
		return -errno;
	}
	debug("A new protection domain is allocated at %p \n", pd);
	/* Now we need a completion channel, were the I/O completion
	 * notifications are sent. Remember, this is different from connection
	 * management (CM) event notifications.
	 * A completion channel is also tied to an RDMA device, hence we will
	 * use cm_client_id->verbs.
	 */
	io_completion_channel = ibv_create_comp_channel(cm_client_id->verbs);
	if (!io_completion_channel)
	{
		rdma_error("Failed to create an I/O completion event channel, %d\n",
		           -errno);
		return -errno;
	}
	debug("An I/O completion event channel is created at %p \n",
	      io_completion_channel);
	/* Now we create a completion queue (CQ) where actual I/O
	 * completion metadata is placed. The metadata is packed into a structure
	 * called struct ibv_wc (wc = work completion). ibv_wc has detailed
	 * information about the work completion. An I/O request in RDMA world
	 * is called "work" ;)
	 */
	cq = ibv_create_cq(cm_client_id->verbs /* which device*/,
	                   CQ_CAPACITY /* maximum capacity*/,
	                   NULL /* user context, not used here */,
	                   io_completion_channel /* which IO completion channel */,
	                   0 /* signaling vector, not used here*/);
	if (!cq)
	{
		rdma_error("Failed to create a completion queue (cq), errno: %d\n",
		           -errno);
		return -errno;
	}
	debug("Completion queue (CQ) is created at %p with %d elements \n",
	      cq, cq->cqe);
	/* Ask for the event for all activities in the completion queue*/
	ret = ibv_req_notify_cq(cq /* on which CQ */,
	                        0 /* 0 = all event type, no filter*/);
	if (ret)
	{
		rdma_error("Failed to request notifications on CQ errno: %d \n",
		           -errno);
		return -errno;
	}
	/* Now the last step, set up the queue pair (send, recv) queues and their capacity.
	 * The capacity here is define statically but this can be probed from the
	 * device. We just use a small number as defined in rdma_common.h */
	bzero(&qp_init_attr, sizeof qp_init_attr);
	qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
	qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
	qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
	qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
	qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
	/* We use same completion queue, but one can use different queues */
	qp_init_attr.recv_cq = cq; /* Where should I notify for receive completion operations */
	qp_init_attr.send_cq = cq; /* Where should I notify for send completion operations */
	/*Lets create a QP */
	ret = rdma_create_qp(cm_client_id /* which connection id */,
	                     pd /* which protection domain*/,
	                     &qp_init_attr /* Initial attributes */);
	if (ret)
	{
		rdma_error("Failed to create QP due to errno: %d\n", -errno);
		return -errno;
	}
	/* Save the reference for handy typing but is not required */
	client_qp = cm_client_id->qp;
	debug("Client QP created at %p\n", client_qp);
	return ret;
}

/* Starts an RDMA server by allocating basic connection resources */
static int start_rdma_server(struct sockaddr_in *server_addr)
{
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
	/*  Open a channel used to report asynchronous communication event */
	cm_event_channel = rdma_create_event_channel();
	if (!cm_event_channel)
	{
		rdma_error("Creating cm event channel failed with errno : (%d)", -errno);
		return -errno;
	}
	debug("RDMA CM event channel is created successfully at %p \n",
	      cm_event_channel);
	/* rdma_cm_id is the connection identifier (like socket) which is used
	 * to define an RDMA connection.
	 */
	ret = rdma_create_id(cm_event_channel, &cm_server_id, NULL, RDMA_PS_TCP);
	if (ret)
	{
		rdma_error("Creating server cm id failed with errno: %d ", -errno);
		return -errno;
	}
	debug("A RDMA connection id for the server is created \n");
	/* Explicit binding of rdma cm id to the socket credentials */
	ret = rdma_bind_addr(cm_server_id, (struct sockaddr*) server_addr);
	if (ret)
	{
		rdma_error("Failed to bind server address, errno: %d \n", -errno);
		return -errno;
	}
	debug("Server RDMA CM id is successfully binded \n");
	/* Now we start to listen on the passed IP and port. However unlike
	 * normal TCP listen, this is a non-blocking call. When a new client is
	 * connected, a new connection management (CM) event is generated on the
	 * RDMA CM event channel from where the listening id was created. Here we
	 * have only one channel, so it is easy. */
	ret = rdma_listen(cm_server_id, 8); /* backlog = 8 clients, same as TCP, see man listen*/
	if (ret)
	{
		rdma_error("rdma_listen failed to listen on server address, errno: %d ",
		           -errno);
		return -errno;
	}
	printf("Server is listening successfully at: %s , port: %d \n",
	       inet_ntoa(server_addr->sin_addr),
	       ntohs(server_addr->sin_port));
	/* now, we expect a client to connect and generate a RDMA_CM_EVNET_CONNECT_REQUEST
	 * We wait (block) on the connection management event channel for
	 * the connect event.
	 */
	ret = process_rdma_cm_event(cm_event_channel,
	                            RDMA_CM_EVENT_CONNECT_REQUEST,
	                            &cm_event);
	if (ret)
	{
		rdma_error("Failed to get cm event, ret = %d \n" , ret);
		return ret;
	}
	/* Much like TCP connection, listening returns a new connection identifier
	 * for newly connected client. In the case of RDMA, this is stored in id
	 * field. For more details: man rdma_get_cm_event
	 */
	cm_client_id = cm_event->id;
	/* now we acknowledge the event. Acknowledging the event free the resources
	 * associated with the event structure. Hence any reference to the event
	 * must be made before acknowledgment. Like, we have already saved the
	 * client id from "id" field before acknowledging the event.
	 */
	ret = rdma_ack_cm_event(cm_event);
	if (ret)
	{
		rdma_error("Failed to acknowledge the cm event errno: %d \n", -errno);
		return -errno;
	}
	debug("A new RDMA client connection id is stored at %p\n", cm_client_id);
	return ret;
}

/* Pre-posts a receive buffer and accepts an RDMA client connection */
static int accept_client_connection()
{
	struct rdma_conn_param conn_param;
	struct rdma_cm_event *cm_event = NULL;
	struct sockaddr_in remote_sockaddr;
	int ret = -1;
	if (!cm_client_id || !client_qp)
	{
		rdma_error("Client resources are not properly setup\n");
		return -EINVAL;
	}
	/* we prepare the receive buffer in which we will receive the client metadata*/
	client_metadata_mr = rdma_buffer_register(pd /* which protection domain */,
	                     &client_metadata_attr /* what memory */,
	                     sizeof(client_metadata_attr) /* what length */,
	                     (IBV_ACCESS_LOCAL_WRITE) /* access permissions */);
	if (!client_metadata_mr)
	{
		rdma_error("Failed to register client attr buffer\n");
		//we assume ENOMEM
		return -ENOMEM;
	}
	/* We pre-post this receive buffer on the QP. SGE credentials is where we
	 * receive the metadata from the client */
	client_recv_sge.addr = (uint64_t) client_metadata_mr->addr; // same as &client_buffer_attr
	client_recv_sge.length = client_metadata_mr->length;
	client_recv_sge.lkey = client_metadata_mr->lkey;
	/* Now we link this SGE to the work request (WR) */
	bzero(&client_recv_wr, sizeof(client_recv_wr));
	client_recv_wr.sg_list = &client_recv_sge;
	client_recv_wr.num_sge = 1; // only one SGE
	ret = ibv_post_recv(client_qp /* which QP */,
	                    &client_recv_wr /* receive work request*/,
	                    &bad_client_recv_wr /* error WRs */);
	if (ret)
	{
		rdma_error("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return ret;
	}
	debug("Receive buffer pre-posting is successful \n");
	/* Now we accept the connection. Recall we have not accepted the connection
	 * yet because we have to do lots of resource pre-allocation */
	memset(&conn_param, 0, sizeof(conn_param));
	/* this tell how many outstanding requests can we handle */
	conn_param.initiator_depth = 3; /* For this exercise, we put a small number here */
	/* This tell how many outstanding requests we expect other side to handle */
	conn_param.responder_resources = 3; /* For this exercise, we put a small number */
	// cm_client_id is set in start_rdma_server, to the first client that connected.
	ret = rdma_accept(cm_client_id, &conn_param);
	if (ret)
	{
		rdma_error("Failed to accept the connection, errno: %d \n", -errno);
		return -errno;
	}
	/* We expect an RDMA_CM_EVNET_ESTABLISHED to indicate that the RDMA
	* connection has been established and everything is fine on both, server
	* as well as the client sides.
	*/
	debug("Going to wait for : RDMA_CM_EVENT_ESTABLISHED event \n");
	ret = process_rdma_cm_event(cm_event_channel,
	                            RDMA_CM_EVENT_ESTABLISHED,
	                            &cm_event);
	if (ret)
	{
		rdma_error("Failed to get the cm event, errnp: %d \n", -errno);
		return -errno;
	}
	/* We acknowledge the event */
	ret = rdma_ack_cm_event(cm_event);
	if (ret)
	{
		rdma_error("Failed to acknowledge the cm event %d\n", -errno);
		return -errno;
	}
	/* Just FYI: How to extract connection information */
	memcpy(&remote_sockaddr /* where to save */,
	       rdma_get_peer_addr(cm_client_id) /* gives you remote sockaddr */,
	       sizeof(struct sockaddr_in) /* max size */);
	printf("A new connection is accepted from %s \n",
	       inet_ntoa(remote_sockaddr.sin_addr));
	return ret;
}

/* This function sends server side buffer metadata to the connected client */
static int send_server_metadata_to_client()
{
	int ret = -1;

	// At this point we expect to have one work completion; the receival of
	// client meta data.
	struct ibv_wc wc[1];
	ret = process_work_completion_events(io_completion_channel, wc, 1);
	if (ret != 1)
	{
		rdma_error("We failed to get 1 work completions , ret = %d \n", ret);
		return ret;
	}

	debug("Client side buffer information is received...\n");
	show_rdma_buffer_attr(&client_metadata_attr);
	debug("The client has requested buffer length of : %d bytes\n", client_metadata_attr.length);

	// Allocate buffer to be used by client for RDMA.
	//buf_for_rwrite = calloc(client_metadata_attr.length, 0);
	buf_for_rwrite = block_mem[0];
	debug("Before register buf = %s   %p\n", buf_for_rwrite, buf_for_rwrite);
	server_buffer_mr = rdma_buffer_alloc1(pd, buf_for_rwrite, client_metadata_attr.length,
	                                      (IBV_ACCESS_REMOTE_READ |
	                                       IBV_ACCESS_LOCAL_WRITE | // Must be set when REMOTE_WRITE is set.
	                                       IBV_ACCESS_REMOTE_WRITE));

	// Prepare memory region which will be sent to client,
	// holding information required to access buffer allocated above.
	server_metadata_attr.address = (uint64_t)server_buffer_mr->addr;
	server_metadata_attr.length = server_buffer_mr->length;
	server_metadata_attr.stag.local_stag = server_buffer_mr->lkey;
	server_metadata_mr = rdma_buffer_register(pd,
	                     &server_metadata_attr,
	                     sizeof(server_metadata_attr),
	                     IBV_ACCESS_LOCAL_WRITE);
	if (!server_metadata_mr)
	{
		rdma_error("Failed to register the server metadata buffer, ret = %d \n", -errno);
		return -errno;
	}

	// Create sge which holds information required by client to access
	// the buffer allocated above.
	struct ibv_sge server_send_sge;
	server_send_sge.addr = (uint64_t)server_metadata_mr->addr;
	server_send_sge.length = server_metadata_mr->length;
	server_send_sge.lkey = server_metadata_mr->lkey;

	// Create work request to send to client
	struct ibv_send_wr server_send_wr;
	bzero(&server_send_wr, sizeof(server_send_wr));
	server_send_wr.sg_list = &server_send_sge;
	server_send_wr.num_sge = 1;
	server_send_wr.opcode = IBV_WR_SEND;
	// This is what's used on the client.
	// sq_sig_all is (implicitly) set to 0, so according to the docs (
	// man ibv_post_send(3)) this should be OK.
	server_send_wr.send_flags = IBV_SEND_SIGNALED;

	// Create WR used by ibv_post_send(3) to tell us which of the WRs
	// given was bad. Since we give only one value, it should be
	// bad_wr == server_send_wr in case of an error and
	// bad_wr == NULL if everything's OK.
	struct ibv_send_wr *bad_wr = NULL;

	//change  to 5
	int* tmp_cnt = (int*)(void*)buf_for_rwrite;
	*tmp_cnt = (int)(-1);
	debug("tmp_cnt=%d\n", *tmp_cnt );
	// Send WR to client.
	ret = ibv_post_send(client_qp, &server_send_wr, &bad_wr);
	debug("After11  post send  to sleep\n");
	long long L1, L2;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	L1 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
	int sh = 1;
	gettimeofday(&tv, NULL);
	L2 = tv.tv_sec * 1000 * 1000 + tv.tv_usec;
	printf("%d ops  duration =  %lld  micro seconds \n", (*tmp_cnt), L2 - L1);

	if (ret)
	{
		rdma_error("Failed to send server metadata, errno: %d\n", -ret);
		return -ret;
	}

	// Here, we could check how many work completions we have on our
	// io_completion_channel, to "ensure" that everything went well.
	// This is done in rdma_client.c->client_send_metadata_to_server.
	// I guess we don't really have to do that.
	return 0;
}

/* This is server side logic. Server passively waits for the client to call
 * rdma_disconnect() and then it will clean up its resources */
static int disconnect_and_cleanup()
{
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
	/* Now we wait for the client to send us disconnect event */
	debug("Waiting for cm event: RDMA_CM_EVENT_DISCONNECTED\n");
	ret = process_rdma_cm_event(cm_event_channel,
	                            RDMA_CM_EVENT_DISCONNECTED,
	                            &cm_event);
	if (ret)
	{
		rdma_error("Failed to get disconnect event, ret = %d \n", ret);
		return ret;
	}
	/* We acknowledge the event */
	ret = rdma_ack_cm_event(cm_event);
	if (ret)
	{
		rdma_error("Failed to acknowledge the cm event %d\n", -errno);
		return -errno;
	}
	printf("A disconnect event is received from the client...\n");
	/* We free all the resources */
	/* Destroy QP */
	rdma_destroy_qp(cm_client_id);
	/* Destroy client cm id */
	ret = rdma_destroy_id(cm_client_id);
	if (ret)
	{
		rdma_error("Failed to destroy client id cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy CQ */
	ret = ibv_destroy_cq(cq);
	if (ret)
	{
		rdma_error("Failed to destroy completion queue cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy completion channel */
	ret = ibv_destroy_comp_channel(io_completion_channel);
	if (ret)
	{
		rdma_error("Failed to destroy completion channel cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy memory buffers */
	rdma_buffer_free(server_buffer_mr);
	rdma_buffer_deregister(server_metadata_mr);
	rdma_buffer_deregister(client_metadata_mr);
	/* Destroy protection domain */
	ret = ibv_dealloc_pd(pd);
	if (ret)
	{
		rdma_error("Failed to destroy client protection domain cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy rdma server id */
	ret = rdma_destroy_id(cm_server_id);
	if (ret)
	{
		rdma_error("Failed to destroy server id cleanly, %d \n", -errno);
		// we continue anyways;
	}
	rdma_destroy_event_channel(cm_event_channel);
	printf("Server shut-down is complete \n");
	return 0;
}


void usage()
{
	printf("Usage:\n");
	printf("rdma_server: [-a <server_addr>] [-p <server_port>]\n");
	printf("(default port is %d)\n", DEFAULT_RDMA_PORT);
	exit(1);
}

int main(int argc, char **argv)
{
	for (int i = 0; i < BLOCK_NUM ; i++)
	{
		block_mem[i] = calloc(BLOCK_SZ, 1);
	}
	int ret, option;
	struct sockaddr_in server_sockaddr;
	bzero(&server_sockaddr, sizeof server_sockaddr);
	server_sockaddr.sin_family = AF_INET; /* standard IP NET address */
	server_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY); /* passed address */

	get_addr("12.12.10.17", (struct sockaddr*) &server_sockaddr);
	server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT); /* use default port */

	ret = start_rdma_server(&server_sockaddr);
	if (ret)
	{
		rdma_error("RDMA server failed to start cleanly, ret = %d \n", ret);
		return ret;
	}
	ret = setup_client_resources();
	if (ret)
	{
		rdma_error("Failed to setup client resources, ret = %d \n", ret);
		return ret;
	}
	ret = accept_client_connection();
	if (ret)
	{
		rdma_error("Failed to handle client cleanly, ret = %d \n", ret);
		return ret;
	}
	ret = send_server_metadata_to_client();
	if (ret)
	{
		rdma_error("Failed to send server metadata to the client, ret = %d \n", ret);
		return ret;
	}
	int* buf = (void*)block_mem[0];
	while (1 == 1)
	{
		printf("buf=%d\n", *buf);
		if (*buf > 0 )
		{
			printf("recv=%d\n", *buf );
			char* ddata = (void*)buf;
			ddata = ddata + sizeof(int);
			double* real_data = (void*)ddata;
			for (int j = 0; j < *buf; j++)
			{
				printf("%lf", real_data[j]);
			}
			printf("\n");
		}
		else
		{
			printf("no data\n");
		}
		sleep(1);
	}
	ret = disconnect_and_cleanup();
	if (ret)
	{
		rdma_error("Failed to clean up resources properly, ret = %d \n", ret);
		return ret;
	}
	return 0;
}
