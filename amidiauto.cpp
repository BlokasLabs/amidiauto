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

#include <map>

#define HOMEPAGE_URL "https://blokas.io/"
#define AMIDIAUTO_VERSION 0x0100

static snd_seq_t *g_seq = NULL;
static int g_port = -1;

enum PortDir
{
	DIR_UNKNOWN = 0,
	DIR_INPUT   = 1 << 0,
	DIR_OUTPUT  = 1 << 1,
	DIR_DUPLEX  = DIR_INPUT | DIR_OUTPUT
};

inline static bool operator <(const snd_seq_addr_t &a, const snd_seq_addr_t &b)
{
	return std::make_pair(a.client, a.port) < std::make_pair(b.client, b.port);
}

inline static bool operator ==(const snd_seq_addr_t &a, const snd_seq_addr_t &b)
{
	return a.client == b.client && a.port == b.port;
}

// Keeps track of one input and one output port.
// This is to try and keep things simple and app performance under control.
// For example, some software may create many input ports, all for the same function,
// if a MIDI note gets sent to all of them, that will cause many duplicate notes
// to be played.
class Client
{
public:
	Client();
	Client(int clientId);

	void setInput(snd_seq_addr_t addr);
	void setOutput(snd_seq_addr_t addr);

	bool isInputSet() const;
	bool isOutputSet() const;

	void clearInput();
	void clearOutput();

	const snd_seq_addr_t *getInput() const;
	const snd_seq_addr_t *getOutput() const;

	bool operator <(const Client &rhs) const;

private:
	int m_clientId;

	bool m_inputInitialized;
	bool m_outputInitialized;

	snd_seq_addr_t m_input;
	snd_seq_addr_t m_output;
};

Client::Client()
	:m_clientId(-1)
	,m_inputInitialized(false)
	,m_outputInitialized(false)
{
}

Client::Client(int clientId)
	:m_clientId(clientId)
	,m_inputInitialized(false)
	,m_outputInitialized(false)
{
}

void Client::setInput(snd_seq_addr_t addr)
{
	m_inputInitialized = true;
	m_input = addr;
}

void Client::setOutput(snd_seq_addr_t addr)
{
	m_outputInitialized = true;
	m_output = addr;
}

bool Client::isInputSet() const
{
	return m_inputInitialized;
}

bool Client::isOutputSet() const
{
	return m_outputInitialized;
}

void Client::clearInput()
{
	m_inputInitialized = false;
}

void Client::clearOutput()
{
	m_outputInitialized = false;
}

const snd_seq_addr_t * Client::getInput() const
{
	return m_inputInitialized ? &m_input : NULL;
}

const snd_seq_addr_t * Client::getOutput() const
{
	return m_outputInitialized ? &m_output : NULL;
}

bool Client::operator <(const Client &rhs) const
{
	return m_clientId < rhs.m_clientId;
}

typedef std::map<int, Client> clients_t;

enum ClientType
{
	CLIENT_SOFTWARE,
	CLIENT_HARDWARE
};

static clients_t g_swClients;
static clients_t g_hwClients;

static Client &getClient(clients_t &list, int clientId)
{
	clients_t::iterator item = list.find(clientId);
	if (item != list.end())
	{
		return item->second;
	}
	else
	{
		return list.insert(std::make_pair(clientId, Client(clientId))).first->second;
	}
}

Client *findClientForPort(snd_seq_addr_t addr, ClientType *type = NULL)
{
	int clientId = addr.client;

	clients_t::iterator item = g_swClients.find(clientId);
	if (item == g_swClients.end())
	{
		item = g_hwClients.find(clientId);
		if (item == g_hwClients.end())
		{
			return NULL;
		}
		else
		{
			if (type) *type = CLIENT_HARDWARE;
		}
	}
	else
	{
		if (type) *type = CLIENT_SOFTWARE;
	}

	return &item->second;
}

static void connect(snd_seq_addr_t output, snd_seq_addr_t input)
{
	printf("Connecting %d:%d to %d:%d\n", output.client, output.port, input.client, input.port);

	snd_seq_port_subscribe_t *subs;
	snd_seq_port_subscribe_alloca(&subs);
	snd_seq_port_subscribe_set_sender(subs, &output);
	snd_seq_port_subscribe_set_dest(subs, &input);
	snd_seq_subscribe_port(g_seq, subs);
}

static PortDir portGetDir(const snd_seq_port_info_t &portInfo)
{
	unsigned result = 0;

	unsigned int caps = snd_seq_port_info_get_capability(&portInfo);

	if (caps & SND_SEQ_PORT_CAP_READ)
	{
		result |= DIR_OUTPUT;
	}
	if (caps & SND_SEQ_PORT_CAP_WRITE)
	{
		result |= DIR_INPUT;
	}

	return (PortDir)result;
}

static bool portAdd(const snd_seq_port_info_t &portInfo)
{
	unsigned int caps = snd_seq_port_info_get_capability(&portInfo);

	if (caps & SND_SEQ_PORT_CAP_NO_EXPORT)
		return false;

	// Ignore System client and its Announce and Timer ports.
	int clientId = snd_seq_port_info_get_client(&portInfo);
	if (clientId == SND_SEQ_CLIENT_SYSTEM)
		return false;

	snd_seq_client_info_t *clientInfo;
	snd_seq_client_info_alloca(&clientInfo);
	if (snd_seq_get_any_client_info(g_seq, clientId, clientInfo) < 0)
		return false;

	// Ignore through ports.
	const char *name = snd_seq_client_info_get_name(clientInfo);
	if (!name || strncmp(name, "Midi Through", 12) == 0)
		return false;

	unsigned int type = snd_seq_port_info_get_type(&portInfo);
	clients_t &list = (type & SND_SEQ_PORT_TYPE_APPLICATION) ? g_swClients : g_hwClients;
	snd_seq_addr_t addr = *snd_seq_port_info_get_addr(&portInfo);

	PortDir dir = portGetDir(portInfo);

	bool gotAdded = false;

	Client &client = getClient(list, addr.client);
	if (dir & DIR_OUTPUT)
	{
		if (!client.isOutputSet())
		{
			client.setOutput(addr);
			gotAdded = true;
		}
	}
	if (dir & DIR_INPUT)
	{
		if (!client.isInputSet())
		{
			client.setInput(addr);
			gotAdded = true;
		}
	}

	return gotAdded;
}

static void portRemove(snd_seq_addr_t addr)
{
	Client *client = findClientForPort(addr);

	if (!client)
		return;

	if (client->isInputSet() && *client->getInput() == addr)
	{
		client->clearInput();
	}
	if (client->isOutputSet() && *client->getOutput() == addr)
	{
		client->clearOutput();
	}
}

static void portAutoConnect(snd_seq_addr_t addr, PortDir dir)
{
	ClientType type;
	Client *client = findClientForPort(addr, &type);
	assert(client);

	if (!client)
		return;

	clients_t &list = type == CLIENT_SOFTWARE ? g_hwClients : g_swClients;

	for (clients_t::iterator itr = list.begin(); itr != list.end(); ++itr)
	{
		Client &client = itr->second;

		const snd_seq_addr_t *input = client.getInput();
		const snd_seq_addr_t *output = client.getOutput();

		if ((dir & DIR_INPUT) && output)
		{
			connect(*output, addr);
		}
		if (dir & DIR_OUTPUT && input)
		{
			connect(addr, *input);
		}
	}
}

static int portsInit()
{
	snd_seq_client_info_t *clientInfo;
	snd_seq_port_info_t *portInfo;

	snd_seq_client_info_alloca(&clientInfo);
	snd_seq_port_info_alloca(&portInfo);
	snd_seq_client_info_set_client(clientInfo, -1);
	while (snd_seq_query_next_client(g_seq, clientInfo) >= 0)
	{
		int clientId = snd_seq_client_info_get_client(clientInfo);

		snd_seq_port_info_set_client(portInfo, clientId);
		snd_seq_port_info_set_port(portInfo, -1);

		while (snd_seq_query_next_port(g_seq, portInfo) >= 0)
		{
			portAdd(*portInfo);
		}
	}

	// Initially connect everything together.
	for (clients_t::const_iterator hwClientItr = g_hwClients.begin(); hwClientItr != g_hwClients.end(); ++hwClientItr)
	{
		const Client &hwClient = hwClientItr->second;

		const snd_seq_addr_t *hwInput = hwClient.getInput();
		const snd_seq_addr_t *hwOutput = hwClient.getOutput();

		if (!hwInput && !hwOutput)
			continue;

		for (clients_t::const_iterator swClientItr = g_swClients.begin(); swClientItr != g_swClients.end(); ++swClientItr)
		{
			const Client &swClient = swClientItr->second;

			const snd_seq_addr_t *swInput = swClient.getInput();
			const snd_seq_addr_t *swOutput = swClient.getOutput();

			if (swInput && hwOutput)
			{
				connect(*hwOutput, *swInput);
			}
			if (swOutput && hwInput)
			{
				connect(*swOutput, *hwInput);
			}
		}
	}
}

static void seqUninit()
{
	if (g_port >= 0)
	{
		snd_seq_delete_simple_port(g_seq, g_port);
		g_port = -1;
	}
	if (g_seq)
	{
		snd_seq_close(g_seq);
		g_seq = NULL;
	}
}

static int seqInit()
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

	result = snd_seq_set_client_name(g_seq, "amidiauto");
	if (result < 0)
	{
		fprintf(stderr, "Failed setting client name! (%d)\n", result);
		goto error;
	}

	result = snd_seq_create_simple_port(
		g_seq,
		"amidiauto",
		SND_SEQ_PORT_CAP_WRITE |
		SND_SEQ_PORT_CAP_NO_EXPORT,
		SND_SEQ_PORT_TYPE_APPLICATION
		);

	if (result < 0)
	{
		fprintf(stderr, "Couldn't create a virtual MIDI port! (%d)\n", result);
		goto error;
	}

	g_port = result;

	result = snd_seq_connect_from(g_seq, g_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);

	if (result < 0)
	{
		fprintf(stderr, "Couldn't connect to System::Anounce port! (%d)\n", result);
		goto error;
	}

	return 0;

error:
	seqUninit();
	return result;
}

static bool handleSeqEvent(snd_seq_t *seq, int port)
{
	do
	{
		snd_seq_event_t *ev;
		snd_seq_event_input(seq, &ev);

		switch (ev->type)
		{
		case SND_SEQ_EVENT_PORT_START:
			{
				printf("%d:%d port appeared.\n", ev->data.addr.client, ev->data.addr.port);

				snd_seq_port_info_t *portInfo;
				snd_seq_port_info_alloca(&portInfo);

				int result = snd_seq_get_any_port_info(g_seq, ev->data.addr.client, ev->data.addr.port, portInfo);
				if (result >= 0)
				{
					if (portAdd(*portInfo))
						portAutoConnect(ev->data.addr, portGetDir(*portInfo));
				}
				else
				{
					fprintf(stderr, "Failed getting port %d:%d info: %d\n", ev->data.addr.client, ev->data.addr.port, result);
				}
			}
			break;
		case SND_SEQ_EVENT_PORT_EXIT:
			printf("%d:%d port removed.\n", ev->data.addr.client, ev->data.addr.port);

			portRemove(ev->data.addr);
			break;
		default:
			break;
		}

		snd_seq_free_event(ev);
	} while (snd_seq_event_input_pending(seq, 0) > 0);

	return false;
}

static int run()
{
	bool done = false;
	int npfd = 0;

	int result = seqInit();

	if (result < 0)
		goto cleanup;

	portsInit();

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
		int n = poll(fds, npfd, -1);
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
	printf("Version %x.%02x, Copyright (C) Blokas Labs " HOMEPAGE_URL "\n", AMIDIAUTO_VERSION >> 8, AMIDIAUTO_VERSION & 0xff);
}

static void printUsage()
{
	printf("Usage: amidiauto\n\nThat's all there is to it! :)\n\n");
	printVersion();
}

int main(int argc, char **argv)
{
	if (argc == 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0))
	{
		printVersion();
		return 0;
	}
	else if (argc != 1)
	{
		printUsage();
		return 0;
	}

	int result = run();

	if (result < 0)
		fprintf(stderr, "Error %d!\n", result);

	return result;
}
