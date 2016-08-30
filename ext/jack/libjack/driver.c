/* -*- mode: c; c-file-style: "linux"; -*- */
/*
    Copyright (C) 2001-2003 Paul Davis
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software 
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
    
*/

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <jack/thread.h>

#include "internal.h"
#include "driver.h"
#include "engine.h"

#ifdef USE_MLOCK
#include <sys/mman.h>
#endif /* USE_MLOCK */

static int dummy_attach (jack_driver_t *drv, jack_engine_t *eng) { return 0; }
static int dummy_detach (jack_driver_t *drv, jack_engine_t *eng) { return 0; }
static int dummy_write (jack_driver_t *drv,
			jack_nframes_t nframes) { return 0; }
static int dummy_read (jack_driver_t *drv, jack_nframes_t nframes) { return 0; }
static int dummy_null_cycle (jack_driver_t *drv,
			     jack_nframes_t nframes) { return 0; }
static int dummy_bufsize (jack_driver_t *drv,
			  jack_nframes_t nframes) {return 0;}
static int dummy_stop (jack_driver_t *drv) { return 0; }
static int dummy_start (jack_driver_t *drv) { return 0; }

void
jack_driver_init (jack_driver_t *driver)
{
	memset (driver, 0, sizeof (*driver));

	driver->attach = dummy_attach;
	driver->detach = dummy_detach;
	driver->write = dummy_write;
	driver->read = dummy_read;
	driver->null_cycle = dummy_null_cycle;
	driver->bufsize = dummy_bufsize;
	driver->start = dummy_start;
	driver->stop = dummy_stop;
}



/****************************
 *** Non-Threaded Drivers ***
 ****************************/

static int dummy_nt_run_cycle (jack_driver_nt_t *drv) { return 0; }
static int dummy_nt_attach    (jack_driver_nt_t *drv) { return 0; }
static int dummy_nt_detach    (jack_driver_nt_t *drv) { return 0; }


/*
 * These are used in driver->nt_run for controlling whether or not
 * driver->engine->driver_exit() gets called (EXIT = call it, PAUSE = don't)
 */
#define DRIVER_NT_RUN   0
#define DRIVER_NT_EXIT  1
#define DRIVER_NT_PAUSE 2
#define DRIVER_NT_DYING 3

static int
jack_driver_nt_attach (jack_driver_nt_t * driver, jack_engine_t * engine)
{
	driver->engine = engine;
	return driver->nt_attach (driver);
}

static int
jack_driver_nt_detach (jack_driver_nt_t * driver, jack_engine_t * engine)
{
	int ret;

	ret = driver->nt_detach (driver);
	driver->engine = NULL;

	return ret;
}

static void *
jack_driver_nt_thread (void * arg)
{
	jack_driver_nt_t * driver = (jack_driver_nt_t *) arg;
	int rc = 0;
	int run;

	/* This thread may start running before pthread_create()
	 * actually stores the driver->nt_thread value.  It's safer to
	 * store it here as well. 
	 */

	driver->nt_thread = pthread_self();

	pthread_mutex_lock (&driver->nt_run_lock);

	while ((run = driver->nt_run) == DRIVER_NT_RUN) {
		pthread_mutex_unlock (&driver->nt_run_lock);

		if ((rc = driver->nt_run_cycle (driver)) != 0) {
			jack_error ("DRIVER NT: could not run driver cycle");
			goto out;
		}

		pthread_mutex_lock (&driver->nt_run_lock);
	}

	pthread_mutex_unlock (&driver->nt_run_lock);

 out:
	if (rc) {
		driver->nt_run = DRIVER_NT_DYING;
		driver->engine->driver_exit (driver->engine);
	}
	pthread_exit (NULL);
}

static int
jack_driver_nt_start (jack_driver_nt_t * driver)
{
	int err;

	/* stop the new thread from really starting until the driver has
	   been started.
	*/
 
	pthread_mutex_lock (&driver->nt_run_lock);
	driver->nt_run = DRIVER_NT_RUN;

	if ((err = jack_client_create_thread (NULL,
					      &driver->nt_thread, 
					      driver->engine->rtpriority,
					      driver->engine->control->real_time,
					      jack_driver_nt_thread, driver)) != 0) {
		jack_error ("DRIVER NT: could not start driver thread!");
		return err;
	}

	if ((err = driver->nt_start (driver)) != 0) {
		/* make the thread run and exit immediately */
		driver->nt_run = DRIVER_NT_EXIT;
		pthread_mutex_unlock (&driver->nt_run_lock);
		jack_error ("DRIVER NT: could not start driver");
		return err;
	}

	/* let the thread run, since the underlying "device" has now been started */

	pthread_mutex_unlock (&driver->nt_run_lock);

	return 0;
}

static int
jack_driver_nt_do_stop (jack_driver_nt_t * driver, int run)
{
	int err;

	pthread_mutex_lock (&driver->nt_run_lock);
	if(driver->nt_run != DRIVER_NT_DYING) {
		driver->nt_run = run;
	}
	pthread_mutex_unlock (&driver->nt_run_lock);

	/* detect when called while the thread is shutting itself down */
	if (driver->nt_thread && driver->nt_run != DRIVER_NT_DYING
	    && (err = pthread_join (driver->nt_thread, NULL)) != 0) {
		jack_error ("DRIVER NT: error waiting for driver thread: %s",
                            strerror (err));
		return err;
	}

	if ((err = driver->nt_stop (driver)) != 0) {
		jack_error ("DRIVER NT: error stopping driver");
		return err;
	}

	return 0;
}

static int
jack_driver_nt_stop (jack_driver_nt_t * driver)
{
	return jack_driver_nt_do_stop (driver, DRIVER_NT_EXIT);
}

static int
jack_driver_nt_bufsize (jack_driver_nt_t * driver, jack_nframes_t nframes)
{
	int err;
	int ret;

	err = jack_driver_nt_do_stop (driver, DRIVER_NT_PAUSE);
	if (err) {
		jack_error ("DRIVER NT: could not stop driver to change buffer size");
		driver->engine->driver_exit (driver->engine);
		return err;
	}

	ret = driver->nt_bufsize (driver, nframes);

	err = jack_driver_nt_start (driver);
	if (err) {
		jack_error ("DRIVER NT: could not restart driver during buffer size change");
		driver->engine->driver_exit (driver->engine);
		return err;
	}

	return ret;
}

void
jack_driver_nt_init (jack_driver_nt_t * driver)
{
	memset (driver, 0, sizeof (*driver));

	jack_driver_init ((jack_driver_t *) driver);

	driver->attach       = (JackDriverAttachFunction)    jack_driver_nt_attach;
	driver->detach       = (JackDriverDetachFunction)    jack_driver_nt_detach;
	driver->bufsize      = (JackDriverBufSizeFunction)   jack_driver_nt_bufsize;
	driver->stop         = (JackDriverStopFunction)      jack_driver_nt_stop;
	driver->start        = (JackDriverStartFunction)     jack_driver_nt_start;

	driver->nt_bufsize   = (JackDriverNTBufSizeFunction) dummy_bufsize;
	driver->nt_start     = (JackDriverNTStartFunction)   dummy_start;
	driver->nt_stop      = (JackDriverNTStopFunction)    dummy_stop;
	driver->nt_attach    =                               dummy_nt_attach;
	driver->nt_detach    =                               dummy_nt_detach;
	driver->nt_run_cycle =                               dummy_nt_run_cycle;


	pthread_mutex_init (&driver->nt_run_lock, NULL);
}

void
jack_driver_nt_finish     (jack_driver_nt_t * driver)
{
	pthread_mutex_destroy (&driver->nt_run_lock);
}
