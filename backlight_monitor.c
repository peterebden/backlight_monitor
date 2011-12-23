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

void adjust_single_brightness(int new_brightness, const char* path) {
  printf("Adjusting brightness to %d\n", new_brightness);
    // open up the device and write the new value in
    FILE* f = fopen(path, "w");
    if(f) {
        fprintf(f, "%d", new_brightness);
		fclose(f);
    } else {
        printf("Could not open device file\n");
    }
}

void adjust_brightness(int screen, int kbd) {
	adjust_single_brightness(screen, screen_backlight_path);
	adjust_single_brightness(kbd, kbd_backlight_path);
}

int interpolate(int a, int b, double c) {
	return (int)(c*(b-a))+a;
}

void continuous_dim_backlight(Display* display, XScreenSaverInfo* info) {
	unsigned long initial_idle = info->idle;
	struct timespec tm_remaining = { 0, 0 };
	struct timespec tm_requested = { 0, 10000000 }; // 10ms
	for(double proportion = 1.0; proportion >= 0.0; proportion -= 0.001) {
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
		if(info->idle < initial_idle) {
			// obviously we've come out of idle in the last sleep. straight back to full brightness.
			adjust_brightness(SCREEN_BRIGHT, KBD_BRIGHT);
			return;
		}
		adjust_brightness(interpolate(SCREEN_DIM, SCREEN_BRIGHT, proportion),
                          interpolate(KBD_DIM, KBD_BRIGHT, proportion));
		nanosleep(&tm_requested, &tm_remaining);
	}
	adjust_brightness(SCREEN_DIM, KBD_DIM);
}

void wait_for_event(Display* display, XScreenSaverInfo* info) {
    // waiting until something happens
    // currently just doing polling, not sure how possible it is to get notified of events from X
    unsigned long last_idle;
    do {
        last_idle = info->idle;
        sleep(1);
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
	adjust_brightness(SCREEN_BRIGHT, KBD_BRIGHT);

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
		printf("wibble\n");
		continuous_dim_backlight(display, info);
		wait_for_event(display, info);
		adjust_brightness(SCREEN_BRIGHT, KBD_BRIGHT);
    }
}
