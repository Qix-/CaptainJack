#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <jack/jack.h>
#include <jack/intclient.h>

jack_client_t *client;
jack_intclient_t intclient;
char *client_name;
char *intclient_name;
char *load_name;
char *load_init = NULL;
char *server_name = NULL;
int wait_opt = 0;

void
signal_handler (int sig)
{
	jack_status_t status;

	fprintf (stderr, "signal received, unloading...");
	status = jack_internal_client_unload (client, intclient);
	if (status & JackFailure)
		fprintf (stderr, "(failed), status = 0x%2.0x\n", status);
	else
		fprintf (stderr, "(succeeded)\n");
	jack_client_close (client);
	exit (0);
}

void
show_usage ()
{
	fprintf (stderr, "usage: %s [ options ] client-name [ load-name "
		 "\n\noptions:\n", client_name);
	fprintf (stderr,
		 "\t-h, --help \t\t print help message\n"
		 "\t-i, --init string\t initialize string\n"
		 "\t-s, --server name\t select JACK server\n"
		 "\t-w, --wait \t\t wait for signal, then unload\n"
		 "\n"
		);
}

int
parse_args (int argc, char *argv[])
{
	int c;
	int option_index = 0;
	char *short_options = "hi:s:w";
	struct option long_options[] = {
		{ "help", 0, 0, 'h' },
		{ "init", required_argument, 0, 'i' },
		{ "server", required_argument, 0, 's' },
		{ "wait", 0, 0, 'w' },
		{ 0, 0, 0, 0 }
	};

	client_name = strrchr(argv[0], '/');
	if (client_name == NULL) {
		client_name = argv[0];
	} else {
		client_name++;
	}

	while ((c = getopt_long (argc, argv, short_options, long_options,
				 &option_index)) >= 0) {
		switch (c) {
		case 'i':
			load_init = optarg;
			break;
		case 's':
			server_name = optarg;
			break;
		case 'w':
			wait_opt = 1;
			break;
		case 'h':
		default:
			show_usage ();
			return 1;
		}
	}

	if (optind == argc) {		/* no positional args? */
		show_usage ();
		return 1;
	}
	if (optind < argc)
		load_name = intclient_name = argv[optind++];

	if (optind < argc)
		load_name = argv[optind++];

	if (optind < argc)
		load_init = argv[optind++];

	//fprintf (stderr, "client-name = `%s', load-name = `%s', "
	//	 "load-init = `%s', wait = %d\n",
	//	 intclient_name, load_name, load_init, wait_opt);

	return 0;			/* args OK */
}

int
main (int argc, char *argv[])
{
	jack_status_t status;

	/* parse and validate command arguments */
	if (parse_args (argc, argv))
		exit (1);		/* invalid command line */

	/* first, become a JACK client */
	client = jack_client_open (client_name, JackServerName,
				   &status, server_name);
	if (client == NULL) {
		fprintf (stderr, "jack_client_open() failed, "
			 "status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		exit (1);
	}

	/* then, load the internal client */
	jack_internal_client_load (client, intclient_name,
                                   (JackLoadName|JackLoadInit),
                                   &status, intclient, load_name, load_init);

        if (status & JackNameNotUnique) {
                fprintf (stderr, "unique internal client name `%s' assigned\n",
                         load_name);
                return 3;
        }

        if (status & JackFailure) {
                fprintf (stderr, "could not load %s, status = 0x%x\n",
                         client_name, status);
                return 2;
        }
        
        
	fprintf (stdout, "%s is running.\n", load_name);

	if (wait_opt) {
		/* define a signal handler to unload the client, then
		 * wait for it to exit */
		signal (SIGQUIT, signal_handler);
		signal (SIGTERM, signal_handler);
		signal (SIGHUP, signal_handler);
		signal (SIGINT, signal_handler);

		while (1) {
			sleep (1);
		}
	}

	return 0;
}
	
		
