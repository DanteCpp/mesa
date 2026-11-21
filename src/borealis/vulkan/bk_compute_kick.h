/*
 * Copyright 2026 Aurora Silicon contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

struct bk_device;
struct bk_queue;

/* Sets the kicker up for a G17C device whose hk_compute_kick_ms is not 0.
 * Called once the device's queues exist; never fails device creation. */
void bk_compute_kick_init(struct bk_device *dev, unsigned period_ms);

/* Stops and joins the kicker thread, waits for the last kicks and destroys
 * the kick queues and objects. Called before anything else of the device is
 * destroyed. Notes that arrive afterwards change nothing. */
void bk_compute_kick_stop(struct bk_device *dev);

/* Frees the rest. Called once the application's queues are finished, so
 * that no submit thread can be in bk_compute_kick_note. */
void bk_compute_kick_finish(struct bk_device *dev);

/* Called from the queue's submission callback after compute work was
 * submitted to a watched queue, with its timeline value already advanced. */
void bk_compute_kick_note(struct bk_device *dev, struct bk_queue *queue);
