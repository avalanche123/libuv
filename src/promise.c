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
#include "uv-common.h"

int uv_promise_init(uv_promise_t* handle) {
  int rc;

  rc = uv_mutex_init(&handle->mutex);

  if (rc != 0)
    return rc;

  rc = uv_cond_init(&handle->cond);

  if (rc != 0)
    uv_mutex_destroy(&handle->mutex);

  handle->status  = UV_PROMISE_PENDING;
  handle->result  = NULL;
  handle->code    = 0;
  handle->waiting = 0;

  return rc;
}

int uv_promise_fulfil(uv_promise_t* handle, void* result) {
  int rc;

  rc = -EINVAL;

  uv_mutex_lock(&handle->mutex);

  do {
    if (handle->status != UV_PROMISE_PENDING)
      break;

    handle->status = UV_PROMISE_FULFILLED;
    handle->result = result;

    if (handle->waiting > 0)
      uv_cond_broadcast(&handle->cond);

    rc = 0;
  }
  while (0);

  uv_mutex_unlock(&handle->mutex);

  return rc;
}

int uv_promise_break(uv_promise_t* handle, int code) {
  int rc;

  rc = -EINVAL;

  uv_mutex_lock(&handle->mutex);

  do {
    if (handle->status != UV_PROMISE_PENDING)
      break;

    handle->status = UV_PROMISE_BROKEN;
    handle->code   = code;

    if (handle->waiting > 0)
      uv_cond_broadcast(&handle->cond);

    rc = 0;
  } while (0);

  uv_mutex_unlock(&handle->mutex);

  return rc;
}

uv_promise_result_t uv_promise_get(uv_promise_t* handle) {
  uv_promise_result_t rs;

  rs = (uv_promise_result_t) {
    .status = UV_PROMISE_CANCELLED,
    .code   = 0,
    .result = NULL
  };

  uv_mutex_lock(&handle->mutex);
  handle->waiting++;

  while(handle->status == UV_PROMISE_PENDING)
    uv_cond_wait(&handle->cond, &handle->mutex);

  handle->waiting--;

  rs.status = handle->status;
  rs.code   = handle->code;
  rs.result = handle->result;

  uv_mutex_unlock(&handle->mutex);

  return rs;
}

uv_promise_result_t uv_promise_trywait(uv_promise_t* handle) {
  uv_promise_result_t rs;

  rs = (uv_promise_result_t) {
    .status = UV_PROMISE_PENDING,
    .code   = 0,
    .result = NULL
  };

  if (uv_mutex_trylock(&handle->mutex) == 0) {
    rs.status = handle->status;
    rs.code   = handle->code;
    rs.result = handle->result;

    uv_mutex_unlock(&handle->mutex);
  }

  return rs;
}

void uv_promise_destroy(uv_promise_t* handle) {
  uv_mutex_lock(&handle->mutex);

  if (UV_PROMISE_PENDING == handle->status)
    handle->status = UV_PROMISE_CANCELLED;

  if (handle->waiting > 0)
    uv_cond_broadcast(&handle->cond);

  uv_mutex_unlock(&handle->mutex);

  uv_mutex_destroy(&handle->mutex);
  uv_cond_destroy(&handle->cond);
}
