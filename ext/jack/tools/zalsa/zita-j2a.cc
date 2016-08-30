// ----------------------------------------------------------------------------
//
//  Copyright (C) 2012 Fons Adriaensen <fons@linuxaudio.org>
//    
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ----------------------------------------------------------------------------
#include <iostream>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <signal.h>
#include "alsathread.h"
#include "jackclient.h"
#include "lfqueue.h"


static Lfq_int32   commq (16);
static Lfq_adata   alsaq (256);
static Lfq_jdata   infoq (256);
static Lfq_audio  *audioq = 0;
static bool stop = false;

static const char   *clopt = "hvLj:d:r:p:n:c:Q:O:";
static bool          v_opt = false;
static bool          L_opt = false;
static const char   *jname = APPNAME;
static const char   *device = 0;
static int           fsamp = 0;
static int           bsize = 0;
static int           nfrag = 2;
static int           nchan = 2;
static int           rqual = 48;
static int           ltcor = 0;


static void help (void)
{
    fprintf (stderr, "\n%s-%s\n", APPNAME, VERSION);
    fprintf (stderr, "(C) 2012-2013 Fons Adriaensen  <fons@linuxaudio.org>\n");
    fprintf (stderr, "Use ALSA playback device as a Jack client, with resampling.\n\n");
    fprintf (stderr, "Usage: %s <options>\n", APPNAME);
    fprintf (stderr, "Options:\n");
    fprintf (stderr, "  -h                 Display this text\n");
    fprintf (stderr, "  -j <jackname>      Name as Jack client [%s]\n", APPNAME);
    fprintf (stderr, "  -d <device>        ALSA playback device [none]\n");  
    fprintf (stderr, "  -r <rate>          Sample rate [48000]\n");
    fprintf (stderr, "  -p <period>        Period size [256]\n");   
    fprintf (stderr, "  -n <nfrags>        Number of fragments [2]\n");   
    fprintf (stderr, "  -c <nchannels>     Number of channels [2]\n");
    fprintf (stderr, "  -Q <quality>       Resampling quality [48]\n");
    fprintf (stderr, "  -O <latency>       Latency adjustment[0]\n");
    fprintf (stderr, "  -L                 Force 16-bit and 2 channels [off]\n");
    fprintf (stderr, "  -v                 Print tracing information [off]\n");
    exit (1);
}

static int procoptions (int ac, const char *av [])
{
    int k;
    
    optind = 1;
    opterr = 0;
    while ((k = getopt (ac, (char **) av, (char *) clopt)) != -1)
    {
        if (optarg && (*optarg == '-'))
        {
	    fprintf (stderr, "  Missing argument for '-%c' option.\n", k); 
            fprintf (stderr, "  Use '-h' to see all options.\n");
            exit (1);
        }
	switch (k)
	{
        case 'h' : help (); exit (0);
        case 'v' : v_opt = true; break;
        case 'L' : L_opt = true; break;
        case 'j' : jname = optarg; break;
        case 'd' : device = optarg; break;
	case 'r' : fsamp = atoi (optarg); break;    
	case 'p' : bsize = atoi (optarg); break;    
	case 'n' : nfrag = atoi (optarg); break;    
	case 'c' : nchan = atoi (optarg); break;    
	case 'Q' : rqual = atoi (optarg); break;    
	case 'O' : ltcor = atoi (optarg); break;    
        case '?':
            if (optopt != ':' && strchr (clopt, optopt))
	    {
                fprintf (stderr, "  Missing argument for '-%c' option.\n", optopt); 
	    }
            else if (isprint (optopt))
	    {
                fprintf (stderr, "  Unknown option '-%c'.\n", optopt);
	    }
            else
	    {
                fprintf (stderr, "  Unknown option character '0x%02x'.\n", optopt & 255);
	    }
            fprintf (stderr, "  Use '-h' to see all options.\n");
            return 1;
        default:
            return 1;
 	}
    }

    return 0;
}

static int parse_options (const char* load_init)
{
        int argsz;
        int argc = 0;
        const char** argv;
        char* args = strdup (load_init);
        char* token;
        char* ptr = args;
        char* savep;

	if (!load_init) {
                return 0;
        }

        argsz = 8; /* random guess at "maxargs" */
        argv = (const char **) malloc (sizeof (char *) * argsz);

        argv[argc++] = APPNAME;

        while (1) {
                
                if ((token = strtok_r (ptr, " ", &savep)) == NULL) {
                        break;
                }
                
                if (argc == argsz) {
                        argsz *= 2;
                        argv = (const char **) realloc (argv, sizeof (char *) * argsz);
                }
                
                argv[argc++] = token;
                ptr = NULL;
        }

        return procoptions (argc, argv);
}

static void printinfo (void)
{
    int     n;
    double  e, r;
    Jdata   *J;

    n = 0;
    e = r = 0;
    while (infoq.rd_avail ())
    {
	J = infoq.rd_datap ();
	if (J->_state == Jackclient::TERM)
	{
	    printf ("Fatal error condition, terminating.\n");
	    stop = true;
	    return;
	}
	else if (J->_state == Jackclient::WAIT)
	{
	    printf ("Detected excessive timing errors, waiting 15 seconds.\n");
	    printf ("This may happen with current Jack1 after freewheeling.\n");
	    n = 0;
	}
	else if (J->_state == Jackclient::SYNC0)
	{
            printf ("Starting synchronisation.\n");
	}
	else if (v_opt)
	{
	    n++;
	    e += J->_error;
	    r += J->_ratio;
	}
	infoq.rd_commit ();
    }
    if (n) printf ("%8.3lf %10.6lf\n", e / n, r / n);
}

Alsa_pcmi      *A = 0;
Alsathread     *P = 0;
Jackclient     *J = 0;

extern "C" {

int jack_initialize (jack_client_t* client, const char* load_init)
{
    int            k, k_del, opts;
    double         t_jack;
    double         t_alsa;
    double         t_del;

    if (parse_options (load_init)) {
            return 1;
    }

    if (device == 0) help ();
    if (rqual < 16) rqual = 16;
    if (rqual > 96) rqual = 96;
    if ((fsamp && fsamp < 8000) || (bsize && bsize < 16) || (nfrag < 2) || (nchan < 1))
    {
	fprintf (stderr, "Illegal parameter value(s).\n");
	return 1;
    }

    J = new Jackclient (client, 0, Jackclient::PLAY, 0);
    usleep (100000);

    /* if SR and/or bufsize are unspecified, use the same values
       as the JACK server.
    */
    
    if (fsamp == 0) 
    {
       fsamp = J->fsamp();
    }

    if (bsize == 0) 
    {
       bsize = J->bsize();
    }

    opts = 0;
    if (v_opt) opts |= Alsa_pcmi::DEBUG_ALL;
    if (L_opt) opts |= Alsa_pcmi::FORCE_16B | Alsa_pcmi::FORCE_2CH;
    A = new Alsa_pcmi (device, 0, 0, fsamp, bsize, nfrag, opts);
    if (A->state ())
    {
	fprintf (stderr, "Can't open ALSA playback device '%s'.\n", device);
	return 1;
    }
    if (v_opt) A->printinfo ();
    if (nchan > A->nplay ())
    {
        nchan = A->nplay ();
	fprintf (stderr, "Warning: only %d channels are available.\n", nchan);
    }
    P = new Alsathread (A, Alsathread::PLAY);
    J->register_ports (nchan);

    t_alsa = (double) bsize / fsamp;
    if (t_alsa < 1e-3) t_alsa = 1e-3;
    t_jack = (double) J->bsize () / J->fsamp (); 
    t_del = 1.5 * t_alsa + t_jack;
    k_del = (int)(t_del * fsamp);
    for (k = 256; k < k_del + J->bsize (); k *= 2);
    audioq = new Lfq_audio (k, nchan);

    P->start (audioq, &commq, &alsaq, J->rprio () + 10);
    J->start (audioq, &commq, &alsaq, &infoq, (double) fsamp / J->fsamp (), k_del, ltcor, rqual);

    return 0;
}

void jack_finish (void* arg)
{
    commq.wr_int32 (Alsathread::TERM);
    usleep (100000);
    delete P;
    delete A;
    delete J;
    delete audioq;
}

} /* extern "C" */
