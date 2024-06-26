/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
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

#include "sprotocol.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

#define PACK(a,b,c,d) ((d << 24) | (c << 16) | (b << 8) | a)
#define BUF_SIZE 4096

struct channel_s
{
  struct {
    char buffer[BUF_SIZE];
    int pos, len;
  } input;
  struct {
    char buffer[BUF_SIZE];
    int pos;
  } output;
  int passed_fd;
  char *buf;
  int buf_size;
};

static ssize_t read_(channel_t *t, int fd, void *data, size_t len)
{
  char msg_control[CMSG_SPACE(1 * sizeof(int))] = {0,};
  int32_t pid;
  struct iovec iov = { .iov_base = data, .iov_len = len };
  struct msghdr msg = {
    .msg_iov = &iov, .msg_iovlen = 1,
    .msg_controllen = sizeof(msg_control),
  };
  msg.msg_control = &msg_control;

  ssize_t recvd;
  do { recvd = recvmsg(fd, &msg, 0); } 
  while (recvd == -1 && errno == EINTR);

  if (recvd == -1)
  {
    if (errno == ECONNRESET)
    {
      fprintf(stderr, "sprotocol:read_: ECONNRESET\n");
      fflush(stderr);
      return 0;
    }
    perror("recvmsg");
    mabort();
  }

  struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);

  if (cm != NULL)
  {
    int *fds0 = (int*)CMSG_DATA(cm);
    int nfds = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);

    if (nfds != 1)
      abort();

    if (t->passed_fd != -1)
      abort();
      //close(t->passed_fd);
    t->passed_fd = fds0[0];
  }

  return recvd;
}

static int buffered_read_at_least(channel_t *t, int fd, char *buf, int atleast, int size)
{
  int n;
  char *org = buf, *ok = buf + atleast;
  if (size < atleast) abort();

  while (1)
  {
    n = read_(t, fd, buf, size);
    if (n == -1)
      pabort();
    else if (n == 0)
      return 0;

    buf += n;
    size -= n;
    if (buf >= ok)
      break;
  }
  return (buf - org);
}

static bool read_all(channel_t *t, int fd, char *buf, int size)
{
  while (size > 0)
  {
    int n = read_(t, fd, buf, size);
    if (n == 0)
      return 0;

    buf += n;
    size -= n;
  }
  return 1;
}

static void write_all(int fd, const char *buf, int size)
{
  while (size > 0)
  {
    int n = write(fd, buf, size);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      perror("sprotocol.c write_all");
      print_backtrace();
      if (errno == ECONNRESET)
        return;
    }
    if (n <= 0)
      mabort();

    buf += n;
    size -= n;
  }
}

static void cflush(channel_t *c, int fd)
{
  int pos = c->output.pos;
  if (pos == 0) return;
  write_all(fd, c->output.buffer, pos);
  c->output.pos = 0;
}

static bool refill_at_least(channel_t *c, int fd, int at_least)
{
  int avail = (c->input.len - c->input.pos);
  if (avail >= at_least)
    return 1;

  memmove(c->input.buffer, c->input.buffer + c->input.pos, avail);

  c->input.pos = 0;
  while (avail < at_least)
  {
    int n = read_(c, fd, c->input.buffer + avail, BUF_SIZE - avail);
    if (n == 0)
    {
      c->input.len = avail;
      return 0;
    }
    avail += n;
  }

  c->input.len = avail;
  return 1;
}

/* HANDSHAKE */

#define HND_SERVER "TEXPRESSOS01"
#define HND_CLIENT "TEXPRESSOC01"

bool channel_handshake(channel_t *c, int fd)
{
  char answer[LEN(HND_CLIENT)];
  write_all(fd, HND_SERVER, LEN(HND_SERVER));
  if (!read_all(c, fd, answer, LEN(HND_CLIENT)))
    return 0;
  c->input.len = c->input.pos = 0;
  c->output.pos = 0;
  return (strncmp(HND_CLIENT, answer, LEN(HND_CLIENT)) == 0);
}

/* PROTOCOL DEFINITION */

#define CASE(K,X) case K##_##X: return STR(X)

const char *query_to_string(enum query q)
{
  switch (q)
  {
    CASE(Q,OPEN);
    CASE(Q,READ);
    CASE(Q,WRIT);
    CASE(Q,CLOS);
    CASE(Q,SIZE);
    CASE(Q,SEEN);
    CASE(Q,GPIC);
    CASE(Q,SPIC);
    CASE(Q,CHLD);
  }
}

const char *answer_to_string(enum answer q)
{
  switch (q)
  {
    CASE(A,DONE);
    CASE(A,PASS);
    CASE(A,SIZE);
    CASE(A,READ);
    CASE(A,FORK);
    CASE(A,OPEN);
    CASE(A,GPIC);
  }
}

const char *ask_to_string(enum ask q)
{
  switch (q)
  {
    CASE(C,FLSH);
  }
}

channel_t *channel_new(void)
{
  channel_t *c = calloc(sizeof(channel_t), 1);
  if (!c) mabort();
  c->buf = malloc(256);
  if (!c->buf) mabort();
  c->buf_size = 256;
  c->passed_fd = -1;
  return c;
}

void channel_free(channel_t *c)
{
  free(c->buf);
  free(c);
}

static void resize_buf(channel_t *t)
{
  int old_size = t->buf_size;
  int new_size = old_size * 2;
  char *buf = malloc(new_size);
  if (!buf) mabort();
  memcpy(buf, t->buf, old_size);
  free(t->buf);
  t->buf = buf;
  t->buf_size = new_size;
}

static int cgetc(channel_t *t, int fd)
{
  if (t->input.pos == t->input.len)
    if (!refill_at_least(t, fd, 1))
      return 0;
  return t->input.buffer[t->input.pos++];
}

static int read_zstr(channel_t *t, int fd, int *pos)
{
  int c, p0 = *pos;
  do {
    if (*pos == t->buf_size)
      resize_buf(t);
    c = cgetc(t, fd);
    t->buf[*pos] = c;
    *pos += 1;
  } while (c != 0);
  return p0;
}

static bool read_bytes(channel_t *t, int fd, int pos, int size)
{
  while (t->buf_size < pos + size)
    resize_buf(t);

  int ipos = t->input.pos, ilen = t->input.len;
  if (ipos + size <= ilen)
  {
    memcpy(&t->buf[pos], t->input.buffer + ipos, size);
    t->input.pos += size;
    return 1;
  }

  int isize = ilen - ipos;
  memcpy(&t->buf[pos], t->input.buffer + ipos, isize);
  pos += isize;
  size -= isize;
  t->input.pos = t->input.len = 0;
  return read_all(t, fd, &t->buf[pos], size);
}

static void write_bytes(channel_t *t, int fd, void *buf, int size)
{
  if (t->output.pos + size <= BUF_SIZE)
  {
    memcpy(t->output.buffer + t->output.pos, buf, size);
    t->output.pos += size;
    return;
  }

  cflush(t, fd);

  if (size > BUF_SIZE)
    write_all(fd, buf, size);
  else
  {
    memcpy(t->output.buffer, buf, size);
    t->output.pos = size;
  }
}

static bool try_read_u32(channel_t *t, int fd, uint32_t *tag)
{
  if (!refill_at_least(t, fd, 4))
    return 0;
  memcpy(tag, t->input.buffer + t->input.pos, 4);
  t->input.pos += 4;
  return 1;
}

static uint32_t read_u32(channel_t *t, int fd)
{
  int avail = t->input.len - t->input.pos;

  if (!refill_at_least(t, fd, 4))
    return 0;

  uint32_t tag;
  memcpy(&tag, t->input.buffer + t->input.pos, 4);
  t->input.pos += 4;

  return tag;
}

static void write_u32(channel_t *t, int fd, uint32_t u)
{
  write_bytes(t, fd, &u, 4);
}

static float read_f32(channel_t *t, int fd)
{
  if (!refill_at_least(t, fd, 4))
    return 0;

  float f;
  memcpy(&f, t->input.buffer + t->input.pos, 4);
  t->input.pos += 4;

  return f;
}

static void write_f32(channel_t *t, int fd, float f)
{
  write_bytes(t, fd, &f, 4);
}

void log_query(FILE *f, query_t *r)
{
  fprintf(f, "%04dms: ", r->time);
  switch (r->tag)
  {
    case Q_OPEN:
      {
        fprintf(f, "OPEN(%d, \"%s\", \"%s\")\n", r->open.fid, r->open.path, r->open.mode);
        break;
      }
    case Q_READ:
      {
        fprintf(f, "READ(%d, %d, %d)\n", r->read.fid, r->read.pos, r->read.size);
        break;
      }
    case Q_WRIT:
      {
        fprintf(f, "WRIT(%d, %d, %d)\n",
            r->writ.fid, r->writ.pos, r->writ.size);
        break;
      }
    case Q_CLOS:
      {
        fprintf(f, "CLOS(%d)\n", r->clos.fid);
        break;
      }
    case Q_SIZE:
      {
        fprintf(f, "SIZE(%d)\n", r->size.fid);
        break;
      }
    case Q_SEEN:
      {
        fprintf(f, "SEEN(%d, %d)\n", r->seen.fid, r->seen.pos);
        break;
      }
    case Q_GPIC:
      {
        fprintf(f, "GPIC(\"%s\",%d,%d)\n", r->gpic.path, r->gpic.type, r->gpic.page);
        break;
      }
    case Q_SPIC:
      {
        fprintf(f, "SPIC(\"%s\", %d, %d, %.02f, %.02f, %.02f, %.02f)\n", 
                r->spic.path, 
                r->spic.cache.type, r->spic.cache.page,
                r->spic.cache.bounds[0], r->spic.cache.bounds[1],
                r->spic.cache.bounds[2], r->spic.cache.bounds[3]);
        break;
      }
    case Q_CHLD:
      {
        fprintf(f, "CHLD(pid:%d, fd:%d)\n", r->chld.pid, r->chld.fd);
        break;
      }
    default:
      mabort();
  }
}

bool channel_has_pending_query(channel_t *t, int fd, int timeout)
{
  if (t->input.pos != t->input.len) return 1;

  struct pollfd pfd;
  int n;
  while(1)
  {
    pfd.fd = fd;
    pfd.events = POLLRDNORM;
    pfd.revents = 0;
    n = poll(&pfd, 1, timeout);
    if (!(n == -1 && errno == EINTR))
      break;
  }

  if (n == -1)
    pabort();
  if (n == 0)
    return 0;
  return 1;
}

enum query channel_peek_query(channel_t *t, int fd)
{
  uint32_t result = read_u32(t, fd);
  if (result == 0)
    abort();
  t->input.pos -= 4;
  return result;
}

bool channel_read_query(channel_t *t, int fd, query_t *r)
{
  uint32_t tag;

  if (!try_read_u32(t, fd, &tag))
    return 0;
  r->tag = tag;
  r->time = read_u32(t, fd);
  int pos = 0;
  switch (tag)
  {
    case Q_OPEN:
      {
        r->open.fid = read_u32(t, fd);
        int pos_path = read_zstr(t, fd, &pos);
        int pos_mode = read_zstr(t, fd, &pos);
        r->open.path = &t->buf[pos_path];
        r->open.mode = &t->buf[pos_mode];
        break;
      }
    case Q_READ:
      {
        r->read.fid = read_u32(t, fd);
        r->read.pos = read_u32(t, fd);
        r->read.size = read_u32(t, fd);
        break;
      }
    case Q_WRIT:
      {
        r->writ.fid = read_u32(t, fd);
        r->writ.pos = read_u32(t, fd);
        r->writ.size = read_u32(t, fd);
        if (!read_bytes(t, fd, 0, r->writ.size))
          return 0;
        r->writ.buf = t->buf;
        break;
      }
    case Q_CLOS:
      {
        r->clos.fid = read_u32(t, fd);
        break;
      }
    case Q_SIZE:
      {
        r->size.fid = read_u32(t, fd);
        break;
      }
    case Q_SEEN:
      {
        r->seen.fid = read_u32(t, fd);
        r->seen.pos = read_u32(t, fd);
        break;
      }
    case Q_GPIC:
      {
        int pos_path = read_zstr(t, fd, &pos);
        r->gpic.path = &t->buf[pos_path];
        r->gpic.type = read_u32(t, fd);
        r->gpic.page = read_u32(t, fd);
        break;
      }
    case Q_SPIC:
      {
        int pos_path = read_zstr(t, fd, &pos);
        r->spic.path = &t->buf[pos_path];
        r->spic.cache.type = read_u32(t, fd);
        r->spic.cache.page = read_u32(t, fd);
        r->spic.cache.bounds[0] = read_f32(t, fd);
        r->spic.cache.bounds[1] = read_f32(t, fd);
        r->spic.cache.bounds[2] = read_f32(t, fd);
        r->spic.cache.bounds[3] = read_f32(t, fd);
        break;
      }
    case Q_CHLD:
      {
        r->chld.pid = read_u32(t, fd);
        if (t->passed_fd == -1)
          abort();
        r->chld.fd = t->passed_fd;
        t->passed_fd = -1;
        break;
      }
    default:
    {
      fprintf(stderr, "unexpected tag: %c%c%c%c\n",
              tag & 0xFF, (tag >> 8) & 0xFF, (tag >> 16) & 0xFF, (tag >> 24) & 0xFF);
      mabort();
    }
  }
  if (LOG)
  {
    fprintf(stderr, "[info] <- ");
    log_query(stderr, r);
  }
  return 1;
}

void channel_write_ask(channel_t *t, int fd, ask_t *a)
{
  write_u32(t, fd, a->tag);
  switch (a->tag)
  {
    case C_FLSH: break;
    default: mabort();
  }
}

static void write_time(channel_t *t, int fd, struct stat_time tm)
{
  write_u32(t, fd, tm.sec);
  write_u32(t, fd, tm.nsec);
}

void channel_write_answer(channel_t *t, int fd, answer_t *a)
{
  if (LOG)
  {
    if (a->tag == A_READ)
      fprintf(stderr, "[info] -> READ %d\n", a->read.size);
    else
      fprintf(stderr, "[info] -> %s\n", answer_to_string(a->tag));
  }
  write_u32(t, fd, a->tag);
  switch (a->tag)
  {
    case A_DONE:
      break;
    case A_PASS:
      break;
    case A_FORK:
      break;
    case A_READ:
      write_u32(t, fd, a->read.size);
      write_bytes(t, fd, t->buf, a->read.size);
      break;
    case A_SIZE:
      write_u32(t, fd, a->size.size);
      break;
    case A_OPEN:
      write_u32(t, fd, a->open.size);
      write_bytes(t, fd, t->buf, a->open.size);
      break;
    case A_GPIC:
      write_f32(t, fd, a->gpic.bounds[0]);
      write_f32(t, fd, a->gpic.bounds[1]);
      write_f32(t, fd, a->gpic.bounds[2]);
      write_f32(t, fd, a->gpic.bounds[3]);
      break;
    default:
      mabort();
  }
}

void channel_flush(channel_t *t, int fd)
{
  cflush(t, fd);
}

void channel_reset(channel_t *t)
{
  t->input.pos = t->input.len = 0;
  t->output.pos = 0;
}

void *channel_get_buffer(channel_t *t, size_t n)
{
  while (n > t->buf_size)
    resize_buf(t);
  return t->buf;
}
