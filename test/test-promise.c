/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
on_timeout_fulfill(uv_timer_t* handle)
{
  uv_promise_t* promise;

  promise = handle->data;
 
  uv_promise_fulfil(promise, "ok");
}

void
on_timeout_break(uv_timer_t* handle)
{
  uv_promise_t* promise;

  promise = handle->data;
 
  uv_promise_break(promise, -5);
}

static void fulfill_promise(void *arg) {
  uv_loop_t* loop;
  uv_timer_t* timeout;
  int rc;

  loop = calloc(1, sizeof(uv_loop_t));
  timeout = calloc(1, sizeof(uv_timer_t));

  ASSERT(loop);
  ASSERT(timeout);

  rc = uv_loop_init(loop);
  ASSERT(0 == rc);
  rc = uv_timer_init(loop, timeout);
  ASSERT(0 == rc);

  timeout->data = arg;

  rc = uv_timer_start(timeout, on_timeout_fulfill, 0, 0);
  ASSERT(0 == rc);

  uv_run(loop, UV_RUN_DEFAULT);
  uv_loop_close(loop);
  free(loop);
}

static void break_promise(void *arg) {
  uv_loop_t* loop;
  uv_timer_t* timeout;
  int rc;

  loop = calloc(1, sizeof(uv_loop_t));
  timeout = calloc(1, sizeof(uv_timer_t));

  ASSERT(loop);
  ASSERT(timeout);

  rc = uv_loop_init(loop);
  ASSERT(0 == rc);
  rc = uv_timer_init(loop, timeout);
  ASSERT(0 == rc);

  timeout->data = arg;

  rc = uv_timer_start(timeout, on_timeout_break, 0, 0);
  ASSERT(0 == rc);

  uv_run(loop, UV_RUN_DEFAULT);
  uv_loop_close(loop);
  free(loop);
}

TEST_IMPL(promise_can_be_fulfilled) {
  uv_promise_t promise;
  uv_thread_t thread;
  uv_promise_result_t result;

  ASSERT(0 == uv_promise_init(&promise));
  ASSERT(0 == uv_thread_create(&thread, fulfill_promise, &promise));

  result = uv_promise_get(&promise);
  ASSERT(UV_PROMISE_FULFILLED == result.status);
  ASSERT(0 == strcmp(result.result, "ok"));

  uv_promise_destroy(&promise);
  uv_thread_join(&thread);

  return 0;
}

TEST_IMPL(promise_can_be_broken) {
  uv_promise_t promise;
  uv_thread_t thread;
  uv_promise_result_t result;

  ASSERT(0 == uv_promise_init(&promise));
  ASSERT(0 == uv_thread_create(&thread, break_promise, &promise));

  result = uv_promise_get(&promise);
  ASSERT(UV_PROMISE_BROKEN == result.status);
  ASSERT(-5 == result.code);

  uv_promise_destroy(&promise);
  uv_thread_join(&thread);

  return 0;
}
