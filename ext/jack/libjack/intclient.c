/* -*- mode: c; c-file-style: "bsd"; -*- */
/*
 *  Copyright (C) 2004 Jack O'Quin
 *  
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2.1 of the License, or
 *  (at your option) any later version.
 *  
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *  
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, write to the Free Software 
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <config.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <jack/intclient.h>
#include <jack/uuid.h>

#include "internal.h"
#include "varargs.h"

#include "local.h"

static int
jack_intclient_request(RequestType type, jack_client_t *client,
                       const char* client_name, jack_options_t options,
                       jack_status_t *status, jack_intclient_t uuid, jack_varargs_t *va)
{
	jack_request_t req;

	memset (&req, 0, sizeof (req));

	if (strlen (client_name) >= sizeof (req.x.intclient.name)) {
		jack_error ("\"%s\" is too long for a JACK client name.\n"
			    "Please use %lu characters or less.",
			    client_name, sizeof (req.x.intclient.name));
		return -1;
	}

	if (va->load_name
	    && (strlen (va->load_name) > sizeof (req.x.intclient.path) - 1)) {
		jack_error ("\"%s\" is too long for a shared object name.\n"
			     "Please use %lu characters or less.",
			    va->load_name, sizeof (req.x.intclient.path) - 1);
		*status |= (JackFailure|JackInvalidOption);
		return -1;
	}

	if (va->load_init
	    && (strlen (va->load_init) > sizeof (req.x.intclient.init) - 1)) {
		jack_error ("\"%s\" is too long for internal client init "
			    "string.\nPlease use %lu characters or less.",
			    va->load_init, sizeof (req.x.intclient.init) - 1);
		*status |= (JackFailure|JackInvalidOption);
		return -1;
	}

	req.type = type;
	req.x.intclient.options = options;
	strncpy (req.x.intclient.name, client_name,
		 sizeof (req.x.intclient.name));
	if (va->load_name)
		strncpy (req.x.intclient.path, va->load_name,
			 sizeof (req.x.intclient.path));
	if (va->load_init)
		strncpy (req.x.intclient.init, va->load_init,
			 sizeof (req.x.intclient.init));

	jack_client_deliver_request (client, &req);

	*status |= req.status;

	if (*status & JackFailure)
		return -1;

	jack_uuid_copy (&uuid, req.x.intclient.uuid);
        return 0;
}

char *
jack_get_internal_client_name (jack_client_t *client,
			       jack_intclient_t intclient)
{
	jack_request_t req;
	char *name;

	memset (&req, 0, sizeof (req));
	req.type = IntClientName;
	req.x.intclient.options = JackNullOption;
	jack_uuid_copy (&req.x.intclient.uuid, intclient);

	jack_client_deliver_request (client, &req);

	if (req.status & JackFailure) {
		return NULL;
	}

	/* allocate storage for returning the name */
	if ((name = strdup (req.x.intclient.name)) == NULL) {
		return NULL;
	}

	return name;
}

int
jack_internal_client_handle (jack_client_t *client,
			     const char *client_name,
			     jack_status_t *status,
                             jack_intclient_t handle)
{
	jack_request_t req;
	jack_status_t my_status;

	if (status == NULL)		/* no status from caller? */
		status = &my_status;	/* use local status word */
	*status = 0;

	memset (&req, 0, sizeof (req));
	req.type = IntClientHandle;
	req.x.intclient.options = JackNullOption;
	strncpy (req.x.intclient.name, client_name,
		 sizeof (req.x.intclient.name));

	*status = jack_client_deliver_request (client, &req);

        if (!jack_uuid_empty (req.x.intclient.uuid)) {
                jack_uuid_copy (&handle, req.x.intclient.uuid);
                return 0;
        }

        return -1;
}

int
jack_internal_client_load_aux (jack_client_t *client,
                               const char *client_name,
                               jack_options_t options,
                               jack_status_t *status, 
                               jack_intclient_t handle, va_list ap)
{
	jack_varargs_t va;
	jack_status_t my_status;

	if (status == NULL)		/* no status from caller? */
		status = &my_status;	/* use local status word */
	*status = 0;

	/* validate parameters */
	if ((options & ~JackLoadOptions)) {
		*status |= (JackFailure|JackInvalidOption);
		return -1;
	}

	/* parse variable arguments */
	jack_varargs_parse (options, ap, &va);

	return jack_intclient_request (IntClientLoad, client, client_name,
				       options, status, handle, &va);
}

int
jack_internal_client_load (jack_client_t *client,
			   const char *client_name,
			   jack_options_t options,
			   jack_status_t *status, 
                           jack_intclient_t handle, ...)
{
    va_list ap;
    va_start(ap, handle);
    int res = jack_internal_client_load_aux(client, client_name, options, status, handle, ap);
    va_end(ap);
    return res;
}

jack_status_t
jack_internal_client_unload (jack_client_t *client,
			     jack_intclient_t intclient)
{
	jack_request_t req;
	jack_status_t status;

	if (intclient) {

		memset (&req, 0, sizeof (req));
		req.type = IntClientUnload;
		req.x.intclient.options = JackNullOption;
		jack_uuid_copy (&req.x.intclient.uuid, intclient);
		jack_client_deliver_request (client, &req);
		status = req.status;

	} else {			/* intclient is null */
		status = (JackNoSuchClient|JackFailure);
	}

	return status;
}
