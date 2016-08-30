#ifndef __jack_shm_h__
#define __jack_shm_h__

#include <limits.h>
#include <sys/types.h>
#include <jack/types.h>

#define MAX_SERVERS 8			/* maximum concurrent servers */
#define MAX_SHM_ID 256			/* generally about 16 per server */
#define JACK_SERVER_NAME_SIZE 256	/* maximum length of server name */
#define JACK_SHM_MAGIC 0x4a41434b	/* shm magic number: "JACK" */
#define JACK_SHM_NULL_INDEX -1		/* NULL SHM index */
#define JACK_SHM_REGISTRY_INDEX -2	/* pseudo SHM index for registry */


/* On Mac OS X, SHM_NAME_MAX is the maximum length of a shared memory
 * segment name (instead of NAME_MAX or PATH_MAX as defined by the
 * standard).
 */
#ifdef USE_POSIX_SHM
#ifndef SHM_NAME_MAX
#define SHM_NAME_MAX NAME_MAX
#endif
typedef char	   shm_name_t[SHM_NAME_MAX];
typedef shm_name_t jack_shm_id_t;
#else /* System V SHM */
typedef int	   jack_shm_id_t;
#endif /* SHM type */

/* shared memory type */
typedef enum {
	shm_POSIX = 1,			/* POSIX shared memory */
	shm_SYSV = 2			/* System V shared memory */
} jack_shmtype_t;

typedef int16_t jack_shm_registry_index_t;

/** 
 * A structure holding information about shared memory allocated by
 * JACK. this persists across invocations of JACK, and can be used by
 * multiple JACK servers.  It contains no pointers and is valid across
 * address spaces.
 *
 * The registry consists of two parts: a header including an array of
 * server names, followed by an array of segment registry entries.
 */
typedef struct _jack_shm_server {
    pid_t                     pid;	/* process ID */
    char		      name[JACK_SERVER_NAME_SIZE];
} jack_shm_server_t;

typedef struct _jack_shm_header {
    uint32_t		      magic;	/* magic number */
    uint16_t		      protocol;	/* JACK protocol version */
    jack_shmtype_t	      type;	/* shm type */
    jack_shmsize_t	      size;	/* total registry segment size */
    jack_shmsize_t	      hdr_len;	/* size of header */
    jack_shmsize_t	      entry_len; /* size of registry entry */
    jack_shm_server_t server[MAX_SERVERS]; /* current server array */
} jack_shm_header_t;

typedef struct _jack_shm_registry {
    jack_shm_registry_index_t index;     /* offset into the registry */
    pid_t                     allocator; /* PID that created shm segment */
    jack_shmsize_t            size;      /* for POSIX unattach */
    jack_shm_id_t             id;        /* API specific, see above */
} jack_shm_registry_t;

#define JACK_SHM_REGISTRY_SIZE (sizeof (jack_shm_header_t) \
				+ sizeof (jack_shm_registry_t) * MAX_SHM_ID)

/** 
 * a structure holding information about shared memory
 * allocated by JACK. this version is valid only
 * for a given address space. It contains a pointer
 * indicating where the shared memory has been
 * attached to the address space.
 */
typedef struct _jack_shm_info {
    jack_shm_registry_index_t index;       /* offset into the registry */
    void		     *attached_at; /* address where attached */
} jack_shm_info_t;

/* utility functions used only within JACK */

extern void jack_shm_copy_from_registry (jack_shm_info_t*, 
					 jack_shm_registry_index_t);
extern void jack_shm_copy_to_registry (jack_shm_info_t*,
				       jack_shm_registry_index_t*);
extern void jack_release_shm_info (jack_shm_registry_index_t);

static inline char* jack_shm_addr (jack_shm_info_t* si) {
	return si->attached_at;
}

/* here beginneth the API */

extern int  jack_register_server (const char *server_name, int new_registry);
extern void jack_unregister_server (const char *server_name);

extern int  jack_initialize_shm (const char *server_name);
extern int  jack_cleanup_shm (void);

extern int  jack_shmalloc (jack_shmsize_t size, jack_shm_info_t* result);
extern void jack_release_shm (jack_shm_info_t*);
extern void jack_destroy_shm (jack_shm_info_t*);
extern int  jack_attach_shm (jack_shm_info_t*);
extern int  jack_resize_shm (jack_shm_info_t*, jack_shmsize_t size);

#endif /* __jack_shm_h__ */
