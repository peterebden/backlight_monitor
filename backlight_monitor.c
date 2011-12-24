#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <signal.h>

const int TIME_BEFORE_DIM = 30;
static const char* screen_backlight_path = "/sys/devices/virtual/backlight/nvidia_backlight/brightness";
static const char* kbd_backlight_path = "/sys/class/leds/smc::kbd_backlight/brightness";
static const char* ac_adapter_path = "/proc/acpi/ac_adapter/ADP1/state";

const int SCREEN_DIM = 1000;
const int SCREEN_BRIGHT = 20000;
const int KBD_DIM = 0;
const int KBD_BRIGHT = 255;

int last_screen_brightness = 20000;
int last_kbd_brightness = 255;
double screen_offset = 0.0;
double kbd_offset = 0.0;

int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return b < a ? a : b; }

double power_adapter_offset() {
    char buf[255];
    FILE* f = fopen(ac_adapter_path, "r");
    if(f) {
	if(fscanf(f, "state:  %s", buf) != 1) {
	    printf("Failed to read power adapter state from %s\n", ac_adapter_path);
	}
	fclose(f);
	if(strstr(buf, "off")) {
	    return -0.5; // half brightness when power adapter offline
	}
    }
    return 0.0;
}

int read_initial_value(const char* path) {
    FILE* f = fopen(path, "r");
    if(!f) {
	printf("Failed to open device %s\n", path);
	exit(2);
    }
    int x = 0;
    if(fscanf(f, "%d", &x) != 1) {
	printf("Failed to read the value from device %s\n", path);
    }
    fclose(f);
    return x;
}

void read_initial_values() {
    // first read to sync up all the values etc
    last_screen_brightness = read_initial_value(screen_backlight_path);
    last_kbd_brightness = read_initial_value(kbd_backlight_path);
}

void adjust_single_brightness(double new_proportion, const char* path, double* offset, int* last_brightness, int min_brightness, int max_brightness) {
    // open up the device and write the new value in
    int current_brightness = *last_brightness;
    FILE* f = fopen(path, "r+");
    if(f) {
	if(fscanf(f, "%d", &current_brightness) == 1) {
	    if(current_brightness != *last_brightness) {
		// something's altered the value since we last wrote it. calculate and apply an offset.
		*offset += (double)(current_brightness - *last_brightness) / (max_brightness - min_brightness);
	    }
	    int new_brightness = (int)((new_proportion + *offset)*(max_brightness - min_brightness)) + min_brightness;
	    new_brightness = min(max(new_brightness, min_brightness), max_brightness);
	    fseek(f, 0, SEEK_SET);
	    printf("Offset %lf\n", *offset);
	    fprintf(f, "%d", new_brightness);
	    *last_brightness = new_brightness;
	}
	fclose(f);
    } else {
        printf("Could not open device file %s\n", path);
    }
}

void adjust_brightness(double proportion) {
    adjust_single_brightness(proportion, screen_backlight_path, &screen_offset, &last_screen_brightness, SCREEN_DIM, SCREEN_BRIGHT);
    adjust_single_brightness(proportion, kbd_backlight_path, &kbd_offset, &last_kbd_brightness, KBD_DIM, KBD_BRIGHT);
}

int interpolate(int a, int b, double c) {
    return (int)(c*(b-a))+a;
}

int continuous_dim_backlight(Display* display, XScreenSaverInfo* info) {
    unsigned long initial_idle = info->idle;
    struct timespec tm_remaining = { 0, 0 };
    struct timespec ten_milliseconds = { 0, 10000000 };
    for(double proportion = 1.0; proportion >= 0.0; proportion -= 0.001) {
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
	if(info->idle < initial_idle) {
	    // obviously we've come out of idle in the last sleep. bail here.
	    return 1;
	}
	adjust_brightness(proportion);
	nanosleep(&ten_milliseconds, &tm_remaining);
    }
    adjust_brightness(0.0);
    return 0;
}

void wait_for_event(Display* display, XScreenSaverInfo* info) {
    // waiting until something happens
    // currently just doing polling, not sure how possible it is to get notified of events from X
    struct timespec tm_remaining = { 0, 0 };
    struct timespec half_second = { 0, 500000000 };
    unsigned long last_idle;
    do {
        last_idle = info->idle;
        nanosleep(&half_second, &tm_remaining);
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
    } while(info->idle >= last_idle);
}

void refresh_power_state() {
    double offset = power_adapter_offset();
    screen_offset += offset;
    kbd_offset += offset;
}
 
int main(int argc, char* argv[]) {
    // Daemonize
    pid_t pid = fork();
    if(pid < 0) {
	printf("Fork failed with %d\n", pid);
    } else if(pid > 0) {
	return 0; // child has forked off correctly, we terminate immediately.
    }

    signal(SIGUSR1, refresh_power_state);

    XScreenSaverInfo* info = XScreenSaverAllocInfo();
    info->idle = 0; // ensure this is initialised since we'll calculate on it shortly
    Display* display = XOpenDisplay(0);
    if(display == NULL) {
        printf("Couldn't connect to X display\n");
        return 1;
    }

    read_initial_values();

    while(1) {
        // we've just gone idle. wait for 30 seconds
        sleep(TIME_BEFORE_DIM - info->idle/1000);
	// now count the idle time
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
	if(info->idle < TIME_BEFORE_DIM) {
	    // we must have been woken in between. go back to waiting.
	    continue;
	}

	// here we have waited the requisite amount of time. dim the display.
	if(!continuous_dim_backlight(display, info)) {
	    wait_for_event(display, info);
	}
	adjust_brightness(1.0);
    }
}
