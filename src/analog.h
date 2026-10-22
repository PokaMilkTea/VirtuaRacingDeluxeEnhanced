/* Analog controls through the patch table (m68kpatch.h): the left stick steers, the
   triggers press the throttle and the brake by how far they go, and all three are kept for
   the game's replay */
#ifndef VRDE_ANALOG_H
#define VRDE_ANALOG_H

#define ANALOG_STEERING_MAX 127   /* the game's steering target: -127 (right) to 127 (left) */
#define ANALOG_PEDAL_MAX    255   /* a trigger, fully pressed */

/* The patches. Until a control leaves its dead zone, the game runs as it is. */
void analog_install(void);

/* The controls for the frames from now on, 0 for any in its dead zone (the pad's buttons
   work it): the steering target, and how far the throttle and the brake are pressed
   (1 to ANALOG_PEDAL_MAX) */
void analog_set_input(int steering, int throttle, int brake);

/* A controller's stick axis (-32768 to 32767, negative = left) as the steering target, 0
   within the dead zone (in percent of the way out); the rest of the way goes from 1 to the
   full target */
int analog_stick_target(int axis, int dead_zone_percent);

/* A trigger's axis (0 to 32767) as a pedal, 0 within the dead zone, then 1 to
   ANALOG_PEDAL_MAX */
int analog_trigger_amount(int axis, int dead_zone_percent);

/* The controls kept for the replay go with a saved state: in their own file. A state
   without one (or with an older one) loads with none: the replay then goes by the pad's
   buttons alone. */
int analog_save_record(const char *path);
int analog_load_record(const char *path);
void analog_clear_record(void);

/* The pad's steering target and pedals computed in C on every tick, exactly as the game
   does, to check the patch table against the original (vrdetrace -x) */
void analog_install_c_pad(void);

#endif
