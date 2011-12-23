#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <stdio.h>
#include <unistd.h>

const int TIME_BEFORE_DIM = 3;
static const char* device_path = "/sys/devices/virtual/backlight/nvidia_backlight/brightness";
const int DIM = 8000;
const int BRIGHT = 20000;

int read_current_brightness() {
    int brightness = DIM;
    FILE* f = fopen(device_path, "r");
	if(f) {
		fscanf(f, "%d", &brightness);
		fclose(f);
	}
	return brightness;
}

void adjust_brightness(int new_brightness) {
  printf("Adjusting brightness to %d\n", new_brightness);
    // open up the device and write the new value in
    FILE* f = fopen(device_path, "w");
    if(f) {
        fprintf(f, "%d", new_brightness);
		fclose(f);
    } else {
        printf("Could not open device file\n");
    }
}

void continuous_dim_backlight(Display* display, XScreenSaverInfo* info, int new_brightness) {
	unsigned long initial_idle = info->idle;
	for(int b = read_current_brightness(); b >= DIM; b -= 300) {
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
		if(info->idle < initial_idle) {
			// obviously we've come out of idle in the last sleep. straight back to full brightness.
			adjust_brightness(BRIGHT);
			return;
		}
		adjust_brightness(b);
		sleep(1);
	}
	adjust_brightness(DIM);
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
		continuous_dim_backlight(display, info, DIM);
		adjust_brightness(DIM);
		wait_for_event(display, info);
		adjust_brightness(BRIGHT);
    }
}
