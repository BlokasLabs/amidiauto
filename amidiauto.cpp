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

#include <string>
#include <map>
#include <set>

#define HOMEPAGE_URL "https://blokas.io/"
#define AMIDIAUTO_VERSION 0x0101

static snd_seq_t *g_seq = NULL;
static int g_port = -1;

enum PortDir
{
	DIR_UNKNOWN = 0,
	DIR_INPUT   = 1 << 0,
	DIR_OUTPUT  = 1 << 1,
	DIR_DUPLEX  = DIR_INPUT | DIR_OUTPUT
};

enum PortType
{
	TYPE_SOFTWARE = 0,
	TYPE_HARDWARE = 1,
};

inline static bool operator <(const snd_seq_addr_t &a, const snd_seq_addr_t &b)
{
	return std::make_pair(a.client, a.port) < std::make_pair(b.client, b.port);
}

inline static bool operator ==(const snd_seq_addr_t &a, const snd_seq_addr_t &b)
{
	return a.client == b.client && a.port == b.port;
}

static std::string getClientName(snd_seq_addr_t addr)
{
	snd_seq_client_info_t *clientInfo;
	snd_seq_client_info_alloca(&clientInfo);
	if (snd_seq_get_any_client_info(g_seq, addr.client, clientInfo) < 0)
		return "";

	const char *name = snd_seq_client_info_get_name(clientInfo);

	return name ? name : "";
}

class ConnectionRules
{
public:
	enum Type
	{
		TYPE_UNKNOWN  = -1,
		TYPE_ALLOW    = 0,
		TYPE_DISALLOW = 1,
	};

	enum Strength
	{
		STRENGTH_NONE       = 0, // Rule does not apply.
		STRENGTH_VERY_VAGUE = 1, // Wildcard matched wildcard.
		STRENGTH_VAGUE      = 2, // One of the sides is a specific name, the other is a wildcard.
		STRENGTH_SPECIFIC   = 3, // Both sides is a specific name.
	};

	void addRule(Type type, const char *output, const char *input);

	bool hasRules() const;

	bool isConnectionAllowed(snd_seq_addr_t output, snd_seq_addr_t input, Strength minimumStrength) const;

private:
	typedef std::pair<std::string, std::string> rule_t;
	typedef std::multimap<std::string, std::string> rules_t;

	Strength evaluate(const rules_t &rules, snd_seq_addr_t output, snd_seq_addr_t input) const;
	static Strength evaluate(const rule_t &rule, const std::string &outputName, const std::string &inputName);

	rules_t m_allowRules;
	rules_t m_disallowRules;
};

void ConnectionRules::addRule(Type type, const char * output, const char * input)
{
	if (!output || !input || type == TYPE_UNKNOWN)
		return;

	if (strchr(output, '*') != NULL && strlen(output) > 1)
		return;

	if (strchr(input, '*') != NULL && strlen(input) > 1)
		return;

	rules_t &rules = type == TYPE_ALLOW ? m_allowRules : m_disallowRules;

	fprintf(stderr, "%s '%s' -> '%s'\n", type == TYPE_ALLOW ? "Allowing" : "Disallowing", output, input);

	rules.insert(std::make_pair(output, input));
}

bool ConnectionRules::hasRules() const
{
	return !m_allowRules.empty() || !m_disallowRules.empty();
}

bool ConnectionRules::isConnectionAllowed(snd_seq_addr_t output, snd_seq_addr_t input, Strength minimumStrength) const
{
	Strength allowStrength = evaluate(m_allowRules, output, input);
	Strength disallowStrength = evaluate(m_disallowRules, output, input);

	return allowStrength >= minimumStrength && allowStrength >= disallowStrength;
}

ConnectionRules::Strength ConnectionRules::evaluate(const rules_t &rules, snd_seq_addr_t output, snd_seq_addr_t input) const
{
	std::string outputName = getClientName(output);
	std::string inputName = getClientName(input);

	Strength strength = STRENGTH_NONE;

	for (rules_t::const_iterator itr = rules.begin(); itr != rules.end(); ++itr)
	{
		Strength s = evaluate(*itr, outputName, inputName);
		if (strength < s)
			strength = s;
	}

	return strength;
}

ConnectionRules::Strength ConnectionRules::evaluate(const rule_t & rule, const std::string &outputName, const std::string &inputName)
{
	if (rule.first == "*")
	{
		if (rule.second == "*")
		{
			return STRENGTH_VERY_VAGUE;
		}
		else if (inputName.find(rule.second) != std::string::npos)
		{
			return STRENGTH_VAGUE;
		}
	}
	else if (outputName.find(rule.first) != std::string::npos)
	{
		if (rule.second == "*")
		{
			return STRENGTH_VAGUE;
		}
		else if (inputName.find(rule.second) != std::string::npos)
		{
			return STRENGTH_SPECIFIC;
		}
	}

	return STRENGTH_NONE;
}

static ConnectionRules g_rules;

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

static PortType portGetType(const snd_seq_port_info_t &portInfo)
{
	const char *name = snd_seq_port_info_get_name(&portInfo);

	if (strncmp("TouchOSC Bridge", name, 15) == 0)
	{
		// Treat touchosc2midi ports as 'hardware' ones.
		return TYPE_HARDWARE;
	}

	unsigned int type = snd_seq_port_info_get_type(&portInfo);

	return (type & SND_SEQ_PORT_TYPE_APPLICATION) ? TYPE_SOFTWARE : TYPE_HARDWARE;
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

	PortType type = portGetType(portInfo);
	clients_t &list = (type == TYPE_SOFTWARE) ? g_swClients : g_hwClients;

	PortDir dir = portGetDir(portInfo);

	bool gotAdded = false;

	snd_seq_addr_t addr = *snd_seq_port_info_get_addr(&portInfo);

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

	clients_t *lists[2] = { &g_swClients, &g_hwClients };
	ConnectionRules::Strength strengths[2] =
	{
		type == CLIENT_SOFTWARE ? ConnectionRules::STRENGTH_SPECIFIC : ConnectionRules::STRENGTH_VERY_VAGUE,
		type == CLIENT_SOFTWARE ? ConnectionRules::STRENGTH_VERY_VAGUE : ConnectionRules::STRENGTH_SPECIFIC
	};

	for (int i=0; i<2; ++i)
	{
		clients_t &list = *lists[i];
		ConnectionRules::Strength strength = strengths[i];

		for (clients_t::iterator itr = list.begin(); itr != list.end(); ++itr)
		{
			Client &client = itr->second;

			const snd_seq_addr_t *input = client.getInput();
			const snd_seq_addr_t *output = client.getOutput();

			if ((dir & DIR_INPUT) && output)
			{
				if (g_rules.isConnectionAllowed(*output, addr, strength))
					connect(*output, addr);
			}
			if (dir & DIR_OUTPUT && input)
			{
				if (g_rules.isConnectionAllowed(addr, *input, strength))
					connect(addr, *input);
			}
		}
	}
}

static void portsConnectAll(const clients_t &listA, const clients_t &listB, ConnectionRules::Strength minimumStrength)
{
	std::set<std::pair<snd_seq_addr_t, snd_seq_addr_t> > handled;

	for (clients_t::const_iterator listAItr = listA.begin(); listAItr != listA.end(); ++listAItr)
	{
		const Client &aClient = listAItr->second;

		const snd_seq_addr_t *aInput = aClient.getInput();
		const snd_seq_addr_t *aOutput = aClient.getOutput();

		if (!aInput && !aOutput)
			continue;

		for (clients_t::const_iterator listBItr = listB.begin(); listBItr != listB.end(); ++listBItr)
		{
			const Client &bClient = listBItr->second;

			const snd_seq_addr_t *bInput = bClient.getInput();
			const snd_seq_addr_t *bOutput = bClient.getOutput();

			if (bInput && aOutput && handled.find(std::make_pair(*aOutput, *bInput)) == handled.end())
			{
				if (g_rules.isConnectionAllowed(*aOutput, *bInput, minimumStrength))
				{
					connect(*aOutput, *bInput);
					handled.insert(std::make_pair(*aOutput, *bInput));
				}
			}
			if (bOutput && aInput && handled.find(std::make_pair(*bOutput, *aInput)) == handled.end())
			{
				if (g_rules.isConnectionAllowed(*bOutput, *aInput, minimumStrength))
				{
					connect(*bOutput, *aInput);
					handled.insert(std::make_pair(*bOutput, *aInput));
				}
			}
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
	portsConnectAll(g_hwClients, g_swClients, ConnectionRules::STRENGTH_VERY_VAGUE);
	portsConnectAll(g_hwClients, g_hwClients, ConnectionRules::STRENGTH_SPECIFIC);
	portsConnectAll(g_swClients, g_swClients, ConnectionRules::STRENGTH_SPECIFIC);
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
		fprintf(stderr, "Unexpected count (%d) of seq fds! Expected 1!\n", npfd);
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

static char *trimWhiteSpace(char *str)
{
	char *end = strchr(str, '\0')-1;

	while (end > str && isspace(*end))
		--end;

	*(end+1) = '\0';

	while (isspace(*str))
		++str;

	return str;
}

static int parseRuleFile(ConnectionRules &rules, const char *fileName)
{
	if (!fileName)
		return -EINVAL;

	FILE *f = fopen(fileName, "rt");
	if (!f)
		return -ENOENT;

	fprintf(stderr, "Reading rules in '%s'...\n", fileName);

	enum { MAX_LENGTH = 1024 };
	char l[MAX_LENGTH];

	unsigned int i=0;

	ConnectionRules::Type type = ConnectionRules::TYPE_UNKNOWN;

	while (!feof(f) && fgets(l, MAX_LENGTH, f) != NULL)
	{
		char *commentMarker = strchr(l, '#');
		if (commentMarker)
			*commentMarker = '\0'; // Cut the string at the beginning of a comment.

		char *line = trimWhiteSpace(l);
		++i;

		if (strlen(line) == 0)
		{
			continue;
		}
		else if (line[0] == '[')
		{
			if (strcmp(line+1, "allow]") == 0)
			{
				type = ConnectionRules::TYPE_ALLOW;
				continue;
			}
			else if (strcmp(line+1, "disallow]") == 0)
			{
				type = ConnectionRules::TYPE_DISALLOW;
				continue;
			}
			else
			{
				fprintf(stderr, "Unknown section on line %u!\n", i-1);
				type = ConnectionRules::TYPE_UNKNOWN;
				continue;
			}
		}

		if (type == ConnectionRules::TYPE_UNKNOWN)
		{
			fprintf(stderr, "Ignoring line %u which is not within [allow] or [disallow] section!\n", i-1);
			continue;
		}

		PortDir dir;

		char *specifier;

		if ((specifier = strstr(line, "<->")) != NULL)
		{
			dir = DIR_DUPLEX;
		}
		else if ((specifier = strstr(line, "->")) != NULL)
		{
			dir = DIR_OUTPUT;
		}
		else if ((specifier = strstr(line, "<-")) != NULL)
		{
			dir = DIR_INPUT;
		}
		else
		{
			fprintf(stderr, "Ignoring line %u, it's missing a direction specifier!\n", i);
			continue;
		}

		*specifier = '\0';

		char *left = line;
		left = trimWhiteSpace(left);

		char *right = specifier + (dir == DIR_DUPLEX ? 3 : 2);
		right = trimWhiteSpace(right);

		if (*left == '\0' || *right == '\0')
		{
			fprintf(stderr, "Ignoring line %u, it's incomplete!\n", i);
			continue;
		}

		switch (dir)
		{
		case DIR_DUPLEX:
			rules.addRule(type, left, right);
			if (strcmp(left, right) != 0)
				rules.addRule(type, right, left);
			break;
		case DIR_INPUT:
			rules.addRule(type, right, left);
			break;
		case DIR_OUTPUT:
			rules.addRule(type, left, right);
			break;
		}
	}

	fclose(f);

	return 0;
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

	int result = parseRuleFile(g_rules, "/etc/amidiauto.conf");

	if (result < 0)
	{
		fprintf(stderr, "Reading '/etc/amidiauto.conf' failed! (%d)\n", result);
	}

	if (!g_rules.hasRules())
	{
		printf("Using default 'allow all' rule.\n", result);
		g_rules.addRule(ConnectionRules::TYPE_ALLOW, "*", "*");
	}

	result = run();

	if (result < 0)
		fprintf(stderr, "Error %d!\n", result);

	return result;
}
