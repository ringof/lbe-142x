/*
 * SPDX-License-Identifier: MIT
 */
#ifndef CLI_VIEW_H
#define CLI_VIEW_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "lbe_device.h"   /* enum lbe_model, struct lbe_status */

struct lbe_model_ops;

/* CLI presentation, decoupled from device I/O so it is unit-testable without
 * hardware. All gating is driven off the model's capability vtable (`ops`),
 * not PID/enum literals -- see issue #8. */

/* Render the option help to `out`. `ops == NULL` prints the generic
 * (all-models) help; otherwise the help is tailored to that model's
 * capabilities. `model` is only consulted to single out the Mini. */
void lbe_print_usage(FILE *out, int model, const struct lbe_model_ops *ops);

/* Render the decoded `--status` report body (the "Device Status (0x..)" block
 * through the optional GNSS echo) to `out` for the given model/ops. */
void lbe_format_status(FILE *out, int model, const struct lbe_model_ops *ops,
                       const struct lbe_status *s);

/* Build the "portable|stationary|..." settable-by-name dynamic-model list into
 * `buf` (for `--help` and the `--dynmodel` error). Returns `buf`. */
const char *lbe_dynmodel_token_list(char *buf, size_t n);

/* Parse a `--dynmodel` argument: a keyword (portable/stationary/...) or a raw
 * u-blox value 0-255. On success writes the value to *out and returns 0;
 * returns -1 if the argument is neither. */
int lbe_dynmodel_parse(const char *arg, uint8_t *out);

#endif /* CLI_VIEW_H */
