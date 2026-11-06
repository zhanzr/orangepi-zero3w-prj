/*
 * blink_hello - Orange Pi Zero 3W (Allwinner A733, Armbian vendor
 * kernel 6.6 sun60iw2)
 *
 * Blinks the single on-board LED (kernel LED class API):
 *   /sys/class/leds/status_led
 * and samples both SoC die temperatures (Allwinner THS sensors) on
 * every toggle:
 *   thermal_zone0 = cpub  (2x Cortex-A76, big cluster)
 *   thermal_zone3 = cpul  (6x Cortex-A55, little cluster)
 *
 * The current LED trigger is saved on start and restored on exit
 * (including on SIGINT/SIGTERM). The LED sysfs nodes are root-owned
 * (0644): run via "make run" (ssh as root) or with sudo.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BLINK_MS 500

#define LED_PATH  "/sys/class/leds/status_led"
#define TEMP_CPUB "/sys/class/thermal/thermal_zone0/temp" /* A76 pair */
#define TEMP_CPUL "/sys/class/thermal/thermal_zone3/temp" /* A55 six  */

static char saved_trigger[160];
static volatile sig_atomic_t quit;

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static int write_sysfs(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");

	if (!f) {
		fprintf(stderr, "ERR: cannot write %s (root?)\n", path);
		return -1;
	}
	fputs(val, f);
	fclose(f);
	return 0;
}

/* trigger file content: "list of triggers [current] ..." */
static void read_trigger(const char *led, char *buf, size_t len)
{
	char path[256];
	char line[512];
	char *open, *close;

	buf[0] = '\0';
	snprintf(path, sizeof(path), "%s/trigger", led);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(line, sizeof(line), f)) {
		open = strchr(line, '[');
		close = open ? strchr(open, ']') : NULL;
		if (open && close && close > open) {
			size_t n = (size_t)(close - open - 1);
			if (n >= len)
				n = len - 1;
			memcpy(buf, open + 1, n);
			buf[n] = '\0';
		}
	}
	fclose(f);
}

static int set_brightness(int on)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/brightness", LED_PATH);
	return write_sysfs(path, on ? "1" : "0");
}

static int set_trigger_none(void)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/trigger", LED_PATH);
	return write_sysfs(path, "none");
}

static int read_temp_mdeg(const char *path, long *mdeg)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (fscanf(f, "%ld", mdeg) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

int main(void)
{
	read_trigger(LED_PATH, saved_trigger, sizeof(saved_trigger));
	if (set_trigger_none() < 0)
		return 1;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);   /* ssh drops mid-run - still restore */

	printf("blinking %s, %d ms per phase, Ctrl-C to stop\n",
	       LED_PATH, BLINK_MS);
	printf("saved trigger: %s\n",
	       saved_trigger[0] ? saved_trigger : "(none)");
	fflush(stdout);

	int on = 0;
	long bdeg = 0, ldeg = 0;

	while (!quit) {
		if (set_brightness(on) < 0)
			break;

		int b_ok = read_temp_mdeg(TEMP_CPUB, &bdeg) == 0;
		int l_ok = read_temp_mdeg(TEMP_CPUL, &ldeg) == 0;

		if (b_ok && l_ok)
			printf("led %s  cpub (A76x2): %ld.%ld C   cpul (A55x6): %ld.%ld C\n",
			       on ? "ON " : "OFF",
			       bdeg / 1000, (bdeg % 1000) / 100,
			       ldeg / 1000, (ldeg % 1000) / 100);
		else
			printf("led %s  die temp: read error\n",
			       on ? "ON " : "OFF");
		fflush(stdout);

		for (int i = 0; i < BLINK_MS / 50 && !quit; i++)
			msleep(50);
		on = !on;
	}

	set_brightness(0);
	if (saved_trigger[0] && strcmp(saved_trigger, "none") != 0)
		write_sysfs(LED_PATH "/trigger", saved_trigger);
	printf("\nstopped, LED trigger restored\n");
	return 0;
}
