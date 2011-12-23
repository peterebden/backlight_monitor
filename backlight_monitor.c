#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

const int TIME_BEFORE_DIM = 3;
static const char* screen_backlight_path = "/sys/devices/virtual/backlight/nvidia_backlight/brightness";
static const char* kbd_backlight_path = "/sys/class/leds/smc::kbd_backlight/brightness";

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

void adjust_single_brightness(double new_proportion, const char* path, double* offset, int* last_brightness, int min_brightness, int max_brightness) {
    // open up the device and write the new value in
	int current_brightness = *last_brightness;
    FILE* f = fopen(path, "r+");
    if(f) {
		fscanf(f, "%d", &current_brightness);
		if(current_brightness != *last_brightness) {
			// something's altered the value since we last wrote it. calculate and apply an offset.
		    *offset += (double)(current_brightness - *last_brightness) / (max_brightness - min_brightness);
		}
		int new_brightness = (int)((new_proportion + *offset)*(max_brightness - min_brightness)) + min_brightness;
		new_brightness = min(max(new_brightness, min_brightness), max_brightness);
		fseek(f, 0, SEEK_SET);
		printf("Offset %lf\n", *offset);
        fprintf(f, "%d", new_brightness);
		fclose(f);
		*last_brightness = new_brightness;
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
 
int main(int argc, char* argv[]) {
    XScreenSaverInfo* info = XScreenSaverAllocInfo();
    XEvent ev;
    info->idle = 0; // ensure this is initialised since we'll calculate on it shortly
    Display* display = XOpenDisplay(0);
    if(display == NULL) {
        printf("Couldn't connect to X display\n");
        return 1;
    }

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
