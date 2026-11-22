/* SPDX-License-Identifier: Apache-2.0 */
#ifndef IO_BOARD_H_
#define IO_BOARD_H_

#include <stddef.h>

/* Number of switch channels described in devicetree (zephyr,user relay-gpios). */
size_t io_board_channel_count(void);

/* Configure relay outputs + button inputs and hook the relays to the device model.
 * Call before dm_init() so the restored state is driven onto the outputs. */
int io_board_init(void);

#endif
