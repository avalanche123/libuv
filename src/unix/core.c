/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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
#include "internal.h"

#include <stddef.h> /* NULL */
#include <stdio.h> /* printf */
#include <stdlib.h>
#include <string.h> /* strerror */
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h> /* PATH_MAX */
#include <sys/uio.h> /* writev */

#ifdef __linux__
# include <sys/ioctl.h>
#endif

#ifdef __sun
# include <sys/types.h>
# include <sys/wait.h>
#endif

#ifdef __APPLE__
# include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif

#ifdef __FreeBSD__
# include <sys/sysctl.h>
# include <sys/wait.h>
#endif

static uv_loop_t default_loop_struct;
static uv_loop_t* default_loop_ptr;

static void uv__finish_close(uv_handle_t* handle);


void uv_close(uv_handle_t* handle, uv_close_cb close_cb) {
  handle->close_cb = close_cb;

  switch (handle->type) {
  case UV_NAMED_PIPE:
    uv__pipe_close((uv_pipe_t*)handle);
    break;

  case UV_TTY:
  case UV_TCP:
    uv__stream_close((uv_stream_t*)handle);
    break;

  case UV_UDP:
    uv__udp_close((uv_udp_t*)handle);
    break;

  case UV_PREPARE:
    uv__prepare_close((uv_prepare_t*)handle);
    break;

  case UV_CHECK:
    uv__check_close((uv_check_t*)handle);
    break;

  case UV_IDLE:
    uv__idle_close((uv_idle_t*)handle);
    break;

  case UV_ASYNC:
    uv__async_close((uv_async_t*)handle);
    break;

  case UV_TIMER:
    uv__timer_close((uv_timer_t*)handle);
    break;

  case UV_PROCESS:
    uv__process_close((uv_process_t*)handle);
    break;

  case UV_FS_EVENT:
    uv__fs_event_close((uv_fs_event_t*)handle);
    break;

  case UV_POLL:
    uv__poll_close((uv_poll_t*)handle);
    break;

  default:
    assert(0);
  }

  handle->flags |= UV_CLOSING;
  uv__make_pending(handle);
}


int uv_is_closing(const uv_handle_t* handle) {
  return handle->flags & (UV_CLOSING | UV_CLOSED);
}


uv_loop_t* uv_default_loop(void) {
  if (default_loop_ptr)
    return default_loop_ptr;

  if (uv__loop_init(&default_loop_struct, /* default_loop? */ 1))
    return NULL;

  return (default_loop_ptr = &default_loop_struct);
}


uv_loop_t* uv_loop_new(void) {
  uv_loop_t* loop;

  if ((loop = malloc(sizeof(*loop))) == NULL)
    return NULL;

  if (uv__loop_init(loop, /* default_loop? */ 0)) {
    free(loop);
    return NULL;
  }

  return loop;
}


void uv_loop_delete(uv_loop_t* loop) {
  uv__loop_delete(loop);
#ifndef NDEBUG
  memset(loop, -1, sizeof *loop);
#endif
  if (loop == default_loop_ptr)
    default_loop_ptr = NULL;
  else
    free(loop);
}


static void uv__run_pending(uv_loop_t* loop) {
  uv_handle_t* p;
  uv_handle_t* q;

  if (!loop->pending_handles)
    return;

  for (p = loop->pending_handles, loop->pending_handles = NULL; p; p = q) {
    q = p->next_pending;
    p->next_pending = NULL;
    p->flags &= ~UV__PENDING;

    if (p->flags & UV_CLOSING) {
      uv__finish_close(p);
      continue;
    }

    switch (p->type) {
    case UV_NAMED_PIPE:
    case UV_TCP:
    case UV_TTY:
      uv__stream_pending((uv_stream_t*)p);
      break;
    default:
      abort();
    }
  }
}


static void uv__poll(uv_loop_t* loop, int block) {
  /* bump the loop's refcount, otherwise libev does
   * a zero timeout poll and we end up busy looping
   */
  ev_ref(loop->ev);
  ev_run(loop->ev, block ? EVRUN_ONCE : EVRUN_NOWAIT);
  ev_unref(loop->ev);
}


static int uv__should_block(uv_loop_t* loop) {
  return ngx_queue_empty(&loop->idle_handles)
      && !ngx_queue_empty(&loop->active_handles);
}


static int uv__run(uv_loop_t* loop) {
  uv__run_idle(loop);
  uv__run_pending(loop);

  if (uv__has_active_handles(loop) || uv__has_active_reqs(loop)) {
    uv__run_prepare(loop);
    /* Need to poll even if there are no active handles left, otherwise
     * uv_work_t reqs won't complete...
     */
    uv__poll(loop, uv__should_block(loop));
    uv__run_check(loop);
  }

  return uv__has_pending_handles(loop)
      || uv__has_active_handles(loop)
      || uv__has_active_reqs(loop);
}


int uv_run(uv_loop_t* loop) {
  while (uv__run(loop));
  return 0;
}


int uv_run_once(uv_loop_t* loop) {
  uv__run(loop);
  return 0;
}


void uv__handle_init(uv_loop_t* loop, uv_handle_t* handle,
    uv_handle_type type) {
  loop->counters.handle_init++;

  handle->loop = loop;
  handle->type = type;
  handle->flags = UV__REF; /* ref the loop when active */
  handle->next_pending = NULL;
}


void uv__finish_close(uv_handle_t* handle) {
  assert(!uv__is_active(handle));
  assert(handle->flags & UV_CLOSING);
  assert(!(handle->flags & UV_CLOSED));
  handle->flags |= UV_CLOSED;

  switch (handle->type) {
    case UV_PREPARE:
    case UV_CHECK:
    case UV_IDLE:
    case UV_ASYNC:
    case UV_TIMER:
    case UV_PROCESS:
      break;

    case UV_NAMED_PIPE:
    case UV_TCP:
    case UV_TTY:
      assert(!ev_is_active(&((uv_stream_t*)handle)->read_watcher));
      assert(!ev_is_active(&((uv_stream_t*)handle)->write_watcher));
      assert(((uv_stream_t*)handle)->fd == -1);
      uv__stream_destroy((uv_stream_t*)handle);
      break;

    case UV_UDP:
      uv__udp_finish_close((uv_udp_t*)handle);
      break;

    case UV_FS_EVENT:
      break;

    case UV_POLL:
      break;

    default:
      assert(0);
      break;
  }


  if (handle->close_cb) {
    handle->close_cb(handle);
  }

  uv__handle_unref(handle);
}


void uv_update_time(uv_loop_t* loop) {
  ev_now_update(loop->ev);
}


int64_t uv_now(uv_loop_t* loop) {
  return (int64_t)(ev_now(loop->ev) * 1000);
}


int uv_is_active(const uv_handle_t* handle) {
  return uv__is_active(handle);
}


static int uv_getaddrinfo_done(eio_req* req_) {
  uv_getaddrinfo_t* req = req_->data;
  struct addrinfo *res = req->res;
#if __sun
  size_t hostlen = strlen(handle->hostname);
#endif

  req->res = NULL;

  uv__req_unregister(req->loop, req);

  free(req->hints);
  free(req->service);
  free(req->hostname);

  if (req->retcode == 0) {
    /* OK */
#if EAI_NODATA /* FreeBSD deprecated EAI_NODATA */
  } else if (req->retcode == EAI_NONAME || req->retcode == EAI_NODATA) {
#else
  } else if (req->retcode == EAI_NONAME) {
#endif
    uv__set_sys_error(req->loop, ENOENT); /* FIXME compatibility hack */
#if __sun
  } else if (req->retcode == EAI_MEMORY && hostlen >= MAXHOSTNAMELEN) {
    uv__set_sys_error(req->loop, ENOENT);
#endif
  } else {
    req->loop->last_err.code = UV_EADDRINFO;
    req->loop->last_err.sys_errno_ = req->retcode;
  }

  req->cb(req, req->retcode, res);

  return 0;
}


static void getaddrinfo_thread_proc(eio_req *req) {
  uv_getaddrinfo_t* handle = req->data;

  handle->retcode = getaddrinfo(handle->hostname,
                                handle->service,
                                handle->hints,
                                &handle->res);
}


/* stub implementation of uv_getaddrinfo */
int uv_getaddrinfo(uv_loop_t* loop,
                   uv_getaddrinfo_t* handle,
                   uv_getaddrinfo_cb cb,
                   const char* hostname,
                   const char* service,
                   const struct addrinfo* hints) {
  eio_req* req;
  uv_eio_init(loop);

  if (handle == NULL || cb == NULL ||
      (hostname == NULL && service == NULL)) {
    uv__set_artificial_error(loop, UV_EINVAL);
    return -1;
  }

  uv__req_init(loop, handle, UV_GETADDRINFO);
  handle->loop = loop;
  handle->cb = cb;

  /* TODO don't alloc so much. */

  if (hints) {
    handle->hints = malloc(sizeof(struct addrinfo));
    memcpy(handle->hints, hints, sizeof(struct addrinfo));
  }
  else {
    handle->hints = NULL;
  }

  /* TODO security! check lengths, check return values. */

  handle->hostname = hostname ? strdup(hostname) : NULL;
  handle->service = service ? strdup(service) : NULL;
  handle->res = NULL;
  handle->retcode = 0;

  /* TODO check handle->hostname == NULL */
  /* TODO check handle->service == NULL */

  req = eio_custom(getaddrinfo_thread_proc, EIO_PRI_DEFAULT,
      uv_getaddrinfo_done, handle, &loop->uv_eio_channel);
  assert(req);
  assert(req->data == handle);

  return 0;
}


void uv_freeaddrinfo(struct addrinfo* ai) {
  if (ai)
    freeaddrinfo(ai);
}


/* Open a socket in non-blocking close-on-exec mode, atomically if possible. */
int uv__socket(int domain, int type, int protocol) {
  int sockfd;

#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  sockfd = socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);

  if (sockfd != -1)
    goto out;

  if (errno != EINVAL)
    goto out;
#endif

  sockfd = socket(domain, type, protocol);

  if (sockfd == -1)
    goto out;

  if (uv__nonblock(sockfd, 1) || uv__cloexec(sockfd, 1)) {
    close(sockfd);
    sockfd = -1;
  }

out:
  return sockfd;
}


int uv__accept(int sockfd, struct sockaddr* saddr, socklen_t slen) {
  int peerfd;

  assert(sockfd >= 0);

  while (1) {
#if __linux__
    peerfd = uv__accept4(sockfd, saddr, &slen, UV__SOCK_NONBLOCK|UV__SOCK_CLOEXEC);
    if (peerfd != -1)
      break;

    if (errno == EINTR)
      continue;

    if (errno != ENOSYS)
      break;
#endif

    if ((peerfd = accept(sockfd, saddr, &slen)) == -1) {
      if (errno == EINTR)
        continue;
      else
        break;
    }

    if (uv__cloexec(peerfd, 1) || uv__nonblock(peerfd, 1)) {
      close(peerfd);
      peerfd = -1;
    }

    break;
  }

  return peerfd;
}


int uv__nonblock(int fd, int set) {
#if FIONBIO
  return ioctl(fd, FIONBIO, &set);
#else
  int flags;

  if ((flags = fcntl(fd, F_GETFL)) == -1) {
    return -1;
  }

  if (set) {
    flags |= O_NONBLOCK;
  } else {
    flags &= ~O_NONBLOCK;
  }

  if (fcntl(fd, F_SETFL, flags) == -1) {
    return -1;
  }

  return 0;
#endif
}


int uv__cloexec(int fd, int set) {
#if __linux__
  /* Linux knows only FD_CLOEXEC so we can safely omit the fcntl(F_GETFD)
   * syscall. CHECKME: That's probably true for other Unices as well.
   */
  return fcntl(fd, F_SETFD, set ? FD_CLOEXEC : 0);
#else
  int flags;

  if ((flags = fcntl(fd, F_GETFD)) == -1) {
    return -1;
  }

  if (set) {
    flags |= FD_CLOEXEC;
  } else {
    flags &= ~FD_CLOEXEC;
  }

  if (fcntl(fd, F_SETFD, flags) == -1) {
    return -1;
  }

  return 0;
#endif
}


/* This function is not execve-safe, there is a race window
 * between the call to dup() and fcntl(FD_CLOEXEC).
 */
int uv__dup(int fd) {
  fd = dup(fd);

  if (fd == -1)
    return -1;

  if (uv__cloexec(fd, 1)) {
    SAVE_ERRNO(close(fd));
    return -1;
  }

  return fd;
}


/* TODO move to uv-common.c? */
size_t uv__strlcpy(char* dst, const char* src, size_t size) {
  const char *org;

  if (size == 0) {
    return 0;
  }

  org = src;
  while (--size && *src) {
    *dst++ = *src++;
  }
  *dst = '\0';

  return src - org;
}


uv_err_t uv_cwd(char* buffer, size_t size) {
  if (!buffer || !size) {
    return uv__new_artificial_error(UV_EINVAL);
  }

  if (getcwd(buffer, size)) {
    return uv_ok_;
  } else {
    return uv__new_sys_error(errno);
  }
}


uv_err_t uv_chdir(const char* dir) {
  if (chdir(dir) == 0) {
    return uv_ok_;
  } else {
    return uv__new_sys_error(errno);
  }
}
