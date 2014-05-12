/*
 * evmapd - An input event remapping daemon for Linux
 *
 * Copyright (c) 2007 Theodoros V. Kalamatianos <nyb@users.sourceforge.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 *
 * Compilation:
 *
 * gcc -Wall -lcfg+ evmapd.c -o evmapd
 */



#define UINPUT_DEVICE		"/dev/input/uinput"

#define DEBUG			1



#include <cfg+.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>



#define msg(m, ...)		info("%s: " m, argv0, ##__VA_ARGS__)

#define RETERR(c, E, r, m, ...)	if (c) { \
					int e = errno; \
					msg(m ": %s\n", ##__VA_ARGS__ , strerror(e)); \
					return (E)?(r):e; \
				}
#define RETERN(c, m, ...)	RETERR(c, 0, 0, m, ##__VA_ARGS__)

#define cfree(p)		if (p != NULL) free(p);



static int detach = 0, grab = 0, log = 0, quiet = 0, verbose = 0;



static char *argv0, *idev = NULL, *pidfile = NULL;

static int ifp = -1, ofp = -1;
static int *kkm = NULL, *krm = NULL, *kam = NULL, *rkm = NULL, *rrm = NULL;
static int *ram = NULL, *akm = NULL, *arm = NULL, *aam = NULL, *nm = NULL;

#define ARR(a, c, x, y)		((a)[((x) * (c)) + (y)])

#define KKM(x, y)		ARR(kkm, 2, (x), (y))
#define KRM(x, y)		ARR(krm, 3, (x), (y))
#define KAM(x, y)		ARR(kam, 3, (x), (y))

#define RKM(x, y)		ARR(rkm, 3, (x), (y))
#define RRM(x, y)		ARR(rrm, 2, (x), (y))
#define RAM(x, y)		ARR(ram, 2, (x), (y))

#define AKM(x, y)		ARR(akm, 3, (x), (y))
#define ARM(x, y)		ARR(arm, 2, (x), (y))
#define AAM(x, y)		ARR(aam, 2, (x), (y))



int info(const char *fmt, ...)
{
	va_list args;
	int ret = 0;

	va_start(args, fmt);
	if (fileno(stderr) >= 0)
		ret = vfprintf(stderr, fmt, args);
	if (log > 1)
		vsyslog(LOG_NOTICE, fmt, args);
	va_end(args);

	return ret;
}

/* PID file creation */
static int write_pid() {
	FILE *fp;

	fp = fopen(pidfile, "w");
	if (fp == NULL)
		return -1;

	fprintf(fp, "%i\n", getpid());

	if (verbose)
		info("Wrote PID file %s\n", pidfile);

	fclose(fp);

	return 0;
}

static void rfree(char **c)
{
	int i;

	if (c == NULL)
		return;

	for (i = 0; c[i] != NULL; ++i)
		free(c[i]);

	free(c);
}

/* Allow SIGTERM to cause graceful termination */
void on_term(int s)
{
	int ret;

	if (detach)
		info("evmapd %s terminating for %s\n", VERSION, idev);

	if (grab) {
		ret = ioctl(ifp, EVIOCGRAB, (void *)0);
		if (ret != 0)
			msg("Warning: could not release %s\n", idev);
	}

	if (log)
		closelog();
	close(ifp);
	close(ofp);

	cfree(idev);
	cfree(kkm);
	cfree(krm);
	cfree(kam);
	cfree(rkm);
	cfree(rrm);
	cfree(ram);
	cfree(akm);
	cfree(arm);
	cfree(aam);
	cfree(nm);

	if (pidfile != NULL) {
		unlink(pidfile);
		free(pidfile);
	}

	exit(0);

	return;
}



/* Convert an array of strings to an int array */
static int str_int(char **s, int **r, char *conv, int col)
{
	int c[4] = { 0, 0, 0, 0 }, i, j, m = 0, n = 0, ret;

	*r = NULL;

	if (s == NULL)
		return 0;
	if ((col < 1) || (col > 4))
		return -1;

	/* Count the valid strings */
	for (i = 0; s[i] != NULL; i++) {
		ret = sscanf(s[i], conv, &(c[0]), &(c[1]), &(c[2]), &(c[3]));
		if (ret == col)
			++n;
	}
	if (n == 0)
		return 0;

	*r = malloc((n + 1) * col * sizeof(int));
	if (*r == NULL)
		return -1;

	/* Assign valid strings */
	for (i = 0; s[i] != NULL; i++) {
		ret = sscanf(s[i], conv, &(c[0]), &(c[1]), &(c[2]), &(c[3]));
		if (ret == col) {
			for (j = 0; j < col; ++j)
				ARR(*r, col, m, j) = c[j];
				++m;
#if DEBUG
			if (m > n) {
				msg("Internal error [%i]", __LINE__);
				errno = ECANCELED;
				return -1;
			}
#endif
		}
	}
	for (i = 0; i < col; ++i)
		ARR(*r, col, n, i) = -1;

#if DEBUG
	if (m != n) {
		msg("Internal error [%i]", __LINE__);
		errno = ECANCELED;
		return -1;
	}
#endif

	return 0;
}

#define STRINT(s, r, conv, col)	ret = str_int(s, &r, conv, col); \
				RETERN(ret < 0, "String array conversion failed"); \
				rfree(s);



#define VERTRIPLET(v)		(v >> 16), (v >> 8) & 0xff, (v & 0xff)

#define USAGE			"evmapd Version " VERSION "\n" \
				"Usage: evmapd -i <input_device> [options]\n" \
				"    General options:\n" \
				"        -D, --daemon		Launch in daemon mode\n" \
				"        -g, --grab		Grab the input device\n" \
				"        -h, --help		Show this help text\n" \
				"        -i, --idev <device>	Specify the device to use for input\n" \
				"        -l, --log		Use the syslog facilities for logging\n" \
				"        -o, --odev <device>	Specify the device to use for output\n" \
				"        -p, --pidfile <file>	Use a file to store the PID\n" \
				"        -q, --quiet		Suppress all console messages\n" \
				"        -v, --verbose		Emit more verbose messages\n" \
				"        -V, --version		Show version information\n" \
				"\n" \
				"    Event remapping options:\n" \
				"        --key-key <from-key>:<to-key>\n" \
				"        --key-rel <from-min-key>,<from-max-key>:<to-rel>\n" \
				"        --key-abs <from-min-key>,<from-max-key>:<to-abs>\n" \
				"        --rel-key <from-rel>:<to-min-key>,<to-max-key>\n" \
				"        --rel-rel <from-rel>:<to-rel>\n" \
				"        --rel-abs <from-rel>:<to-abs>\n" \
				"        --abs-key <from-abs>:<to-min-key>,<to-max-key>\n" \
				"        --abs-rel <from-abs>:<to-rel>\n" \
				"        --abs-abs <from-abs>:<to-abs>\n" \
				"\n" \
				"    <*-key>, <*-rel> and <*-abs> are numeric event codes.\n" \
				"    Multiple remapping options may be specified.\n" \
				"\n" \
				"    Default values:\n" \
				"        --absconf <default-abs-min>,<default-abs-max>\n" \
				"        --relconf <default-rel-min>,<default-rel-max>\n" \
				"\n" \
				"    ABS event normalisation options:\n" \
				"        --norm <abs>\n" \
				"        --normconf <ignore>[,<range>[,<rst>[,<spike>[,<min-spike>]]]]\n" \
				"\n" \
				"        <ignore>    Number of initial events to ignore. Avoids\n" \
				"                    confusing the normalisation code when the\n" \
				"		     device is still settling.\n" \
				"\n" \
				"        <range>     Require at least 1/<range> coverage of the\n" \
				"                    absolute range to perform normalisation.\n" \
				"\n" \
				"        <rst>       Reset the normalisation algorithm every <rst>\n" \
				"                    ABS events.\n" \
				"\n" \
				"        <spike>     Ignore ABS axis changes over 1/<spike> of the\n" \
				"                    absolute range. May help with devices that\n" \
				"                    every now and then perform erratically and\n" \
				"                    send out random values\n" \
				"\n" \
				"        <min-spike> Require <min-spike> absolute range to perform\n" \
				"                    spike detection. Setting this to a small value\n" \
				"                    avoids algorithm artifacts with devices with\n" \
				"                    small ranges, such as joystick POV switches.\n" \
				"\n" \
				"    The --norm option may be used multiple times to specify more\n" \
				"    than one ABS axis to perform normalisation on.\n" \
				"\n"



static int usage(int r)
{
	info(USAGE);
	return r;
}

#define EV_EV			0

#define LEN(t, b)		(((b - 1) / (sizeof(t) * 8)) + 1)
#define POS(c, b)		(b / (sizeof((c)[0]) * 8))
#define OFF(c, b)		(b % (sizeof((c)[0]) * 8))
#define GET(c, b)		((c[POS(c, b)] >> OFF(c, b)) & 1)
#define SET(c, b, v)		(c)[POS(c, b)] = (((c)[POS(c, b)] & ~(1 << OFF(c, b))) | (((v) > 0) << OFF(c, b)))

#if DEBUG
#define INQ(i, m)		ret = ioctl(ifp, EVIOC##i, m); \
				RETERN(ret < 0, "Unable to query input device %s (%i)", idev, __LINE__)
#define OSET(i, m)		ret = ioctl(ofp, UI_##i, m); \
				RETERN(ret < 0, "Unable to configure output device %s (%i)", odev, __LINE__)
#else
#define INQ(i, m)		ret = ioctl(ifp, EVIOC##i, m); \
				RETERN(ret < 0, "Unable to query input device %s", idev)
#define OSET(i, m)		ret = ioctl(ofp, UI_##i, m); \
				RETERN(ret < 0, "Unable to configure output device %s", odev)
#endif

#define OSETBIT(t)		for (i = 0; i < t##_MAX; ++i) \
					if GET(obits[EV_##t], i) \
						OSET(SET_##t##BIT, i); \

#define NONEG(x)		if (x < 0) x = 0;

#define AC			ac[ev.code]

static void listbits(unsigned long evbits[EV_MAX][LEN(long, KEY_MAX)], int bits, int max, char *dsc)
{
	int i, j = 0;

	if GET(evbits[0], bits) {
		info("\t%s:\n", dsc);
		for (i = 0; i < max; ++i) {
			if GET(evbits[bits], i) {
				if ((j % 8) == 0)
					info("\t");
				info("\t%d", i);
				++j;
				if ((j % 8) == 0)
					info("\n");
			}
		}
		info("\n%s", ((j % 8) != 0)?"\n":"");
	}
}

int main(int argc, char **argv)
{
	int help = 0, version = 0;
	int amin = -32767, amax = 32767, rmin = -128, rmax = 128;
	int nign = 0, nrng = 0, nrst = 0, nspk = 0, nspkmin = 2; 

	char *odev = UINPUT_DEVICE;

	int iver;
	char iphys[256];
	struct uinput_user_dev uidev = {};
	unsigned long ibits[EV_MAX][LEN(long, KEY_MAX)];

	char ophys[256];
	struct uinput_user_dev uodev = {};
	unsigned long obits[EV_MAX][LEN(long, KEY_MAX)];

	int i, j, ret;
	unsigned long rbits[EV_MAX][LEN(long, KEY_MAX)];
	struct input_event ev;


	argv0 = argv[0];

	char **kkmap = NULL, **krmap = NULL, **kamap = NULL, **rkmap = NULL, **rrmap = NULL;
	char **ramap = NULL, **akmap = NULL, **armap = NULL, **aamap = NULL;
	char *acfg = NULL, *ncfg = NULL, *rcfg = NULL;

	struct cfg_option options[] = {
		{"daemon",	'D',	NULL, CFG_BOOL,		(void *) &detach,	0},
		{"grab",	'g',	NULL, CFG_BOOL,		(void *) &grab,		0},
		{"help",	'h',	NULL, CFG_BOOL,		(void *) &help,		0},
		{"log",		'l',	NULL, CFG_BOOL,		(void *) &log,		0},
		{"quiet",	'q',	NULL, CFG_BOOL,		(void *) &quiet,	0},
		{"verbose",	'v',	NULL, CFG_BOOL,		(void *) &verbose,	0},
		{"version",	'V',	NULL, CFG_BOOL,		(void *) &version,	0},

		{"idev",	'i',	NULL, CFG_STR,		(void *) &idev,		0},
		{"odev",	'o',	NULL, CFG_STR,		(void *) &odev,		0},
		{"pidfile",	'p',	NULL, CFG_STR,		(void *) &pidfile,	0},

		{"key-key",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &kkmap,	0},
		{"key-rel",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &krmap,	0},
		{"key-abs",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &kamap,	0},
		{"rel-key",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &rkmap,	0},
		{"rel-rel",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &rrmap,	0},
		{"rel-abs",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &ramap,	0},
		{"abs-key",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &akmap,	0},
		{"abs-rel",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &armap,	0},
		{"abs-abs",	0,	NULL, CFG_STR+CFG_MULTI,(void *) &aamap,	0},

		{"absconf",	0,	NULL, CFG_STR,		(void *) &acfg,		0},
		{"relconf",	0,	NULL, CFG_STR,		(void *) &rcfg,		0},

		{"norm",	0,	NULL, CFG_INT+CFG_MULTI,(void *) &nm,		0},
		{"normconf",	0,	NULL, CFG_STR,		(void *) &ncfg,		0},

		CFG_END_OF_LIST
	};

	CFG_CONTEXT cxt = cfg_get_context(options);
	RETERN(cxt == NULL, "Cannot parse command line arguments");

	cfg_set_cmdline_context(cxt, 1, -1, argv);
	ret = cfg_parse(cxt);
	if (ret != CFG_OK) {
		msg("Cannot parse command line arguments: %s\n", cfg_get_error_str(cxt));
		return EINVAL;
	}


	if (help)
		return usage(0);
	if (version) {
		info("evmapd Version " VERSION "\n");
		return 0;
	}
	if (idev == NULL) {
		msg("No input device specified\n\n");
		return usage(EINVAL);
	}

	/* Map parsing */
	STRINT(kkmap, kkm, "%i:%i", 2);
	STRINT(krmap, krm, "%i,%i:%i", 3);
	STRINT(kamap, kam, "%i,%i:%i", 3);
	STRINT(rkmap, rkm, "%i:%i,%i", 3);
	STRINT(rrmap, rrm, "%i:%i", 2);
	STRINT(ramap, ram, "%i:%i", 2);
	STRINT(akmap, akm, "%i:%i,%i", 3);
	STRINT(armap, arm, "%i:%i", 2);
	STRINT(aamap, aam, "%i:%i", 2);

	/* Fine-tuning controls */
	if (acfg != NULL) {
		ret = sscanf(acfg, "%i,%i", &amin, &amax);
		RETERR(ret < 1, ret >= 0, EINVAL, "Could not parse absconf parameters");
		free(acfg);
	}
	if (rcfg != NULL) {
		ret = sscanf(rcfg, "%i,%i", &rmin, &rmax);
		RETERR(ret < 1, ret >= 0, EINVAL, "Could not parse relconf parameters");
		free(rcfg);
	}

	if (ncfg != NULL) {
		ret = sscanf(ncfg, "%i,%i,%i,%i,%i", &nign, &nrng, &nrst, &nspk, &nspkmin);
		RETERR(ret < 1, ret >= 0, EINVAL, "Could not parse normconf parameters");
		free(ncfg);

		NONEG(nign); NONEG(nrng); NONEG(nrst); NONEG(nspk); NONEG(nspkmin);
	}


	/* Setup ABS auto-calibration code */
	enum { IGN, RDY, RMIN, RMAX, ACNT, AMIN, AMAX, LAST };
	int ac[ABS_MAX + 1][8];
	memset(ac, 0, sizeof(ac));
	for (i = 0; i <= ABS_MAX; ++i) {
		ac[i][IGN] = nign;
	}


	/* Open the syslog facility */
	if (log == 1) {
		openlog("evmapd", LOG_PID, LOG_DAEMON);
		log = 2;
	}
	if (quiet && !detach) {
		fclose(stdin);
		fclose(stdout);
		fclose(stderr);
	}

	/* Open the input device */
	ifp = open(idev, O_RDONLY);
	RETERN(ifp < 0, "Unable to open input device %s", idev);

	/* Open the output device */
	ofp = open(odev, O_WRONLY);
	RETERN(ofp < 0, "Unable to open output device %s", odev);
	
	/* Grab the input device */
	if (grab) {
		ret = ioctl(ifp, EVIOCGRAB, (void *)1);
		RETERN(ret < 0, "Unable to grab input device %s", idev);
	}


	/* Get the input device information */
	memset(ibits, 0, sizeof(ibits));
	memset(rbits, 0, sizeof(rbits));
	memset(obits, 0, sizeof(obits));

	INQ(GVERSION, &iver);
	INQ(GID, &(uidev.id));
	INQ(GNAME(sizeof(uidev.name)), uidev.name);
	INQ(GPHYS(sizeof(iphys)), iphys);
	INQ(GBIT(0, EV_MAX), ibits[0]);

	for (i = 1; i < EV_MAX; ++i) {
		if GET(ibits[0], i) {
			ioctl(ifp, EVIOCGBIT(i, KEY_MAX), ibits[i]);

			if (i == EV_ABS) {
				for (j = 0; j < ABS_MAX; ++j) {
					if GET(ibits[i], j) {
						struct input_absinfo abs;

						INQ(GABS(j), &abs);
						uidev.absmax[j] = abs.maximum;
						uidev.absmin[j] = abs.minimum;
						uidev.absfuzz[j] = abs.fuzz;
						uidev.absflat[j] = abs.flat;
					}
				}
			}
		}
	}

	/* Print input device information */
	if (verbose) {
		info("Input device: %s\n"
		    "\tName: %s\n"
		    "\tPhys: %s\n"
		    "\tBus: %u / Vendor: %u / Product: %u / Version: %u.%u.%u / Driver: %d.%d.%d\n"
		    "\n",
		    idev,
		    uidev.name,
		    iphys,
		    uidev.id.bustype, uidev.id.vendor, uidev.id.product, VERTRIPLET(uidev.id.version), VERTRIPLET(iver)
		);

		info("\tEvent types:");
		for (i = 1; i < EV_MAX; ++i)
			if GET(ibits[0], i)
				info(" %i", i);
		info("\n\n");

		listbits(ibits, EV_KEY, KEY_MAX, "KEY");
		listbits(ibits, EV_REL, REL_MAX, "REL");

		if GET(ibits[0], EV_ABS) {
			info("\tABS:\n");
			for (i = 0; i < ABS_MAX; ++i)
				if GET(ibits[EV_ABS], i)
					info("\t\t%2d)  Min:%6d   Max:%6d   Fuzz:%6d   Flat:%6d\n", i,
						uidev.absmin[i], uidev.absmax[i], uidev.absfuzz[i], uidev.absflat[i]);
			info("\n");
		}

		listbits(ibits, EV_MSC, MSC_MAX, "MSC");
		listbits(ibits, EV_SW, SW_MAX, "SW");
		listbits(ibits, EV_LED, LED_MAX, "LED");
		listbits(ibits, EV_SND, SND_MAX, "SND");

		info("\n");
	}


	/* The output device information */
	memset(&(uodev.id), 0, sizeof(uodev.id));
	snprintf(ophys, sizeof(ophys), "evmapd/%i", getpid());
	
	uodev = uidev;

	if (kkm != NULL) {
		SET(obits[EV_EV], EV_KEY, 1);
		for (i = 0; KKM(i, 0) != -1; ++i)
			if GET(ibits[EV_KEY], KKM(i, 0)) {
				SET(rbits[EV_KEY], KKM(i, 0), 1);
				SET(obits[EV_KEY], KKM(i, 1), 1);
			}
	}
	if (krm != NULL) {
		SET(obits[EV_EV], EV_REL, 1);
		for (i = 0; KRM(i, 0) != -1; ++i)
			if (GET(ibits[EV_KEY], KRM(i, 0)) &&
					GET(ibits[EV_KEY], KRM(i, 1))) {
				SET(rbits[EV_KEY], KRM(i, 0), 1);
				SET(rbits[EV_KEY], KRM(i, 1), 1);
				SET(obits[EV_REL], KRM(i, 2), 1);
			}
	}
	if (kam != NULL) {
		SET(obits[EV_EV], EV_ABS, 1);
		for (i = 0; KAM(i, 0) != -1; ++i)
			if (GET(ibits[EV_KEY], KAM(i, 0)) &&
					GET(ibits[EV_KEY], KAM(i, 1))) {
				SET(rbits[EV_KEY], KAM(i, 0), 1);
				SET(rbits[EV_KEY], KAM(i, 1), 1);
				SET(obits[EV_ABS], KAM(i, 2), 1);
				if ((uodev.absmin[KAM(i, 2)] == 0) &&
						(uodev.absmax[KAM(i, 2)] == 0)) {
					uodev.absmin[KAM(i, 2)] = amin;
					uodev.absmax[KAM(i, 2)] = amax;
				}
			}
	}

	if (rkm != NULL) {
		SET(obits[EV_EV], EV_KEY, 1);
		for (i = 0; RKM(i, 0) != -1; ++i)
			if GET(ibits[EV_REL], RKM(i, 0)) {
				SET(rbits[EV_REL], RKM(i, 0), 1);
				SET(obits[EV_KEY], RKM(i, 1), 1);
				SET(obits[EV_KEY], RKM(i, 2), 1);
			}
	}
	if (rrm != NULL) {
		SET(obits[EV_EV], EV_REL, 1);
		for (i = 0; RRM(i, 0) != -1; ++i)
			if GET(ibits[EV_REL], RRM(i, 0)) {
				SET(rbits[EV_REL], RRM(i, 0), 1);
				SET(obits[EV_REL], RRM(i, 1), 1);
			}
	}
	if (ram != NULL) {
		SET(obits[EV_EV], EV_ABS, 1);
		for (i = 0; RAM(i, 0) != -1; ++i)
			if GET(ibits[EV_REL], RAM(i, 0)) {
				SET(rbits[EV_REL], RAM(i, 0), 1);
				SET(obits[EV_ABS], RAM(i, 1), 1);
				if ((uodev.absmin[RAM(i, 1)] == 0) &&
						(uodev.absmax[RAM(i, 1)] == 0)) {
					uodev.absmin[RAM(i, 1)] = amin;
					uodev.absmax[RAM(i, 1)] = amax;
				}
			}
	}

	if (akm != NULL) {
		SET(obits[EV_EV], EV_KEY, 1);
		for (i = 0; AKM(i, 0) != -1; ++i)
			if GET(ibits[EV_ABS], AKM(i, 0)) {
				SET(rbits[EV_ABS], AKM(i, 0), 1);
				SET(obits[EV_KEY], AKM(i, 1), 1);
				SET(obits[EV_KEY], AKM(i, 2), 1);
			}
	}
	if (arm != NULL) {
		SET(obits[EV_EV], EV_REL, 1);
		for (i = 0; ARM(i, 0) != -1; ++i)
			if GET(ibits[EV_ABS], ARM(i, 0)) {
				SET(rbits[EV_ABS], AKM(i, 0), 1);
				SET(obits[EV_REL], ARM(i, 1), 1);
			}
	}
	if (aam != NULL) {
		SET(obits[EV_EV], EV_ABS, 1);
		for (i = 0; AAM(i, 0) != -1; ++i)
			if GET(ibits[EV_ABS], AAM(i, 0)) {
				SET(rbits[EV_ABS], AAM(i, 0), 1);
				SET(obits[EV_ABS], AAM(i, 1), 1);
				if ((uodev.absmin[AAM(i, 1)] == 0) &&
						(uodev.absmax[AAM(i, 1)] == 0)) {
					uodev.absmin[AAM(i, 1)] = uodev.absmin[AAM(i, 0)];
					uodev.absmax[AAM(i, 1)] = uodev.absmax[AAM(i, 0)];
					uodev.absfuzz[AAM(i, 1)] = uodev.absfuzz[AAM(i, 0)];
					uodev.absflat[AAM(i, 1)] = uodev.absflat[AAM(i, 0)];
				}
			}
	}

	/* Do not let through the remapped event bits */	
	for (i = 0; i < EV_MAX; ++i)
		for (j = 0; j < LEN(long, KEY_MAX); ++j)
			obits[i][j] |= (ibits[i][j] & ~rbits[i][j]);

	/* Print output device information */
	if (verbose) {
		info("Output device: %s\n"
		    "\tName: %s\n"
		    "\tPhys: %s\n"
		    "\tBus: %u / Vendor: %u / Product: %u / Version: %u.%u.%u\n"
		    "\n",
		    odev,
		    uodev.name,
		    ophys,
		    uodev.id.bustype, uodev.id.vendor, uodev.id.product, VERTRIPLET(uodev.id.version)
		);

		info("\tEvent types:");
		for (i = 1; i < EV_MAX; ++i)
			if GET(obits[0], i)
				info(" %i", i);
		info("\n\n");

		listbits(obits, EV_KEY, KEY_MAX, "KEY");
		listbits(obits, EV_REL, REL_MAX, "REL");

		if GET(obits[0], EV_ABS) {
			info("\tABS:\n");
			for (i = 0; i < ABS_MAX; ++i)
				if GET(obits[EV_ABS], i)
					info("\t\t%2d)  Min:%6d   Max:%6d   Fuzz:%6d   Flat:%6d\n", i,
						uodev.absmin[i], uodev.absmax[i], uodev.absfuzz[i], uodev.absflat[i]);
			info("\n");
		}

		listbits(obits, EV_MSC, MSC_MAX, "MSC");
		listbits(obits, EV_SW, SW_MAX, "SW");
		listbits(obits, EV_LED, LED_MAX, "LED");
		listbits(obits, EV_SND, SND_MAX, "SND");

		info("\n");
	}


	/* Prepare the output device */
	OSET(SET_PHYS, ophys);
	OSETBIT(EV);
	OSETBIT(KEY);
	OSETBIT(REL);
	OSETBIT(ABS);
	OSETBIT(MSC);
	OSETBIT(LED);
	OSETBIT(SND);
	OSETBIT(FF);
	OSETBIT(SW);

	ret = write(ofp, &uodev, sizeof(uodev));
	RETERR(ret < sizeof(uodev), ret >= 0, EIO, "Unable to configure output device %s", odev);
	OSET(DEV_CREATE, NULL);


	/* Daemon mode */
	if (detach) {
		ret = daemon(0, 0);
		RETERN(ret < 0, "Could not run in the background");
		info("evmapd %s launched for %s using %s for output (PID: %i)\n",
				VERSION, idev, odev, getpid());
	}

	/* PID file support */
	if (pidfile != NULL) {
		ret = write_pid();
		RETERN(ret < 0, "Could not write PID file %s", pidfile);
	}

	/* Setup the signal handlers */
	signal(SIGTERM, on_term);


#define _RCV			ret = read(ifp, &ev, sizeof(ev)); \
				RETERR(ret < sizeof(ev), ret >= 0, EIO, "Unable to receive event from %s", idev);

#define _SND			ret = write(ofp, &ev, sizeof(ev)); \
				RETERR(ret < sizeof(ev), ret >= 0, EIO, "Unable to send event to %s", odev); \
				if (ev.type == EV_KEY) SET(rbits[EV_KEY], ev.code, ev.value);

#if DEBUG
#define RCV			_RCV \
				if (verbose) \
					info("IN: %6i %6i %6i\n", ev.type, ev.code, ev.value);
#define SND			if (verbose) \
					info("OUT: %6i %6i %6i\n", ev.type, ev.code, ev.value); \
				_SND
#else
#define RCV			_RCV
#define SND			_SND
#endif


	/* The event loop */
	memset(rbits, 0, sizeof(rbits));
	while (1) {
		int irng;

		RCV;

		/* Event processing */
		j = 1;
		ret = 1;
		switch (ev.type) {
			case EV_KEY:
				if (ret && (kkm != NULL))
					for (i = 0; KKM(i, 0) != -1; ++i)
						if (KKM(i, 0) == ev.code) {
							ev.code = KKM(i, 1);
							ret = 0;
							break;
						}
				if (ret && (krm != NULL))
					for (i = 0; KRM(i, 0) != -1; ++i)
						if ((KRM(i, 0) == ev.code) || (KRM(i, 1) == ev.code)) {
							ev.type = EV_REL;
							if (ev.value > 0)
								ev.value = (ev.code == KRM(i, 0))?rmin:rmax;
							else
								ev.value = rmin + (rmax - rmin) / 2;
							ev.code = KRM(i, 2);
							ret = 0;
							break;
						}
				if (ret && (kam != NULL))
					for (i = 0; KAM(i, 0) != -1; ++i)
						if ((KAM(i, 0) == ev.code) || (KAM(i, 1) == ev.code)) {
							ev.type = EV_ABS;
							if (ev.value > 0)
								ev.value = (ev.code == KAM(i, 0))?
									uodev.absmin[KAM(i, 2)]:uodev.absmax[KAM(i, 2)];
							else
								ev.value = uodev.absmin[KAM(i, 2)] +
									(uodev.absmax[KAM(i, 2)] - uodev.absmin[KAM(i, 2)]) / 2;
							ev.code = KAM(i, 2);
							ret = 0;
							break;
						}
				break;
			case EV_REL:
				if (ret && (rkm != NULL))
					for (i = 0; RKM(i, 0) != -1; ++i)
						if (RKM(i, 0) == ev.code) {
							ev.type = EV_KEY;
							if (ev.value < 0) {
								if GET(rbits[EV_KEY], RKM(i, 2)) {
									ev.code = RKM(i, 2);
									ev.value = 0;
									SND;
								}
								ev.code = RKM(i, 1);
								ev.value = 1;
							} else if (ev.value > 0) {
								if GET(rbits[EV_KEY], RKM(i, 1)) {
									ev.code = RKM(i, 1);
									ev.value = 0;
									SND;
								}
								ev.code = RKM(i, 2);
								ev.value = 1;
							} else {
								j = 0;
								ev.value = 0;
								if GET(rbits[EV_KEY], RKM(i, 1)) {
									ev.code = RKM(i, 1);
									SND;
								}
								if GET(rbits[EV_KEY], RKM(i, 2)) {
									ev.code = RKM(i, 2);
									SND;
								}
							}
							ret = 0;
							break;
						}
				if (ret && (rrm != NULL))
					for (i = 0; RRM(i, 0) != -1; ++i)
						if (RRM(i, 0) == ev.code) {
							ev.code = RRM(i, 1);
							ret = 0;
							break;
						}
				if (ret && (ram != NULL))
					for (i = 0; RAM(i, 0) != -1; ++i)
						if (RAM(i, 0) == ev.code) {
							ev.type = EV_ABS;
							ev.code = RAM(i, 1);
							if (ev.value < rmin)
								ev.value = rmin;
							if (ev.value > rmax)
								ev.value = rmax;
							ev.value = ((ev.value - rmin) * (uodev.absmax[RAM(i, 1)] - uodev.absmin[RAM(i, 1)])) /
									(rmax - rmin) + uodev.absmin[RAM(i, 1)];
							ret = 0;
							break;
						}
				break;
			case EV_ABS:
				irng = uidev.absmax[ev.code] - uidev.absmin[ev.code];

				if (nm != NULL) {
					/* Auto-calibration */
					for (i = 0; nm[i] != -1; ++i)
						if (nm[i] == ev.code) {
							if (AC[RDY]) {
								/* Spike protection */
								if ((nspk > 0) && (nspkmin < irng)) {
									if (labs((long)ev.value - (long)AC[LAST]) * (long)(nspk) > (long)irng)
										break;
									AC[LAST] = ev.value;
								}
								
								/* Auto-calibration reset code */
								if (nrst > 0) {
									if (AC[ACNT] > 0) {
										++AC[ACNT];

										if (ev.value < AC[AMIN])
											AC[AMIN] = ev.value;	
										if (ev.value > AC[AMAX])
											AC[AMAX] = ev.value;
								
										if (AC[ACNT] >= nrst) {
											if ((nrng == 0) || ((long)(AC[AMAX] - AC[AMIN]) * (long)nrng >= irng)) {
												AC[RMIN] = AC[AMIN];
												AC[RMAX] = AC[AMAX];
												AC[AMIN] = 0;
												AC[AMAX] = 0;
												AC[ACNT] = 0;
											} else {
												AC[ACNT] = nrst - 1;
											}
										}
									} else {
										if (AC[AMIN] == 0) {
											AC[AMIN] = ev.value;
										} else {
											if (AC[AMIN] < ev.value) {
												AC[AMAX] = ev.value;
												++AC[ACNT];
											} else if (AC[AMIN] > ev.value) {
												AC[AMAX] = AC[AMIN];
												AC[AMIN] = ev.value;
												++AC[ACNT];
											}
										}
									}
								}
								
								if (ev.value < AC[RMIN])
									AC[RMIN] = ev.value;	
								if (ev.value > AC[RMAX])
									AC[RMAX] = ev.value;
									
								/* The actual auto-calibration formula */
								if ((nrng == 0) || ((long)(AC[RMAX] - AC[RMIN]) * (long)nrng >= irng))
									ev.value = (irng * (ev.value - AC[RMIN])) / (AC[RMAX] - AC[RMIN]) +
											uidev.absmin[ev.code];
							} else {
								/* Ignore initial events */
								if (AC[IGN] > 0) {
									--AC[IGN];
									break;
								}
								
								if (AC[RMIN] == 0) {
									AC[RMIN] = ev.value;
								} else {
									/* Spike protection */
									if ((nspk > 0) && (nspkmin < irng)) {
										if (labs((long)ev.value - (long)AC[RMIN]) * (long)(nspk) > (long)irng)
											break;
										AC[LAST] = ev.value;
									}
									
									if (AC[RMIN] < ev.value) {
										AC[RMAX] = ev.value;
										AC[RDY] = 1;
									} else if (AC[RMIN] > ev.value) {
										AC[RMAX] = AC[RMIN];
										AC[RMIN] = ev.value;
										AC[RDY] = 1;
									}
								}
							}
						}
				}

				if (ret && (akm != NULL))
					for (i = 0; AKM(i, 0) != -1; ++i)
						if (AKM(i, 0) == ev.code) {
							ev.type = EV_KEY;
							if (ev.value <= (uidev.absmin[ev.code] + (irng / 4))) {
								if GET(rbits[EV_KEY], AKM(i, 2)) {
									ev.code = AKM(i, 2);
									ev.value = 0;
									SND;
								}
								ev.code = AKM(i, 1);
								ev.value = 1;
							} else if (ev.value >= (uidev.absmax[ev.code] - (irng / 4))) {
								if GET(rbits[EV_KEY], AKM(i, 1)) {
									ev.code = AKM(i, 1);
									ev.value = 0;
									SND;
								}
								ev.code = AKM(i, 2);
								ev.value = 1;
							} else {
								ev.value = 0;
								if GET(rbits[EV_KEY], AKM(i, 1)) {
									ev.code = AKM(i, 1);
									SND;
								}
								if GET(rbits[EV_KEY], AKM(i, 2)) {
									ev.code = AKM(i, 2);
									SND;
								}
								j = 0;
							}
							ret = 0;
							break;
						}
				if (ret && (arm != NULL))
					for (i = 0; ARM(i, 0) != -1; ++i)
						if (ARM(i, 0) == ev.code) {
							ev.type = EV_REL;
							ev.value = rmin + ((ev.value - uidev.absmin[ev.code]) * (rmax - rmin)) / irng;
							ev.code = ARM(i, 1);
							ret = 0;
							break;
						}
				if (ret && (aam != NULL))
					for (i = 0; AAM(i, 0) != -1; ++i)
						if (AAM(i, 0) == ev.code) {
							ev.value = uodev.absmin[AAM(i, 1)] + ((ev.value - uidev.absmin[ev.code]) *
									(uodev.absmax[AAM(i, 1)] - uodev.absmin[AAM(i, 1)])) / irng;
							ev.code = AAM(i, 1);
							ret = 0;
							break;
						}
				break;
		}

		if (j)
			SND;
	}


	return 0;
}
