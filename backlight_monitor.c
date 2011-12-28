#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <signal.h>

int time_before_dim = 90;
static const char* screen_backlight_path = "/sys/devices/virtual/backlight/nvidia_backlight/brightness";
static const char* kbd_backlight_path = "/sys/class/leds/smc::kbd_backlight/brightness";
static const char* ac_adapter_path = "/proc/acpi/ac_adapter/ADP1/state";
static const char* sensor_path = "/sys/devices/platform/applesmc.768/light";

const int SCREEN_DIM = 1000;
int screen_bright = 20000;
const int KBD_DIM = 0;
int kbd_bright = 255;

int last_screen_brightness = 20000;
int last_kbd_brightness = 255;
double screen_offset = 0.0;
double kbd_offset = 0.0;
double power_multiplier = 1.0;
double last_proportion = 1.0;
double screen_multiplier = 1.0;
double kbd_multiplier = 1.0;
int daemonize = 1;

const double SCREEN_SENSOR_LOOKUP[] = { 0.5, 0.6, 0.68, 0.76, 0.80, 0.84, 0.87, 0.90, 0.92, 0.93, 0.94, 0.95, 0.96, 0.97, 0.98, 0.99 };
const double KBD_SENSOR_LOOKUP[] = { 1.0, 0.9, 0.82, 0.76, 0.71, 0.67, 0.64, 0.61, 0.59, 0.57, 0.56, 0.55, 0.54, 0.53, 0.52, 0.51 };

int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return b < a ? a : b; }
#define countof(array) (sizeof(array)/sizeof(array[0]))

double power_adapter_multiplier() {
    char buf[255];
    FILE* f = fopen(ac_adapter_path, "r");
    if(f) {
	if(fscanf(f, "state:  %s", buf) != 1) {
	    fprintf(stderr, "Failed to read power adapter state from %s\n", ac_adapter_path);
	}
	fclose(f);
	if(strstr(buf, "off")) {
	    return 0.5; // half brightness when power adapter offline
	}
    }
    return 1.0; // full brightness otherwise
}

void adjust_single_brightness(double new_proportion, const char* path, double* offset, int* last_brightness, int min_brightness, int max_brightness, double sensor_multiplier) {
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
	    new_brightness = min(max(new_brightness * power_multiplier * sensor_multiplier, min_brightness), max_brightness);
	    fseek(f, 0, SEEK_SET);
	    fprintf(f, "%d", new_brightness);
	    *last_brightness = new_brightness;
	}
	fclose(f);
    } else {
        printf("Could not open device file %s\n", path);
    }
}

void adjust_brightness(double proportion) {
    adjust_single_brightness(proportion, screen_backlight_path, &screen_offset, &last_screen_brightness, SCREEN_DIM, screen_bright, screen_multiplier);
    adjust_single_brightness(proportion, kbd_backlight_path, &kbd_offset, &last_kbd_brightness, KBD_DIM, kbd_bright, kbd_multiplier);
    last_proportion = proportion;
}

int interpolate(int a, int b, double c) {
    return (int)(c*(b-a))+a;
}

void update_light_sensor() {
    int x = 255, y = 0;
    FILE* f = fopen(sensor_path, "r");
    if(f) {
	// the second number always appears to be 0, but am reading it anyway just in case
	if(fscanf(f, "(%d,%d)", &x, &y) != 2) {
	    fprintf(stderr, "Didn't read exactly two entries from light sensor\n");
	}
	fclose(f);
    } else {
	fprintf(stderr, "Can't open light sensor for reading\n");
    }
    // now calculate updates to keyboard and screen
    // this is a bit sucky in that the resolution of the sensor seems to be
    // inadequate; there's quite an interesting range underneath the bottom of the scale
    // and in practice values over 20 or so don't make a lot of difference.
    //
    // the screen changes in rough proportion to ambient light
    // and the keyboard in inverse proportion to it
    double screen = (x < 0 || x >= countof(SCREEN_SENSOR_LOOKUP) ? 1.0 : SCREEN_SENSOR_LOOKUP[x]);
    // the keyboard multiplier changes in inverse proportion
    double kbd = (x < 0 || x > countof(KBD_SENSOR_LOOKUP) ? 0.5 : KBD_SENSOR_LOOKUP[x]);

    if(screen != screen_multiplier || kbd != kbd_multiplier) {
	screen_multiplier = screen;
	kbd_multiplier = kbd;
	adjust_brightness(last_proportion);
    }
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
	update_light_sensor();
    } while(info->idle >= last_idle);
}

void refresh_adapter_state() {
    power_multiplier = power_adapter_multiplier();
    // call again with the last adjustment passed in case it's changed.
    adjust_brightness(last_proportion);
}

void set_initial_value(const char* path, int value) {
    FILE* f = fopen(path, "w");
    if(!f) {
	printf("Failed to open device %s\n", path);
	exit(EXIT_FAILURE);
    }
    fprintf(f, "%d", value);
    fclose(f);
}

void set_initial_values() {
    // we might have a multiplier from the ac adapter
    refresh_adapter_state();
    // set the initial values to what we expect and set the 'last values'
    set_initial_value(screen_backlight_path, last_screen_brightness = (int)(power_multiplier * screen_bright));
    set_initial_value(kbd_backlight_path, last_kbd_brightness = (int)(power_multiplier * kbd_bright));
    update_light_sensor();
}

void parse_options(int argc, char* argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "ds:k:t:")) != -1) {
        switch(opt) {
        case 'd':
            daemonize = 0;
            break;
        case 's':
            screen_bright = atoi(optarg);
            break;
	case 'k':
	    kbd_bright = atoi(optarg);
	    break;
	case 't':
	    time_before_dim = atoi(optarg);
	    break;
        default: /* '?' */
            fprintf(stderr, "Usage: %s [-d] [-s max_screen_brightness] [-k max_keyboard_brightness] [-t time_before_dim]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }
}
 
int main(int argc, char* argv[]) {
    parse_options(argc, argv);
    // Daemonize, unless we're passed -d
    if(daemonize) {
	pid_t pid = fork();
	if(pid < 0) {
	    printf("Fork failed with %d\n", pid);
	} else if(pid > 0) {
	    return EXIT_SUCCESS; // child has forked off correctly, we terminate immediately.
	}
    }

    signal(SIGUSR1, refresh_adapter_state);

    XScreenSaverInfo* info = XScreenSaverAllocInfo();
    info->idle = 0; // ensure this is initialised since we'll calculate on it shortly
    Display* display = XOpenDisplay(0);
    if(display == NULL) {
        printf("Couldn't connect to X display\n");
        return EXIT_FAILURE;
    }

    set_initial_values();

    // NB. ideally we would use select() or something to wait for the applesmc sysfs entry
    //     to change, but it doesn't seem to work...

    while(1) {
        // we've just gone idle. wait in 2 second chunks to keep checking the backlight
	for(int i = 0; i < time_before_dim * 1000 - info->idle; i += 2000) {
	    sleep(2);
	    update_light_sensor();
	}
	// now check the idle time again
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
	if(info->idle < time_before_dim*1000) {
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
