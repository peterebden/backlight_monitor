/* 
    backlight_monitor

    This is a (relatively) simple program for monitoring the X idle state
    and light sensor to decide when to dim the screen, and implementing a
    slow smooth fade to dark when the machine goes idle.
    Various other features including screenlocking (via activating a 
    third-party locker binary) have crept in over time as well.
    
    Copyright (c) 2011-2012 Peter Ebden <peter.ebden@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <X11/Xlib.h>
#include <X11/extensions/scrnsaver.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <signal.h>

int time_before_dim = 90; // no of seconds to wait before starting to dim the screen

// paths to various things
static const char* screen_backlight_path = "/sys/devices/virtual/backlight/nvidia_backlight/brightness";
static const char* kbd_backlight_path = "/sys/class/leds/smc::kbd_backlight/brightness";
static const char* ac_adapter_path = "/proc/acpi/ac_adapter/ADP1/state"; // TODO: use /sys entries instead
static const char* sensor_path = "/sys/devices/platform/applesmc.768/light";

// constants for the min/max settings for screen/keyboard
const int SCREEN_DIM = 500;
int screen_bright = 20000;
const int KBD_DIM = 0;
int kbd_bright = 255;

int last_screen_brightness = -1;
int last_kbd_brightness = -1;
double screen_offset = 0.0;
double kbd_offset = 0.0;
double power_multiplier = 1.0;
double last_proportion = 1.0;
double screen_multiplier = 1.0;
double kbd_multiplier = 1.0;
int daemonize = 1;
int is_dimmed = 0;

unsigned long lock_delay_ms = 10 * 60 * 1000; // lock screen after 10 minutes idle
static const char* screen_lock_command = "/usr/bin/slimlock"; // command to run to lock screen
#define dprintf if(!daemonize) printf

// tables that determine the steps to take to dim the screen/keyboard.
// these are necessary because the dimming process is nonlinear
// (ie. dimming from 1000->800 is much more noticeable than 20000->19000)
const double SCREEN_SENSOR_LOOKUP[] = { 0.5, 0.55, 0.60, 0.64, 0.68, 0.72, 0.75, 0.78, 0.81, 0.84, 0.86,
                                        0.88, 0.90, 0.91, 0.92, 0.93, 0.94, 0.95, 0.96, 0.97, 0.98, 0.99 };
const double KBD_SENSOR_LOOKUP[] = { 1.0, 0.95, 0.90, 0.86, 0.82, 0.78, 0.75, 0.72, 0.69, 0.66, 0.64, 0.62, 
                                     0.60, 0.59, 0.58, 0.57, 0.56, 0.55, 0.54, 0.53, 0.52, 0.51 };

int min(int a, int b) { return a < b ? a : b; }
int max(int a, int b) { return b < a ? a : b; }
#define countof(array) (sizeof(array)/sizeof(array[0]))

// return the multiplier applied based on whether or not the power adapter is connected
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

// adjust either the screen or keyboard brightness given the relevant set of factors
void adjust_single_brightness(double new_proportion, const char* path, double* offset, int* last_brightness, int min_brightness, int max_brightness, double sensor_multiplier) {
    // open up the device and write the new value in
    int current_brightness = *last_brightness;
    FILE* f = fopen(path, "r+");
    if(f) {
	if(fscanf(f, "%d", &current_brightness) == 1) {
	    if(current_brightness != *last_brightness && *last_brightness >= 0) {
		// something's altered the value since we last wrote it. calculate and apply an offset.
		*offset += (double)(current_brightness - *last_brightness) / (max_brightness - min_brightness);
	    }
	    int new_brightness = (int)((new_proportion + *offset)*(max_brightness - min_brightness)) + min_brightness;
	    if(is_dimmed) {
		new_brightness = min_brightness * power_multiplier; // never adjust any higher than the min when fully dimmed (except for ac adapter multiplier)
	    } else {
		new_brightness = min(max(new_brightness * power_multiplier * sensor_multiplier, min_brightness), max_brightness);
	    }
	    fseek(f, 0, SEEK_SET);
	    fprintf(f, "%d", new_brightness);
	    *last_brightness = new_brightness;
	}
	fclose(f);
    } else {
        fprintf(stderr, "Could not open device file %s\n", path);
    }

    // print some debugging info if it'll be visible
    dprintf("Adjusting brightness at %s\n"
	    "    New proportion: %lf\n"
	    "    New brightness: %d\n"
            "    Power multiplier: %lf\n"
	    "    Sensor multiplier: %lf\n"
	    "    Offset: %lf\n",
            path, new_proportion, *last_brightness, power_multiplier, sensor_multiplier, *offset);
}

// adjust both screen and keyboard backlights to given proportion
void adjust_brightness(double proportion) {
    adjust_single_brightness(proportion, screen_backlight_path, &screen_offset, &last_screen_brightness, SCREEN_DIM, screen_bright, screen_multiplier);
    adjust_single_brightness(proportion, kbd_backlight_path, &kbd_offset, &last_kbd_brightness, KBD_DIM, kbd_bright, kbd_multiplier);
    last_proportion = proportion;
}

int interpolate(int a, int b, double c) {
    return (int)(c*(b-a))+a;
}

// reread data from the ambient light sensor at the top of the lid
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
    // (ie. 0 can be anything from "pitch darkness" to "room with 60W lightbulb")
    // and in practice values over 20 or so don't make a lot of difference.
    //
    // the screen changes in rough proportion to ambient light
    // and the keyboard in inverse proportion to it
    double screen = (x < 0 || x >= countof(SCREEN_SENSOR_LOOKUP) ? 1.0 : SCREEN_SENSOR_LOOKUP[x]);
    // the keyboard multiplier changes in inverse proportion
    double kbd = (x < 0 || x > countof(KBD_SENSOR_LOOKUP) ? 0.5 : KBD_SENSOR_LOOKUP[x]);

    if(screen != screen_multiplier || kbd != kbd_multiplier) {

	dprintf("Light sensor value changed. Updating brightness\n"
		"    New sensor value: %d\n"
		"    New screen multiplier: %lf\n"
		"    New keyboard multiplier: %lf\n",
		x, screen, kbd);

	screen_multiplier = screen;
	kbd_multiplier = kbd;
	adjust_brightness(last_proportion);
    }
}

// implements the gradual dimming of the backlight once the machine has gone idle
int continuous_dim_backlight(Display* display, XScreenSaverInfo* info) {
    unsigned long initial_idle = info->idle;
    struct timespec tm_remaining = { 0, 0 };
    struct timespec ten_milliseconds = { 0, 10000000 };
    double proportion;
    for(proportion = 1.0; proportion >= 0.0; proportion -= 0.001) {
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
	if(info->idle < initial_idle) {
	    // obviously we've come out of idle in the last sleep. bail here.
	    return 1;
	}
	adjust_brightness(proportion);
	nanosleep(&ten_milliseconds, &tm_remaining);
    }
    adjust_brightness(0.0);
    is_dimmed = 1;
    return 0;
}

// locks the screen once machine has been deemed to be idle for a long time
void lock_screen() {
    // right, we simply want to run an arbitrary program here (usually slimlock)
    // system() doesn't work because we don't want to wait for it to return, so looks
    // like it's vfork()+exec() to the rescue
    dprintf("Forking to lock screen\n");
    pid_t pid = vfork();
    if(pid < 0) {
	fprintf(stderr, "VFork failed with %d\n", pid);
    } else if(pid == 0) {
        // we are now the child process. run the screen locker.
	execl(screen_lock_command, screen_lock_command, NULL);
	exit(EXIT_FAILURE); // we shouldn't get here. if we do exec() has failed - there's not much to be done, just bail.
    } else {
	dprintf("Forked child process %d. Continuing.\n", pid);
    }
}

// Waits until the user moves mouse or presses a key
void wait_for_event(Display* display, XScreenSaverInfo* info) {
    // waiting until something happens
    // currently just doing polling, not sure how possible it is to get notified of this event by X
    // it's not hard to get mouse/keyboard events for your window but I don't think you can get *any* such event
    // TODO: is there a different approach? could we hook into devices in /dev or something? may not be worth going down that rabbit hole though...
    struct timespec tm_remaining = { 0, 0 };
    struct timespec half_second = { 0, 500000000 }; // this is about the longest period that still feels reasonably responsive when undimming the screen
    unsigned long last_idle;
    int locked_screen = 0;
    do {
        last_idle = info->idle;
        nanosleep(&half_second, &tm_remaining);
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
	update_light_sensor();

	// now lock screen if we've gone over the threshold - but obviously only the first time
	// slimlock checks itself if it's already running, but we don't want to spawn new slimlock
	// processes every second if they're not going to do anything!
	if(!locked_screen && info->idle >= lock_delay_ms) {
	    lock_screen();
	    locked_screen = 1;
	}
    } while(info->idle >= last_idle);
}

// rereads the power adapter state and updates appropriately
void refresh_adapter_state() {
    power_multiplier = power_adapter_multiplier();
    // call again with the last adjustment passed in case it's changed.
    adjust_brightness(last_proportion);
}

void parse_options(int argc, char* argv[]) {
    // TODO: no doubt there are more things that really should be in here - various paths and whatnot
    int opt;
    while ((opt = getopt(argc, argv, "ds:k:t:l:")) != -1) {
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
	case 'l':
	    lock_delay_ms = atoi(optarg) * 1000;
	    break;
        default: /* '?' */
            fprintf(stderr, "Usage: %s [-d] [-s max_screen_brightness] [-k max_keyboard_brightness] [-t time_before_dim] [-l lock_delay]\n", argv[0]);
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
	    fprintf(stderr, "Fork failed with %d\n", pid);
	} else if(pid > 0) {
	    return EXIT_SUCCESS; // child has forked off correctly, we terminate immediately.
	}
    }

    // we will be sent a USR1 by acpid when the power adapter is plugged/unplugged. at that point we reread its state.
    // this approach saves us from having to poll that every few seconds to see if it's changed
    // (one of the few places where it's practical to avoid polling)
    signal(SIGUSR1, refresh_adapter_state);

    XScreenSaverInfo* info = XScreenSaverAllocInfo();
    info->idle = 0; // ensure this is initialised since we'll calculate on it shortly
    Display* display = XOpenDisplay(0);
    if(display == NULL) {
        fprintf(stderr, "Couldn't connect to X display\n");
        return EXIT_FAILURE;
    }

    // do some initial updates to make sure everything's been read at first
    // this will set the initial brightness values for us as well
    refresh_adapter_state();
    update_light_sensor();

    // NB. ideally we would use select() or something to wait for the applesmc sysfs entry
    //     to change, but it doesn't seem to work. Not sure that's possible on this kind of hardware sensor?

    while(1) {
        // we've just gone idle. wait in 2 second chunks to keep checking the light sensor
        int i;
	for(i = 0; i < time_before_dim * 1000 - info->idle; i += 2000) {
	    sleep(2);
	    update_light_sensor();
	    dprintf("Time until dimming planned to begin: %ld\n", time_before_dim - info->idle/1000 - i/1000);
	}
	// now check the idle time again
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
	if(info->idle < time_before_dim*1000) {
	    // we must have been woken in between. go back to waiting.
	    continue;
	}

	// here we have waited the requisite amount of time. dim the display.
	dprintf("Dimming display\n");
	if(!continuous_dim_backlight(display, info)) {
	    wait_for_event(display, info);
	}
	// once we get here, we are undimming because something's happened
	is_dimmed = 0;
	adjust_brightness(1.0);
    }
}
