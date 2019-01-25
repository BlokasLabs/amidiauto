/*
 * amidiauto - ALSA MIDI autoconnect daemon.
 * Copyright (C) 2019  Vilniaus Blokas UAB, https://blokas.io/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>

#define HOMEPAGE_URL "https://blokas.io/"
#define AMIDITHRU_VERSION 0x0100

static snd_seq_t *g_seq;
static int g_port;

static void seqUninit()
{
	if (g_port)
	{
		snd_seq_delete_simple_port(g_seq, g_port);
		g_port = 0;
	}
	if (g_seq)
	{
		snd_seq_close(g_seq);
		g_seq = NULL;
	}
}

static int seqInit(const char *portName)
{
	if (g_seq != NULL)
	{
		fprintf(stderr, "Already initialized!\n");
		return -EINVAL;
	}

	int result = snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
	if (result < 0)
	{
		fprintf(stderr, "Couldn't open ALSA sequencer! (%d)\n", result);
		goto error;
	}

	result = snd_seq_set_client_name(g_seq, portName);
	if (result < 0)
	{
		fprintf(stderr, "Failed setting client name! (%d)\n", result);
		goto error;
	}

	result = snd_seq_create_simple_port(
		g_seq,
		portName,
		SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
		SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
		SND_SEQ_PORT_CAP_DUPLEX,
		SND_SEQ_PORT_TYPE_HARDWARE | SND_SEQ_PORT_TYPE_MIDI_GENERIC
		);

	if (result < 0)
	{
		fprintf(stderr, "Couldn't create a virtual MIDI port! (%d)\n", result);
		goto error;
	}

	g_port = result;

	return 0;

error:
	seqUninit();
	return result;
}

static bool handleSeqEvent(snd_seq_t *seq, int portId)
{
	do
	{
		snd_seq_event_t *ev;
		snd_seq_event_input(seq, &ev);
		snd_seq_ev_set_source(ev, portId);
		snd_seq_ev_set_subs(ev);
		snd_seq_ev_set_direct(ev);
		snd_seq_event_output_direct(seq, ev);
		snd_seq_free_event(ev);
	} while (snd_seq_event_input_pending(seq, 0) > 0);

	return false;
}

static int run(const char *name)
{
	if (!name || strlen(name) == 0)
		return -EINVAL;

	bool done = false;
	int npfd = 0;

	int result = seqInit(name);

	if (result < 0)
		goto cleanup;

	npfd = snd_seq_poll_descriptors_count(g_seq, POLLIN);
	if (npfd != 1)
	{
		fprintf(stderr, "Unexpected count (%d) of seq fds! Expected 1!", npfd);
		result = -EINVAL;
		goto cleanup;
	}

	pollfd fds[1];
	snd_seq_poll_descriptors(g_seq, &fds[0], 1, POLLIN);

	while (!done)
	{
		int n = poll(fds, sizeof(fds)/sizeof(fds[0]), -1);
		if (n < 0)
		{
			fprintf(stderr, "Polling failed! (%d)\n", errno);
			result = -errno;
			goto cleanup;
		}

		if (fds[0].revents)
		{
			--n;
			done = handleSeqEvent(g_seq, g_port);
		}

		assert(n == 0);
	}

cleanup:
	seqUninit();

	return result;
}

static void printVersion(void)
{
	printf("Version %x.%02x, Copyright (C) Blokas Labs " HOMEPAGE_URL "\n", AMIDITHRU_VERSION >> 8, AMIDITHRU_VERSION & 0xff);
}

static void printUsage()
{
	printf("Usage: amidithru \"Virtual Port Name\"\n"
		"Example:\n"
		"\tamidithru \"VirtualThru\"\n"
		"\n"
		);
	printVersion();
}

int main(int argc, char **argv)
{
	if (argc == 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0))
	{
		printVersion();
		return 0;
	}
	else if (argc != 2)
	{
		printUsage();
		return 0;
	}

	int result = run(argv[1]);

	if (result < 0)
		fprintf(stderr, "Error %d!\n", result);

	return result;
}
