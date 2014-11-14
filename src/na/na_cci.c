/*
 * Copyright (C) 2013 Argonne National Laboratory, Department of Energy,
 * Chicago Argonne, LLC and The HDF Group. All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification, and
 * redistribution, is contained in the COPYING file that can be found at the
 * root of the source code distribution tree.
 */

#include "na_cci.h"
#include "na_private.h"
#include "na_error.h"

#include "mercury_hash_table.h"
#include "mercury_queue.h"
#include "mercury_thread.h"
#include "mercury_thread_mutex.h"
#include "mercury_time.h"
#include "mercury_atomic.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/queue.h>

/****************/
/* Local Macros */
/****************/
/* Max tag */
#define NA_CCI_MAX_TAG (NA_TAG_UB >> 2)

/* Default tag used for one-sided over two-sided */
#define NA_CCI_RMA_REQUEST_TAG (NA_CCI_MAX_TAG + 1)

#define NA_CCI_PRIVATE_DATA(na_class) \
    ((struct na_cci_private_data *)(na_class->private_data))

/************************************/
/* Local Type and Struct Definition */
/************************************/

typedef uint32_t cci_msg_tag_t;
typedef uint64_t cci_size_t;
typedef uintptr_t cci_op_id_t;
typedef struct na_cci_addr na_cci_addr_t;
typedef struct na_cci_op_id na_cci_op_id_t;
typedef struct na_cci_mem_handle na_cci_mem_handle_t;

/* na_cci_addr */
struct na_cci_addr {
	cci_connection_t *cci_addr;	/* CCI addr */
	TAILQ_HEAD(prx,na_cci_op_id) rxs; /* Posted recvs */
	TAILQ_HEAD(erx,na_cci_info_recv_expected) early; /* Expected recvs not yet posted */
	char *uri;			/* Peer's URI */
	na_bool_t	unexpected;	/* Address generated from unexpected
					 * recv */
	na_bool_t	self;	/* Boolean for self */
};

struct na_cci_mem_handle {
	cci_rma_handle_t h;
	na_ptr_t	base;	/* Initial address of memory */
	na_size_t	size;	/* Size of memory */
	na_uint8_t	attr;	/* Flag of operation access */
};

typedef enum na_cci_rma_op {
	NA_CCI_RMA_PUT,		/* Request a put operation */
	NA_CCI_RMA_GET		/* Request a get operation */
}		na_cci_rma_op_t;

struct na_cci_info_lookup {
	na_addr_t	addr;
};

struct na_cci_info_send_unexpected {
	cci_op_id_t	op_id;	/* CCI operation ID */
};

struct na_cci_info_recv_unexpected {
	void           *buf;
	cci_size_t	buf_size;
	cci_size_t	actual_size;
	na_cci_addr_t	*na_cci_addr;
	cci_msg_tag_t	tag;
};

struct na_cci_info_send_expected {
	cci_op_id_t	op_id;	/* CCI operation ID */
};

struct na_cci_info_recv_expected {
	TAILQ_ENTRY(na_cci_info_recv_expected) entry;
	cci_op_id_t	op_id;	/* CCI operation ID */
	void		*buf;
	cci_size_t	buf_size;
	cci_size_t	actual_size;
	cci_msg_tag_t	tag;
};

struct na_cci_info_put {
	cci_op_id_t	request_op_id;
	cci_op_id_t	transfer_op_id;
	na_bool_t	transfer_completed;
	cci_size_t	transfer_actual_size;
	cci_op_id_t	completion_op_id;
	cci_size_t	completion_actual_size;
	na_bool_t	internal_progress;
	cci_connection_t *remote_addr;
};

struct na_cci_info_get {
	cci_op_id_t	request_op_id;
	cci_op_id_t	transfer_op_id;
	cci_size_t	transfer_actual_size;
	na_bool_t	internal_progress;
	cci_connection_t *remote_addr;
};

/* na_cci_op_id  TODO uint64_t cookie for cancel ? */
struct na_cci_op_id {
	TAILQ_ENTRY(na_cci_op_id) entry;
	na_context_t   *context;
	na_cb_type_t	type;
	na_cb_t		callback;	/* Callback */
	void           *arg;
	na_bool_t	completed;	/* Operation completed */
	union {
		struct na_cci_info_lookup lookup;
		struct na_cci_info_send_unexpected send_unexpected;
		struct na_cci_info_recv_unexpected recv_unexpected;
		struct na_cci_info_send_expected send_expected;
		struct na_cci_info_recv_expected recv_expected;
		struct na_cci_info_put put;
		struct na_cci_info_get get;
	}		info;
};

struct na_cci_private_data {
	cci_endpoint_t *endpoint;
	TAILQ_HEAD(crx,na_cci_op_id) early;	/* Unexpected rxs not yet posted */
	hg_thread_mutex_t test_unexpected_mutex;	/* Mutex */
	hg_queue_t     *unexpected_msg_queue;	/* Posted unexpected message queue */
	hg_thread_mutex_t unexpected_msg_queue_mutex;	/* Mutex */
	hg_queue_t     *unexpected_op_queue;	/* Unexpected op queue */
	hg_thread_mutex_t unexpected_op_queue_mutex;	/* Mutex */
};

typedef union cci_msg {
	struct msg_size {
		uint32_t	expect	: 1;
		uint32_t	tag	:31;
	} size;

	struct msg_send {
		uint32_t	expect	: 1;
		uint32_t	tag	:31;
		char		data[1];
	} send;

	uint32_t net;
} cci_msg_t;

/********************/
/* Local Prototypes */
/********************/

/* check_protocol */
static		na_bool_t
na_cci_check_protocol(
	const char *protocol_name
);

/* initialize */
static na_return_t
na_cci_initialize(
	na_class_t * na_class,
	const struct na_info *na_info,
	na_bool_t listen
);

/**
 * initialize
 *
 * \param method_list [IN]      (Optional) list of available methods depend on
 *                              CCI configuration, e.g., tcp, verbs, gni, sm, ...
 * \param listen_addr [IN]      (Optional) e.g., CCI URI
 */
static na_return_t
na_cci_init(
	    na_class_t * na_class,
	    const struct na_info *na_info
);

/* finalize */
static na_return_t
na_cci_finalize(
		na_class_t * na_class
);

/* addr_lookup */
static na_return_t
na_cci_addr_lookup(
		   na_class_t * na_class,
		   na_context_t * context,
		   na_cb_t callback,
		   void *arg,
		   const char *name,
		   na_op_id_t * op_id
);

/* addr_self */
static na_return_t
na_cci_addr_self(
		 na_class_t * na_class,
		 na_addr_t * addr
);

/* addr_free */
static na_return_t
na_cci_addr_free(
		 na_class_t * na_class,
		 na_addr_t addr
);

/* addr_is_self */
static		na_bool_t
na_cci_addr_is_self(
		    na_class_t * na_class,
		    na_addr_t addr
);

/* addr_to_string */
static na_return_t
na_cci_addr_to_string(
		      na_class_t * na_class,
		      char *buf,
		      na_size_t buf_size,
		      na_addr_t addr
);

/* msg_get_max */
static na_size_t
na_cci_msg_get_max_expected_size(
				 na_class_t * na_class
);

static na_size_t
na_cci_msg_get_max_unexpected_size(
				   na_class_t * na_class
);

static		na_tag_t
na_cci_msg_get_max_tag(
		       na_class_t * na_class
);

/* msg_send_unexpected */
static na_return_t
na_cci_msg_send_unexpected(
			   na_class_t * na_class,
			   na_context_t * context,
			   na_cb_t callback,
			   void *arg,
			   const void *buf,
			   na_size_t buf_size,
			   na_addr_t dest,
			   na_tag_t tag,
			   na_op_id_t * op_id
);

/* msg_recv_unexpected */
static na_return_t
na_cci_msg_recv_unexpected(
			   na_class_t * na_class,
			   na_context_t * context,
			   na_cb_t callback,
			   void *arg,
			   void *buf,
			   na_size_t buf_size,
			   na_op_id_t * op_id
);

/* msg_send_expected */
static na_return_t
na_cci_msg_send_expected(
			 na_class_t * na_class,
			 na_context_t * context,
			 na_cb_t callback,
			 void *arg,
			 const void *buf,
			 na_size_t buf_size,
			 na_addr_t dest,
			 na_tag_t tag,
			 na_op_id_t * op_id
);

/* msg_recv_expected */
static na_return_t
na_cci_msg_recv_expected(
			 na_class_t * na_class,
			 na_context_t * context,
			 na_cb_t callback,
			 void *arg,
			 void *buf,
			 na_size_t buf_size,
			 na_addr_t source,
			 na_tag_t tag,
			 na_op_id_t * op_id
);

static na_return_t
na_cci_msg_unexpected_push(
			   na_class_t * na_class,
			   struct na_cci_info_recv_unexpected *rx
);

static struct na_cci_info_recv_unexpected *
na_cci_msg_unexpected_pop(
			  na_class_t * na_class);

static na_return_t
na_cci_msg_unexpected_op_push(
			      na_class_t * na_class,
			      struct na_cci_op_id *na_cci_op_id
);

static struct na_cci_op_id *
na_cci_msg_unexpected_op_pop(
			     na_class_t * na_class
);

/* mem_handle */
static na_return_t
na_cci_mem_handle_create(
			 na_class_t * na_class,
			 void *buf,
			 na_size_t buf_size,
			 unsigned long flags,
			 na_mem_handle_t * mem_handle
);

static na_return_t
na_cci_mem_handle_free(
		       na_class_t * na_class,
		       na_mem_handle_t mem_handle
);

static na_return_t
na_cci_mem_register(
		    na_class_t * na_class,
		    na_mem_handle_t mem_handle
);

static na_return_t
na_cci_mem_deregister(
		      na_class_t * na_class,
		      na_mem_handle_t mem_handle
);

/* mem_handle serialization */
static na_size_t
na_cci_mem_handle_get_serialize_size(
				     na_class_t * na_class,
				     na_mem_handle_t mem_handle
);

static na_return_t
na_cci_mem_handle_serialize(
			    na_class_t * na_class,
			    void *buf,
			    na_size_t buf_size,
			    na_mem_handle_t mem_handle
);

static na_return_t
na_cci_mem_handle_deserialize(
			      na_class_t * na_class,
			      na_mem_handle_t * mem_handle,
			      const void *buf,
			      na_size_t buf_size
);

/* put */
static na_return_t
na_cci_put(
	   na_class_t * na_class,
	   na_context_t * context,
	   na_cb_t callback,
	   void *arg,
	   na_mem_handle_t local_mem_handle,
	   na_offset_t local_offset,
	   na_mem_handle_t remote_mem_handle,
	   na_offset_t remote_offset,
	   na_size_t length,
	   na_addr_t remote_addr,
	   na_op_id_t * op_id
);

/* get */
static na_return_t
na_cci_get(
	   na_class_t * na_class,
	   na_context_t * context,
	   na_cb_t callback,
	   void *arg,
	   na_mem_handle_t local_mem_handle,
	   na_offset_t local_offset,
	   na_mem_handle_t remote_mem_handle,
	   na_offset_t remote_offset,
	   na_size_t length,
	   na_addr_t remote_addr,
	   na_op_id_t * op_id
);

/* progress */
static na_return_t
na_cci_progress(
		na_class_t * na_class,
		na_context_t * context,
		unsigned int timeout
);

static na_return_t
na_cci_complete(
		struct na_cci_op_id *na_cci_op_id
);

static void
na_cci_release(
	       struct na_cb_info *callback_info,
	       void *arg
);

/* cancel */
static na_return_t
na_cci_cancel(
	      na_class_t * na_class,
	      na_context_t * context,
	      na_op_id_t op_id
);

/*******************/
/* Local Variables */
/*******************/

const na_class_t na_cci_class_g = {
	NULL,			/* private_data */
	"cci",			/* name */
	na_cci_check_protocol,	/* check_protocol */
	na_cci_initialize,	/* initialize */
	na_cci_finalize,	/* finalize */
	NULL,			/* context_create */
	NULL,			/* context_destroy */
	na_cci_addr_lookup,	/* addr_lookup */
	na_cci_addr_free,	/* addr_free */
	na_cci_addr_self,	/* addr_self */
	NULL,			/* addr_dup */
	na_cci_addr_is_self,	/* addr_is_self */
	na_cci_addr_to_string,	/* addr_to_string */
	na_cci_msg_get_max_expected_size,	/* msg_get_max_expected_size */
	na_cci_msg_get_max_unexpected_size,	/* msg_get_max_expected_size */
	na_cci_msg_get_max_tag,	/* msg_get_max_tag */
	na_cci_msg_send_unexpected,	/* msg_send_unexpected */
	na_cci_msg_recv_unexpected,	/* msg_recv_unexpected */
	na_cci_msg_send_expected,	/* msg_send_expected */
	na_cci_msg_recv_expected,	/* msg_recv_expected */
	na_cci_mem_handle_create,	/* mem_handle_create */
	NULL,			/* mem_handle_create_segment */
	na_cci_mem_handle_free,	/* mem_handle_free */
	na_cci_mem_register,	/* mem_register */
	na_cci_mem_deregister,	/* mem_deregister */
	NULL,			/* mem_publish */
	NULL,			/* mem_unpublish */
	na_cci_mem_handle_get_serialize_size,	/* mem_handle_get_serialize_size
						 * */
	na_cci_mem_handle_serialize,	/* mem_handle_serialize */
	na_cci_mem_handle_deserialize,	/* mem_handle_deserialize */
	na_cci_put,		/* put */
	na_cci_get,		/* get */
	na_cci_progress,	/* progress */
	na_cci_cancel		/* cancel */
};

/********************/
/* Plugin callbacks */
/********************/

static NA_INLINE int
pointer_equal(void *location1, void *location2)
{
	return location1 == location2;
}

/*---------------------------------------------------------------------------*/
static na_bool_t
na_cci_check_protocol(const char *protocol_name)
{
	na_bool_t	accept = NA_FALSE;
	int		ret = 0;
	uint32_t	caps = 0;
	cci_device_t   *const *devices, *device = NULL;

	/*
	 * init CCI, get_devices, and check if a device on this transport
	 * exists and is up
	 */

	/* Initialize CCI */
	ret = cci_init(CCI_ABI_VERSION, 0, &caps);
	if (ret) {
		NA_LOG_ERROR("cci_init() failed with %s",
			     cci_strerror(NULL, ret));
		goto out;
	}
	/* Get the available devices */
	ret = cci_get_devices(&devices);
	if (ret) {
		NA_LOG_ERROR("cci_get_devices() failed with %s",
			     cci_strerror(NULL, ret));
		goto out;
	}
	for (device = devices[0]; device != NULL; device++) {
		if (!strcmp(device->transport, protocol_name)) {
			if (!device->up) {
				NA_LOG_ERROR("device %s (transport %s) is down",
					     device->name, device->transport);
				continue;
			}
			break;
		}
	}

	if (!device) {
		NA_LOG_ERROR("requested transport %s is not available",
			     protocol_name);
		goto out;
	}
	if (device)
		accept = NA_TRUE;

out:
	return accept;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_initialize(na_class_t * na_class, const struct na_info *na_info,
		  na_bool_t listen)
{
	int rc = 0;
	uint32_t caps = 0;
	cci_device_t * const *devices = NULL, *device = NULL;
	cci_endpoint_t *endpoint = NULL;
	char *uri = NULL;
	na_return_t ret = NA_SUCCESS;
	hg_queue_t     *unexpected_msg_queue = NULL;
	hg_queue_t     *unexpected_op_queue = NULL;

	/* Initialize CCI */
	rc = cci_init(CCI_ABI_VERSION, 0, &caps);
	if (rc) {
		NA_LOG_ERROR("cci_init() failed with %s",
			     cci_strerror(NULL, rc));
		goto out;
	}

	/* Get the available devices */
	rc = cci_get_devices(&devices);
	if (rc) {
		NA_LOG_ERROR("cci_get_devices() failed with %s",
				cci_strerror(NULL, rc));
		goto out;
	}

	for (device = devices[0]; device != NULL; device++) {
		if (!strcmp(device->transport, na_info->protocol_name)) {
			if (!device->up) {
				NA_LOG_ERROR("device %s tranport %s is down",
						device->name, device->transport);
				continue;
			}
			break;
		}
	}

	na_class->private_data = malloc(sizeof(struct na_cci_private_data));
	if (!na_class->private_data) {
		NA_LOG_ERROR("Could not allocate NA private data class");
		ret = NA_NOMEM_ERROR;
		goto out;
	}

	/* Create an endpoint using the requested transport */
	rc = cci_create_endpoint(device, 0, &endpoint, NULL);
	if (rc) {
		NA_LOG_ERROR("cci_create_endpoint() failed with %s",
				cci_strerror(NULL, rc));
		goto out;
	}
	NA_CCI_PRIVATE_DATA(na_class)->endpoint = endpoint;


	rc = cci_get_opt(endpoint, CCI_OPT_ENDPT_URI, &uri);
	if (rc) {
		NA_LOG_ERROR("cci_get_opt(URI) failed with %s",
				cci_strerror(endpoint, rc));
		goto out;
	}

	fprintf(stderr, "opened %s\n", uri);
	ret = na_cci_init(na_class, na_info);

out:
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_init(na_class_t * na_class, const struct na_info *na_info)
{
	hg_queue_t     *unexpected_msg_queue = NULL;
	hg_queue_t     *unexpected_op_queue = NULL;
	na_return_t	ret = NA_SUCCESS;
	int		rc;

	na_class->private_data = malloc(sizeof(struct na_cci_private_data));
	if (!na_class->private_data) {
		NA_LOG_ERROR("Could not allocate NA private data class");
		ret = NA_NOMEM_ERROR;
		goto out;
	}

	/* Create queue for unexpected messages */
	unexpected_msg_queue = hg_queue_new();
	if (!unexpected_msg_queue) {
		NA_LOG_ERROR("Could not create unexpected message queue");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue = unexpected_msg_queue;

	/* Create queue for making progress on operation IDs */
	unexpected_op_queue = hg_queue_new();
	if (!unexpected_op_queue) {
		NA_LOG_ERROR("Could not create unexpected op queue");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue = unexpected_op_queue;

	/* Initialize mutex/cond */
	hg_thread_mutex_init(&NA_CCI_PRIVATE_DATA(na_class)->test_unexpected_mutex);
	hg_thread_mutex_init(
		  &NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue_mutex);
	hg_thread_mutex_init(
		   &NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue_mutex);

out:
	if (ret != NA_SUCCESS) {
		na_cci_finalize(na_class);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_finalize(na_class_t * na_class)
{
	na_return_t	ret = NA_SUCCESS;
	int		rc;

	/* Check that unexpected op queue is empty */
	if (!hg_queue_is_empty(
			NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue)) {
		NA_LOG_ERROR("Unexpected op queue should be empty");
		ret = NA_PROTOCOL_ERROR;
	}
	/* Free unexpected op queue */
	hg_queue_free(NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue);

	/* Check that unexpected message queue is empty */
	if (!hg_queue_is_empty(
		       NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue)) {
		NA_LOG_ERROR("Unexpected msg queue should be empty");
		ret = NA_PROTOCOL_ERROR;
	}
	/* Free unexpected message queue */
	hg_queue_free(NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue);

	/* Finalize CCI */
	rc = cci_finalize();
	if (rc) {
		NA_LOG_ERROR("CCI_finalize() failed with %s",
				cci_strerror(NULL, rc));
		ret = NA_PROTOCOL_ERROR;
	}
	/* Destroy mutex/cond */
	hg_thread_mutex_destroy(
		       &NA_CCI_PRIVATE_DATA(na_class)->test_unexpected_mutex);
	hg_thread_mutex_destroy(
		  &NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue_mutex);
	hg_thread_mutex_destroy(
		   &NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue_mutex);

	free(na_class->private_data);

	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_addr_lookup(na_class_t NA_UNUSED * na_class, na_context_t * context,
	    na_cb_t callback, void *arg, const char *name, na_op_id_t * op_id)
{
	struct na_cci_op_id *na_cci_op_id = NULL;
	na_cci_addr_t *na_cci_addr = NULL;
	na_return_t	ret = NA_SUCCESS;
	int		rc;

	/* Allocate op_id */
	na_cci_op_id = (struct na_cci_op_id *)malloc(sizeof(struct na_cci_op_id));
	if (!na_cci_op_id) {
		NA_LOG_ERROR("Could not allocate NA CCI operation ID");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	na_cci_op_id->context = context;
	na_cci_op_id->type = NA_CB_LOOKUP;
	na_cci_op_id->callback = callback;
	na_cci_op_id->arg = arg;
	na_cci_op_id->completed = NA_FALSE;

	/* Allocate addr */
	na_cci_addr = (na_cci_addr_t *)malloc(sizeof(struct na_cci_addr));
	if (!na_cci_addr) {
		NA_LOG_ERROR("Could not allocate CCI addr");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	na_cci_addr->cci_addr = NULL;
	TAILQ_INIT(&na_cci_addr->rxs);
	TAILQ_INIT(&na_cci_addr->early);
	na_cci_addr->unexpected = NA_FALSE;
	na_cci_addr->self = NA_FALSE;
	na_cci_op_id->info.lookup.addr = (na_addr_t) na_cci_addr;

	/* Assign op_id */
	*op_id = (na_op_id_t) na_cci_op_id;

out:
	if (ret != NA_SUCCESS) {
		free(na_cci_addr);
		free(na_cci_op_id);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_addr_self(na_class_t NA_UNUSED * na_class, na_addr_t * addr)
{
	na_cci_addr_t *na_cci_addr = NULL;
	na_return_t	ret = NA_SUCCESS;

	/* Allocate addr */
	na_cci_addr = (na_cci_addr_t *)malloc(sizeof(struct na_cci_addr));
	if (!na_cci_addr) {
		NA_LOG_ERROR("Could not allocate CCI addr");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	na_cci_addr->cci_addr = 0;
	na_cci_addr->unexpected = NA_FALSE;
	na_cci_addr->self = NA_TRUE;

	*addr = (na_addr_t) na_cci_addr;

out:
	if (ret != NA_SUCCESS) {
		free(na_cci_addr);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_addr_free(na_class_t NA_UNUSED * na_class, na_addr_t addr)
{
	na_cci_addr_t *na_cci_addr = (struct na_cci_addr *)addr;
	na_return_t	ret = NA_SUCCESS;

	/* Cleanup peer_addr */
	if (!na_cci_addr) {
		NA_LOG_ERROR("NULL CCI addr");
		ret = NA_INVALID_PARAM;
		return ret;
	}
	free(na_cci_addr);
	na_cci_addr = NULL;

	return ret;
}

/*---------------------------------------------------------------------------*/
static na_bool_t
na_cci_addr_is_self(na_class_t NA_UNUSED * na_class, na_addr_t addr)
{
	na_cci_addr_t *na_cci_addr = (struct na_cci_addr *)addr;

	return na_cci_addr->self;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_addr_to_string(na_class_t NA_UNUSED * na_class, char *buf,
		      na_size_t buf_size, na_addr_t addr)
{
	na_cci_addr_t *na_cci_addr = NULL;
	const char     *cci_rev_addr;
	na_return_t	ret = NA_SUCCESS;

	na_cci_addr = (na_cci_addr_t *)addr;

	if (strlen(na_cci_addr->uri) > buf_size) {
		NA_LOG_ERROR("Buffer size too small to copy addr");
		ret = NA_SIZE_ERROR;
		return ret;
	}
	strcpy(buf, na_cci_addr->uri);

	return ret;
}

/*---------------------------------------------------------------------------*/
static na_size_t
na_cci_msg_get_max_expected_size(na_class_t *na_class)
{
	na_size_t max_expected_size =
		NA_CCI_PRIVATE_DATA(na_class)->endpoint->device->max_send_size;

	return max_expected_size;
}

/*---------------------------------------------------------------------------*/
static na_size_t
na_cci_msg_get_max_unexpected_size(na_class_t *na_class)
{
	na_size_t max_unexpected_size =
		NA_CCI_PRIVATE_DATA(na_class)->endpoint->device->max_send_size;

	return max_unexpected_size;
}

/*---------------------------------------------------------------------------*/
static na_tag_t
na_cci_msg_get_max_tag(na_class_t NA_UNUSED * na_class)
{
	na_tag_t	max_tag = NA_CCI_MAX_TAG;

	return max_tag;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_msg_send_unexpected(na_class_t *na_class,
	 na_context_t * context, na_cb_t callback, void *arg, const void *buf,
	 na_size_t buf_size, na_addr_t dest, na_tag_t tag, na_op_id_t * op_id)
{
	cci_size_t	cci_buf_size = (cci_size_t) buf_size;
	na_cci_addr_t *na_cci_addr = (struct na_cci_addr *)dest;
	struct na_cci_op_id *na_cci_op_id = NULL;
	na_return_t	ret = NA_SUCCESS;
	int		rc;
	cci_msg_t	msg;
	struct iovec	iov[2];

	/* Allocate op_id */
	na_cci_op_id = (struct na_cci_op_id *)malloc(sizeof(struct na_cci_op_id));
	if (!na_cci_op_id) {
		NA_LOG_ERROR("Could not allocate NA CCI operation ID");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	na_cci_op_id->context = context;
	na_cci_op_id->type = NA_CB_SEND_UNEXPECTED;
	na_cci_op_id->callback = callback;
	na_cci_op_id->arg = arg;
	na_cci_op_id->completed = NA_FALSE;
	na_cci_op_id->info.send_unexpected.op_id = 0;

	msg.send.expect = 0;
	msg.send.tag = tag;

	iov[0].iov_base = &msg;
	iov[0].iov_len = sizeof(msg.size);
	iov[1].iov_base = (void*) buf;
	iov[1].iov_len = buf_size;

	/* Post the CCI unexpected send request */
	rc = cci_sendv(na_cci_addr->cci_addr, iov, 2, na_cci_op_id, 0);
	if (rc) {
		cci_endpoint_t *endpoint = NA_CCI_PRIVATE_DATA(na_class)->endpoint;
		NA_LOG_ERROR("cci_sendv() failed with %s",
				cci_strerror(endpoint, rc));
		ret = NA_PROTOCOL_ERROR;
		goto out;
	}
	/* If immediate completion, directly add to completion queue */
	if (rc) {
		ret = na_cci_complete(na_cci_op_id);
		if (ret != NA_SUCCESS) {
			NA_LOG_ERROR("Could not complete operation");
			goto out;
		}
	}
	/* Assign op_id */
	*op_id = (na_op_id_t) na_cci_op_id;

out:
	if (ret != NA_SUCCESS) {
		free(na_cci_op_id);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_msg_recv_unexpected(na_class_t * na_class, na_context_t * context,
		   na_cb_t callback, void *arg, void *buf, na_size_t buf_size,
			   na_op_id_t * op_id)
{
	struct na_cci_op_id *na_cci_op_id = NULL;
	struct na_cci_info_recv_unexpected *rx = NULL;
	na_bool_t	progressed = NA_FALSE;
	na_return_t	ret = NA_SUCCESS;

	/* Allocate na_op_id */
	na_cci_op_id = (struct na_cci_op_id *)malloc(sizeof(struct na_cci_op_id));
	if (!na_cci_op_id) {
		NA_LOG_ERROR("Could not allocate NA CCI operation ID");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	na_cci_op_id->context = context;
	na_cci_op_id->type = NA_CB_RECV_UNEXPECTED;
	na_cci_op_id->callback = callback;
	na_cci_op_id->arg = arg;
	na_cci_op_id->completed = NA_FALSE;
	na_cci_op_id->info.recv_unexpected.buf = buf;
	na_cci_op_id->info.recv_unexpected.buf_size = (cci_size_t) buf_size;

	/* Look for an unexpected message already received */
	rx = na_cci_msg_unexpected_pop(na_class);

	if (rx) {
		na_size_t msg_len = rx->buf_size;

		if (na_cci_op_id->info.recv_unexpected.buf_size < msg_len)
			msg_len = na_cci_op_id->info.recv_unexpected.buf_size;
		memcpy(na_cci_op_id->info.recv_unexpected.buf, rx->buf, msg_len);
		na_cci_op_id->info.recv_unexpected.actual_size = msg_len;
		na_cci_op_id->info.recv_unexpected.na_cci_addr = rx->na_cci_addr;
		na_cci_op_id->info.recv_unexpected.tag = rx->tag;

		free(rx->buf);
		free(rx);

		ret = na_cci_complete(na_cci_op_id);
		if (ret != NA_SUCCESS) {
			NA_LOG_ERROR("Could not complete operation");
			goto out;
		}
	} else {
		/* Nothing has been received yet so add op_id to progress queue */
		ret = na_cci_msg_unexpected_op_push(na_class, na_cci_op_id);
		if (ret != NA_SUCCESS) {
			NA_LOG_ERROR("Could not push operation ID");
			goto out;
		}
	}

	/* Assign op_id */
	*op_id = (na_op_id_t) na_cci_op_id;

out:
	if (ret != NA_SUCCESS) {
		free(na_cci_op_id);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_msg_unexpected_push(na_class_t * na_class,
			   struct na_cci_info_recv_unexpected *rx)
{
	na_return_t	ret = NA_SUCCESS;

	if (!rx) {
		NA_LOG_ERROR("NULL unexpected info");
		ret = NA_INVALID_PARAM;
		goto out;
	}
	hg_thread_mutex_lock(
		  &NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue_mutex);

	if (!hg_queue_push_head(NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue,
				(hg_queue_value_t) rx)) {
		NA_LOG_ERROR("Could not push unexpected info to unexpected msg queue");
		ret = NA_NOMEM_ERROR;
	}
	hg_thread_mutex_unlock(
		  &NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue_mutex);

out:
	return ret;
}

/*---------------------------------------------------------------------------*/
static struct na_cci_info_recv_unexpected *
na_cci_msg_unexpected_pop(na_class_t * na_class)
{
	struct na_cci_info_recv_unexpected *rx;
	hg_queue_value_t queue_value;

	hg_thread_mutex_lock(
		  &NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue_mutex);

	queue_value = hg_queue_pop_tail(
			 NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue);
	rx = (queue_value != HG_QUEUE_NULL) ?
		(struct na_cci_info_recv_unexpected *)queue_value : NULL;

	hg_thread_mutex_unlock(
		  &NA_CCI_PRIVATE_DATA(na_class)->unexpected_msg_queue_mutex);

	return rx;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_msg_unexpected_op_push(na_class_t * na_class,
			      struct na_cci_op_id *na_cci_op_id)
{
	na_return_t	ret = NA_SUCCESS;

	if (!na_cci_op_id) {
		NA_LOG_ERROR("NULL operation ID");
		ret = NA_INVALID_PARAM;
		goto done;
	}
	hg_thread_mutex_lock(&NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue_mutex);

	if (!hg_queue_push_head(NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue,
				(hg_queue_value_t) na_cci_op_id)) {
		NA_LOG_ERROR("Could not push ID to unexpected op queue");
		ret = NA_NOMEM_ERROR;
	}
	hg_thread_mutex_unlock(
		   &NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue_mutex);

done:
	return ret;
}

/*---------------------------------------------------------------------------*/
static struct na_cci_op_id *
na_cci_msg_unexpected_op_pop(na_class_t * na_class)
{
	struct na_cci_op_id *na_cci_op_id;
	hg_queue_value_t queue_value;

	hg_thread_mutex_lock(&NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue_mutex);

	queue_value = hg_queue_pop_tail(
			  NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue);
	na_cci_op_id = (queue_value != HG_QUEUE_NULL) ?
		(struct na_cci_op_id *)queue_value : NULL;

	hg_thread_mutex_unlock(
		   &NA_CCI_PRIVATE_DATA(na_class)->unexpected_op_queue_mutex);

	return na_cci_op_id;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_msg_send_expected(na_class_t *na_class, na_context_t * context,
	     na_cb_t callback, void *arg, const void *buf, na_size_t buf_size,
			 na_addr_t dest, na_tag_t tag, na_op_id_t * op_id)
{
	cci_size_t	cci_buf_size = (cci_size_t) buf_size;
	na_cci_addr_t *na_cci_addr = (struct na_cci_addr *)dest;
	struct na_cci_op_id *na_cci_op_id = NULL;
	na_return_t	ret = NA_SUCCESS;
	int		rc;
	cci_msg_t	msg;
	struct iovec	iov[2];

	/* Allocate op_id */
	na_cci_op_id = (struct na_cci_op_id *)malloc(sizeof(struct na_cci_op_id));
	if (!na_cci_op_id) {
		NA_LOG_ERROR("Could not allocate NA CCI operation ID");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	na_cci_op_id->context = context;
	na_cci_op_id->type = NA_CB_SEND_EXPECTED;
	na_cci_op_id->callback = callback;
	na_cci_op_id->arg = arg;
	na_cci_op_id->completed = NA_FALSE;
	na_cci_op_id->info.send_expected.op_id = 0;

	msg.send.expect = 1;
	msg.send.tag = tag;

	iov[0].iov_base = &msg;
	iov[0].iov_len = sizeof(msg.size);
	iov[1].iov_base = (void*) buf;
	iov[1].iov_len = buf_size;

	/* Post the CCI send request */
	rc = cci_sendv(na_cci_addr->cci_addr, iov, 2, na_cci_op_id, 0);
	if (rc) {
		cci_endpoint_t *endpoint = NA_CCI_PRIVATE_DATA(na_class)->endpoint;
		NA_LOG_ERROR("cci_sendv() failed with %s",
				cci_strerror(endpoint, rc));
		ret = NA_PROTOCOL_ERROR;
		goto out;
	}
	/* If immediate completion, directly add to completion queue */
	if (rc) {
		ret = na_cci_complete(na_cci_op_id);
		if (ret != NA_SUCCESS) {
			NA_LOG_ERROR("Could not complete operation");
			goto out;
		}
	}
	/* Assign op_id */
	*op_id = (na_op_id_t) na_cci_op_id;

out:
	if (ret != NA_SUCCESS) {
		free(na_cci_op_id);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_msg_recv_expected(na_class_t NA_UNUSED * na_class, na_context_t * context,
		   na_cb_t callback, void *arg, void *buf, na_size_t buf_size,
			 na_addr_t source, na_tag_t tag, na_op_id_t * op_id)
{
	cci_size_t	cci_buf_size = (cci_size_t) buf_size;
	na_cci_addr_t *na_cci_addr = (struct na_cci_addr *)source;
	cci_msg_tag_t	cci_tag = (cci_msg_tag_t) tag;
	struct na_cci_info_recv_expected *rx = NULL;
	struct na_cci_op_id *na_cci_op_id = NULL;
	na_return_t	ret = NA_SUCCESS;
	int		rc;

	/* Allocate na_op_id */
	na_cci_op_id = (struct na_cci_op_id *)calloc(1, sizeof(*na_cci_op_id));
	if (!na_cci_op_id) {
		NA_LOG_ERROR("Could not allocate NA CCI operation ID");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	na_cci_op_id->context = context;
	na_cci_op_id->type = NA_CB_RECV_EXPECTED;
	na_cci_op_id->callback = callback;
	na_cci_op_id->arg = arg;
	na_cci_op_id->completed = NA_FALSE;
	na_cci_op_id->info.recv_expected.op_id = 0;
	na_cci_op_id->info.recv_expected.buf = buf;
	na_cci_op_id->info.recv_expected.buf_size = cci_buf_size;
	na_cci_op_id->info.recv_expected.actual_size = 0;
	na_cci_op_id->info.recv_expected.tag = cci_tag;

	/* See if it has already arrived */
	if (!TAILQ_EMPTY(&na_cci_addr->early)) {
		TAILQ_FOREACH(rx, &na_cci_addr->early, entry) {
			if (rx->tag == cci_tag) {
				/* Found, copy to final buffer, and complete it */
				na_size_t len = buf_size > rx->buf_size ?
					buf_size : rx->buf_size;
				memcpy(buf, rx->buf, len);
				na_cci_op_id->info.recv_expected.actual_size = len;
				TAILQ_REMOVE(&na_cci_addr->early, rx, entry);
				free(rx->buf);
				free(rx);
				ret = na_cci_complete(na_cci_op_id);
				if (ret != NA_SUCCESS) {
					NA_LOG_ERROR("Could not complete operation");
				}
				goto out;
			}
		}
	}

	/* Queue the recv request */
	TAILQ_INSERT_TAIL(&na_cci_addr->rxs, na_cci_op_id, entry);

	/* Assign op_id */
	*op_id = (na_op_id_t) na_cci_op_id;

out:
	if (ret != NA_SUCCESS) {
		free(na_cci_op_id);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_mem_handle_create(na_class_t NA_UNUSED * na_class, void *buf,
	na_size_t buf_size, unsigned long flags, na_mem_handle_t * mem_handle)
{
	na_ptr_t	cci_buf_base = (na_ptr_t) buf;
	na_cci_mem_handle_t *na_cci_mem_handle = NULL;
	cci_size_t	cci_buf_size = (cci_size_t) buf_size;
	na_return_t	ret = NA_SUCCESS;

	/* Allocate memory handle (use calloc to avoid uninitialized transfer) */
	na_cci_mem_handle = (na_cci_mem_handle_t *)
		calloc(1, sizeof(na_cci_mem_handle_t));
	if (!na_cci_mem_handle) {
		NA_LOG_ERROR("Could not allocate NA CCI memory handle");
		ret = NA_NOMEM_ERROR;
		goto out;
	}

	na_cci_mem_handle->base = cci_buf_base;
	na_cci_mem_handle->size = cci_buf_size;
	na_cci_mem_handle->attr = flags;

	*mem_handle = (na_mem_handle_t) na_cci_mem_handle;

out:
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_mem_handle_free(na_class_t NA_UNUSED * na_class,
		       na_mem_handle_t mem_handle)
{
	na_cci_mem_handle_t *cci_mem_handle = (na_cci_mem_handle_t *)mem_handle;

	free(cci_mem_handle);

	return NA_SUCCESS;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_mem_register(na_class_t *na_class, na_mem_handle_t mem_handle)
{
	na_cci_mem_handle_t *na_cci_mem_handle = mem_handle;
	cci_endpoint_t *e = NA_CCI_PRIVATE_DATA(na_class)->endpoint;
	cci_rma_handle_t *h = NULL;
	int rc = 0, flags = CCI_FLAG_READ;
	na_return_t ret = NA_SUCCESS;

	if (na_cci_mem_handle->attr & NA_MEM_READWRITE)
		flags |= CCI_FLAG_WRITE;

	rc = cci_rma_register(e, (void*)na_cci_mem_handle->base,
			na_cci_mem_handle->size, flags, &h);
	if (rc) {
		NA_LOG_ERROR("cci_rma_register() failed with %s", cci_strerror(e, rc));
		ret = NA_PROTOCOL_ERROR;
		goto out;
	}

	memcpy((void*)&na_cci_mem_handle->h, h, sizeof(*h));

out:
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_mem_deregister(na_class_t *na_class, na_mem_handle_t mem_handle)
{
	na_cci_mem_handle_t *na_cci_mem_handle = mem_handle;
	cci_endpoint_t *e = NA_CCI_PRIVATE_DATA(na_class)->endpoint;
	int rc = 0;
	na_return_t ret = NA_SUCCESS;

	rc = cci_rma_deregister(e, &na_cci_mem_handle->h);
	if (rc) {
		NA_LOG_ERROR("cci_rma_deregister() failed with %s", cci_strerror(e, rc));
		ret = NA_PROTOCOL_ERROR;
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_size_t
na_cci_mem_handle_get_serialize_size(na_class_t NA_UNUSED * na_class,
				     na_mem_handle_t mem_handle)
{
	na_cci_mem_handle_t *na_cci_mem_handle = mem_handle;

	return sizeof(*na_cci_mem_handle);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_mem_handle_serialize(na_class_t NA_UNUSED * na_class, void *buf,
			    na_size_t buf_size, na_mem_handle_t mem_handle)
{
	na_cci_mem_handle_t *na_cci_mem_handle =
				(na_cci_mem_handle_t *)mem_handle;
	na_return_t	ret = NA_SUCCESS;
	na_size_t	len = sizeof(*na_cci_mem_handle);

	if (buf_size < len) {
		NA_LOG_ERROR("Buffer size too small for serializing parameter");
		ret = NA_SIZE_ERROR;
		goto out;
	}
	/* Copy struct */
	memcpy(buf, na_cci_mem_handle, len);

out:
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_mem_handle_deserialize(na_class_t NA_UNUSED * na_class,
	    na_mem_handle_t * mem_handle, const void *buf, na_size_t buf_size)
{
	na_cci_mem_handle_t *na_cci_mem_handle = NULL;
	na_return_t	ret = NA_SUCCESS;
	na_size_t	len = sizeof(*na_cci_mem_handle);

	if (buf_size < len) {
		NA_LOG_ERROR("Buffer size too small for deserializing parameter");
		ret = NA_SIZE_ERROR;
		goto out;
	}
	na_cci_mem_handle = (na_cci_mem_handle_t *) calloc(1, len);
	if (!na_cci_mem_handle) {
		NA_LOG_ERROR("Could not allocate NA CCI memory handle");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	/* Copy struct */
	memcpy(na_cci_mem_handle, buf, len);

	*mem_handle = (na_mem_handle_t) na_cci_mem_handle;

out:
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_put(na_class_t * na_class, na_context_t * context, na_cb_t callback,
	void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
	   na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
	   na_size_t length, na_addr_t remote_addr, na_op_id_t * op_id)
{
	na_cci_mem_handle_t *cci_local_mem_handle = (na_cci_mem_handle_t *)local_mem_handle;
	cci_size_t	cci_local_offset = (cci_size_t) local_offset;
	na_cci_mem_handle_t *cci_remote_mem_handle = (na_cci_mem_handle_t *)remote_mem_handle;
	cci_size_t	cci_remote_offset = (cci_size_t) remote_offset;
	na_cci_addr_t *na_cci_addr = (struct na_cci_addr *)remote_addr;
	cci_size_t	cci_length = (cci_size_t) length;
	struct na_cci_op_id *na_cci_op_id = NULL;
	na_return_t	ret = NA_SUCCESS;
	int		rc;
	cci_endpoint_t *e = NA_CCI_PRIVATE_DATA(na_class)->endpoint;
	cci_connection_t *c = na_cci_addr->cci_addr;
	cci_rma_handle_t *local = &cci_local_mem_handle->h;
	cci_rma_handle_t *remote = &cci_remote_mem_handle->h;;

	if (cci_remote_mem_handle->attr != NA_MEM_READWRITE) {
		NA_LOG_ERROR("Registered memory requires write permission");
		ret = NA_PERMISSION_ERROR;
		goto out;
	}
	/* Allocate op_id */
	na_cci_op_id = (struct na_cci_op_id *)malloc(sizeof(struct na_cci_op_id));
	if (!na_cci_op_id) {
		NA_LOG_ERROR("Could not allocate NA CCI operation ID");
		ret = NA_NOMEM_ERROR;
		goto out;
	}
	na_cci_op_id->context = context;
	na_cci_op_id->type = NA_CB_PUT;
	na_cci_op_id->callback = callback;
	na_cci_op_id->arg = arg;
	na_cci_op_id->completed = NA_FALSE;
	na_cci_op_id->info.put.request_op_id = 0;
	na_cci_op_id->info.put.transfer_op_id = 0;
	na_cci_op_id->info.put.transfer_completed = NA_FALSE;
	na_cci_op_id->info.put.transfer_actual_size = 0;
	na_cci_op_id->info.put.completion_op_id = 0;
	na_cci_op_id->info.put.completion_actual_size = 0;
	na_cci_op_id->info.put.internal_progress = NA_FALSE;
	na_cci_op_id->info.put.remote_addr = na_cci_addr->cci_addr;

	/* Post the CCI RMA */
	rc = cci_rma(c, NULL, 0, local, local_offset, remote, remote_offset,
			length, na_cci_op_id, CCI_FLAG_WRITE);
	if (rc) {
		NA_LOG_ERROR("cci_rma() failed with %s", cci_strerror(e, rc));
		ret = NA_PROTOCOL_ERROR;
		goto out;
	}

	/* Assign op_id */
	*op_id = (na_op_id_t) na_cci_op_id;

out:
	if (ret != NA_SUCCESS) {
		free(na_cci_op_id);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_get(na_class_t * na_class, na_context_t * context, na_cb_t callback,
	void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
	   na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
	   na_size_t length, na_addr_t remote_addr, na_op_id_t * op_id)
{
	na_cci_mem_handle_t *cci_local_mem_handle =
	(na_cci_mem_handle_t *)local_mem_handle;
	cci_size_t	cci_local_offset = (cci_size_t) local_offset;
	na_cci_mem_handle_t *cci_remote_mem_handle =
	(na_cci_mem_handle_t *)remote_mem_handle;
	cci_size_t	cci_remote_offset = (cci_size_t) remote_offset;
	na_cci_addr_t *na_cci_addr = (struct na_cci_addr *)remote_addr;
	cci_size_t	cci_length = (cci_size_t) length;
	struct na_cci_op_id *na_cci_op_id = NULL;
	na_return_t	ret = NA_SUCCESS;
	int		rc;
	cci_endpoint_t *e = NA_CCI_PRIVATE_DATA(na_class)->endpoint;
	cci_connection_t *c = na_cci_addr->cci_addr;
	cci_rma_handle_t *local = &cci_local_mem_handle->h;
	cci_rma_handle_t *remote = &cci_remote_mem_handle->h;;

	/* Allocate op_id */
	na_cci_op_id = (struct na_cci_op_id *)malloc(sizeof(struct na_cci_op_id));
	if (!na_cci_op_id) {
		NA_LOG_ERROR("Could not allocate NA CCI operation ID");
		ret = NA_NOMEM_ERROR;
		goto done;
	}
	na_cci_op_id->context = context;
	na_cci_op_id->type = NA_CB_GET;
	na_cci_op_id->callback = callback;
	na_cci_op_id->arg = arg;
	na_cci_op_id->completed = NA_FALSE;
	na_cci_op_id->info.get.request_op_id = 0;
	na_cci_op_id->info.get.transfer_op_id = 0;
	na_cci_op_id->info.get.transfer_actual_size = 0;
	na_cci_op_id->info.get.internal_progress = NA_FALSE;
	na_cci_op_id->info.get.remote_addr = na_cci_addr->cci_addr;

	/* Post the CCI RMA */
	rc = cci_rma(c, NULL, 0, local, local_offset, remote, remote_offset,
			length, na_cci_op_id, CCI_FLAG_READ);
	if (rc) {
		NA_LOG_ERROR("cci_rma() failed %s", cci_strerror(e, rc));
		ret = NA_PROTOCOL_ERROR;
		goto done;
	}
	/* Assign op_id */
	*op_id = (na_op_id_t) na_cci_op_id;

done:
	if (ret != NA_SUCCESS) {
		free(na_cci_op_id);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static void
handle_send(na_class_t *class, na_context_t *context,
		cci_endpoint_t *e, cci_event_t *event)
{

	return;
}

/*---------------------------------------------------------------------------*/
static void
handle_recv_expected(na_class_t *na_class, na_context_t *context,
		cci_endpoint_t *e, cci_event_t *event)
{
	cci_connection_t *c = event->recv.connection;
	na_cci_addr_t *na_cci_addr = c->context;
	cci_msg_t *msg = (void*) event->recv.ptr;
	na_size_t msg_len = event->recv.len - sizeof(msg->size);
	na_cci_op_id_t *na_cci_op_id = NULL;
	struct na_cci_info_recv_expected *rx = NULL;
	int rc = 0;
	na_return_t ret;

	TAILQ_FOREACH(na_cci_op_id, &na_cci_addr->rxs, entry) {
		if (na_cci_op_id->info.recv_expected.tag == msg->send.tag) {
			na_size_t len = msg_len;

			if (na_cci_op_id->info.recv_expected.buf_size < len)
				len = na_cci_op_id->info.recv_expected.buf_size;
			memcpy(na_cci_op_id->info.recv_unexpected.buf, msg->send.data, len);
			na_cci_op_id->info.recv_expected.actual_size = len;
			TAILQ_REMOVE(&na_cci_addr->rxs, na_cci_op_id, entry);
			ret = na_cci_complete(na_cci_op_id);
			if (ret != NA_SUCCESS) {
				NA_LOG_ERROR("Could not complete expected recv");
			}
			goto out;
		}
	}

	/* Early receive, cache it */
	rx = calloc(1, sizeof(*rx));
	if (!rx) {
		NA_LOG_ERROR("Unable to allocate expected recv - dropping recv");
		goto out;
	}

	rx->buf = calloc(1, msg_len);
	if (!rx->buf) {
		rc = CCI_ENOMEM;
		goto out;
	}

	memcpy(rx->buf, msg->send.data, msg_len);
	rx->buf_size = rx->actual_size = msg_len;
	rx->tag = msg->send.tag;

	TAILQ_INSERT_TAIL(&na_cci_addr->early, rx, entry);

out:
	if (rc) {
		if (rx)
			free(rx->buf);
		free(rx);
	}
	return;
}

/*---------------------------------------------------------------------------*/
static void
handle_recv_unexpected(na_class_t *na_class, na_context_t *context,
		cci_endpoint_t *e, cci_event_t *event)
{
	cci_connection_t *c = event->recv.connection;
	na_cci_addr_t *na_cci_addr = c->context;
	cci_msg_t *msg = (void*) event->recv.ptr;
	na_size_t msg_len = event->recv.len - sizeof(msg->size);
	na_cci_op_id_t *na_cci_op_id = NULL;
	struct na_cci_info_recv_unexpected *rx = NULL;
	int rc = 0;
	na_return_t ret;

	na_cci_op_id = na_cci_msg_unexpected_op_pop(na_class);

	if (na_cci_op_id) {
		na_size_t len =
			na_cci_op_id->info.recv_unexpected.buf_size <
			event->recv.len - msg_len ?
			na_cci_op_id->info.recv_unexpected.buf_size :
			msg_len;
		na_cci_op_id->info.recv_unexpected.na_cci_addr = na_cci_addr;
		na_cci_op_id->info.recv_unexpected.actual_size = len;
		na_cci_op_id->info.recv_unexpected.tag = msg->send.tag;
		memcpy(na_cci_op_id->info.recv_unexpected.buf, msg->send.data, len);
		ret = na_cci_complete(na_cci_op_id);
		if (ret != NA_SUCCESS) {
			NA_LOG_ERROR("failed to complete unexpected recv");
			goto out;
		}
	} else {
		rx = calloc(1, sizeof(*rx));
		if (!rx) {
			NA_LOG_ERROR("Could not allocate memory for unexpected recv - "
					"dropping the message");
			rc = CCI_ENOMEM;
			goto out;
		}
		rx->buf = calloc(1, msg_len);
		if (!rx->buf) {
			NA_LOG_ERROR("Could not allocate memory for unexpected recv - "
					"dropping the message");
			rc = CCI_ENOMEM;
			goto out;
		}
		memcpy(rx->buf, msg->send.data, msg_len);
		rx->buf_size = rx->actual_size = msg_len;
		rx->na_cci_addr = na_cci_addr;
		rx->tag = msg->send.tag;

		ret = na_cci_msg_unexpected_push(na_class, rx);
		if (ret != NA_SUCCESS) {
			NA_LOG_ERROR("Unable to push unexpected recv");
			rc = CCI_ERROR;
		}
	}

out:
	if (rc) {
		if (rx)
			free(rx->buf);
		free(rx);
	}

	return;
}

/*---------------------------------------------------------------------------*/
static void
handle_recv(na_class_t *na_class, na_context_t *context,
		cci_endpoint_t *e, cci_event_t *event)
{
	cci_msg_t *msg = (void*) event->recv.ptr;

	if (msg->send.expect) {
		handle_recv_expected(na_class, context, e, event);
	} else {
		handle_recv_unexpected(na_class, context, e, event);
	}

	return;
}

/*---------------------------------------------------------------------------*/
static void
handle_connect_request(na_class_t *class, na_context_t *context,
		cci_endpoint_t *e, cci_event_t *event)
{
	return;
}

/*---------------------------------------------------------------------------*/
static void
handle_connect(na_class_t *class, na_context_t *context,
		cci_endpoint_t *e, cci_event_t *event)
{
	return;
}

/*---------------------------------------------------------------------------*/
static void
handle_accept(na_class_t *class, na_context_t *context,
		cci_endpoint_t *e, cci_event_t *event)
{
	return;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_progress(na_class_t * na_class, na_context_t * context,
		unsigned int timeout)
{
	double		remaining = timeout / 1000.0;	/* Convert timeout in ms
							 * into seconds */
	na_return_t	ret = NA_TIMEOUT;
	cci_endpoint_t	*e = NA_CCI_PRIVATE_DATA(na_class)->endpoint;

	do {
		int		rc;
		hg_time_t	t1, t2;
		na_bool_t	progressed = NA_FALSE;
		cci_event_t	*event = NULL;

		hg_time_get_current(&t1);

		rc = cci_get_event(e, &event);
		if (rc) {
			if (rc != CCI_EAGAIN)
				NA_LOG_ERROR("cci_return_event() failed %s",
						cci_strerror(e, rc));

			hg_time_get_current(&t2);
			remaining -= hg_time_to_double(hg_time_subtract(t2, t1));
			continue;
		}

		/* We got an event, handle it */
		switch (event->type) {
		case CCI_EVENT_SEND:
			handle_send(na_class, context, e, event);
			break;
		case CCI_EVENT_RECV:
			handle_recv(na_class, context, e, event);
			break;
		case CCI_EVENT_CONNECT_REQUEST:
			handle_connect_request(na_class, context, e, event);
			break;
		case CCI_EVENT_CONNECT:
			handle_connect(na_class, context, e, event);
			break;
		case CCI_EVENT_ACCEPT:
			handle_accept(na_class, context, e, event);
			break;
		default:
			NA_LOG_ERROR("unhandled %s event",
					cci_event_type_str(event->type));
		}

		/* We progressed, return success */
		ret = NA_SUCCESS;

		rc = cci_return_event(event);
		if (rc)
			NA_LOG_ERROR("cci_return_event() failed %s", cci_strerror(e, rc));

	} while (remaining > 0 && ret != NA_SUCCESS);

	return ret;
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_complete(struct na_cci_op_id *na_cci_op_id)
{
	struct na_cb_info *callback_info = NULL;
	na_return_t	ret = NA_SUCCESS;

	/* Mark op id as completed */
	na_cci_op_id->completed = NA_TRUE;

	/* Allocate callback info */
	callback_info = (struct na_cb_info *)malloc(sizeof(struct na_cb_info));
	if (!callback_info) {
		NA_LOG_ERROR("Could not allocate callback info");
		ret = NA_NOMEM_ERROR;
		goto done;
	}
	callback_info->arg = na_cci_op_id->arg;
	callback_info->ret = ret;
	callback_info->type = na_cci_op_id->type;

	switch (na_cci_op_id->type) {
	case NA_CB_LOOKUP:
		callback_info->info.lookup.addr = na_cci_op_id->info.lookup.addr;
		break;
	case NA_CB_SEND_UNEXPECTED:
		break;
	case NA_CB_RECV_UNEXPECTED:
		{
			/* Fill callback info */
			callback_info->info.recv_unexpected.actual_buf_size =
				(na_size_t) na_cci_op_id->info.recv_unexpected.actual_size;
			callback_info->info.recv_unexpected.source =
				(na_addr_t) na_cci_op_id->info.recv_unexpected.na_cci_addr;
			callback_info->info.recv_unexpected.tag =
				(na_tag_t) na_cci_op_id->info.recv_unexpected.tag;
		}
		break;
	case NA_CB_SEND_EXPECTED:
		break;
	case NA_CB_RECV_EXPECTED:
		/* Check buf_size and actual_size */
		if (na_cci_op_id->info.recv_expected.actual_size !=
		    na_cci_op_id->info.recv_expected.buf_size) {
			NA_LOG_ERROR("Buffer size and actual transfer size do not match");
			ret = NA_SIZE_ERROR;
			goto done;
		}
		break;
	case NA_CB_PUT:
		break;
	case NA_CB_GET:
		break;
	default:
		NA_LOG_ERROR("Operation not supported");
		ret = NA_INVALID_PARAM;
		break;
	}

	ret = na_cb_completion_add(na_cci_op_id->context, na_cci_op_id->callback,
				callback_info, &na_cci_release, na_cci_op_id);
	if (ret != NA_SUCCESS) {
		NA_LOG_ERROR("Could not add callback to completion queue");
		goto done;
	}
done:
	if (ret != NA_SUCCESS) {
		free(callback_info);
	}
	return ret;
}

/*---------------------------------------------------------------------------*/
static void
na_cci_release(struct na_cb_info *callback_info, void *arg)
{
	struct na_cci_op_id *na_cci_op_id = (struct na_cci_op_id *)arg;

	if (na_cci_op_id && !na_cci_op_id->completed) {
		NA_LOG_ERROR("Releasing resources from an uncompleted operation");
	}
	free(callback_info);
	free(na_cci_op_id);
}

/*---------------------------------------------------------------------------*/
static na_return_t
na_cci_cancel(na_class_t NA_UNUSED * na_class, na_context_t * context,
	      na_op_id_t op_id)
{
	struct na_cci_op_id *na_cci_op_id = (struct na_cci_op_id *)op_id;
	na_return_t	ret = NA_PROTOCOL_ERROR;

	/* TODO */
	/* If received is queued, dequeue and free? */

	return ret;
}