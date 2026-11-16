/*
 * The modem: its AT commands and result codes, dialling and answering
 * against a fake far end, the escape with its guard time, DTR, the
 * phonebook, telnet, and two modems calling each other over TCP on the
 * loopback interface (modemnet.h).
 */
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/modem.h"
#include "../src/modemnet.h"
#include "test.h"

#define HZ 1000u                        /* (a tick a millisecond) */

static struct modem m;
static struct {
	char host[256];
	unsigned port;
	int dials, hangups, answers;
	bool can_dial, can_answer;
	uint8_t sent[1024];
	size_t sent_len;
} far;
static struct {
	int readies, line_changes;
	bool dcd, cts;
} dte;
static uint64_t now;

static bool fake_dial(void *ctx, const char *host, unsigned port)
{
	(void)ctx;
	snprintf(far.host, sizeof far.host, "%s", host);
	far.port = port;
	far.dials++;
	return far.can_dial;
}

static bool fake_answer(void *ctx)
{
	(void)ctx;
	far.answers++;
	return far.can_answer;
}

static void fake_send(void *ctx, const uint8_t *data, size_t len)
{
	(void)ctx;
	for (size_t k = 0; k < len && far.sent_len < sizeof far.sent; k++)
		far.sent[far.sent_len++] = data[k];
}

static void fake_hangup(void *ctx)
{
	(void)ctx;
	far.hangups++;
}

static void dte_ready(void *ctx)
{
	(void)ctx;
	dte.readies++;
}

static void dte_lines(void *ctx, bool dcd, bool cts)
{
	(void)ctx;
	dte.line_changes++;
	dte.dcd = dcd;
	dte.cts = cts;
}

static void fresh(void)
{
	memset(&m, 0, sizeof m);
	memset(&far, 0, sizeof far);
	memset(&dte, 0, sizeof dte);
	far.can_dial = far.can_answer = true;
	m.line = (struct modem_line){ fake_dial, fake_answer, fake_send, fake_hangup, NULL };
	m.dte = (struct modem_dte){ dte_ready, dte_lines, NULL };
	now = 0;
	modem_init(&m, HZ);
	modem_dte_lines(&m, true, true, now);
}

/* What the modem has for the computer, as text */
static const char *got(void)
{
	static char out[2048];
	size_t n = 0;
	int b;
	while ((b = modem_to_dte(&m)) >= 0 && n + 1 < sizeof out)
		out[n++] = (char)b;
	out[n] = '\0';
	return out;
}

/* Characters from the computer, 10 ms apart */
static void type(const char *s)
{
	for (; *s; s++) {
		modem_from_dte(&m, (uint8_t)*s, now);
		now += 10;
	}
}

static void wait_ms(uint64_t ms)
{
	now += ms;
	modem_set_time(&m, now);
}

static void test_commands(void)
{
	fresh();
	type("AT\r");
	CHECK(strcmp(got(), "AT\r\r\nOK\r\n") == 0);           /* (echoed: E1) */
	type("ATE0\r");
	CHECK(strcmp(got(), "ATE0\r\r\nOK\r\n") == 0);
	type("at\r");
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	type("ATJ\r");
	CHECK(strcmp(got(), "\r\nERROR\r\n") == 0);
	type("noise before AT\r");                              /* (what comes before AT is ignored) */
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	type("ATV0\r");
	CHECK(strcmp(got(), "0\r") == 0);
	type("ATJ\r");
	CHECK(strcmp(got(), "4\r") == 0);
	type("ATQ1\r");
	CHECK(strcmp(got(), "") == 0);
	type("ATQ0V1\r");
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);

	/* Mac OS 8.5's iMac Internal 56k script, and the rest it may send */
	type("AT&FE0Q0V1X4&C1&K3S95=1S7=75S0=0\r");
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	CHECK_EQ(m.s[7], 75);
	CHECK_EQ(m.s[95], 1);
	CHECK_EQ(m.dcd_mode, 1);
	CHECK(!m.echo);
	const char *more[] = { "AT\\N0\r", "AT%C0\r", "ATM0\r", "ATL3\r", "ATW2\r", "AT&D2\r", "ATX1S6=4\r",
			       "AT+MS=V90\r", "ATB0N1\r", "AT&FL3E0Q0V1X4&C1&K3S95=0&D2S7=75S0=0W2\r", "AT&W\r" };
	for (unsigned k = 0; k < sizeof more / sizeof more[0]; k++) {
		type(more[k]);
		CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	}
	type("ATS7?\r");
	CHECK(strcmp(got(), "\r\n075\r\n\r\nOK\r\n") == 0);
	type("ATS12=25S12?\r");
	CHECK(strcmp(got(), "\r\n025\r\n\r\nOK\r\n") == 0);
	type("ATI12\r");                                        /* (Mac OS 9's script picks its settings by this) */
	CHECK(strcmp(got(), "\r\nD1\r\n\r\nOK\r\n") == 0);
	type("ATZ\r");
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	CHECK(m.echo);
	CHECK_EQ(m.s[12], 50);
	type("A/");                                             /* (the last command again: ATZ) */
	CHECK(strcmp(got(), "A/\r\nOK\r\n") == 0);
	type("ATE0\rATX\b\r");                                  /* (backspace) */
	CHECK(strcmp(got(), "ATE0\r\r\nOK\r\n\r\nOK\r\n") == 0);
}

static void dial_up(const char *cmd)
{
	type(cmd);
	got();
	modem_connected(&m);
}

static void test_dialling(void)
{
	fresh();
	type("ATE0\r");
	got();
	CHECK(!dte.dcd);                                        /* (&C1: no carrier yet) */
	CHECK(dte.cts);
	type("ATDT127.0.0.1:5555\r");
	CHECK_EQ(far.dials, 1);
	CHECK(strcmp(far.host, "127.0.0.1") == 0);
	CHECK_EQ(far.port, 5555);
	CHECK(strcmp(got(), "") == 0);                          /* (until the far end answers) */
	CHECK_EQ(m.call, MODEM_DIALLING);
	modem_connected(&m);
	CHECK(strcmp(got(), "\r\nCONNECT 56000\r\n") == 0);
	CHECK(dte.dcd);
	CHECK(m.online);
	type("hello");
	CHECK_EQ(far.sent_len, 5);
	CHECK(memcmp(far.sent, "hello", 5) == 0);
	modem_received(&m, (const uint8_t *)"there", 5);
	CHECK(strcmp(got(), "there") == 0);
	modem_far_hangup(&m);
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
	CHECK(!dte.dcd);
	CHECK_EQ(m.call, MODEM_IDLE);

	/* busy, and with X0 only NO CARRIER */
	type("ATDTbusy.example.com\r");
	CHECK_EQ(far.port, 23);
	CHECK_EQ(m.telnet, MODEM_TELNET_ON);
	modem_failed(&m, true);
	CHECK(strcmp(got(), "\r\nBUSY\r\n") == 0);
	type("ATX0DT10.0.0.1:99\r");
	modem_failed(&m, true);
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
	type("ATX4\r");
	got();

	/* nothing behind the line (instruction time): NO CARRIER at once */
	far.can_dial = false;
	type("ATDT10.0.0.1:99\r");
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
	far.can_dial = true;

	/* a number nobody has, and S7's timeout */
	type("ATDT5551234\r");
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
	type("ATS7=3DT10.0.0.1:99\r");
	wait_ms(2900);
	CHECK(strcmp(got(), "") == 0);
	wait_ms(200);
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
	CHECK(far.hangups >= 1);

	/* a key pressed while dialling gives up */
	type("ATDT10.0.0.1:99\r");
	type("x");
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
	CHECK_EQ(m.call, MODEM_IDLE);

	/* ATH and ATO */
	dial_up("ATDT10.0.0.1:99\r");
	got();
	wait_ms(1100);
	type("+++");
	wait_ms(1100);
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	type("ATO\r");
	CHECK(strcmp(got(), "\r\nCONNECT 56000\r\n") == 0);
	wait_ms(1100);
	type("+++");
	wait_ms(1100);
	got();
	int hangups = far.hangups;
	type("ATH\r");
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	CHECK_EQ(far.hangups, hangups + 1);
	CHECK(!dte.dcd);
	type("ATO\r");
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
}

static void test_escape(void)
{
	fresh();
	type("ATE0\r");
	got();
	dial_up("ATDT10.0.0.1:99\r");
	got();
	far.sent_len = 0;

	/* without the guard time before it, +++ is data */
	type("x+++");
	wait_ms(1500);
	CHECK(m.online);
	CHECK(strcmp(got(), "") == 0);
	CHECK_EQ(far.sent_len, 4);

	/* nor with something right after it */
	wait_ms(1100);
	type("+++y");
	wait_ms(1500);
	CHECK(m.online);
	CHECK_EQ(far.sent_len, 8);

	/* with both: command mode, the call kept, the +++ having gone too */
	wait_ms(1100);
	type("+++");
	wait_ms(900);
	CHECK(m.online);
	wait_ms(200);
	CHECK(!m.online);
	CHECK_EQ(m.call, MODEM_CONNECTED);
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	CHECK_EQ(far.sent_len, 11);
	CHECK_EQ(modem_room(&m), 0);                    /* (the far end's data waits there in command mode) */
	type("ATO\r");
	got();

	/* S2 and S12 change the character and the guard time (1/50 s) */
	type("x");
	wait_ms(1100);
	type("+++");
	wait_ms(1100);
	got();
	type("ATS2=33S12=10O\r");
	CHECK(strcmp(got(), "\r\nCONNECT 56000\r\n") == 0);
	wait_ms(300);
	type("!!!");
	wait_ms(250);
	CHECK(!m.online);
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	type("ATS2=43S12=50O\r");
	got();

	/* +++ followed at once by a command, as Mac OS's modem scripts hang up ("+++ATH") */
	wait_ms(1100);
	int hangups = far.hangups;
	type("+++ATH\r");
	CHECK(!m.online);
	CHECK_EQ(m.call, MODEM_IDLE);
	CHECK_EQ(far.hangups, hangups + 1);
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);

	/* +++AT without its CR is data after all */
	dial_up("ATDT10.0.0.1:99\r");
	got();
	far.sent_len = 0;
	wait_ms(1100);
	type("+++ATx");
	CHECK_EQ(far.sent_len, 3);
	wait_ms(1100);
	CHECK(m.online);
	CHECK_EQ(far.sent_len, 6);
	CHECK(memcmp(far.sent, "+++ATx", 6) == 0);
}

static void test_dtr(void)
{
	fresh();
	type("ATE0\r");
	got();
	dial_up("ATDT10.0.0.1:99\r");
	got();
	/* a moment's drop (a serial driver resetting its channel) changes nothing */
	modem_dte_lines(&m, false, true, now);
	wait_ms(20);
	modem_dte_lines(&m, true, true, now);
	wait_ms(100);
	CHECK_EQ(m.call, MODEM_CONNECTED);
	CHECK(m.online);
	/* &D2: dropped for S25 (0.05 s), it hangs up */
	modem_dte_lines(&m, false, true, now);
	CHECK_EQ(modem_next_event(&m), now + 50);
	wait_ms(60);
	CHECK_EQ(m.call, MODEM_IDLE);
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
	/* &D1: to command mode, the call kept */
	modem_dte_lines(&m, true, true, now);
	type("AT&D1\r");
	got();
	dial_up("ATDT10.0.0.1:99\r");
	got();
	modem_dte_lines(&m, false, true, now);
	wait_ms(60);
	CHECK(!m.online);
	CHECK_EQ(m.call, MODEM_CONNECTED);
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	/* &D0: ignored; &C0: DCD always on */
	modem_dte_lines(&m, true, true, now);
	type("ATH&D0&C0\r");
	got();
	CHECK(dte.dcd);
	dial_up("ATDT10.0.0.1:99\r");
	modem_dte_lines(&m, false, true, now);
	wait_ms(100);
	CHECK(m.online);
}

static void test_answering(void)
{
	fresh();
	type("ATE0\r");
	got();
	wait_ms(0);
	modem_incoming(&m, "10.0.0.2:4000");
	wait_ms(0);
	CHECK(strcmp(got(), "\r\nRING\r\n") == 0);
	CHECK_EQ(m.s[1], 1);
	wait_ms(5999);
	CHECK(strcmp(got(), "") == 0);
	wait_ms(1);
	CHECK(strcmp(got(), "\r\nRING\r\n") == 0);
	type("ATA\r");
	CHECK_EQ(far.answers, 1);
	CHECK(strcmp(got(), "\r\nCONNECT 56000\r\n") == 0);
	CHECK(dte.dcd);
	char state[300];
	CHECK(strcmp(modem_state_text(&m, state, sizeof state), "connected from 10.0.0.2:4000") == 0);
	type("ATH\r");     /* (online: data) */
	CHECK_EQ(far.sent_len, 4);
	modem_far_hangup(&m);
	got();

	/* S0: answered after that many rings */
	type("ATS0=2\r");
	got();
	modem_incoming(&m, "10.0.0.2:4001");
	wait_ms(0);
	CHECK(strcmp(got(), "\r\nRING\r\n") == 0);
	wait_ms(6000);
	CHECK(strcmp(got(), "\r\nRING\r\n\r\nCONNECT 56000\r\n") == 0);
	CHECK_EQ(m.call, MODEM_CONNECTED);
	modem_far_hangup(&m);
	got();

	/* a caller who gives up: the ringing stops, with no result */
	modem_incoming(&m, "10.0.0.2:4002");
	wait_ms(0);
	got();
	modem_far_hangup(&m);
	CHECK_EQ(m.call, MODEM_IDLE);
	CHECK(strcmp(got(), "") == 0);
	type("ATA\r");
	CHECK(strcmp(got(), "\r\nNO CARRIER\r\n") == 0);
}

static void test_phonebook(void)
{
	struct modem_phonebook pb;
	char err[200];
	const char *text = "# BBSes\n"
			   "555-1234   bbs.example.com            # telnet, port 23\n"
			   "5552000 bbs.example.com:2323 telnet\n"
			   "  5553000\t127.0.0.1:5555 raw\n"
			   "\n"
			   "555-4000 [::1]:6000\n";
	CHECK_EQ(modem_phonebook_parse(&pb, text, err, sizeof err), 4);
	CHECK(strcmp(pb.entry[0].number, "5551234") == 0);
	CHECK(strcmp(pb.entry[0].host, "bbs.example.com") == 0);
	CHECK_EQ(pb.entry[0].port, 23);
	CHECK_EQ(pb.entry[1].telnet, MODEM_TELNET_ON);
	CHECK_EQ(pb.entry[2].telnet, MODEM_TELNET_OFF);
	CHECK(strcmp(pb.entry[3].host, "::1") == 0);
	CHECK_EQ(pb.entry[3].port, 6000);

	char host[256];
	unsigned port;
	enum modem_telnet telnet;
	CHECK(modem_address(&pb, "555-1234", host, sizeof host, &port, &telnet));
	CHECK(strcmp(host, "bbs.example.com") == 0 && port == 23 && telnet == MODEM_TELNET_ON);
	CHECK(modem_address(&pb, "5553000", host, sizeof host, &port, &telnet));
	CHECK(port == 5555 && telnet == MODEM_TELNET_OFF);
	CHECK(modem_address(&pb, "555 2000;", host, sizeof host, &port, &telnet));
	CHECK(port == 2323 && telnet == MODEM_TELNET_ON);
	CHECK(!modem_address(&pb, "5559999", host, sizeof host, &port, &telnet));
	CHECK(!modem_address(NULL, "", host, sizeof host, &port, &telnet));
	CHECK(modem_address(NULL, " localhost ", host, sizeof host, &port, &telnet));
	CHECK(strcmp(host, "localhost") == 0 && port == 23);
	CHECK(modem_address(NULL, "host.example.com:6400", host, sizeof host, &port, &telnet));
	CHECK(port == 6400 && telnet == MODEM_TELNET_AUTO);
	CHECK(!modem_address(NULL, "host:99999", host, sizeof host, &port, &telnet));
	CHECK(!modem_address(NULL, "host:", host, sizeof host, &port, &telnet));

	CHECK_EQ(modem_phonebook_parse(&pb, "5551234\n", err, sizeof err), -1);
	CHECK(strstr(err, "line 1") != NULL);
	CHECK_EQ(modem_phonebook_parse(&pb, "# fine\n5551234 host:23 ssh\n", err, sizeof err), -1);
	CHECK(strstr(err, "line 2") != NULL);
	CHECK_EQ(modem_phonebook_parse(&pb, "5551234 5551235\n", err, sizeof err), -1);

	/* dialled through the modem, in parts (";" stays in command mode, as the iMac's script dials) */
	fresh();
	CHECK_EQ(modem_phonebook_parse(&pb, text, err, sizeof err), 4);
	m.book = &pb;
	type("ATE0\r");
	got();
	type("ATDT555;\r");
	CHECK(strcmp(got(), "\r\nOK\r\n") == 0);
	CHECK_EQ(far.dials, 0);
	type("ATDT-3000\r");
	CHECK_EQ(far.dials, 1);
	CHECK(strcmp(far.host, "127.0.0.1") == 0 && far.port == 5555);
}

static void test_telnet(void)
{
	static const uint8_t iac_do_binary[3] = { 255, 253, 0 };
	fresh();
	type("ATE0\r");
	got();
	dial_up("ATDTbbs.example.com\r");      /* (port 23: telnet) */
	got();
	far.sent_len = 0;
	/* negotiation: binary and suppress-go-ahead both ways, echo from the far end, nothing else */
	const uint8_t in[] = { 'a', 255, 253, 0, 255, 251, 1, 255, 251, 3, 255, 253, 24, 255, 250, 24, 1, 255, 240,
			       255, 255, 'b', 255, 241, 'c', 255, 253, 0 };
	modem_received(&m, in, sizeof in);
	const char *text = got();
	CHECK_EQ(strlen(text), 4);
	CHECK(memcmp(text, "a\xFF" "bc", 4) == 0);
	const uint8_t replies[] = { 255, 251, 0, 255, 253, 1, 255, 253, 3, 255, 252, 24 };
	CHECK_EQ(far.sent_len, sizeof replies);
	CHECK(memcmp(far.sent, replies, sizeof replies) == 0);  /* (and DO binary again: no reply) */
	far.sent_len = 0;
	uint8_t ff = 0xFF;
	modem_from_dte(&m, ff, now);            /* (0xFF doubled) */
	CHECK_EQ(far.sent_len, 2);
	CHECK(far.sent[0] == 0xFF && far.sent[1] == 0xFF);
	modem_far_hangup(&m);
	got();

	/* another port: telnet only if the far end begins with it */
	dial_up("ATDThost.example.com:6400\r");
	got();
	modem_received(&m, (const uint8_t *)"\xFF\xFFplain", 7);
	CHECK(memcmp(got(), "\xFF\xFFplain", 7) == 0);
	CHECK_EQ(m.telnet, MODEM_TELNET_OFF);
	modem_far_hangup(&m);
	got();
	dial_up("ATDThost.example.com:6400\r");
	got();
	far.sent_len = 0;
	modem_received(&m, iac_do_binary, 3);
	CHECK_EQ(m.telnet, MODEM_TELNET_ON);
	CHECK_EQ(far.sent_len, 3);
	CHECK(strcmp(got(), "") == 0);
}

/* ---- two modems calling each other over TCP (loopback) ---- */

static struct modem pair[2];
static struct modemnet net[2];

static const char *pair_got(int k)
{
	static char out[2][512];
	size_t n = 0;
	int b;
	while ((b = modem_to_dte(&pair[k])) >= 0 && n + 1 < sizeof out[k])
		out[k][n++] = (char)b;
	out[k][n] = '\0';
	return out[k];
}

static void pair_type(int k, const char *s)
{
	for (; *s; s++)
		modem_from_dte(&pair[k], (uint8_t)*s, pair[k].now);
}

/* Polls both until `done` says so or a second has passed */
static bool pair_until(bool (*done)(void))
{
	for (int t = 0; t < 1000; t++) {
		modemnet_poll(&net[0]);
		modemnet_poll(&net[1]);
		if (done())
			return true;
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL);
	}
	return false;
}

static bool ringing(void) { return pair[0].call == MODEM_RINGING; }
static bool dialled(void) { return pair[1].call == MODEM_CONNECTED; }
static bool data_in(void) { return pair[0].qcount >= 5; }
static bool reply_in(void) { return pair[1].qcount >= 6; }
static bool hung_up(void) { return pair[0].call == MODEM_IDLE; }

static void test_tcp(void)
{
	char err[300];
	for (int k = 0; k < 2; k++) {
		memset(&pair[k], 0, sizeof pair[k]);
		modemnet_init(&net[k], &pair[k]);
		pair[k].line = (struct modem_line){ modemnet_dial, modemnet_answer, modemnet_send, modemnet_hangup,
						    &net[k] };
		modem_init(&pair[k], HZ);
		modem_dte_lines(&pair[k], true, true, 0);
		pair_type(k, "ATE0\r");
		pair_got(k);
	}
	if (!modemnet_listen(&net[0], "127.0.0.1:0", err, sizeof err)) {
		printf("modem: no listening on the loopback interface here (%s): skipped\n", err);
		return;
	}
	CHECK(net[0].port != 0);
	CHECK(modemnet_active(&net[0]));
	CHECK(!modemnet_active(&net[1]));
	char cmd[64];
	snprintf(cmd, sizeof cmd, "ATDT127.0.0.1:%u\r", net[0].port);
	pair_type(1, cmd);
	CHECK(pair_until(ringing));
	modem_set_time(&pair[0], 0);
	CHECK(strcmp(pair_got(0), "\r\nRING\r\n") == 0);
	CHECK(pair_until(dialled));
	CHECK(strcmp(pair_got(1), "\r\nCONNECT 56000\r\n") == 0);
	pair_type(0, "ATA\r");
	CHECK(strcmp(pair_got(0), "\r\nCONNECT 56000\r\n") == 0);

	pair_type(1, "hello");
	CHECK(pair_until(data_in));
	CHECK(strcmp(pair_got(0), "hello") == 0);
	pair_type(0, "there!");
	CHECK(pair_until(reply_in));
	CHECK(strcmp(pair_got(1), "there!") == 0);

	pair[1].now += 1100;
	pair_type(1, "+++ATH\r");
	CHECK(strcmp(pair_got(1), "\r\nOK\r\n") == 0);
	CHECK(pair_until(hung_up));
	CHECK(strcmp(pair_got(0), "+++\r\nNO CARRIER\r\n") == 0);     /* (the +++ went over first) */

	/* nobody there: NO CARRIER (refused: BUSY) */
	int closed = net[0].port;
	modemnet_free(&net[0]);
	snprintf(cmd, sizeof cmd, "ATDT127.0.0.1:%d\r", closed);
	pair_type(1, cmd);
	for (int t = 0; t < 1000 && pair[1].call == MODEM_DIALLING; t++) {
		modemnet_poll(&net[1]);
		nanosleep(&(struct timespec){ 0, 1000000 }, NULL);
	}
	CHECK(strcmp(pair_got(1), "\r\nBUSY\r\n") == 0);
	modemnet_free(&net[1]);
}

void test_modem(void)
{
	test_commands();
	test_dialling();
	test_escape();
	test_dtr();
	test_answering();
	test_phonebook();
	test_telnet();
	test_tcp();
}
