/*
    Internal interfaces for JACK transport engine.

    Copyright (C) 2003 Jack O'Quin
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

int	jack_set_sample_rate (jack_engine_t *engine, jack_nframes_t nframes);
int	jack_timebase_reset (jack_engine_t *engine,
			     jack_uuid_t client_id);
int	jack_timebase_set (jack_engine_t *engine,
			   jack_uuid_t client_id, int conditional);
void	jack_transport_activate (jack_engine_t *engine,
				 jack_client_internal_t *client);
void	jack_transport_init (jack_engine_t *engine);
void	jack_transport_client_exit (jack_engine_t *engine,
				    jack_client_internal_t *client);
void	jack_transport_client_new (jack_client_internal_t *client);
int	jack_transport_client_reset_sync (jack_engine_t *engine,
					  jack_uuid_t client_id);
int	jack_transport_client_set_sync (jack_engine_t *engine,
					jack_uuid_t client_id);
void	jack_transport_cycle_end (jack_engine_t *engine);
void	jack_transport_cycle_start(jack_engine_t *engine, jack_time_t time);
int	jack_transport_set_sync_timeout (jack_engine_t *engine,
					 jack_time_t usecs);
