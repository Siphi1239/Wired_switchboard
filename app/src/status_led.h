/* SPDX-License-Identifier: Apache-2.0 */
#ifndef STATUS_LED_H_
#define STATUS_LED_H_

enum status_led_mode {
	STATUS_BOOT,         /* on            */
	STATUS_NO_NETWORK,   /* fast blink    */
	STATUS_NETWORK_UP,   /* slow blink    */
	STATUS_ONLINE,       /* off (quiet in normal operation), short flash every 5 s */
	STATUS_ERROR,        /* double blink  */
};

int status_led_init(void);
void status_led_set(enum status_led_mode mode);

#endif
